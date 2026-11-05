"""Inert file deployment. Preflight is read-only; apply is separate and single-use.

No socket maintenance, GPG, systemd mutations, activation or private key reads.
Run only through the self-contained hash-pinned workspace bundle.
"""
import base64
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time

HOME='/var/lib/zethropol-aur-release'
GNUPG=HOME+'/gnupg'
STATE='/var/lib/zethropol-aur-agent-lifecycle-949'
ACTOR=HOME+'/agent-lifecycle-v2'
RELEASE='/usr/lib/zethropol/aur-agent-lifecycle-949/releases/home-route-v2-20261103'
UNIT_NAME='zethropol-aur-manifest-agent.service'
UNIT='/etc/systemd/system/'+UNIT_NAME
ROLLBACK='/etc/systemd/system/.zethropol-aur-lifecycle-v2-rollback'
ACL={'system.posix_acl_access','system.posix_acl_default'}
D=os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC
F=os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC|os.O_NOATIME
CONFIG_HASH='250266c8436ebf3f94595d564f042fc5691b7d7104ca4f71c4d4f4ac1f804bae'
SOCKET_INODES={'S.gpg-agent':1936362,'S.gpg-agent.extra':1936363,'S.gpg-agent.browser':1936364,'S.gpg-agent.ssh':1936365}
SYSTEMD_ROOTS=('/etc/systemd/system','/run/systemd/system','/run/systemd/transient',
               '/run/systemd/generator','/run/systemd/generator.early','/run/systemd/generator.late',
               '/usr/local/lib/systemd/system','/usr/lib/systemd/system','/lib/systemd/system',
               '/etc/systemd/system.control','/run/systemd/system.control','/run/systemd/attached','/etc/systemd/system.attached')

class Blocked(RuntimeError):pass

def require(value,why):
    if not value:raise Blocked(why)

def canonical(obj):return json.dumps(obj,sort_keys=True,separators=(',',':')).encode()
def sha(raw):return hashlib.sha256(raw).hexdigest()
def fp(s):return (s.st_dev,s.st_ino,s.st_mode,s.st_uid,s.st_gid,s.st_nlink,s.st_size,s.st_mtime_ns,s.st_ctime_ns)
def metadata(s):return dict(zip(('device','inode','mode','uid','gid','nlink','size','mtime_ns','ctime_ns'),fp(s)))

def directory(fd,path):
    s=os.fstat(fd);require(stat.S_ISDIR(s.st_mode),'directory type: '+path)
    if path in (HOME,GNUPG,ACTOR):require((s.st_uid,s.st_gid,stat.S_IMODE(s.st_mode))==(949,948,0o700),'private directory: '+path)
    else:require((s.st_uid,s.st_gid)==(0,0) and not s.st_mode&0o022,'root ancestor: '+path)
    require(not ACL.intersection(os.listxattr(fd)),'directory ACL: '+path)
    return s

def walk(path):
    require(path.startswith('/') and str(Path(path))==path and '..' not in Path(path).parts,'canonical path')
    fd=os.open('/',D);directory(fd,'/')
    try:
        current=''
        for part in Path(path).parts[1:]:
            current+='/'+part;child=os.open(part,D,dir_fd=fd);os.close(fd);fd=child;directory(fd,current)
        return fd
    except BaseException:os.close(fd);raise

def reachable(path,fd):
    require(fp(os.fstat(fd))==fp(os.stat(path,follow_symlinks=False)),'changed linked parent: '+path)

def public_file(path,uid=0,gid=0,mode=0o644):
    parent=walk(str(Path(path).parent))
    try:
        fd=os.open(Path(path).name,F,dir_fd=parent)
        try:
            s=os.fstat(fd)
            require(stat.S_ISREG(s.st_mode) and s.st_nlink==1 and (s.st_uid,s.st_gid)==(uid,gid),'public regular ownership/link: '+path)
            require(stat.S_IMODE(s.st_mode)==mode and s.st_size<=2097152,'public mode/size: '+path)
            require(not ACL.intersection(os.listxattr(fd)),'public ACL: '+path)
            raw=os.pread(fd,s.st_size+1,0)
            require(len(raw)==s.st_size and fp(s)==fp(os.fstat(fd))==fp(os.stat(Path(path).name,dir_fd=parent,follow_symlinks=False)),'public read race: '+path)
            reachable(str(Path(path).parent),parent)
            return {'metadata':metadata(s),'sha256':sha(raw),'acl':{}},raw
        finally:os.close(fd)
    finally:os.close(parent)

def socket_inventory():
    fd=walk(GNUPG)
    try:
        parent=fp(os.fstat(fd));names=sorted(n for n in os.listdir(fd) if n.startswith('S.'))
        require(set(SOCKET_INODES)<=set(names),'four preserved sockets required')
        result={}
        for name in names:
            s=os.stat(name,dir_fd=fd,follow_symlinks=False)
            pin=os.open(name,os.O_PATH|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=fd)
            try:
                anchor='/proc/self/fd/'+str(fd)+'/'+name
                acl={n:os.getxattr(anchor,n,follow_symlinks=False).hex() for n in sorted(ACL.intersection(os.listxattr(anchor,follow_symlinks=False)))}
                require(fp(s)==fp(os.fstat(pin))==fp(os.stat(name,dir_fd=fd,follow_symlinks=False)),'socket pin race')
                if name in SOCKET_INODES:
                    require(stat.S_ISSOCK(s.st_mode) and (s.st_dev,s.st_ino,s.st_uid,s.st_gid,stat.S_IMODE(s.st_mode),s.st_nlink,s.st_ctime_ns)==
                            (38,SOCKET_INODES[name],949,948,0o700,1,1793481059412486406) and not acl,'supplied socket preimage drift: '+name)
                result[name]={'metadata':metadata(s),'acl':acl}
            finally:os.close(pin)
        require(names==sorted(n for n in os.listdir(fd) if n.startswith('S.')) and parent==fp(os.fstat(fd)),'socket directory race')
        reachable(GNUPG,fd);return result
    finally:os.close(fd)

def host_context():
    require(sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode,'isolated interpreter required')
    require(os.getresuid()==(0,0,0),'host root required')
    require(Path('/proc/self/uid_map').read_text().split()==['0','0','4294967295'],'initial user namespace required')
    ns={}
    for kind in ('user','pid','mnt','net'):
        ns[kind]=os.stat('/proc/self/ns/'+kind).st_ino
        require(ns[kind]==os.stat('/proc/1/ns/'+kind).st_ino,'PID1 namespace mismatch')
    return {'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip(),'namespaces':ns}

def preservation(manifest):
    result={}
    protected=manifest['protected_signer_baseline'];require(len(protected)==13,'thirteen protected signer files')
    for spec in protected:
        item,_=public_file(spec['target'],mode=int(spec['mode'],8))
        require(item['sha256']==spec['sha256'],'protected signer hash drift: '+spec['target'])
        result[spec['target']]=item
    config_path=GNUPG+'/gpg-agent.conf'
    mode=stat.S_IMODE(os.stat(config_path,follow_symlinks=False).st_mode)
    require(mode in (0o600,0o644),'config mode')
    config,_=public_file(config_path,949,948,mode)
    require(config['sha256']==CONFIG_HASH,'preserved config hash drift')
    result[config_path]=config
    return {'public_files':result,'socket_objects':socket_inventory()}

def exists_preimage(path):
    # Validate every existing prefix, including when intermediate directory is absent.
    fd=os.open('/',D);directory(fd,'/');current=''
    try:
        parts=Path(path).parts[1:]
        for i,part in enumerate(parts):
            current+='/'+part
            try:s=os.stat(part,dir_fd=fd,follow_symlinks=False)
            except FileNotFoundError:return None
            if i==len(parts)-1:return metadata(s)
            child=os.open(part,D,dir_fd=fd);os.close(fd);fd=child;directory(fd,current)
    finally:os.close(fd)

def collisions(manifest):
    result={}
    for spec in manifest['directories']+manifest['files']:
        path=spec['target'];s=exists_preimage(path);result[path]=s
        if spec['preimage']=='ABSENT_REQUIRED':require(s is None,'deployment collision: '+path)
        elif s is not None:
            fd=walk(path)
            try:require((s['uid'],s['gid'],stat.S_IMODE(s['mode']))==(spec['uid'],spec['gid'],int(spec['mode'],8)),'structural directory metadata')
            finally:os.close(fd)
    return result

def systemd_output(args):
    result=subprocess.run(['/usr/bin/systemctl',*args],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                          timeout=15,check=False,env={'PATH':'/usr/bin','LC_ALL':'C','SYSTEMD_PAGER':'cat','SYSTEMD_COLORS':'0'})
    require(result.returncode==0 or (args[0]=='show' and result.returncode==4),'systemd manager unavailable')
    return result.stdout.decode('utf-8',errors='strict')

def manager_state(allow_published=False):
    raw=systemd_output(['show',UNIT_NAME,'--no-pager','--property=LoadState,ActiveState,SubState,UnitFileState,FragmentPath,DropInPaths,Job'])
    fields=dict(line.split('=',1) for line in raw.splitlines() if '=' in line)
    require(fields.get('ActiveState')=='inactive' and fields.get('SubState')=='dead','manager unit active/state')
    loaded=fields.get('LoadState')=='loaded'
    require(fields.get('LoadState')=='not-found' or (allow_published and loaded and fields.get('FragmentPath')==UNIT),'manager unit collision')
    require(fields.get('UnitFileState','') in (('','static') if allow_published else ('',)),'unit enabled/state')
    require((not fields.get('FragmentPath') or allow_published and fields['FragmentPath']==UNIT) and not fields.get('DropInPaths') and fields.get('Job','') in ('','0'),'manager fragment/dropin/job collision')
    jobs=systemd_output(['list-jobs','--no-legend','--no-pager','--plain'])
    require(not any(UNIT_NAME in row.split() for row in jobs.splitlines()),'queued unit job')
    # Never invoke reload/start/stop/enable/mask here.
    return fields

def systemd_collisions(allow_published=False):
    for root in SYSTEMD_ROOTS:
        p=Path(root)
        try:os.lstat(root)
        except FileNotFoundError:continue
        # /lib may be a platform alias; resolve only for read-only systemd inventory.
        checked=walk(str(p.resolve(strict=True)));os.close(checked)
        require(p.is_dir(),'systemd search root type')
        for entry in p.iterdir():
            entry_stat=os.lstat(entry)
            if entry.name in (UNIT_NAME,UNIT_NAME+'.d','zethropol-aur-manifest-.service.d','zethropol-aur-.service.d','zethropol-.service.d','service.d'):
                require(allow_published and str(entry)==UNIT,'unit/dropin collision: '+str(entry))
            if stat.S_ISLNK(entry_stat.st_mode):
                require(Path(os.readlink(entry)).name!=UNIT_NAME,'unit alias collision: '+str(entry))
            if entry.name.endswith(('.wants','.requires','.upholds')):
                require(stat.S_ISDIR(os.stat(entry).st_mode),'dependency directory type')
                try:os.lstat(entry/UNIT_NAME)
                except FileNotFoundError:pass
                else:raise Blocked('enable/dependency link collision')
    return manager_state(allow_published)

def bundle_data(bundle):
    manifest=bundle['manifest'];require(sha(canonical(manifest))==bundle['manifest_sha256'],'manifest digest')
    payload={path:base64.b64decode(raw,validate=True) for path,raw in bundle['payload'].items()}
    allowed={RELEASE+'/agent_lifecycle_949.py',RELEASE+'/native_socket_route_readonly.py',RELEASE+'/policy.json',UNIT,ACTOR+'/lifecycle.lock'}
    require(set(payload)==allowed=={f['target'] for f in manifest['files']},'exact five-file allowlist')
    for f in manifest['files']:require(sha(payload[f['target']])==f['sha256'],'embedded payload digest')
    policy=json.loads(payload[RELEASE+'/policy.json'])
    require(policy['activation_approved'] is False and policy['activation_id'] is None and policy['quarantine_receipt'] is None,'inactive policy required')
    return manifest,payload

def capture(bundle):
    manifest,_=bundle_data(bundle);context=host_context()
    before=preservation(manifest);targets=collisions(manifest);manager=systemd_collisions()
    after=preservation(manifest);require(before==after,'preservation changed during preflight')
    require(context==host_context(),'host context changed')
    return {'context':context,'preservation':before,'targets':targets,'manager':manager,'manifest_sha256':bundle['manifest_sha256']}

def validate_capture(envelope,current,now):
    require(set(envelope)=={'schema','transaction_id','issued_boottime_ns','capture'} and envelope['schema']==1,'capture envelope fields')
    require(re.fullmatch('[0-9a-f]{32}',envelope['transaction_id']) is not None,'new transaction ID required')
    require(type(envelope['issued_boottime_ns']) is int and 0<=envelope['issued_boottime_ns']<=now,'invalid preflight observation time')
    require(envelope['capture']==current,'fresh preflight binding drift')

def noreplace(srcfd,src,dstfd,dst):
    libc=ctypes.CDLL(None,use_errno=True);fn=getattr(libc,'renameat2',None)
    require(fn is not None,'renameat2 required')
    if fn(srcfd,os.fsencode(src),dstfd,os.fsencode(dst),1):raise OSError(ctypes.get_errno(),'RENAME_NOREPLACE failed')

def write_new(parent,name,raw,uid=0,gid=0,mode=0o600):
    fd=os.open(name,os.O_RDWR|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o600,dir_fd=parent)
    try:
        os.fchown(fd,uid,gid);os.fchmod(fd,mode)
        view=memoryview(raw)
        while view:
            count=os.write(fd,view);require(count>0,'short write');view=view[count:]
        s=os.fstat(fd)
        require(stat.S_ISREG(s.st_mode) and (s.st_uid,s.st_gid,stat.S_IMODE(s.st_mode),s.st_nlink)==(uid,gid,mode,1) and not ACL.intersection(os.listxattr(fd)),'new file metadata')
        require(os.pread(fd,len(raw)+1,0)==raw,'new file byte check');os.fsync(fd)
    finally:os.close(fd)
    os.fsync(parent)

def mkdir_new(parent,name,uid=0,gid=0,mode=0o700):
    os.mkdir(name,0o700,dir_fd=parent);fd=os.open(name,D,dir_fd=parent)
    try:
        os.fchown(fd,uid,gid);os.fchmod(fd,mode)
        s=os.fstat(fd);require((s.st_uid,s.st_gid,stat.S_IMODE(s.st_mode))==(uid,gid,mode) and not ACL.intersection(os.listxattr(fd)),'new directory metadata')
        os.fsync(fd);os.fsync(parent)
    except BaseException:os.close(fd);raise
    return fd

def ensure_directory(spec):
    parent=walk(str(Path(spec['target']).parent))
    try:
        try:fd=mkdir_new(parent,Path(spec['target']).name,spec['uid'],spec['gid'],int(spec['mode'],8))
        except FileExistsError:
            require(spec['preimage']=='ABSENT_OR_EXACT_SAFE_METADATA','new directory collision')
            fd=walk(spec['target'])
            require((os.fstat(fd).st_uid,os.fstat(fd).st_gid,stat.S_IMODE(os.fstat(fd).st_mode))==(spec['uid'],spec['gid'],int(spec['mode'],8)),'existing structural directory')
        reachable(str(Path(spec['target']).parent),parent);return fd
    finally:os.close(parent)

def publish_directory(path,members,txid,uid=0,gid=0,mode=0o755):
    parent=walk(str(Path(path).parent));name=Path(path).name;stage='.'+name+'.'+txid+'.pending'
    # Never create/chown staging entries inside UID949-owned HOME.
    staging=walk(STATE+'/deployment/'+txid) if path==ACTOR else os.dup(parent)
    fd=None
    try:
        fd=mkdir_new(staging,stage)
        require(os.fstat(staging).st_dev==os.fstat(parent).st_dev,'staging EXDEV forbidden')
        for name_in,raw,owner,group,perm in members:write_new(fd,name_in,raw,owner,group,perm)
        os.fchown(fd,uid,gid);os.fchmod(fd,mode);os.fsync(fd)
        require(not ACL.intersection(os.listxattr(fd)),'staging ACL')
        noreplace(staging,stage,parent,name);os.fsync(staging);os.fsync(parent)
        reachable(path,fd);reachable(str(Path(path).parent),parent)
        return (os.fstat(fd).st_dev,os.fstat(fd).st_ino)
    finally:
        if fd is not None:os.close(fd)
        os.close(staging);os.close(parent)

def postflight(manifest,baseline):
    for f in manifest['files']:
        item,_=public_file(f['target'],f['uid'],f['gid'],int(f['mode'],8));require(item['sha256']==f['sha256'],'postflight payload hash')
    for d in manifest['directories']:
        fd=walk(d['target'])
        try:
            s=os.fstat(fd);require((s.st_uid,s.st_gid,stat.S_IMODE(s.st_mode))==(d['uid'],d['gid'],int(d['mode'],8)),'postflight directory metadata')
        finally:os.close(fd)
    require(preservation(manifest)==baseline['preservation'],'postflight protected drift')
    systemd_collisions(allow_published=True)


def apply(bundle,envelope,progress=None):
    progress=progress if progress is not None else {}
    progress.update(stage='bundle-validation',write_phase_entered=False)
    manifest,payload=bundle_data(bundle)
    progress['stage']='preflight-revalidation'
    current=capture(bundle);validate_capture(envelope,current,time.clock_gettime_ns(time.CLOCK_BOOTTIME))
    txid=envelope['transaction_id'];journal=None;published_unit=None
    specs={d['target']:d for d in manifest['directories']}
    # First write creates a previously absent root state. Retain every failure;
    # its presence prevents replay, even if interrupted before claim creation.
    progress.update(stage='state-creation',write_phase_entered=True)
    state=ensure_directory(specs[STATE])
    try:
        deploy=ensure_directory(specs[STATE+'/deployment'])
        try:journal=mkdir_new(deploy,txid)
        finally:os.close(deploy)
        claim=canonical({'transaction_id':txid,'manifest_sha256':bundle['manifest_sha256'],
                         'bundle_sha256':BUNDLE_SHA,'capture_sha256':sha(canonical(envelope))})
        write_new(journal,'claim.json',claim)
        write_new(journal,'manifest.json',canonical(manifest));write_new(journal,'preflight.json',canonical(envelope))
        try:
            for path in (STATE+'/quarantine',ROLLBACK,str(Path(RELEASE).parent.parent),str(Path(RELEASE).parent)):
                fd=ensure_directory(specs[path]);os.close(fd)
            publish_directory(RELEASE,[(Path(p).name,raw,0,0,0o644) for p,raw in payload.items() if p.startswith(RELEASE+'/')],txid)
            publish_directory(ACTOR,[('lifecycle.lock',b'',949,948,0o600)],txid,949,948,0o700)
            require(preservation(manifest)==current['preservation'],'pre-unit preservation drift');systemd_collisions()
            parent=walk(str(Path(UNIT).parent))
            try:
                stage='.'+UNIT_NAME+'.'+txid+'.pending';write_new(parent,stage,payload[UNIT],mode=0o644)
                staged=os.stat(stage,dir_fd=parent,follow_symlinks=False)
                noreplace(parent,stage,parent,UNIT_NAME);os.fsync(parent)
                published_unit=(staged.st_dev,staged.st_ino)
            finally:os.close(parent)
            postflight(manifest,current)
            post=canonical({'status':'PASS_INERT_FILE_DEPLOYMENT','manifest_sha256':bundle['manifest_sha256'],
                            'protected_capture_sha256':sha(canonical(current['preservation'])),'host_changed':True,
                            'socket_changed':False,'activation_approved':False,'agent_started':False,'daemon_reloaded':False})
            write_new(journal,'postflight.json',post)
            result={'status':'PASS_INERT_FILE_DEPLOYMENT','journal':STATE+'/deployment/'+txid,'manifest_sha256':bundle['manifest_sha256'],
                    'record_sha256':{'claim.json':sha(claim),'manifest.json':sha(canonical(manifest)),
                                     'preflight.json':sha(canonical(envelope)),'postflight.json':sha(post)},
                    'activation_approved':False,'quarantine_authorized':False,'agent_started':False}
            return result
        except BaseException:
            rollback={'status':'INCOMPLETE_RETAINED','unit_withdrawn':False}
            if published_unit is not None:
                try:
                    manager_state(allow_published=True);parent=walk(str(Path(UNIT).parent));dest=walk(ROLLBACK)
                    try:
                        s=os.stat(UNIT_NAME,dir_fd=parent,follow_symlinks=False)
                        require((s.st_dev,s.st_ino)==published_unit,'foreign unit; no rollback')
                        item,_=public_file(UNIT);require(item['sha256']==sha(payload[UNIT]),'unit rollback drift')
                        require(os.fstat(parent).st_dev==os.fstat(dest).st_dev,'rollback EXDEV forbidden')
                        noreplace(parent,UNIT_NAME,dest,txid+'.unit');os.fsync(parent);os.fsync(dest)
                        rollback.update(unit_withdrawn=True,retained_unit=ROLLBACK+'/'+txid+'.unit')
                    finally:os.close(parent);os.close(dest)
                except Exception:rollback['status']='BLOCKED_ROLLBACK_OPERATOR_REVIEW'
            write_new(journal,'rollback.json',canonical(rollback))
            raise
    finally:
        if journal is not None:os.close(journal)
        os.close(state)


def fish_quote(text):return "'"+text.replace('\\','\\\\').replace("'","\\'")+"'"

def bootstrap(path,digest,mode,capture_b64=''):
    return f"""import hashlib,os,stat
path={path!r}
fd=os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
try:
 parts=path.strip('/').split('/')
 for part in parts[:-1]:
  child=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=fd);os.close(fd);fd=child
 f=os.open(parts[-1],os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC,dir_fd=fd)
 try:
  s=os.fstat(f)
  if not stat.S_ISREG(s.st_mode) or s.st_nlink!=1 or not 0<s.st_size<=2097152:raise SystemExit('BLOCKED bundle metadata')
  raw=os.pread(f,s.st_size+1,0)
  if len(raw)!=s.st_size or hashlib.sha256(raw).hexdigest()!={digest!r}:raise SystemExit('BLOCKED bundle digest')
 finally:os.close(f)
finally:os.close(fd)
exec(compile(raw,'pinned-inert-file-transaction','exec'),{{'__name__':'__main__','BUNDLE_PATH':path,'BUNDLE_SHA':{digest!r},'MODE':{mode!r},'CAPTURE_B64':{capture_b64!r}}})
"""

def main(bundle,mode,capture_b64=''):
    progress={'stage':'entry','write_phase_entered':False}
    try:
        if mode=='preflight':
            observed=capture(bundle)
            envelope={'schema':1,'transaction_id':os.urandom(16).hex(),'issued_boottime_ns':time.clock_gettime_ns(time.CLOCK_BOOTTIME),'capture':observed}
            encoded=base64.b64encode(canonical(envelope)).decode()
            command='/usr/bin/sudo /usr/bin/python3 -I -S -B -c '+fish_quote(bootstrap(BUNDLE_PATH,BUNDLE_SHA,'apply',encoded))
            result={'status':'PASS_FILE_DEPLOYMENT_PREFLIGHT','manifest_sha256':bundle['manifest_sha256'],
                    'preservation_sha256':sha(canonical(observed['preservation'])),'transaction_id':envelope['transaction_id'],
                    'full_preflight_revalidated_before_first_write':True,'provider_deadline_unchanged':True,
                    'host_changed':False,'activation_approved':False,'quarantine_authorized':False,
                    'separate_single_use_apply_command_follows':True}
        elif mode=='apply':result=apply(bundle,json.loads(base64.b64decode(capture_b64,validate=True)),progress)
        else:raise Blocked('unsupported mode')
        print(json.dumps(result,sort_keys=True))
        if mode=='preflight':print('\n# Separate inert-files application command; run only in an exclusive root deployment window.\n'+command)
    except Exception as e:
        print(json.dumps({'status':'BLOCKED_FILE_DEPLOYMENT','reason':str(e) if isinstance(e,Blocked) else type(e).__name__,
                          'errno':e.errno if isinstance(e,OSError) else None,
                          'partial_artifacts_may_exist':progress['write_phase_entered'],
                          'this_invocation_write_phase_entered':progress['write_phase_entered'],
                          'failure_stage':progress['stage'],'prior_artifacts_not_excluded':mode=='apply',
                          'activation_approved':False,'quarantine_authorized':False,'agent_started':False},sort_keys=True))
        raise SystemExit(1)

if __name__=='__main__':main(BUNDLE,MODE,CAPTURE_B64)
