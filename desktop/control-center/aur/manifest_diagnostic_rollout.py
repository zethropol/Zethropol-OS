"""New immutable directory rollout only. Never modify legacy signer or sign.

Use only a reviewed hash-pinned isolated bootstrap. No services, trust, keys,
approvals or old transaction artifacts are touched. Failed artifacts retained.
"""
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
from contextlib import contextmanager

ROOT_UID=0
ROOT_GID=0
ACL={'system.posix_acl_access','system.posix_acl_default'}
DFLAGS=os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC
FFLAGS=os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC
PROTOCOL='zethropol-manifest-diagnostics/1'
PROTECTED_PLAN='2ef3c2471498a8c86e09c28ca6732f6bc231d65ef9f1a915768f1cff8ef53711'
PROTECTED_PATHS={'/etc/zethropol-update/aur-manifest-signer.json'}|{
    '/usr/lib/zethropol/aur/'+name+'.py' for name in ('manifest_workflow','provider','pipeline','provision',
    'worker','trust','sealed','quota','journal','scoped','root_limits','staged_root')}


def require(ok,message):
    if not ok:raise RuntimeError(message)


def sha(raw):return hashlib.sha256(raw).hexdigest()


def canonical(value):return json.dumps(value,sort_keys=True,separators=(',',':')).encode()


def metadata(fd,directory=False,mode=None):
    s=os.fstat(fd)
    require((stat.S_ISDIR(s.st_mode) if directory else stat.S_ISREG(s.st_mode)) and
            (s.st_uid,s.st_gid)==(ROOT_UID,ROOT_GID) and not s.st_mode&0o022 and
            not ACL.intersection(os.listxattr(fd)) and (directory or s.st_nlink==1),
            'Unsafe root-owned inode/ACL')
    require(mode is None or stat.S_IMODE(s.st_mode)==mode,'Unexpected mode')
    return s


def walk(path,trusted=True):
    require(path.startswith('/') and str(Path(path))==path and '..' not in Path(path).parts,'Canonical absolute path required')
    fd=os.open('/',DFLAGS)
    try:
        if trusted:metadata(fd,True)
        for part in Path(path).parts[1:]:
            child=os.open(part,DFLAGS,dir_fd=fd);os.close(fd);fd=child
            if trusted:metadata(fd,True)
        return fd
    except BaseException:os.close(fd);raise


def readpath(path,trusted=False):
    parent,name=str(Path(path).parent),Path(path).name
    pfd=walk(parent,trusted)
    try:fd=os.open(name,FFLAGS,dir_fd=pfd)
    finally:os.close(pfd)
    try:
        s=os.fstat(fd)
        require(stat.S_ISREG(s.st_mode) and s.st_nlink==1 and 0<s.st_size<=2*1024*1024,'Bounded regular source required')
        if trusted:metadata(fd,mode=0o644)
        raw=os.pread(fd,s.st_size,0);after=os.fstat(fd)
        require((s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns,s.st_ctime_ns)==
                (after.st_dev,after.st_ino,after.st_size,after.st_mtime_ns,after.st_ctime_ns),'Source changed during read')
        return raw
    finally:os.close(fd)


def absent(fd,name):
    try:os.stat(name,dir_fd=fd,follow_symlinks=False)
    except FileNotFoundError:return
    raise RuntimeError('Existing target; no reuse or replacement')


def noreplace(srcfd,src,dstfd,dst):
    libc=ctypes.CDLL(None,use_errno=True)
    function=getattr(libc,'renameat2',None)
    require(function is not None,'renameat2 required; no fallback')
    if function(srcfd,os.fsencode(src),dstfd,os.fsencode(dst),1):
        code=ctypes.get_errno();raise OSError(code,'Atomic no-replace rename failed')


@contextmanager
def critical():
    previous=signal.pthread_sigmask(signal.SIG_BLOCK,{signal.SIGINT,signal.SIGTERM,signal.SIGHUP})
    try:yield
    finally:signal.pthread_sigmask(signal.SIG_SETMASK,previous)


def writefile(parent,name,raw,mode=0o644):
    fd=os.open(name,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,mode,dir_fd=parent)
    try:
        os.fchown(fd,ROOT_UID,ROOT_GID);os.fchmod(fd,mode)
        view=memoryview(raw)
        while view:
            count=os.write(fd,view);require(count>0,'Short write');view=view[count:]
        metadata(fd,mode=mode);os.fsync(fd)
    finally:os.close(fd)
    os.fsync(parent)


def checktree(parent,name,payload):
    fd=os.open(name,DFLAGS,dir_fd=parent)
    try:
        s=metadata(fd,True,0o755)
        require(set(os.listdir(fd))==set(payload),'Incomplete/extra/mixed release tree')
        for member,raw in payload.items():
            child=os.open(member,FFLAGS,dir_fd=fd)
            try:
                info=metadata(child,mode=0o644)
                require(info.st_size==len(raw) and sha(os.pread(child,info.st_size,0))==sha(raw),'Mixed or changed member')
            finally:os.close(child)
        return s.st_dev,s.st_ino
    finally:os.close(fd)


class Transaction:
    def __init__(self,parent,release,payload,audit,protected,postflight):
        require(len(release)==64 and all(c in '0123456789abcdef' for c in release),'Exact release digest required')
        require(all(Path(name).name==name and name not in ('.','..') for name in payload),'Flat release members required')
        self.parent=parent;self.release=release;self.payload=payload;self.audit=audit
        self.protected=protected;self.postflight=postflight
        self.final='aur-manifest-diagnostics-'+release
        self.stage='.aur-manifest-diagnostics-'+release+'.pending'
        self.quarantine='.aur-manifest-diagnostics-'+release+'.quarantine'
        self.owned=None;self.published=False

    def rollback(self):
        require(self.published and self.owned is not None,'No owned publication to roll back')
        require(checktree(self.parent,self.final,self.payload)==self.owned,'Foreign publication; retain everything')
        absent(self.parent,self.quarantine)
        with critical():
            noreplace(self.parent,self.final,self.parent,self.quarantine)
            self.published=False;os.fsync(self.parent)
        self.audit('rollback-quarantined',release=self.release)

    def apply(self):
        for name in (self.final,self.stage,self.quarantine):absent(self.parent,name)
        self.protected();self.audit('preflight-pass',release=self.release)
        os.mkdir(self.stage,mode=0o700,dir_fd=self.parent)
        os.fsync(self.parent)
        fd=os.open(self.stage,DFLAGS,dir_fd=self.parent)
        try:
            metadata(fd,True,0o700)
            for member,raw in self.payload.items():writefile(fd,member,raw)
            os.fchmod(fd,0o755);os.fsync(fd)
            self.owned=checktree(self.parent,self.stage,self.payload)
        finally:os.close(fd)
        self.protected();self.audit('staging-complete',release=self.release,dev=self.owned[0],ino=self.owned[1])
        try:
            with critical():
                noreplace(self.parent,self.stage,self.parent,self.final)
                self.published=True;os.fsync(self.parent)
            self.audit('published',release=self.release,dev=self.owned[0],ino=self.owned[1])
            require(checktree(self.parent,self.final,self.payload)==self.owned,'Published inode drift')
            self.protected();self.postflight(self.final)
            self.audit('success',release=self.release,files_verified=len(self.payload),signing=False)
        except BaseException:
            if self.published:self.rollback()
            raise


def loadplan(raw):
    plan=json.loads(raw)
    require(set(plan)=={'schema','purpose','release_sha256','files','protected','protected_plan_sha256'} and
            type(plan['schema']) is int and plan['schema']==1 and
            plan['purpose']=='zethropol-manifest-diagnostics-rollout','Exact rollout plan required')
    require(len(plan['protected'])==13,'Protected thirteen required')
    require(plan['protected_plan_sha256']==PROTECTED_PLAN and
            {row['path'] for row in plan['protected']}==PROTECTED_PATHS,'Exact accepted protected baseline required')
    require(10<=len(plan['files'])<=24,'Bounded release list required')
    names=[];payload={}
    for row in plan['files']:
        require(set(row)=={'name','source','sha256','mode'} and row['mode']=='0644' and
                isinstance(row['name'],str) and Path(row['name']).name==row['name'] and
                row['name'] not in ('.','..') and row['name'] not in names,'Exact unique member required')
        names.append(row['name']);body=readpath(row['source']);require(sha(body)==row['sha256'],'Payload pin changed')
        if row['name'].endswith('.py'):compile(body,row['name'],'exec')
        payload[row['name']]=body
    require('bundle.json' in payload and sha(payload['bundle.json'])==plan['release_sha256'],'Bundle release pin mismatch')
    description=json.loads(payload['bundle.json'])
    require(description=={'schema':1,'protocol':PROTOCOL,'files':[
        {'name':name,'sha256':sha(body),'mode':'0644'} for name,body in sorted(payload.items()) if name!='bundle.json']} and
        payload['bundle.json']==canonical(description),'Payload and bundle disagree')
    return plan,payload


def protected_check(plan,expected=None):
    snapshot={}
    for row in plan['protected']:
        require(set(row)=={'path','sha256'},'Exact protected pin required')
        path=row['path']
        require(path=='/etc/zethropol-update/aur-manifest-signer.json' or
                (str(Path(path).parent)=='/usr/lib/zethropol/aur' and Path(path).suffix=='.py'),'Protected scope conflict')
        require(sha(readpath(path,True))==row['sha256'],'Protected signer baseline drift; no deployment')
        info=Path(path).lstat()
        snapshot[path]=(info.st_dev,info.st_ino,info.st_mode,info.st_uid,info.st_gid,info.st_size,info.st_mtime_ns,info.st_ctime_ns)
    require(expected is None or snapshot==expected,'Protected signer inode/metadata changed; preserve all')
    return snapshot


def main(plan_path,plan_sha,mode):
    require(mode in ('--check','--apply','--rollback'),'Explicit reviewed mode required')
    require(os.getuid()==os.geteuid()==0,'Root deployment only')
    require(sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode,'Isolated -I -S -B bootstrap required')
    raw=readpath(plan_path);require(sha(raw)==plan_sha,'Rollout plan pin changed')
    plan,payload=loadplan(raw);baseline=protected_check(plan)
    parent=walk('/usr/lib/zethropol');release=plan['release_sha256']
    transaction_name='aur-manifest-diagnostic-rollout-'+release
    try:
        if mode=='--check':
            for name in ('aur-manifest-diagnostics-'+release,'.aur-manifest-diagnostics-'+release+'.pending',
                         '.aur-manifest-diagnostics-'+release+'.quarantine'):absent(parent,name)
            vfd=walk('/var/lib/zethropol')
            try:absent(vfd,transaction_name)
            finally:os.close(vfd)
            print('PASS check files='+str(len(payload))+' protected=13 signing=false');return
        vfd=walk('/var/lib/zethropol');auditfd=None;txfd=None
        try:
            if mode=='--apply':
                absent(vfd,transaction_name);os.mkdir(transaction_name,mode=0o700,dir_fd=vfd);os.fsync(vfd)
                txfd=os.open(transaction_name,DFLAGS,dir_fd=vfd);metadata(txfd,True,0o700)
                writefile(txfd,'plan.json',raw,0o600)
                auditfd=os.open('audit.jsonl',os.O_RDWR|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o600,dir_fd=txfd)
                metadata(auditfd,mode=0o600);os.fsync(txfd)
            else:
                txfd=os.open(transaction_name,DFLAGS,dir_fd=vfd);metadata(txfd,True,0o700)
                auditfd=os.open('audit.jsonl',os.O_RDWR|os.O_APPEND|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=txfd)
                info=metadata(auditfd,mode=0o600);require(info.st_size<=1024*1024,'Bounded rollout audit required')
                history=[json.loads(line) for line in os.pread(auditfd,info.st_size,0).splitlines()]
                require(history and all(r.get('release')==release for r in history),'Audit release mismatch')
            def audit(event,**fields):
                body=canonical({'event':event,**fields})+b'\n';view=memoryview(body)
                while view:
                    n=os.write(auditfd,view);require(n>0,'Audit short write');view=view[n:]
                os.fsync(auditfd)
            def postflight(name):
                result=subprocess.run(['/usr/bin/python3','-I','-S','-B','/usr/lib/zethropol/'+name+'/manifest_bundle_launcher.py','--protocol-check'],
                    stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=15,
                    env={'PATH':'/usr/bin','LC_ALL':'C'})
                require(result.returncode==0 and result.stdout==('PASS protocol='+PROTOCOL+' signing=false\n').encode(),
                        'Public-only protocol postflight failed')
            tx=Transaction(parent,release,payload,audit,lambda:protected_check(plan,baseline),postflight)
            if mode=='--apply':tx.apply()
            else:
                publications=[r for r in history if r.get('event')=='published']
                require(len(publications)==1 and history[-1].get('event')=='success','No unambiguous completed publication; preserve for review')
                row=publications[0];tx.owned=(row['dev'],row['ino']);tx.published=True
                tx.rollback();protected_check(plan,baseline)
            print('PASS '+mode[2:]+' release='+release+' protected=13 signing=false')
        finally:
            if auditfd is not None:os.close(auditfd)
            if txfd is not None:os.close(txfd)
            os.close(vfd)
    finally:os.close(parent)
