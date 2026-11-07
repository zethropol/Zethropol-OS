"""Home-route v2 lifecycle candidate. No cleanup, signing or automatic restart.

The produce entrypoint requires exclusive rescue init and existing exact manifest
approval. root-claim/run consume only its committed, boot-bound one-shot records.
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
    verify_activation(p)
    return p

def check_quarantine_receipt(policy,root=False):
    phase('separate-stale-quarantine-proof')
    receipt=json.loads(root_input(policy['quarantine_receipt']))
    require(set(receipt)=={'schema','maintenance_id','before','after','destination','source_absent','directories_restored',
                           'activation_id','manifest_sha256','sockets'})
    require(type(receipt['schema']) is int and receipt['schema']==2 and receipt['activation_id']==policy['activation_id'])
    record=verify_activation(policy)
    require(receipt['manifest_sha256']==record['manifest_sha256'])
    require(set(receipt['sockets'])==set(SOCKET_NAMES))
    mid=receipt['maintenance_id'];require(isinstance(mid,str) and re.fullmatch('[0-9a-f]{32}',mid))
    require(policy['quarantine_receipt']==STATE+'/quarantine.'+mid+'.json')
    require(receipt['before']==policy['old_socket'] and
            receipt['source_absent'] is True and receipt['directories_restored'] is True)
    destination=STATE+'/quarantine/'+mid+'/S.gpg-agent'
    require(receipt['destination']==destination)
    after=receipt['after'];require(isinstance(after,dict) and set(after)==set(policy['old_socket']))
    require(all(after[k]==policy['old_socket'][k] for k in after if k!='ctime_ns'))
    require(isinstance(after['ctime_ns'],str) and after['ctime_ns'].isdigit())
    for name,pair in receipt['sockets'].items():
        require(isinstance(pair,dict) and set(pair)=={'before','after'})
        socket_absent(GNUPG+'/'+name)
        if pair['before'] is None:
            require(pair['after'] is None)
            if root:socket_absent(STATE+'/quarantine/'+mid+'/'+name)
            continue
        validate_old_socket_binding(pair['before']);validate_old_socket_binding(pair['after'])
        require(all(pair['before'][key]==pair['after'][key] for key in pair['before'] if key!='ctime_ns'))
        if name==SOCKET_NAMES[0]:require(pair=={'before':receipt['before'],'after':receipt['after']})
        if root:
            archived=STATE+'/quarantine/'+mid+'/'+name
            require(socket_snapshot(archived)==pair['after'] and no_listener(archived))
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
    verify_activation(policy)
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
    verify_activation(policy)
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

# This is an operation of the existing lifecycle, not a second approval service.
# The existing root-protected manifest approval remains the sole owner authority.
INACTIVE_POLICY_SHA='3b080ec729b0d03567f4a6139b3cd0675e97e97beaae3ace2b2dc7ab8201654b'
MANIFEST_BUNDLE_SHA='3149c27c6c66a0c33a58a2e57f8279def163ebd3b9c99977ce112001c176e3f2'
MANIFEST_BUNDLE='/usr/lib/zethropol/aur-manifest-diagnostics-'+MANIFEST_BUNDLE_SHA
APPROVALS='/var/lib/zethropol/aur-manifest-approvals'
SOCKET_NAMES=('S.gpg-agent','S.gpg-agent.ssh','S.gpg-agent.extra','S.gpg-agent.browser')


def canonical(value):return json.dumps(value,sort_keys=True,separators=(',',':')).encode()
def sha(raw):return hashlib.sha256(raw).hexdigest()


def sync_directory(path):
    fd=os.open(path,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
    try:os.fsync(fd)
    finally:os.close(fd)


def durable_create(path,raw):
    """An incomplete record is deliberately retained on failure; never replace it."""
    parent=os.open(str(Path(path).parent),os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
    try:
        fd=os.open(Path(path).name,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o644,dir_fd=parent)
        try:
            os.fchmod(fd,0o644)
            view=memoryview(raw)
            while view:
                written=os.write(fd,view);require(written>0);view=view[written:]
            os.fsync(fd)
        finally:os.close(fd)
        os.fsync(parent)
    finally:os.close(parent)


def owner_authorization(manifest_sha):
    """Use the installed pinned signer's EXISTING validation, without signing."""
    phase('existing-owner-manifest-authorization');require(digest_ok(manifest_sha))
    raw=root_input(MANIFEST_BUNDLE+'/bundle.json');require(sha(raw)==MANIFEST_BUNDLE_SHA)
    bundle=json.loads(raw)
    for row in bundle['files']:
        require(re.fullmatch(r'[a-z_]+\.py',row['name']) is not None)
        require(sha(root_input(MANIFEST_BUNDLE+'/'+row['name'],1024**2))==row['sha256'])
        module=sys.modules.get(row['name'][:-3])
        require(module is None or getattr(module,'__file__',None)==MANIFEST_BUNDLE+'/'+row['name'])
    launcher=types.ModuleType('protected_manifest_launcher')
    launcher.__file__=MANIFEST_BUNDLE+'/manifest_bundle_launcher.py'
    exec(compile(root_input(launcher.__file__),launcher.__file__,'exec'),launcher.__dict__)
    launcher.verify_bundle(MANIFEST_BUNDLE)
    import importlib
    from contextlib import ExitStack
    sys.path.insert(0,MANIFEST_BUNDLE)
    try:workflow=importlib.import_module('manifest_workflow')
    finally:sys.path.pop(0)
    config,directory,approval=workflow.context(manifest_sha,actor=False)
    require(config['uid']==949 and config['user']=='zethropol-aur-release' and config['gnupg_home']==GNUPG and
            config['signer']=='F3F36BFA6D0FC60DA1CB34F2927519292F6F497B')
    p=pwd.getpwnam(config['user'])
    require((p.pw_uid,p.pw_gid,p.pw_dir,p.pw_shell)==(949,948,HOME,'/usr/bin/nologin'))
    with ExitStack() as stack:
        blob=workflow.manifest_blob(directory/'manifest.json',stack)
        workflow.validate_authorization(blob,manifest_sha,approval)
    approval_raw=root_input(APPROVALS+'/'+manifest_sha+'.json')
    require(json.loads(approval_raw)==approval)
    return {'manifest_sha256':manifest_sha,'approval_sha256':sha(approval_raw),'expires':approval['expires']}


def boot_selection():
    words=Path('/proc/cmdline').read_text().split()
    selected=[]
    for operation in ('activate','quarantine'):
        prefix='zethropol.uid949.'+operation+'='
        values=[word[len(prefix):] for word in words if word.startswith(prefix)]
        require(len(values)==1 and digest_ok(values[0]));selected.append(values[0])
    require(selected[0]==selected[1])
    return selected[0]


def exclusive_boot_window(manifest_sha):
    """Only a trusted rescue init BEFORE other userspace, never a live-root scan.

    PID 1 in the initial namespaces, no other userspace tasks/threads, two explicit
    exact-manifest kernel selections. No daemon, pkexec, shell or systemd service
    can satisfy this. The boot operator must arrange the trusted rescue init;
    this program never edits boot configuration or reboots the host.
    """
    root_host_identity();phase('exclusive-rescue-init-required')
    # Linux reserves these nsfs inode numbers for the initial PID/user
    # namespaces. Comparing only with /proc/1 would accept a nested PID1.
    require(os.stat('/proc/self/ns/pid').st_ino==0xEFFFFFFC and
            os.stat('/proc/self/ns/user').st_ino==0xEFFFFFFD)
    require(os.getpid()==1 and len(list(Path('/proc/1/task').iterdir()))==1)
    require(boot_selection()==manifest_sha)
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit() or entry.name=='1':continue
        # PID1 has not forked: any other userspace task invalidates exclusivity.
        # Disappearing/unreadable tasks fail closed too.
        status=dict(row.split(':',1) for row in (entry/'status').read_text().splitlines() if ':' in row)
        require(status.get('Kthread','').strip()=='1')


def rename_socket(source,destination):
    # Linux renameat2(RENAME_NOREPLACE); no unlink, overwrite, connect or fallback.
    libc=ctypes.CDLL(None,use_errno=True)
    result=libc.renameat2(-100,os.fsencode(source),-100,os.fsencode(destination),1)
    if result!=0:raise OSError(ctypes.get_errno(),'Quarantine rename refused')


def pin_binary(path):
    for parent in Path(path).parents:
        s=parent.lstat();require(stat.S_ISDIR(s.st_mode) and (s.st_uid,s.st_gid)==(0,0) and
                                not s.st_mode&0o022 and not ACL.intersection(os.listxattr(parent,follow_symlinks=False)))
    before=node(path,0,0,0o755,False)
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC)
    try:
        require(stable_stat(before)==stable_stat(os.fstat(fd)))
        with os.fdopen(os.dup(fd),'rb') as stream:value=hashlib.file_digest(stream,'sha256').hexdigest()
        require(stable_stat(before)==stable_stat(os.fstat(fd))==stable_stat(os.lstat(path)))
        return value
    finally:os.close(fd)


def verify_activation(policy):
    phase('committed-producer-binding')
    aid=policy['activation_id'];require(isinstance(aid,str) and re.fullmatch('[0-9a-f]{32}',aid))
    record=json.loads(root_input(STATE+'/activation.'+aid+'.json'))
    require(set(record)=={'schema','purpose','activation_id','policy_sha256','receipt_sha256','boot_id',
                          'namespace_inodes','manifest_sha256','approval_sha256','expires'})
    require(type(record['schema']) is int and record['schema']==1 and record['purpose']=='zethropol-lifecycle-activation' and
            record['activation_id']==aid and record['policy_sha256']==policy['_policy_sha256'] and
            record['boot_id']==boot_id() and record['namespace_inodes']==namespace_inodes())
    require(type(record['expires']) is int and time.time()<record['expires'] and digest_ok(record['manifest_sha256']))
    raw=root_input(APPROVALS+'/'+record['manifest_sha256']+'.json')
    require(sha(raw)==record['approval_sha256'])
    approval=json.loads(raw)
    require(approval=={'schema':1,'purpose':'zethropol-approved-root-manifest',
                       'manifest_sha256':record['manifest_sha256'],'expires':record['expires']})
    require(sha(root_input(policy['quarantine_receipt']))==record['receipt_sha256'])
    return record


def produce_activation(manifest_sha):
    """One-shot exclusive-maintenance producer. No start/sign/retry/rollback.

    Consume the exact manifest intent before ANY socket mutation. An interrupted
    operation keeps the intent, moved objects and journal; it cannot be replayed.
    Publish approved policy last. Consumers also require its exact producer record.
    """
    exclusive_boot_window(manifest_sha)
    authorization=owner_authorization(manifest_sha)
    raw=root_input(POLICY);require(sha(raw)==INACTIVE_POLICY_SHA);policy=json.loads(raw)
    require(policy['activation_approved'] is False and policy['activation_id'] is None and policy['quarantine_receipt'] is None)
    require(policy['schema']==2 and policy['generation']==GENERATION and policy['uid']==949 and policy['gid']==948 and
            policy['purpose']=='zethropol-agent-home-lifecycle-949' and policy['socket_path']==SOCKET and policy['gnupg_home']==GNUPG)
    node(STATE,0,0,0o755);node(STATE+'/quarantine',0,0,0o700)
    lock=actor_lock(root=True)
    try:
        # CSPRNG identifiers are never caller supplied; exact-manifest O_EXCL is
        # the durable replay/race/crash barrier, even before a receipt exists.
        import secrets
        aid=secrets.token_hex(16);mid=secrets.token_hex(16)
        require(aid!=mid and int(aid,16)!=0 and int(mid,16)!=0)
        intent=STATE+'/production.'+manifest_sha+'.used'
        durable_create(intent,canonical(dict(authorization,activation_id=aid,maintenance_id=mid,boot_id=boot_id())))
        for path in (HOME,GNUPG):node(path,949,948,0o700)
        node(GNUPG+'/private-keys-v1.d',949,948,0o700)  # Metadata only, never keys.
        require(not(set(owner_configuration_presence())-{'owner-agent-config'}))
        policy['agent_config_metadata']=config_metadata(os.lstat(CONFIG))
        verified_config(policy)
        policy['binary_sha256']={path:pin_binary(path) for path in ('/usr/bin/gpg-agent','/usr/bin/gpgconf')}
        for key,path in (('helper_sha256',__file__),('route_reader_sha256',ROUTE_READER),('unit_sha256',UNIT_PATH)):
            policy[key]=sha(root_input(path))
        before={}
        btime=int(next(row.split()[1] for row in Path('/proc/stat').read_text().splitlines() if row.startswith('btime ')))*10**9
        for name in SOCKET_NAMES:
            path=GNUPG+'/'+name
            try:snapshot=socket_snapshot(path)
            except FileNotFoundError:snapshot=None
            if snapshot is not None:
                validate_old_socket_binding(snapshot)
                require(int(snapshot['ctime_ns'])<btime and no_listener(path))
            before[name]=snapshot
        require(before[SOCKET_NAMES[0]]==policy['old_socket'] and before[SOCKET_NAMES[0]] is not None)
        require({p.name for p in Path(GNUPG).iterdir() if p.name.startswith('S.gpg-agent')}<=set(SOCKET_NAMES))
        destination=STATE+'/quarantine/'+mid
        os.mkdir(destination,0o700);node(destination,0,0,0o700)
        journal=STATE+'/maintenance.'+mid+'.jsonl'
        durable_create(journal,canonical({'event':'prepared','activation_id':aid,'authorization':authorization,'sockets':before})+b'\n')
        sync_directory(destination);sync_directory(STATE+'/quarantine')
        exclusive_boot_window(manifest_sha)
        after={}
        for name in SOCKET_NAMES:
            source=GNUPG+'/'+name;target=destination+'/'+name
            if before[name] is None:socket_absent(source);after[name]=None;continue
            require(socket_snapshot(source)==before[name] and no_listener(source))
            rename_socket(source,target)
            sync_directory(GNUPG);sync_directory(destination)
            after[name]=socket_snapshot(target)
            require(all(after[name][key]==before[name][key] for key in before[name] if key!='ctime_ns'))
            socket_absent(source);require(no_listener(target))
        # Parent ownership/modes/ACLs never change. Rename changes only their
        # timestamps; do not falsify ctime or restore historical timestamps.
        for path in (HOME,GNUPG):node(path,949,948,0o700)
        for path in (GNUPG,destination,STATE+'/quarantine'):
            fd=os.open(path,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
            try:os.fsync(fd)
            finally:os.close(fd)
        common_checks(policy)
        require(owner_authorization(manifest_sha)==authorization)
        receipt={'schema':2,'maintenance_id':mid,'activation_id':aid,'manifest_sha256':manifest_sha,
                 'before':before[SOCKET_NAMES[0]],'after':after[SOCKET_NAMES[0]],'destination':destination+'/'+SOCKET_NAMES[0],
                 'source_absent':True,'directories_restored':True,
                 'sockets':{name:{'before':before[name],'after':after[name]} for name in SOCKET_NAMES}}
        receipt_path=STATE+'/quarantine.'+mid+'.json';receipt_raw=canonical(receipt)
        durable_create(receipt_path,receipt_raw)
        policy.update(activation_id=aid,activation_approved=True,quarantine_receipt=receipt_path)
        policy_raw=canonical(policy)
        record=dict(authorization,schema=1,purpose='zethropol-lifecycle-activation',activation_id=aid,
                    policy_sha256=sha(policy_raw),receipt_sha256=sha(receipt_raw),boot_id=boot_id(),namespace_inodes=namespace_inodes())
        durable_create(STATE+'/activation.'+aid+'.json',canonical(record))
        staged=POLICY+'.'+aid+'.prepared';durable_create(staged,policy_raw)
        require(root_input(POLICY)==raw)
        require(owner_authorization(manifest_sha)==authorization)
        os.replace(staged,POLICY)
        fd=os.open(str(Path(POLICY).parent),os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
        try:os.fsync(fd)
        finally:os.close(fd)
        verified=load_policy();check_quarantine_receipt(verified,root=True)
        # Append only to the real producer's exclusive journal, never old audit.
        fd=os.open(journal,os.O_WRONLY|os.O_APPEND|os.O_NOFOLLOW|os.O_CLOEXEC)
        try:audit_line(fd,'activation-produced',activation_id=aid,policy_sha256=sha(policy_raw),manifest_sha256=manifest_sha)
        finally:os.close(fd)
        return record
    finally:os.close(lock)


def rescue_event(record):
    """Keep PID1 outcomes in the kernel journal without forking a logger."""
    print(json.dumps(record),flush=True)
    try:
        fd=os.open('/dev/kmsg',os.O_WRONLY|os.O_CLOEXEC|os.O_NOFOLLOW|os.O_NONBLOCK)
        try:
            info=os.fstat(fd)
            if stat.S_ISCHR(info.st_mode) and os.major(info.st_rdev)==1 and os.minor(info.st_rdev)==11:
                os.write(fd,b'<5>zethropol-uid949-rescue: '+canonical(record)+b'\n')
        finally:os.close(fd)
    except OSError:pass


def rescue_init():
    # This must be the boot operator's trusted /init, never a live-host service.
    # Do not exit PID1 or leave a failed maintenance boot in a retry loop.
    root_host_identity();require(os.getpid()==1)
    pin_binary('/usr/lib/systemd/systemd')
    try:
        result=produce_activation(boot_selection())
        rescue_event({'status':'ACTIVATION_PRODUCED','activation_id':result['activation_id'],
                      'manifest_sha256':result['manifest_sha256'],'signing':False})
    except Exception as error:
        record={'status':'BLOCKED','phase':PHASE,'signing':False,'retry_permitted':False,
                'errno':error.errno if isinstance(error,OSError) else None,
                'error_type':type(error).__name__,'pid':os.getpid()}
        if PHASE=='exclusive-rescue-init-required':
            record['namespace_inodes']=namespace_inodes()
            record['task_count']=len(list(Path('/proc/1/task').iterdir()))
            record['unexpected_userspace']=[]
            for entry in Path('/proc').iterdir():
                if not entry.name.isdigit() or entry.name=='1':continue
                try:
                    fields=dict(row.split(':',1) for row in (entry/'status').read_text().splitlines() if ':' in row)
                    if fields.get('Kthread','').strip()!='1':
                        record['unexpected_userspace'].append({'pid':int(entry.name),
                            'name':fields.get('Name','').strip(),'state':fields.get('State','').strip()})
                except OSError:pass
        rescue_event(record)
    os.execve('/usr/lib/systemd/systemd',['/usr/lib/systemd/systemd'],ENV)


def main(mode,manifest_sha=None):
    if mode=='rescue-init':
        require(manifest_sha is None);return rescue_init()
    if mode=='produce':
        return produce_activation(manifest_sha)
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
    try:main(sys.argv[1] if len(sys.argv) in (2,3) else '',sys.argv[2] if len(sys.argv)==3 else None)
    except Exception as error:
        code=error.errno if isinstance(error,OSError) and type(error.errno) is int else None
        print(json.dumps({'status':'BLOCKED','phase':PHASE,'errno':code,'agent_started_by_review':False,'signing':False},separators=(',',':')),flush=True)
        raise SystemExit(1)
