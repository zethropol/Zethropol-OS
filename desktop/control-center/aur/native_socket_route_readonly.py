"""Namespace-free, fixed UID949 gpgconf query. Never starts/connects to agents.

Root is used only to validate public executable metadata and drop identity.
Landlock >=3 denies filesystem mutation/other executables/private key reads;
seccomp denies all syscalls outside a fixed query/loader allowlist. No fallback.
"""
import ctypes
import errno
import json
import os
import platform
import array
import re
import selectors
import socket
import time
from types import SimpleNamespace
import stat
import subprocess
import sys

HOME='/var/lib/zethropol-aur-release/gnupg'
CANDIDATE=HOME+'/S.gpg-agent'  # Accepted host route; runtime design cancelled.
GPGCONF='/usr/bin/gpgconf'
SECCOMP='/usr/lib/libseccomp.so.2'
LOADERS={'x86_64':'/lib64/ld-linux-x86-64.so.2','aarch64':'/lib/ld-linux-aarch64.so.1'}
ENV={'PATH':'/usr/bin','LC_ALL':'C'}  # manifest_workflow.sign GPG subprocess
ARGV=[GPGCONF,'--homedir',HOME,'--list-dirs','agent-socket']
MARKER='ZETHROPOL_ROUTE_V2 '
PHASE='initial'
ALLOW_SYSCALLS=(
    'read','write','readv','writev','close','close_range','fstat','newfstatat',
    'stat','lstat','statx','lseek','mmap','mprotect','munmap','mremap','madvise',
    'brk','rt_sigaction','rt_sigprocmask','rt_sigreturn','sigaltstack',
    'pread64','preadv','access','faccessat','faccessat2','arch_prctl',
    'set_tid_address','set_robust_list','rseq','prlimit64','getrandom',
    'getuid','geteuid','getgid','getegid','getgroups','getpid','getppid','gettid',
    'uname','readlink','readlinkat','getcwd','fcntl','clock_gettime',
    'gettimeofday','time','sysinfo','sched_getaffinity','futex',
    'open','openat','execve','getdents64','exit','exit_group',
)


class Blocked(OSError):
    pass


def phase(name):
    global PHASE
    PHASE=name


def check(ok,code=errno.EPERM):
    if not ok:raise Blocked(code,'fixed route guard')


def kernel_ok(value):
    if value<0:
        e=ctypes.get_errno()
        raise Blocked(e or errno.EIO,'kernel confinement refused')
    return value


def mapped(rows,value):
    for row in rows.splitlines():
        inner,outer,count=map(int,row.split())
        if inner<=value<inner+count:return outer+value-inner==value
    return False


def process_context():
    phase('identity-process-context')
    check(os.getresuid()==(949,949,949) and os.getresgid()==(948,948,948) and os.getgroups()==[948])
    with open('/proc/self/status') as f:
        fields=dict(line.split(':',1) for line in f if ':' in line)
    check(fields['Uid'].split()==['949']*4 and fields['Gid'].split()==['948']*4)
    for field in ('CapEff','CapPrm','CapInh','CapAmb'):check(int(fields[field],16)==0)
    check(fields['NoNewPrivs'].strip()=='1')
    # An inherited ERRNO filter has higher precedence than USER_NOTIF and could
    # hide mkdir failure/fallback. Never certify a route in that context.
    phase('inherited-seccomp-policy')
    check(fields.get('Seccomp','').strip()=='0',errno.ENOTSUP)
    phase('identity-namespace-mapping')
    with open('/proc/self/uid_map') as f:check(mapped(f.read(),949))
    with open('/proc/self/gid_map') as f:check(mapped(f.read(),948))
    check(os.getcwd()=='/')
    return {'identity':'PASS_REAL_EFFECTIVE_SAVED_FS_UID949_GID948',
            'capabilities':'ZERO','no_new_privs':True,'cwd':'/',
            'supplementary_groups':[948],'pid':os.getpid(),'parent_pid':os.getppid(),
            'kernel_uids':[949]*4,'kernel_gids':[948]*4,'inherited_seccomp':0,
            'namespace_creation':False}


def drop_identity(libc):
    # Same ordering/mechanism as manifest_bundle_launcher.drop_identity; no
    # signer module is imported or invoked. Root setuid also drops saved UID.
    phase('identity-setgroups');os.setgroups([948])
    phase('identity-setgid');os.setgid(948)
    phase('identity-setuid');os.setuid(949)
    phase('identity-clear-capabilities')
    class Header(ctypes.Structure):
        _fields_=[('version',ctypes.c_uint32),('pid',ctypes.c_int)]
    class Data(ctypes.Structure):
        _fields_=[('effective',ctypes.c_uint32),('permitted',ctypes.c_uint32),('inheritable',ctypes.c_uint32)]
    kernel_ok(libc.capset(ctypes.byref(Header(0x20080522,0)),(Data*2)()))
    phase('no-new-privileges');kernel_ok(libc.prctl(38,1,0,0,0))


def home_context():
    phase('fixed-home-metadata')
    # Metadata only, no private directory enumeration or file content reads.
    for path in ('/','/var','/var/lib','/var/lib/zethropol-aur-release',HOME):
        s=os.lstat(path)
        private=path in ('/var/lib/zethropol-aur-release',HOME)
        check(stat.S_ISDIR(s.st_mode))
        check((s.st_uid,s.st_gid)==((949,948) if private else (0,0)))
        check(stat.S_IMODE(s.st_mode)==0o700 if private else not s.st_mode&0o022)
        check(not {'system.posix_acl_access','system.posix_acl_default'}&set(os.listxattr(path,follow_symlinks=False)))
    check(os.access(HOME,os.X_OK))


def landlock(libc):
    phase('landlock-abi')
    check(platform.machine() in ('x86_64','aarch64'),errno.ENOTSUP)
    libc.syscall.restype=ctypes.c_long
    abi=kernel_ok(libc.syscall(444,0,0,1))
    check(abi>=3,errno.ENOTSUP)
    phase('landlock-create-readonly-ruleset')
    # ABI3 includes REFER and TRUNCATE. Later ioctl rights are unnecessary:
    # ioctl is absent from the seccomp allowlist, as are metadata mutations.
    attr=ctypes.c_uint64((1<<15)-1)
    ruleset=kernel_ok(libc.syscall(444,ctypes.byref(attr),ctypes.sizeof(attr),0))
    class Beneath(ctypes.Structure):
        _pack_=1
        _fields_=[('access',ctypes.c_uint64),('fd',ctypes.c_int32)]
    def rule(path,rights,optional=False):
        phase('landlock-add-rule')
        try:fd=os.open(path,os.O_PATH|os.O_CLOEXEC|os.O_NOFOLLOW)
        except FileNotFoundError:
            if optional:return
            raise
        try:
            s=os.fstat(fd)
            check(not stat.S_ISLNK(s.st_mode),errno.ELOOP)
            kernel_ok(libc.syscall(445,ruleset,1,ctypes.byref(Beneath(rights,fd)),0))
        finally:os.close(fd)
    try:
        rule('/',1<<3)  # Directory reads only. File reads granted separately.
        for path in ('/usr','/etc'):rule(path,1<<2)
        # No HOME-wide file permission, hence no private-keys-v1.d reads.
        for name in ('common.conf','gpgconf.ctl'):rule(HOME+'/'+name,1<<2,optional=True)
        rule(GPGCONF,(1<<0)|(1<<2))
        # The kernel also checks PT_INTERP execution permission. No blanket
        # execute permission on /usr; only gpgconf and its native ELF loader.
        rule(os.path.realpath(LOADERS[platform.machine()]),(1<<0)|(1<<2))
        phase('landlock-restrict-self')
        kernel_ok(libc.syscall(446,ruleset,0))
    finally:os.close(ruleset)
    return abi


def seccomp(library,transport_fd=None):
    phase('seccomp-allowlist')
    library.seccomp_init.argtypes=[ctypes.c_uint32];library.seccomp_init.restype=ctypes.c_void_p
    library.seccomp_syscall_resolve_name.argtypes=[ctypes.c_char_p];library.seccomp_syscall_resolve_name.restype=ctypes.c_int
    library.seccomp_rule_add.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_int,ctypes.c_uint]
    library.seccomp_load.argtypes=[ctypes.c_void_p]
    library.seccomp_release.argtypes=[ctypes.c_void_p]
    library.seccomp_attr_set.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_uint32]
    ctx=library.seccomp_init(0x7fc00000 if transport_fd is not None else 0x50000|errno.EPERM)
    check(bool(ctx),errno.ENOMEM)
    try:
        rc=library.seccomp_attr_set(ctx,9,1)  # API_SYSRAWRC: preserve kernel errno
        if rc<0:raise Blocked(-rc,'raw errno attribute rejected')
        class Compare(ctypes.Structure):
            _fields_=[('arg',ctypes.c_uint),('op',ctypes.c_uint),('a',ctypes.c_uint64),('b',ctypes.c_uint64)]
        library.seccomp_rule_add_array.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_int,ctypes.c_uint,ctypes.POINTER(Compare)]
        for name in ALLOW_SYSCALLS:
            nr=library.seccomp_syscall_resolve_name(name.encode())
            if nr<0:continue  # Architecture-specific names absent on this arch.
            if name in ('open','openat'):
                mask=os.O_ACCMODE|os.O_CREAT|os.O_TRUNC|os.O_APPEND|(os.O_TMPFILE & ~os.O_DIRECTORY)
                cmp=Compare(1 if name=='open' else 2,7,mask,0)  # MASKED_EQ
                rc=library.seccomp_rule_add_array(ctx,0x7fff0000,nr,1,ctypes.byref(cmp))
            else:rc=library.seccomp_rule_add(ctx,0x7fff0000,nr,0)
            if rc<0:raise Blocked(-rc,'seccomp rule rejected')
        if transport_fd is not None:
            cmp=Compare(0,4,transport_fd,0)  # sendmsg ONLY on this inherited control FD
            nr=library.seccomp_syscall_resolve_name(b'sendmsg')
            rc=library.seccomp_rule_add_array(ctx,0x7fff0000,nr,1,ctypes.byref(cmp))
            if rc<0:raise Blocked(-rc,'control FD rule rejected')
        rc=library.seccomp_load(ctx)
        if rc<0:raise Blocked(-rc,'seccomp load rejected')
        if transport_fd is not None:
            library.seccomp_notify_fd.argtypes=[ctypes.c_void_p]
            listener=library.seccomp_notify_fd(ctx)
            if listener<0:raise Blocked(-listener,'notification listener unavailable')
            return listener
    finally:library.seccomp_release(ctx)


def emit(record):
    print(MARKER+json.dumps(record,sort_keys=True),file=sys.stderr,flush=True)


def child(actor=False):
    try:
        phase('confinement-library-load')
        libc=ctypes.CDLL(None,use_errno=True)
        library=ctypes.CDLL(SECCOMP,use_errno=True)
        transport=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM,0,fileno=TRANSPORT_FD)
        transport.set_inheritable(False)
        if actor:
            phase('actor-no-new-privileges');kernel_ok(libc.prctl(38,1,0,0,0))
        else:drop_identity(libc)
        context=process_context()
        home_context()
        context['landlock_abi']=landlock(libc)
        listener=seccomp(library,transport.fileno())
        phase('confinement-listener-transfer')
        transport.sendmsg([b'R'],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',[listener]))])
        os.close(listener);transport.close()
        context['seccomp_notification']=True
        phase('confinement-attested-before-exec')
        emit(dict(context,phase=PHASE,status='QUERY_CONTEXT_READY'))
        phase('gpgconf-exec')
        os.execve(GPGCONF,ARGV,ENV)
    except OSError as e:
        emit({'status':'BLOCKED','phase':PHASE,'errno':e.errno or errno.EIO,
              'errno_name':errno.errorcode.get(e.errno or errno.EIO,'UNKNOWN')})
    except Exception:
        emit({'status':'BLOCKED','phase':PHASE,'errno':None,'reason':'CONTEXT_OR_PROTOCOL_INVALID'})
    return 1


def valid_socket_path(path):
    if path==HOME+'/S.gpg-agent':return True
    prefixes=('/run/user/949/gnupg/','/run/gnupg/user/949/',
              '/var/run/user/949/gnupg/','/var/run/gnupg/user/949/')
    return (any(path.startswith(prefix) for prefix in prefixes) and
            path.endswith('/S.gpg-agent') and
            re.fullmatch(r'/[A-Za-z0-9._/-]{1,255}',path) is not None and
            not any(part in ('','.','..') for part in path.split('/')[1:]))


def evaluate(answer):
    result={'agent_started':False,'signing_attempted':False,'credential_readiness':'NOT_PROBED','headless_pinentry_required':False,
            'route_binding':'NOT_VERIFIED','service_context_equality':'NOT_PROVEN',
            'proposed_service_socket':CANDIDATE,'namespace_creation':False}
    records=[]
    for line in answer.stderr.decode('ascii',errors='replace').splitlines():
        if line.startswith(MARKER):records.append(json.loads(line[len(MARKER):]))
    if records and records[-1].get('status')=='BLOCKED':
        return dict(result,native_route_probe='BLOCKED_NAMESPACE_FREE_QUERY',**records[-1])
    if len(records)!=1 or records[0].get('status')!='QUERY_CONTEXT_READY':
        return dict(result,native_route_probe='BLOCKED_CHILD_CONTEXT_UNATTESTED',phase='child-before-attestation',
                    errno=None,child_returncode=answer.returncode,reason='NO_VERIFIED_CHILD_CONTEXT')
    context=records[0]
    denials=getattr(answer,'denials',[])
    if denials:
        return dict(result,native_route_probe='BLOCKED_QUERY_SIDE_EFFECT_OR_POLICY_SYSCALL',
                    phase='confined-query-denied-syscall',errno=errno.EPERM,errno_name='EPERM',
                    denied_syscalls=denials,reason='SUPERVISOR_DENIED_NO_CONTINUE',
                    child_returncode=answer.returncode)
    if (context.get('identity')!='PASS_REAL_EFFECTIVE_SAVED_FS_UID949_GID948' or
            context.get('capabilities')!='ZERO' or context.get('supplementary_groups')!=[948] or
            context.get('no_new_privs') is not True or context.get('landlock_abi',0)<3 or
            context.get('seccomp_notification') is not True):
        return dict(result,native_route_probe='BLOCKED_CHILD_CONTEXT_INVALID',phase='context-validation',errno=errno.EPERM)
    result.update(context)
    if answer.returncode:
        return dict(result,native_route_probe='BLOCKED_GPGCONF',phase='gpgconf-query',errno=None,
                    child_returncode=answer.returncode,reason='GPGCONF_NONZERO_NO_SYSCALL_ERRNO_OBSERVED')
    try:path=answer.stdout.decode('ascii').removesuffix('\n')
    except UnicodeError:path=''
    if not valid_socket_path(path):
        return dict(result,native_route_probe='BLOCKED_UNEXPECTED_NATIVE_ROUTE',phase='route-selection',errno=None)
    return dict(result,native_route_probe='COMPLETED_READONLY_HOST_UID949',phase='route-compared',errno=0,
                socket_path=path,signer_service_path_equality='MATCH' if path==CANDIDATE else 'MISMATCH',
                service_design=('NATIVE_HOME_SOCKET_CANDIDATE' if path==CANDIDATE else
                                'BLOCKED_SERVICE_PATH_MUST_FOLLOW_NATIVE_ROUTE'))


class NotifyData(ctypes.Structure):
    _fields_=[('nr',ctypes.c_int),('arch',ctypes.c_uint32),('ip',ctypes.c_uint64),('args',ctypes.c_uint64*6)]

class Notification(ctypes.Structure):
    _fields_=[('id',ctypes.c_uint64),('pid',ctypes.c_uint32),('flags',ctypes.c_uint32),('data',NotifyData)]

class Response(ctypes.Structure):
    _fields_=[('id',ctypes.c_uint64),('val',ctypes.c_int64),('error',ctypes.c_int32),('flags',ctypes.c_uint32)]


def drop_supervisor_if_root(pid):
    if os.geteuid()!=0:return
    phase('supervisor-child-kernel-identity')
    with open('/proc/'+str(pid)+'/status') as f:
        fields=dict(line.split(':',1) for line in f if ':' in line)
    check(fields['Uid'].split()==['949']*4 and fields['Gid'].split()==['948']*4)
    # No root credentials needed once the confined child has handed over its
    # listener. The supervisor can still deny requests and kill its same-UID child.
    try:drop_identity(ctypes.CDLL(None,use_errno=True))
    except OSError as e:
        phase('supervisor-identity-drop');raise e
    phase('supervisor-identity-check')
    check(os.getresuid()==(949,949,949) and os.getresgid()==(948,948,948) and os.getgroups()==[948])


def supervise(argv,**kwargs):
    """Unnamed socketpair transfers only a listener FD, never an agent socket.

    Every notification is answered EPERM; never SECCOMP_USER_NOTIF_FLAG_CONTINUE.
    Unknown/new syscalls cannot silently change the computed route.
    """
    phase('supervision-library-load')
    lib=ctypes.CDLL(SECCOMP,use_errno=True)
    check(lib.seccomp_api_get()>=5,errno.ENOTSUP)
    lib.seccomp_notify_receive.argtypes=[ctypes.c_int,ctypes.POINTER(Notification)]
    lib.seccomp_notify_respond.argtypes=[ctypes.c_int,ctypes.POINTER(Response)]
    lib.seccomp_syscall_resolve_num_arch.argtypes=[ctypes.c_uint32,ctypes.c_int]
    lib.seccomp_syscall_resolve_num_arch.restype=ctypes.c_char_p
    phase('supervision-unnamed-channel')
    parent,control=socket.socketpair(socket.AF_UNIX,socket.SOCK_STREAM)
    proc=None;listener=None
    try:
        command=list(argv)
        command[-1]='TRANSPORT_FD='+str(control.fileno())+'\n'+command[-1]
        phase('child-launch')
        proc=subprocess.Popen(command,pass_fds=(control.fileno(),),**{k:v for k,v in kwargs.items() if k!='timeout'})
        control.close()
        deadline=time.monotonic()+kwargs.get('timeout',12)
        phase('confinement-listener-handshake')
        parent.settimeout(max(0.001,deadline-time.monotonic()))
        data,ancillary,flags,_=parent.recvmsg(1,socket.CMSG_SPACE(array.array('i').itemsize))
        fds=[]
        for level,kind,raw in ancillary:
            if level==socket.SOL_SOCKET and kind==socket.SCM_RIGHTS:
                values=array.array('i');values.frombytes(raw[:len(raw)//values.itemsize*values.itemsize]);fds.extend(values)
        if not data:
            stdout,stderr=proc.communicate(timeout=max(0.001,deadline-time.monotonic()))
            return SimpleNamespace(returncode=proc.returncode,stdout=stdout,stderr=stderr,denials=[])
        if data!=b'R' or flags&socket.MSG_CTRUNC or len(fds)!=1:
            for fd in fds:os.close(fd)
            raise Blocked(errno.EPROTO,'listener protocol rejected')
        listener=fds[0]
        drop_supervisor_if_root(proc.pid)
        os.set_blocking(listener,False)
        streams={proc.stdout:bytearray(),proc.stderr:bytearray()};denials=[]
        phase('confined-query-supervision')
        with selectors.DefaultSelector() as selector:
            selector.register(listener,selectors.EVENT_READ,'notification')
            for stream in streams:
                os.set_blocking(stream.fileno(),False)
                selector.register(stream,selectors.EVENT_READ,stream)
            while any(key.data!='notification' for key in selector.get_map().values()):
                remaining=deadline-time.monotonic()
                if remaining<=0:raise subprocess.TimeoutExpired(argv,kwargs.get('timeout',12))
                for key,_ in selector.select(min(remaining,0.25)):
                    if key.data=='notification':
                        request=Notification()
                        ctypes.set_errno(0)
                        rc=lib.seccomp_notify_receive(listener,ctypes.byref(request))
                        if rc==-errno.ECANCELED and ctypes.get_errno():rc=-ctypes.get_errno()
                        if rc in (-errno.EAGAIN,-errno.ENOENT,-errno.EINTR):
                            if proc.poll() is not None:selector.unregister(listener)
                            continue
                        if rc<0:raise Blocked(-rc,'notification receive failed')
                        check(request.pid==proc.pid,errno.EPROTO)
                        name=lib.seccomp_syscall_resolve_num_arch(request.data.arch,request.data.nr)
                        name=name.decode('ascii') if name else 'unknown'
                        if not re.fullmatch('[a-z0-9_]+',name):name='unknown'
                        denials.append({'syscall':name,'number':request.data.nr,'errno':errno.EPERM})
                        check(len(denials)<=64,errno.EOVERFLOW)
                        response=Response(request.id,0,-errno.EPERM,0)
                        ctypes.set_errno(0)
                        rc=lib.seccomp_notify_respond(listener,ctypes.byref(response))
                        if rc==-errno.ECANCELED and ctypes.get_errno():rc=-ctypes.get_errno()
                        if rc<0 and rc!=-errno.ENOENT:raise Blocked(-rc,'notification response failed')
                    else:
                        chunk=os.read(key.data.fileno(),4096)
                        if not chunk:selector.unregister(key.data)
                        else:
                            streams[key.data].extend(chunk)
                            check(len(streams[key.data])<=8192,errno.EOVERFLOW)
        proc.wait(timeout=max(0.001,deadline-time.monotonic()))
        return SimpleNamespace(returncode=proc.returncode,stdout=bytes(streams[proc.stdout]),stderr=bytes(streams[proc.stderr]),denials=denials)
    finally:
        parent.close();control.close()
        if proc is not None:
            if proc.poll() is None:proc.kill();proc.wait()
            proc.stdout.close();proc.stderr.close()
        if listener is not None:os.close(listener)


def probe(execute=None):
    phase('root-entry')
    base={'agent_started':False,'signing_attempted':False,'route_binding':'NOT_VERIFIED',
          'namespace_creation':False,'service_context_equality':'NOT_PROVEN'}
    try:
        check(os.getresuid()==(0,0,0))
        with open('/proc/self/uid_map') as f:
            rows=f.read();check(mapped(rows,0) and mapped(rows,949))
        phase('trusted-executable-and-library')
        from agent_pinentry_route_review import trusted
        check(platform.machine() in LOADERS,errno.ENOTSUP)
        for path in (GPGCONF,os.path.realpath(SECCOMP),os.path.realpath('/usr/bin/python3'),os.path.realpath(LOADERS[platform.machine()])):trusted(path)
        # Source is embedded by the hash-pinned bundle; no actor-side workspace
        # import, filesystem script load or historical authorization data.
        source=CHILD_SOURCE
        if source is None:
            with open(__file__) as f:source="__name__='route_child'\n"+f.read()+'\nraise SystemExit(child())\n'
        phase('child-launch')
        answer=(execute or supervise)(['/usr/bin/python3','-I','-S','-B','-c',source],stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=ENV,cwd='/',timeout=12,close_fds=True)
        phase('child-protocol')
        return evaluate(answer)
    except TimeoutError:
        return dict(base,native_route_probe='BLOCKED_QUERY_TIMEOUT',phase=PHASE,errno=errno.ETIMEDOUT)
    except OSError as e:
        return dict(base,native_route_probe='BLOCKED_NAMESPACE_FREE_PREFLIGHT',phase=PHASE,
                    errno=e.errno or errno.EIO,errno_name=errno.errorcode.get(e.errno or errno.EIO,'UNKNOWN'))
    except subprocess.TimeoutExpired:
        return dict(base,native_route_probe='BLOCKED_QUERY_TIMEOUT',phase=PHASE,errno=errno.ETIMEDOUT)
    except Exception:
        return dict(base,native_route_probe='BLOCKED_CONTEXT_OR_PROTOCOL',phase=PHASE,errno=None)


def probe_actor(source,execute=None):
    """Startup-only actor query; no identity changes or root-mode imports.

    The lifecycle loader supplies pinned peer source from its root-owned release.
    This function is never used to repeat an already accepted host measurement.
    """
    phase('actor-route-entry')
    base={'agent_started':False,'signing_attempted':False,'namespace_creation':False}
    try:
        check(os.getresuid()==(949,949,949) and os.getresgid()==(948,948,948) and os.getgroups()==[948])
        payload="__name__='route_actor_child'\n"+source+'\nraise SystemExit(child(actor=True))\n'
        answer=(execute or supervise)(['/usr/bin/python3','-I','-S','-B','-c',payload],stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=ENV,cwd='/',timeout=12,close_fds=True)
        phase('actor-route-protocol')
        return evaluate(answer)
    except (TimeoutError,subprocess.TimeoutExpired):
        return dict(base,native_route_probe='BLOCKED_QUERY_TIMEOUT',phase=PHASE,errno=errno.ETIMEDOUT)
    except OSError as e:
        return dict(base,native_route_probe='BLOCKED_ACTOR_ROUTE',phase=PHASE,errno=e.errno or errno.EIO)
    except Exception:
        return dict(base,native_route_probe='BLOCKED_ACTOR_PROTOCOL',phase=PHASE,errno=None)


# Replaced by a hash-pinned source literal by the packager; local development
# uses this module's exact current source bytes. No host signer file is changed.
CHILD_SOURCE=None


def main():
    print(json.dumps(probe(),sort_keys=True))

if __name__=='__main__':main()
