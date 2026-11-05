"""Home-route v2 lifecycle candidate. No cleanup, signing or automatic restart.

Only future explicitly authorized systemd activation may use root-claim/run.
Workspace policy is unapproved; the existing home socket always blocks start.
"""
import errno
import ctypes
import platform
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import signal
import socket
import stat
import subprocess
import sys
import time
import types

HOME='/var/lib/zethropol-aur-release'
GNUPG=HOME+'/gnupg'
SOCKET=GNUPG+'/S.gpg-agent'
AUX_SOCKET=GNUPG+'/S.gpg-agent.ssh'  # Native daemon creates this even without enabling SSH support.
GENERATION='home-route-v2-20261103'
RELEASE='/usr/lib/zethropol/aur-agent-lifecycle-949/releases/'+GENERATION
POLICY=RELEASE+'/policy.json'
ROUTE_READER=RELEASE+'/native_socket_route_readonly.py'
UNIT='zethropol-aur-manifest-agent.service'
UNIT_PATH='/etc/systemd/system/'+UNIT
STATE='/var/lib/zethropol-aur-agent-lifecycle-949'
AUDIT=HOME+'/agent-lifecycle-v2'
CONFIG=GNUPG+'/gpg-agent.conf'
CONFIG_HASH='250266c8436ebf3f94595d564f042fc5691b7d7104ca4f71c4d4f4ac1f804bae'
ENV={'PATH':'/usr/bin','LC_ALL':'C'}
ACL={'system.posix_acl_access','system.posix_acl_default'}
# Linux pathname sockets only: no general owner-only mode acceptance.
SOCKET_MODES=('0o600','0o700')
ROOT_UID=0
ROOT_GID=0
PHASE='initial'
CLAIM_WINDOW_NS=60_000_000_000  # Lifecycle startup only; no provider TTL change.

class Refused(RuntimeError):pass

def phase(name):
    global PHASE
    PHASE=name

def require(ok):
    if not ok:raise Refused('Lifecycle guard rejected')

def digest_ok(value):
    return isinstance(value,str) and re.fullmatch('[0-9a-f]{64}',value) is not None

def actor_identity():
    phase('actor-kernel-identity')
    require(os.getresuid()==(949,949,949) and os.getresgid()==(948,948,948) and os.getgroups()==[948])
    fields=dict(line.split(':',1) for line in Path('/proc/self/status').read_text().splitlines() if ':' in line)
    require(fields['Uid'].split()==['949']*4 and fields['Gid'].split()==['948']*4)
    require(all(int(fields[k],16)==0 for k in ('CapEff','CapPrm','CapInh','CapAmb')))
    require(fields['NoNewPrivs'].strip()=='1' and fields['Seccomp'].strip()=='0')
    for kind,value in (('uid',949),('gid',948)):
        rows=[tuple(map(int,row.split())) for row in Path('/proc/self/'+kind+'_map').read_text().splitlines()]
        require(any(inner<=value<inner+count and outer+value-inner==value for inner,outer,count in rows))
    p=pwd.getpwnam('zethropol-aur-release')
    require((p.pw_uid,p.pw_gid,p.pw_dir,p.pw_shell)==(949,948,HOME,'/usr/bin/nologin'))

def node(path,uid,gid,mode,directory=True):
    p=Path(path);s=p.lstat()
    require((stat.S_ISDIR(s.st_mode) if directory else stat.S_ISREG(s.st_mode)) and
            (s.st_uid,s.st_gid)==(uid,gid) and stat.S_IMODE(s.st_mode)==mode and
            not ACL.intersection(os.listxattr(p,follow_symlinks=False)))
    if not directory:require(s.st_nlink==1)
    return s

def stable_stat(s):
    return (s.st_dev,s.st_ino,s.st_uid,s.st_gid,s.st_mode,s.st_nlink,s.st_size,s.st_mtime_ns,s.st_ctime_ns)


def root_input(path,limit=65536):
    p=Path(path)
    for parent in p.parents:
        s=parent.lstat();require(stat.S_ISDIR(s.st_mode) and (s.st_uid,s.st_gid)==(ROOT_UID,ROOT_GID) and
                                not s.st_mode&0o022 and not ACL.intersection(os.listxattr(parent,follow_symlinks=False)))
    s=node(p,ROOT_UID,ROOT_GID,0o644,False);require(0<s.st_size<=limit)
    fd=os.open(p,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC|(os.O_NOATIME if os.geteuid()==0 else 0))
    try:
        before=os.fstat(fd);require(stable_stat(before)==stable_stat(s))
        raw=os.pread(fd,limit+1,0)
        require(len(raw)==s.st_size and stable_stat(os.fstat(fd))==stable_stat(before) and stable_stat(p.lstat())==stable_stat(before))
        return raw
    finally:os.close(fd)

def socket_snapshot(path=SOCKET):
    s=os.lstat(path)
    require(stat.S_ISSOCK(s.st_mode) and s.st_nlink==1)
    attrs={name:os.getxattr(path,name,follow_symlinks=False).hex() for name in sorted(ACL.intersection(os.listxattr(path,follow_symlinks=False)))}
    require(os.lstat(path)==s)
    return {'device':str(s.st_dev),'inode':str(s.st_ino),'uid':s.st_uid,'gid':s.st_gid,
            'mode':oct(stat.S_IMODE(s.st_mode)),'ctime_ns':str(s.st_ctime_ns),'acl':attrs}

def old_socket_check(policy):
    require(socket_snapshot()==policy['old_socket'])

def owner_configuration_presence():
    paths={'owner-agent-config':CONFIG,'owner-common-config':GNUPG+'/common.conf',
           'system-route-control':'/etc/gnupg/gpgconf.ctl','vendor-route-control':'/usr/lib/gnupg/gpgconf.ctl',
           'binary-route-control':'/usr/bin/gpgconf.ctl','alternate-runtime-root':'/run/gnupg/user/949'}
    found=[]
    for label,path in paths.items():
        try:Path(path).lstat()
        except FileNotFoundError:continue
        found.append(label)
    return found

def owner_configs_absent():
    # Retained only for historical diagnostic callers, never used by v2 startup.
    blockers=owner_configuration_presence()
    if blockers:
        error=Refused('Existing owner configuration requires review');error.public_blockers=blockers;raise error

def config_metadata(s):
    return {'device':str(s.st_dev),'inode':str(s.st_ino),'uid':s.st_uid,'gid':s.st_gid,
            'mode':oct(stat.S_IMODE(s.st_mode)),'ctime_ns':str(s.st_ctime_ns),'size':s.st_size}

def verified_config(policy):
    phase('preserved-owner-config-pin')
    s=os.lstat(CONFIG)
    require(stat.S_ISREG(s.st_mode) and (s.st_uid,s.st_gid)==(949,948) and s.st_nlink==1 and
            stat.S_IMODE(s.st_mode) in (0o600,0o644) and 0<s.st_size<=16384 and
            config_metadata(s)==policy['agent_config_metadata'])
    fd=os.open(CONFIG,os.O_RDONLY|os.O_NOFOLLOW|os.O_NOATIME|os.O_CLOEXEC)
    try:
        require(config_metadata(os.fstat(fd))==config_metadata(s) and not ACL.intersection(os.listxattr(fd)))
        raw=os.pread(fd,16385,0)
        require(config_metadata(os.fstat(fd))==config_metadata(s) and config_metadata(os.lstat(CONFIG))==config_metadata(s))
        require(len(raw)==s.st_size and hashlib.sha256(raw).hexdigest()==policy['agent_config_sha256']==CONFIG_HASH)
    finally:os.close(fd)
    # This existing reviewed configuration is exactly one pinentry-program line.
    # No include, debug/log destination, cache relaxation or extra socket option.
    rows=[line.strip() for line in raw.decode('ascii').splitlines() if line.strip() and not line.strip().startswith('#')]
    require(len(rows)==1)
    fields=rows[0].split();require(len(fields)==2 and fields[0]=='pinentry-program' and fields[1].startswith('/'))
    require(all(part not in ('','.','..') for part in fields[1].split('/')[1:]))
    return raw

def any_uid949_agent():
    for p in Path('/proc').iterdir():
        if not p.name.isdigit():continue
        try:
            if (p/'comm').read_text().strip()!='gpg-agent':continue
            rows=[line.split()[1:] for line in (p/'status').read_text().splitlines() if line.startswith('Uid:')]
            require(len(rows)==1)
            if '949' in rows[0]:return True
        except FileNotFoundError:continue
    return False

def no_listener(path):
    # Reject any bound row, including a nonlistening endpoint. No connection.
    for line in Path('/proc/net/unix').read_text().splitlines()[1:]:
        parts=line.split(None,7)
        if len(parts)==8 and parts[7]==path:return False
    return True

def socket_absent(path=SOCKET):
    phase('home-socket-must-be-absent')
    try:os.lstat(path)
    except FileNotFoundError:return
    raise Refused('Existing home socket preserved; separate maintenance required')

def common_checks(policy):
    phase('home-ancestor-metadata')
    for path in ('/','/var','/var/lib'):
        s=Path(path).lstat();require(stat.S_ISDIR(s.st_mode) and (s.st_uid,s.st_gid)==(0,0) and
                                  not s.st_mode&0o022 and not ACL.intersection(os.listxattr(path,follow_symlinks=False)))
    node(HOME,949,948,0o700);node(GNUPG,949,948,0o700)
    node(GNUPG+'/private-keys-v1.d',949,948,0o700)  # Directory metadata only.
    phase('owner-route-control-review')
    require(not (set(owner_configuration_presence())-{'owner-agent-config'}))
    verified_config(policy)
    phase('no-existing-agent-or-listener')
    require(no_listener(SOCKET) and no_listener(AUX_SOCKET) and not any_uid949_agent())
    socket_absent(AUX_SOCKET)
    socket_absent()  # Never let gpg-agent handle an existing stale socket.

def validate_old_socket_binding(old):
    require(isinstance(old,dict) and set(old)=={'device','inode','uid','gid','mode','ctime_ns','acl'})
    require((old['uid'],old['gid'],old['acl'])==(949,948,{}) and
            old['mode'] in SOCKET_MODES)
    for key in ('device','inode','ctime_ns'):require(isinstance(old[key],str) and old[key].isdigit())

def load_policy():
    phase('root-owned-home-lifecycle-policy')
    raw=root_input(POLICY);p=json.loads(raw)
    keys={'schema','purpose','generation','uid','gid','gnupg_home','socket_path','old_socket','quarantine_receipt',
          'helper_sha256','route_reader_sha256','unit_sha256','binary_sha256','agent_config_sha256',
          'agent_config_metadata','activation_id','activation_approved'}
    require(set(p)==keys and type(p['schema']) is int and p['schema']==2 and p['purpose']=='zethropol-agent-home-lifecycle-949')
    require(p['generation']==GENERATION and type(p['uid']) is int and p['uid']==949 and type(p['gid']) is int and p['gid']==948 and
            p['gnupg_home']==GNUPG and p['socket_path']==SOCKET and p['activation_approved'] is True)
    require(isinstance(p['activation_id'],str) and re.fullmatch('[0-9a-f]{32}',p['activation_id']))
    for key,path in (('helper_sha256',__file__),('route_reader_sha256',ROUTE_READER),('unit_sha256',UNIT_PATH)):
        require(digest_ok(p[key]) and hashlib.sha256(root_input(path)).hexdigest()==p[key])
    require(set(p['binary_sha256'])=={'/usr/bin/gpg-agent','/usr/bin/gpgconf'})
    for path,expected in p['binary_sha256'].items():
        require(digest_ok(expected))
        # root_input-like ancestry checks; binaries are executable 0755.
        for parent in Path(path).parents:
            s=parent.lstat();require(stat.S_ISDIR(s.st_mode) and (s.st_uid,s.st_gid)==(0,0) and not s.st_mode&0o022 and not ACL.intersection(os.listxattr(parent,follow_symlinks=False)))
        s=node(path,0,0,0o755,False)
        fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC)
        try:
            require(stable_stat(os.fstat(fd))==stable_stat(s))
            with os.fdopen(os.dup(fd),'rb') as stream:actual=hashlib.file_digest(stream,'sha256').hexdigest()
            require(actual==expected and stable_stat(os.fstat(fd))==stable_stat(s) and stable_stat(Path(path).lstat())==stable_stat(s))
        finally:os.close(fd)
    require(p['agent_config_sha256']==CONFIG_HASH and isinstance(p['agent_config_metadata'],dict))
    validate_old_socket_binding(p['old_socket'])
    receipt=p['quarantine_receipt']
    require(isinstance(receipt,str) and re.fullmatch(re.escape(STATE)+r'/quarantine\.[0-9a-f]{32}\.json',receipt))
    p['_policy_sha256']=hashlib.sha256(raw).hexdigest()
    return p

def check_quarantine_receipt(policy,root=False):
    phase('separate-stale-quarantine-proof')
    receipt=json.loads(root_input(policy['quarantine_receipt']))
    require(set(receipt)=={'schema','maintenance_id','before','after','destination','source_absent','directories_restored'})
    mid=receipt['maintenance_id'];require(isinstance(mid,str) and re.fullmatch('[0-9a-f]{32}',mid))
    require(receipt['schema']==1 and receipt['before']==policy['old_socket'] and
            receipt['source_absent'] is True and receipt['directories_restored'] is True)
    destination=STATE+'/quarantine/'+mid+'/S.gpg-agent'
    require(receipt['destination']==destination)
    after=receipt['after'];require(isinstance(after,dict) and set(after)==set(policy['old_socket']))
    require(all(after[k]==policy['old_socket'][k] for k in after if k!='ctime_ns'))
    require(isinstance(after['ctime_ns'],str) and after['ctime_ns'].isdigit())
    socket_absent()
    if root:
        node(STATE+'/quarantine',0,0,0o700);node(STATE+'/quarantine/'+mid,0,0,0o700)
        require(socket_snapshot(destination)==after and no_listener(destination))
    return receipt

def actor_lock(root=False):
    phase('exclusive-lifecycle-lock')
    if not root:actor_identity()
    node(AUDIT,949,948,0o700)
    parent=os.open(AUDIT,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
    try:fd=os.open('lifecycle.lock',os.O_RDWR|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=parent)
    finally:os.close(parent)
    try:
        s=os.fstat(fd);require(stat.S_ISREG(s.st_mode) and (s.st_uid,s.st_gid)==(949,948) and
                               stat.S_IMODE(s.st_mode)==0o600 and s.st_nlink==1 and not ACL.intersection(os.listxattr(fd)))
        fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);return fd
    except BaseException:os.close(fd);raise

def boot_id():return Path('/proc/sys/kernel/random/boot_id').read_text().strip()
def boottime_ns():return time.clock_gettime_ns(time.CLOCK_BOOTTIME)
def claim_path(policy):return STATE+'/activation.'+policy['activation_id']+'.used'

def namespace_inodes():
    return {kind:os.stat('/proc/self/ns/'+kind).st_ino for kind in ('user','net','pid')}

def root_host_identity():
    phase('root-initial-host-context')
    require(os.getresuid()==(0,0,0) and Path('/proc/self/uid_map').read_text().split()==['0','0','4294967295'])
    for kind,value in namespace_inodes().items():require(os.stat('/proc/1/ns/'+kind).st_ino==value)


def root_claim(policy):
    phase('one-shot-root-activation-claim')
    node(STATE,0,0,0o755)
    parent=os.open(STATE,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
    try:
        fd=os.open(Path(claim_path(policy)).name,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o644,dir_fd=parent)
        try:
            os.fchmod(fd,0o644)
            audit_line(fd,'root-activation-consumed',policy_sha256=policy['_policy_sha256'],activation_id=policy['activation_id'],
                       boot_id=boot_id(),namespace_inodes=namespace_inodes(),start_deadline_boottime_ns=str(boottime_ns()+CLAIM_WINDOW_NS))
        finally:os.close(fd)
        os.fsync(parent)
    finally:os.close(parent)

def verify_claim(policy):
    phase('current-boot-one-shot-root-claim')
    record=json.loads(root_input(claim_path(policy)))
    require(record['event']=='root-activation-consumed' and record['policy_sha256']==policy['_policy_sha256'] and
            record['activation_id']==policy['activation_id'] and record['boot_id']==boot_id())
    left=int(record['start_deadline_boottime_ns'])-boottime_ns();require(0<left<=CLAIM_WINDOW_NS)
    require(record['namespace_inodes']==namespace_inodes())

def consume_actor_start(policy):
    phase('one-shot-actor-start')
    parent=os.open(AUDIT,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
    try:
        fd=os.open('start.'+policy['activation_id']+'.used',os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o600,dir_fd=parent)
        try:audit_line(fd,'actor-start-consumed',policy_sha256=policy['_policy_sha256'])
        finally:os.close(fd)
        os.fsync(parent)
    finally:os.close(parent)

def prepare_home(policy):
    actor_identity();common_checks(policy);verify_claim(policy);check_quarantine_receipt(policy)
    phase('native-home-route-startup-gate')
    raw=root_input(ROUTE_READER)
    require(hashlib.sha256(raw).hexdigest()==policy['route_reader_sha256'])
    reader=types.ModuleType('pinned_actor_route');reader.__file__=ROUTE_READER
    exec(compile(raw,ROUTE_READER,'exec'),reader.__dict__)
    result=reader.probe_actor(raw.decode('utf-8'))
    require(result.get('native_route_probe')=='COMPLETED_READONLY_HOST_UID949' and result.get('socket_path')==SOCKET and
            result.get('signer_service_path_equality')=='MATCH' and result.get('agent_started') is False)
    socket_absent();require(no_listener(SOCKET));verify_claim(policy)
    raw=verified_config(policy)
    fd=os.memfd_create('reviewed-existing-agent-config',os.MFD_CLOEXEC|os.MFD_ALLOW_SEALING)
    try:
        require(os.write(fd,raw)==len(raw));os.lseek(fd,0,os.SEEK_SET)
        fcntl.fcntl(fd,fcntl.F_ADD_SEALS,fcntl.F_SEAL_WRITE|fcntl.F_SEAL_GROW|fcntl.F_SEAL_SHRINK|fcntl.F_SEAL_SEAL)
        return fd
    except BaseException:os.close(fd);raise

def agent_confinement():
    """Child-only: create native sockets, never remove/modify existing files.

    Prevents gpg-agent's own stale-socket cleanup, including a bind-time race.
    Normal shutdown may leave its new socket node; no restart/cleanup follows.
    """
    libc=ctypes.CDLL(None,use_errno=True);libc.syscall.restype=ctypes.c_long
    def ok(value):
        if value<0:raise OSError(ctypes.get_errno() or errno.EIO,'agent confinement refused')
        return value
    require(platform.machine() in ('x86_64','aarch64'))
    ok(libc.prctl(38,1,0,0,0))
    require(ok(libc.syscall(444,0,0,1))>=3)
    attr=ctypes.c_uint64((1<<15)-1)
    ruleset=ok(libc.syscall(444,ctypes.byref(attr),ctypes.sizeof(attr),0))
    class Rule(ctypes.Structure):
        _pack_=1
        _fields_=[('access',ctypes.c_uint64),('fd',ctypes.c_int32)]
    try:
        loader={'x86_64':'/lib64/ld-linux-x86-64.so.2','aarch64':'/lib/ld-linux-aarch64.so.1'}[platform.machine()]
        rules=[('/',1<<3),('/usr',1<<2),('/etc',1<<2),(GNUPG,(1<<2)|(1<<9)),
               ('/usr/bin/gpg-agent',(1<<0)|(1<<2)),(os.path.realpath(loader),(1<<0)|(1<<2)),
               ('/dev/null',1<<2),('/dev/random',1<<2),('/dev/urandom',1<<2)]
        for path,rights in rules:
            fd=os.open(path,os.O_PATH|os.O_NOFOLLOW|os.O_CLOEXEC)
            try:ok(libc.syscall(445,ruleset,1,ctypes.byref(Rule(rights,fd)),0))
            finally:os.close(fd)
        ok(libc.syscall(446,ruleset,0))
    finally:os.close(ruleset)
    # Landlock denies write/remove/create-regular/rename/truncate. This filter
    # closes metadata/ioctl/io_uring escapes not covered by ABI3 Landlock.
    lib=ctypes.CDLL('/usr/lib/libseccomp.so.2',use_errno=True)
    lib.seccomp_init.argtypes=[ctypes.c_uint32];lib.seccomp_init.restype=ctypes.c_void_p
    lib.seccomp_syscall_resolve_name.argtypes=[ctypes.c_char_p]
    lib.seccomp_rule_add.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_int,ctypes.c_uint]
    lib.seccomp_load.argtypes=[ctypes.c_void_p];lib.seccomp_release.argtypes=[ctypes.c_void_p]
    class Compare(ctypes.Structure):
        _fields_=[('arg',ctypes.c_uint),('op',ctypes.c_uint),('a',ctypes.c_uint64),('b',ctypes.c_uint64)]
    lib.seccomp_rule_add_array.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_int,ctypes.c_uint,ctypes.POINTER(Compare)]
    ctx=lib.seccomp_init(0x7fff0000);require(bool(ctx))
    def deny(name,cmp=None,code=errno.EPERM):
        nr=lib.seccomp_syscall_resolve_name(name.encode())
        if nr<0:return
        rc=(lib.seccomp_rule_add_array(ctx,0x50000|code,nr,1,ctypes.byref(cmp)) if cmp is not None else
            lib.seccomp_rule_add(ctx,0x50000|code,nr,0))
        require(rc==0)
    try:
        for name in ('chmod','fchmod','fchmodat','fchmodat2','chown','fchown','lchown','fchownat',
                     'setxattr','lsetxattr','fsetxattr','removexattr','lremovexattr','fremovexattr',
                     'utime','utimes','futimesat','utimensat','ioctl','io_uring_setup','open_by_handle_at',
                     'ptrace','process_vm_readv','process_vm_writev','mount','umount2','unshare','setns','execveat'):
            deny(name)
        for name in ('socket','socketpair'):deny(name,Compare(0,1,socket.AF_UNIX,0))
        for flag in (0x00020000,0x02000000,0x04000000,0x08000000,0x10000000,0x20000000,0x40000000,0x00000080):
            deny('clone',Compare(0,7,flag,flag))
        deny('clone3',code=errno.ENOSYS)
        require(lib.seccomp_load(ctx)==0)
    finally:lib.seccomp_release(ctx)


def agent_argv(configfd):
    return ['/usr/bin/gpg-agent','--options','/proc/self/fd/'+str(configfd),'--homedir',GNUPG,'--daemon','--no-detach',
            '--no-allow-loopback-pinentry','--no-allow-external-cache','--extra-socket','/dev/null','--browser-socket','/dev/null']

def audit_line(fd,event,**fields):
    raw=(json.dumps({'schema':2,'event':event,'time_ns':str(time.time_ns()),**fields},sort_keys=True,separators=(',',':'))+'\n').encode()
    view=memoryview(raw)
    while view:
        count=os.write(fd,view);require(count>0);view=view[count:]
    os.fsync(fd)

def signal_owned(pidfd,signum):
    try:signal.pidfd_send_signal(pidfd,signum)
    except ProcessLookupError:pass

def enable_subreaper():
    phase('owned-daemon-subreaper')
    libc=ctypes.CDLL(None,use_errno=True)
    if libc.prctl(36,1,0,0,0)<0:raise OSError(ctypes.get_errno(),'subreaper refused')
    value=ctypes.c_int()
    if libc.prctl(37,ctypes.byref(value),0,0,0)<0:raise OSError(ctypes.get_errno(),'subreaper query refused')
    require(value.value==1)

def direct_children():
    return [int(p) for p in Path('/proc/self/task/'+str(os.getpid())+'/children').read_text().split()]

def owned_agent_identity(pid):
    p=Path('/proc')/str(pid)
    fields=dict(line.split(':',1) for line in (p/'status').read_text().splitlines() if ':' in line)
    require(fields['PPid'].strip()==str(os.getpid()) and fields['Uid'].split()==['949']*4 and fields['Gid'].split()==['948']*4)
    require(all(int(fields[k],16)==0 for k in ('CapEff','CapPrm','CapInh','CapAmb')) and fields['NoNewPrivs'].strip()=='1')
    require(os.readlink(p/'exe')=='/usr/bin/gpg-agent')
    require((p/'cgroup').read_text()==Path('/proc/self/cgroup').read_text())
    return (p/'stat').read_text().rsplit(')',1)[1].split()[19]  # Start ticks; PID reuse guard.

class OwnedAgent:
    def __init__(self,pid,pidfd,start_ticks):
        self.pid=pid;self.pidfd=pidfd;self.start_ticks=start_ticks;self.returncode=None
    def poll(self):
        if self.returncode is None:
            result=os.waitid(os.P_PIDFD,self.pidfd,os.WEXITED|os.WNOHANG)
            if result is not None:self.returncode=result.si_status if result.si_code==os.CLD_EXITED else -result.si_status
        return self.returncode
    def wait(self,timeout=None):
        deadline=None if timeout is None else time.monotonic()+timeout
        while self.poll() is None:
            if deadline is not None and time.monotonic()>=deadline:raise subprocess.TimeoutExpired('owned-agent',timeout)
            time.sleep(0.05)
        return self.returncode

def adopt_agent(launcher,deadline):
    phase('adopt-native-forked-agent')
    while launcher.poll() is None:
        require(time.monotonic()<deadline);time.sleep(0.05)
    require(launcher.returncode==0)
    while time.monotonic()<deadline:
        pids=direct_children()
        require(len(pids)<=1)
        if pids:
            pid=pids[0];before=owned_agent_identity(pid);fd=os.pidfd_open(pid)
            try:
                require(owned_agent_identity(pid)==before)
                return OwnedAgent(pid,fd,before)
            except BaseException:os.close(fd);raise
        time.sleep(0.05)
    raise Refused('No owned native agent descendant')

def reap_owned_agents():
    # Error path: only direct same-cgroup UID949 gpg-agent children of this
    # subreaper. Never pgrep/killall or signal an arbitrary discovered agent.
    for pid in direct_children():
        before=owned_agent_identity(pid);fd=os.pidfd_open(pid)
        try:
            require(owned_agent_identity(pid)==before)
            owned=OwnedAgent(pid,fd,before);signal_owned(fd,signal.SIGTERM)
            try:owned.wait(timeout=10)
            except subprocess.TimeoutExpired:signal_owned(fd,signal.SIGKILL);owned.wait(timeout=5)
        finally:os.close(fd)


def child_endpoint_ready(child):
    phase('owned-agent-home-endpoint')
    try:
        if child.poll() is not None:return False
        require(owned_agent_identity(child.pid)==child.start_ticks)
        p=Path('/proc')/str(child.pid)
        owned={os.readlink(fd) for fd in (p/'fd').iterdir()}
        rows=[line.split(None,7) for line in Path('/proc/net/unix').read_text().splitlines()[1:]]
        for path in (SOCKET,AUX_SOCKET):
            snap=socket_snapshot(path)
            require((snap['uid'],snap['gid'],snap['acl'])==(949,948,{}) and int(snap['mode'],8)&0o077==0)
            inodes={row[6] for row in rows if len(row)==8 and row[7]==path and int(row[3],16)&0x10000}
            require(len(inodes)==1 and 'socket:['+next(iter(inodes))+']' in owned)
            require(socket_snapshot(path)==snap)
        return True
    except FileNotFoundError:return False


def notify_ready():
    phase('systemd-ready-notification')
    address=os.environ.get('NOTIFY_SOCKET');require(isinstance(address,str) and address.startswith(('/','@')))
    if address.startswith('@'):address='\0'+address[1:]
    with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM|socket.SOCK_CLOEXEC) as sock:
        sock.connect(address);sock.sendall(b'READY=1\nSTATUS=Owned UID949 native home socket verified; credentials not probed')

def supervise(policy,configfd,popen=subprocess.Popen):
    actor_identity();common_checks(policy);verify_claim(policy)
    node(AUDIT,949,948,0o700)
    consume_actor_start(policy)
    parent=os.open(AUDIT,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
    af=os.open(os.urandom(16).hex()+'.jsonl',os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o600,dir_fd=parent)
    child=None;pidfd=None;launcher=None;launcher_pidfd=None;stop=False
    def requested(*args):
        nonlocal stop
        stop=True
    previous={s:signal.signal(s,requested) for s in (signal.SIGTERM,signal.SIGINT)}
    try:
        os.fsync(parent);audit_line(af,'start-guards-pass',uid=949,gid=948,socket_path=SOCKET,policy_sha256=policy['_policy_sha256'])
        phase('agent-process-launch')
        require(not stop)
        socket_absent();socket_absent(AUX_SOCKET);require(no_listener(SOCKET) and no_listener(AUX_SOCKET))
        enable_subreaper()
        phase('agent-process-launch')
        launcher=popen(agent_argv(configfd),pass_fds=(configfd,),stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                    env=dict(ENV),start_new_session=True,preexec_fn=agent_confinement)
        launcher_pidfd=os.pidfd_open(launcher.pid)
        deadline=time.monotonic()+8
        child=adopt_agent(launcher,deadline);pidfd=child.pidfd
        require(not stop)
        os.close(launcher_pidfd);launcher_pidfd=None
        audit_line(af,'agent-process-started',pid=str(child.pid),launcher_pid=str(launcher.pid),start_ticks=child.start_ticks)
        while not child_endpoint_ready(child):
            require(not stop and child.poll() is None and time.monotonic()<deadline);time.sleep(0.05)
        audit_line(af,'owned-native-home-endpoint-ready',socket_path=SOCKET,credential_readiness='NOT_PROBED')
        require(not stop);notify_ready()
        phase('agent-process-supervision')
        while child.poll() is None:
            if stop:
                audit_line(af,'stop-requested');signal_owned(pidfd,signal.SIGTERM)
                try:child.wait(timeout=10)
                except subprocess.TimeoutExpired:signal_owned(pidfd,signal.SIGKILL);child.wait(timeout=5)
                break
            time.sleep(0.1)
        audit_line(af,'agent-process-exited',returncode=child.returncode)
        phase('agent-process-exit');require(stop and child.returncode in (0,-signal.SIGTERM))
    except BaseException as error:
        audit_line(af,'lifecycle-blocked',phase=PHASE,errno=error.errno if isinstance(error,OSError) else None)
        raise
    finally:
        for s,handler in previous.items():signal.signal(s,handler)
        if child is not None and child.poll() is None:
            if pidfd is not None:signal_owned(pidfd,signal.SIGTERM)
            try:child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                if pidfd is not None:signal_owned(pidfd,signal.SIGKILL)
                child.wait(timeout=5)
        if launcher is not None and launcher.poll() is None:
            if launcher_pidfd is not None:signal_owned(launcher_pidfd,signal.SIGTERM)
            else:launcher.terminate()
            try:launcher.wait(timeout=10)
            except subprocess.TimeoutExpired:
                if launcher_pidfd is not None:signal_owned(launcher_pidfd,signal.SIGKILL)
                else:launcher.kill()
                launcher.wait(timeout=5)
        if child is None and launcher is not None:reap_owned_agents()
        if launcher_pidfd is not None:os.close(launcher_pidfd)
        if pidfd is not None:os.close(pidfd)
        os.close(af);os.close(parent)

def main(mode):
    if mode=='root-claim':
        root_host_identity()
        policy=load_policy();lock=actor_lock(root=True)
        try:common_checks(policy);check_quarantine_receipt(policy,root=True);root_claim(policy)
        finally:os.close(lock)
        return
    require(mode=='run');actor_identity();policy=load_policy();lock=actor_lock()
    configfd=None
    try:
        configfd=prepare_home(policy);supervise(policy,configfd)
    finally:
        if configfd is not None:os.close(configfd)
        os.close(lock)

if __name__=='__main__':
    try:main(sys.argv[1] if len(sys.argv)==2 else '')
    except Exception as error:
        code=error.errno if isinstance(error,OSError) and type(error.errno) is int else None
        print(json.dumps({'status':'BLOCKED','phase':PHASE,'errno':code,'agent_started_by_review':False,'signing':False},separators=(',',':')),flush=True)
        raise SystemExit(1)
