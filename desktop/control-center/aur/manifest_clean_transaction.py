"""Clean exact-hash transaction coordinator; --check never creates host files.

Execute only pinned bytes via the reviewed bootstrap. Signing requires NEW
explicit exact-hash consent. Closed deployment/old attempts are never rerun.
"""
import fcntl
import hashlib
import importlib
import json
import os
from pathlib import Path
import pwd
import grp
import re
import stat
import subprocess
import sys
import time
from contextlib import ExitStack

RELEASE='3149c27c6c66a0c33a58a2e57f8279def163ebd3b9c99977ce112001c176e3f2'
BUNDLE_PATH='/usr/lib/zethropol/aur-manifest-diagnostics-'+RELEASE
APPROVALS='/var/lib/zethropol/aur-manifest-approvals'
PENDING='/var/lib/zethropol-aur-manifests/pending'
HOME='/var/lib/zethropol-aur-release'
GPG_HOME=HOME+'/gnupg'
PRIMARY='F3F36BFA6D0FC60DA1CB34F2927519292F6F497B'
POLICY='/etc/zethropol-update/aur-trust/policy.json'
OLD_HASHES={'4597d7e9243d516cf55718caa8aedaf9efc4901c351d94364508cc0ff3a2dc21',
            '45865bc212aabdb26fffe6a7d1a716525a38f763d8110d8808d77188bc2e279f',
            'bdd617a78db3bf3bda1ea073f11e585a753352bf5a3da86eee6941a0b4f2c8fe'}
ACL={'system.posix_acl_access','system.posix_acl_default'}
DFLAGS=os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC
FFLAGS=os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC
CURRENT_PHASE='initial'


def phase(name):
    global CURRENT_PHASE
    CURRENT_PHASE=name


class Blocked(RuntimeError):pass


def require(ok):
    if not ok:raise Blocked('Exact transaction precondition rejected')


def canonical(obj):return json.dumps(obj,sort_keys=True,separators=(',',':')).encode()
def digest(raw):return hashlib.sha256(raw).hexdigest()


def validate_plan(plan,authorized=None,apply=False,now=None):
    require(set(plan)=={'schema','purpose','manifest_sha256','expires','manifest_path','nonce','inputs',
                       'bundle_files','config','public_probe','source_bindings'})
    require(type(plan['schema']) is int and plan['schema']==1 and plan['purpose']=='zethropol-clean-manifest-transaction')
    require(type(plan['manifest_sha256']) is str and re.fullmatch('[0-9a-f]{64}',plan['manifest_sha256']) and
            plan['manifest_sha256'] not in OLD_HASHES)
    current=time.time() if now is None else now
    require(type(plan['expires']) is int and current<plan['expires']<=current+86400)
    require(type(plan['nonce']) is str and re.fullmatch('[0-9a-f]{32}',plan['nonce']))
    require(not apply or authorized==plan['manifest_sha256'])
    require(len(plan['bundle_files'])==16 and len(plan['inputs'])>=294)


def names(plan):
    sha=plan['manifest_sha256'];nonce=plan['nonce']
    return sha+'.json',sha,sha+'.'+nonce+'.signing-audit.jsonl',sha+'.'+nonce+'.signing-provenance.json'


def meta(fd,uid=0,gid=0,directory=False,mode=None):
    s=os.fstat(fd)
    require((stat.S_ISDIR(s.st_mode) if directory else stat.S_ISREG(s.st_mode)) and
            (s.st_uid,s.st_gid)==(uid,gid) and not s.st_mode&0o022 and
            not ACL.intersection(os.listxattr(fd)) and (directory or s.st_nlink==1))
    require(mode is None or stat.S_IMODE(s.st_mode)==mode)
    return s


def walk(path,special=None,trusted=True):
    require(path.startswith('/') and str(Path(path))==path and '..' not in Path(path).parts)
    special=special or {};fd=os.open('/',DFLAGS);current=''
    try:
        if trusted:meta(fd,directory=True)
        for part in Path(path).parts[1:]:
            child=os.open(part,DFLAGS,dir_fd=fd);os.close(fd);fd=child;current+='/'+part
            if trusted:
                uid=special.get(current,0)
                meta(fd,uid,948 if uid==949 else 0,True,0o700 if uid==949 else None)
        return fd
    except BaseException:os.close(fd);raise


def read_checked(path,sha=None,trusted=False,data=True,limit=512*1024*1024):
    parent=walk(str(Path(path).parent),trusted=trusted)
    try:fd=os.open(Path(path).name,FFLAGS,dir_fd=parent)
    finally:os.close(parent)
    try:
        s=os.fstat(fd);require(stat.S_ISREG(s.st_mode) and s.st_nlink==1 and 0<s.st_size<=limit)
        if trusted:meta(fd,mode=0o644)
        with os.fdopen(os.dup(fd),'rb') as f:actual=hashlib.file_digest(f,'sha256').hexdigest()
        after=os.fstat(fd)
        require((s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns,s.st_ctime_ns)==
                (after.st_dev,after.st_ino,after.st_size,after.st_mtime_ns,after.st_ctime_ns))
        require(sha is None or actual==sha)
        if not data:return actual
        require(s.st_size<=2*1024*1024);raw=os.pread(fd,s.st_size,0);require(digest(raw)==actual);return raw
    finally:os.close(fd)


def load_bundle(plan):
    for name,sha in plan['bundle_files'].items():
        require(Path(name).name==name)
        read_checked(BUNDLE_PATH+'/'+name,sha,True,False)
    require(digest(read_checked(BUNDLE_PATH+'/bundle.json',RELEASE,True))==RELEASE)
    sys.path.insert(0,BUNDLE_PATH)
    modules=[importlib.import_module(name) for name in ('manifest_bundle_launcher','manifest_workflow','manifest_failure_capture')]
    require(all(str(Path(m.__file__).parent)==BUNDLE_PATH for m in modules))
    launcher,producer,consumer=modules
    launcher.verify_bundle(BUNDLE_PATH)  # Normal fresh-operation version guard, not rollout acceptance.
    launcher.protocol_check(producer,consumer)
    return launcher,producer,consumer


def identity():
    p=pwd.getpwnam('zethropol-aur-release')
    require((p.pw_uid,p.pw_gid,p.pw_dir,p.pw_shell)==(949,948,HOME,'/usr/bin/nologin'))
    require(grp.getgrgid(948).gr_name=='zethropol-aur-release')
    require(pwd.getpwnam('zethropol-aur-archive').pw_uid==951 and pwd.getpwnam('zethropol-aur-attestor').pw_uid==950)


def absent(parent,name):
    try:os.stat(name,dir_fd=parent,follow_symlinks=False)
    except FileNotFoundError:return
    raise Blocked('Existing exact target; no reuse or replacement')


def sealed_public(raw):
    fd=os.memfd_create('manifest-public-preflight',os.MFD_CLOEXEC|os.MFD_ALLOW_SEALING)
    try:
        view=memoryview(raw)
        while view:
            n=os.write(fd,view);require(n>0);view=view[n:]
        os.fchown(fd,949,948);os.fchmod(fd,0o400)
        fcntl.fcntl(fd,fcntl.F_ADD_SEALS,fcntl.F_SEAL_WRITE|fcntl.F_SEAL_GROW|fcntl.F_SEAL_SHRINK|fcntl.F_SEAL_SEAL)
        return fd
    except BaseException:os.close(fd);raise


PUBLIC_PROBE=r'''
import json,os,subprocess,sys
p='identity'
try:
 if (os.getuid(),os.geteuid(),os.getgid(),os.getegid(),os.getgroups())!=(949,949,948,948,[948]):raise RuntimeError()
 p='public-file-access'
 for path in sys.argv[5:]:
  fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC);os.close(fd)
 p='pending-parent-access'
 if not os.access('/var/lib/zethropol-aur-manifests/pending',os.X_OK|os.W_OK):raise RuntimeError()
 p='approval-parent-access'
 if not os.access('/var/lib/zethropol/aur-manifest-approvals',os.R_OK|os.X_OK):raise RuntimeError()
 if os.access('/var/lib/zethropol/aur-manifest-approvals',os.W_OK):raise RuntimeError()
 p='public-gpgv'
 fds=tuple(int(v) for v in sys.argv[1:4]);a,s,k=fds
 r=subprocess.run(['/usr/bin/gpgv','--homedir','/','--status-fd','1','--keyring','/proc/self/fd/'+str(k),'--','/proc/self/fd/'+str(s),'/proc/self/fd/'+str(a)],pass_fds=fds,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=20,env={'PATH':'/usr/bin','LC_ALL':'C'})
 valid=[line.split() for line in r.stdout.decode('ascii').splitlines() if line.startswith('[GNUPG:] VALIDSIG ')]
 if r.returncode or len(valid)!=1 or valid[0][-1]!=sys.argv[4]:raise RuntimeError()
 print('PASS uid949=verified gid948=verified public-gpgv=verified file-access=verified key-agent=not-probed')
except Exception:
 print('BLOCKED uid949-public-preflight phase='+p);raise SystemExit(1)
'''


def public_probe(plan,launcher):
    probe=plan['public_probe'];fds=[]
    try:
        for name in ('archive','signature','keyring'):
            row=probe[name];fds.append(sealed_public(read_checked(row['path'],row['sha256'])))
        result=subprocess.run(['/usr/bin/python3','-I','-S','-B','-c',PUBLIC_PROBE,
            *[str(fd) for fd in fds],probe['primary'],BUNDLE_PATH+'/manifest_workflow.py',POLICY,
            '/etc/zethropol-update/aur-trust/manifest.gpg'],pass_fds=tuple(fds),preexec_fn=launcher.drop_identity,
            stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=25,
            env={'PATH':'/usr/bin','LC_ALL':'C','HOME':HOME})
        if result.returncode:
            match=re.fullmatch(rb'BLOCKED uid949-public-preflight phase=(identity|public-file-access|pending-parent-access|approval-parent-access|public-gpgv)\n',result.stdout)
            if match:phase('uid949-'+match[1].decode('ascii'))
        require(result.returncode==0 and result.stdout==b'PASS uid949=verified gid948=verified public-gpgv=verified file-access=verified key-agent=not-probed\n')
    finally:
        for fd in fds:os.close(fd)


def preflight(plan,stack):
    phase('identity-and-expiry')
    validate_plan(plan);identity()
    phase('manifest-and-public-inputs')
    raw=read_checked(plan['manifest_path'],plan['manifest_sha256'])
    manifest=json.loads(raw);require(raw==canonical(manifest) and manifest['expires']==plan['expires'] and len(manifest['packages'])==145)
    for row in plan['inputs']:read_checked(row['path'],row['sha256'],row['trusted'],False)
    phase('installed-producer-consumer-compatibility')
    modules=load_bundle(plan);launcher,producer,consumer=modules
    phase('signer-config-and-public-policy')
    config=json.loads(read_checked('/etc/zethropol-update/aur-manifest-signer.json',trusted=True))
    require(config==plan['config'] and config['signer']==PRIMARY and producer.signer_model(config)[1]==949)
    policy=json.loads(read_checked(POLICY,trusted=True))['manifest'];require(PRIMARY in policy['signers'])
    read_checked('/etc/zethropol-update/aur-trust/'+policy['keyring'],policy['keyring_sha256'],True,False)
    phase('gpg-home-and-ancestors-metadata')
    gfd=walk(GPG_HOME,{HOME:949,GPG_HOME:949});stack.callback(os.close,gfd)
    phase('fresh-approval-pending-audit-targets')
    pfd=walk(PENDING,{PENDING:949});stack.callback(os.close,pfd)
    afd=walk(APPROVALS);stack.callback(os.close,afd)
    aname,pname,audit,provenance=names(plan)
    for name in (aname,audit,provenance):absent(afd,name)
    absent(pfd,pname)
    phase('uid949-public-verification-probe')
    public_probe(plan,launcher)
    validate_plan(plan)
    return raw,manifest,modules,(afd,pfd,gfd)


def create(parent,name,raw,uid=0,gid=0,mode=0o600):
    fd=os.open(name,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,mode,dir_fd=parent)
    try:
        os.fchown(fd,uid,gid);os.fchmod(fd,mode)
        view=memoryview(raw)
        while view:
            n=os.write(fd,view);require(n>0);view=view[n:]
        meta(fd,uid,gid,mode=mode);os.fsync(fd)
    finally:os.close(fd)
    os.fsync(parent)


def event_writer(fd,plan):
    sequence=0
    def event(name,**fields):
        nonlocal sequence
        require(name in {'preflight-pass','owner-approval-created','pending-prepared','signing-attempt-started',
                         'signer-process-outcome','independent-signature-verified','blocked'})
        sequence+=1
        raw=canonical({'schema':1,'sequence':sequence,'event':name,'time_ns':str(time.time_ns()),
                       'manifest_sha256':plan['manifest_sha256'],'nonce':plan['nonce'],**fields})+b'\n'
        view=memoryview(raw)
        while view:
            n=os.write(fd,view);require(n>0);view=view[n:]
        os.fsync(fd)
    return event


def apply_transaction(plan,raw,modules,parents,stack):
    launcher,producer,consumer=modules;afd,pfd,gfd=parents
    aname,pname,audit_name,provenance_name=names(plan)
    phase('audit-create')
    af=os.open(audit_name,os.O_RDWR|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o600,dir_fd=afd)
    stack.callback(os.close,af);meta(af,mode=0o600);os.fsync(afd)
    event=event_writer(af,plan)
    try:
        event('preflight-pass',bundle_sha256=RELEASE)
        validate_plan(plan)
        phase('owner-approval-create')
        approval={'schema':1,'purpose':'zethropol-approved-root-manifest','manifest_sha256':plan['manifest_sha256'],'expires':plan['expires']}
        create(afd,aname,canonical(approval),mode=0o644);event('owner-approval-created')
        validate_plan(plan)
        phase('pending-create')
        os.mkdir(pname,mode=0o700,dir_fd=pfd);dfd=os.open(pname,DFLAGS,dir_fd=pfd);stack.callback(os.close,dfd)
        os.fchown(dfd,949,948);os.fchmod(dfd,0o700);meta(dfd,949,948,True,0o700)
        create(dfd,'manifest.json',raw,949,948);os.fsync(pfd);event('pending-prepared')
        # Recheck immutable bundle and private-directory identity immediately before key use.
        phase('immediate-pre-sign-recheck');load_bundle(plan);identity();validate_plan(plan)
        again=walk(GPG_HOME,{HOME:949,GPG_HOME:949})
        try:require((os.fstat(again).st_dev,os.fstat(again).st_ino)==(os.fstat(gfd).st_dev,os.fstat(gfd).st_ino))
        finally:os.close(again)
        event('signing-attempt-started')
        phase('signer-child')
        launcher.observe_process(['/usr/bin/python3','-I','-S','-B',BUNDLE_PATH+'/manifest_workflow.py',
                                  'sign',plan['manifest_sha256']],consumer,plan['manifest_sha256'],event)
        validate_plan(plan)
        phase('independent-public-signature-verification')
        current=walk(PENDING+'/'+pname,{PENDING:949,PENDING+'/'+pname:949});stack.callback(os.close,current)
        require((os.fstat(current).st_dev,os.fstat(current).st_ino)==(os.fstat(dfd).st_dev,os.fstat(dfd).st_ino))
        fds=[]
        for name in ('manifest.json','manifest.json.sig'):
            fd=os.open(name,FFLAGS,dir_fd=dfd);stack.callback(os.close,fd);info=meta(fd,949,948,mode=0o600)
            require(0<info.st_size<=1024*1024);fds.append(fd)
        policy=producer.TrustPolicy.load(POLICY,role='manifest');stack.callback(policy.keyring.close)
        verified=stack.enter_context(producer.SignedArchive(*fds,policy,archive_limit=1024*1024))
        require(verified.signer==PRIMARY and verified.archive.sha256==plan['manifest_sha256'])
        require(os.pread(verified.archive.fd,verified.archive.size,0)==raw)
        producer.validate_authorization(verified.archive,plan['manifest_sha256'],approval)
        read_checked(APPROVALS+'/'+aname,digest(canonical(approval)),True,False)
        event('independent-signature-verified',primary=PRIMARY,signature_sha256=verified.signature.sha256)
        audit=os.pread(af,os.fstat(af).st_size,0);records=[json.loads(line) for line in audit.splitlines()]
        require([r['sequence'] for r in records]==list(range(1,len(records)+1)) and
                all(r['manifest_sha256']==plan['manifest_sha256'] and r['nonce']==plan['nonce'] for r in records) and
                [r['event'] for r in records]==['preflight-pass','owner-approval-created','pending-prepared',
                    'signing-attempt-started','signer-process-outcome','independent-signature-verified'])
        outcome=[r['outcome'] for r in records if r['event']=='signer-process-outcome']
        require(len(outcome)==1 and outcome[0]['status']=='SIGNER_COMPLETED_UNVERIFIED' and outcome[0]['child_returncode']==0)
        provenance={'schema':1,'scope':'Manifest signing only; no signed-root or production authority',
                    'manifest_sha256':plan['manifest_sha256'],'nonce':plan['nonce'],'bundle_sha256':RELEASE,
                    'signature_sha256':verified.signature.sha256,'primary':PRIMARY,'uid':949,'gid':948,
                    'expires':plan['expires'],'approval_sha256':digest(canonical(approval)),
                    'audit_sha256':digest(audit),'audit_records':len(records)}
        validate_plan(plan);create(afd,provenance_name,canonical(provenance),mode=0o644)
        read_checked(APPROVALS+'/'+provenance_name,digest(canonical(provenance)),True,False)
        validate_plan(plan)
        print('PASS clean-manifest signature=verified audit=verified provenance=verified production=false')
    except BaseException as error:
        try:event('blocked',kind='signer-process' if isinstance(error,consumer.SignerProcessBlocked) else 'coordinator-guard')
        except BaseException:pass
        if isinstance(error,consumer.SignerProcessBlocked):
            print('BLOCKED '+canonical(error.outcome).decode())
        raise


def main(plan,mode,authorized=None):
    phase('new-exact-hash-authorization')
    require(mode in ('--check','--apply'))
    validate_plan(plan,authorized,apply=mode=='--apply')
    require(os.getuid()==os.geteuid()==0 and sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode)
    require(Path('/proc/self/uid_map').read_text().split()==['0','0','4294967295'])
    with ExitStack() as stack:
        raw,manifest,modules,parents=preflight(plan,stack)
        if mode=='--check':
            print('PASS clean-preflight manifest='+plan['manifest_sha256']+' packages=145 uid=949 gid=948 public-gpgv=verified key-agent=not-probed authorization=required signing=false')
            return
        apply_transaction(plan,raw,modules,parents,stack)
