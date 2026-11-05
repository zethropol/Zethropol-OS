"""Approval-bound manifest signing and public-only publication.

No bootstrap, key export, install or service activation. The sign action must
only be invoked after explicit owner permission to use the real private key.
Neither this program nor a candidate request grants that permission.
"""
import os
import json
from pathlib import Path
import pwd
import re
import stat
import subprocess
import sys
import time
from contextlib import ExitStack, contextmanager

if __name__ == '__main__':
    # Validate the import path before loading privileged publisher dependencies.
    for name in ('manifest_workflow','provider','pipeline','provision','worker','trust',
                 'sealed','quota','journal','scoped','root_limits','staged_root','rebuild_acceptance'):
        path=Path(__file__).resolve().parent/(name+'.py')
        for item in (path,*path.parents):
            info=item.lstat()
            if info.st_uid!=0 or info.st_mode&0o022 or stat.S_ISLNK(info.st_mode):
                raise SystemExit('Root-owned immutable manifest implementation required')
    sys.path.insert(0,str(Path(__file__).resolve().parent))

from worker import Rejected, canonical
from sealed import Blob
from trust import TrustPolicy, SignedArchive, approved_manifest
from provider import document, root_path, write, sync

CONFIG=Path('/etc/zethropol-update/aur-manifest-signer.json')
APPROVALS=Path('/var/lib/zethropol/aur-manifest-approvals')
PENDING=Path('/var/lib/zethropol-aur-manifests/pending')
PUBLIC=Path('/var/lib/zethropol/aur-provider-inputs')
POLICY='/etc/zethropol-update/aur-trust/policy.json'
SIGNER_USER='zethropol-aur-manifest'
SIGNER_HOME='/var/lib/zethropol-aur-manifests'
SIGNER_GPG_HOME=SIGNER_HOME+'/gnupg'
OWNER_USER='zethropol-aur-release'
OWNER_UID=949
OWNER_HOME='/var/lib/zethropol-aur-release'
OWNER_GPG_HOME=OWNER_HOME+'/gnupg'

SIGNING_PHASES=frozenset(('authorization-context','policy-load','manifest-authorization',
    'signature-buffer','gpg-signing','signature-verification','authorization-recheck','signature-write'))
DIAGNOSTIC_PROTOCOL='zethropol-manifest-diagnostics/1'


class SigningPhaseRejected(Rejected):
    """Fixed public diagnostic only; never expose private exception text."""
    def __init__(self,phase,returncode=None,kind='rejected',os_errno=None):
        if phase not in SIGNING_PHASES:
            raise ValueError('Unknown signing phase')
        self.phase=phase
        self.returncode=returncode if type(returncode) is int and -255<=returncode<=255 else None
        self.kind=kind if kind in ('rejected','permission-denied','os-error','invalid-input',
                                  'timeout','subprocess-error','backend-nonzero') else 'rejected'
        self.os_errno=os_errno if type(os_errno) is int and 1<=os_errno<=4095 else None
        super().__init__('Manifest signing rejected at '+phase)


@contextmanager
def signing_phase(phase):
    if phase not in SIGNING_PHASES:
        raise ValueError('Unknown signing phase')
    try:
        yield
    except SigningPhaseRejected:
        raise
    except (OSError,ValueError,TypeError,KeyError,RuntimeError,subprocess.SubprocessError) as error:
        if isinstance(error,PermissionError):kind='permission-denied'
        elif isinstance(error,OSError):kind='os-error'
        elif isinstance(error,subprocess.TimeoutExpired):kind='timeout'
        elif isinstance(error,subprocess.SubprocessError):kind='subprocess-error'
        elif isinstance(error,(ValueError,TypeError,KeyError)):kind='invalid-input'
        else:kind='rejected'
        raise SigningPhaseRejected(phase,kind=kind,os_errno=error.errno if isinstance(error,OSError) else None) from None


def failure_message(error):
    if isinstance(error,SigningPhaseRejected):
        suffix='' if error.returncode is None else ' returncode='+str(error.returncode)
        return 'ZETHROPOL_MANIFEST_FAILURE phase='+error.phase+suffix
    return 'Manifest workflow rejected; no signing/publication authority inferred'


def failure_record(error,manifest_sha256):
    """One bounded JSON record, no paths, argv, exception text or raw GPG output."""
    if not isinstance(manifest_sha256,str) or not re.fullmatch('[0-9a-f]{64}',manifest_sha256):
        manifest_sha256=None
    known=isinstance(error,SigningPhaseRejected)
    record={'schema':1,'protocol':DIAGNOSTIC_PROTOCOL,'purpose':'zethropol-manifest-signing-failure','status':'BLOCKED',
            'manifest_sha256':manifest_sha256,'phase':error.phase if known else 'unknown',
            'kind':error.kind if known else 'rejected','os_errno':error.os_errno if known else None,
            'backend_returncode':error.returncode if known else None}
    return 'ZETHROPOL_MANIFEST_FAILURE_JSON '+json.dumps(record,sort_keys=True,separators=(',',':'))


def dedicated_identity(config,account,other_uids):
    if (config['user']!=SIGNER_USER or config['gnupg_home']!=SIGNER_GPG_HOME or
            account.pw_uid==0 or account.pw_dir!=SIGNER_HOME or
            account.pw_shell not in ('/usr/bin/nologin','/bin/nologin') or
            account.pw_uid in other_uids):
        raise Rejected('Separate locked-down Manifest signer identity required')


def signer_model(config):
    """No implicit migration or caller-selected identity/home. Schema 1 stays legacy."""
    base={'schema','purpose','user','gnupg_home','signer'}
    if not isinstance(config,dict) or type(config.get('schema')) is not int:
        raise Rejected('Explicit versioned Manifest signer configuration required')
    if config.get('purpose')!='zethropol-manifest-signer':
        raise Rejected('Wrong Manifest signer purpose')
    if config['schema']==1 and set(config)==base:
        if config['user']!=SIGNER_USER or config['gnupg_home']!=SIGNER_GPG_HOME:
            raise Rejected('Legacy dedicated account/home required; no implicit owner migration')
        return SIGNER_USER,None,SIGNER_HOME,SIGNER_GPG_HOME
    if (config['schema']==2 and set(config)==base|{'identity_model','uid'} and
            config['identity_model']=='existing-key-owner' and
            config['user']==OWNER_USER and type(config['uid']) is int and config['uid']==OWNER_UID and
            config['gnupg_home']==OWNER_GPG_HOME):
        return OWNER_USER,OWNER_UID,OWNER_HOME,OWNER_GPG_HOME
    raise Rejected('Exact approved owner identity model required')


def owner_identity(config,account,other_uids):
    user,uid,home,_=signer_model(config)
    if uid is None:
        dedicated_identity(config,account,other_uids)
    elif (account.pw_uid!=uid or account.pw_dir!=home or
          account.pw_shell not in ('/usr/bin/nologin','/bin/nologin') or account.pw_uid in other_uids):
        raise Rejected('Pinned existing Manifest owner identity changed')


def private_home(home,uid):
    # Publisher must never call this: only the signing actor inspects key-home metadata.
    for item in (home,*home.parents):
        info=item.lstat()
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid not in (0,uid) or info.st_mode&0o022):
            raise Rejected('Unsafe GPG home/ancestor ownership or mode')
    info=home.lstat()
    if info.st_uid!=uid or info.st_mode&0o077:
        raise Rejected('Existing private signer-owned GPG home required')


def approval_valid(approval, sha256, now):
    if (not isinstance(approval,dict) or set(approval)!={'schema','purpose','manifest_sha256','expires'} or
            type(approval['schema']) is not int or approval['schema']!=1 or
            approval['purpose']!='zethropol-approved-root-manifest' or
            approval['manifest_sha256']!=sha256 or type(approval['expires']) is not int or
            not now<approval['expires']<=now+86400):
        raise Rejected('Exact fresh owner-approved manifest digest required')


def context(sha256, actor):
    if not isinstance(sha256,str) or not re.fullmatch('[0-9a-f]{64}',sha256):
        raise Rejected('Exact manifest SHA256 required')
    approval=document(APPROVALS/(sha256+'.json'))
    approval_valid(approval,sha256,int(time.time()))
    config=document(CONFIG)
    user,_,_,gpg_home=signer_model(config)
    account=pwd.getpwnam(user)
    other_uids={pwd.getpwnam(user).pw_uid for user in ('zethropol-aur-archive','zethropol-aur-attestor')}
    owner_identity(config,account,other_uids)
    if account.pw_uid==0 or (actor and (os.getuid()!=account.pw_uid or os.geteuid()!=account.pw_uid)):
        raise Rejected('Manifest signing requires the configured unprivileged owner')
    directory=PENDING/sha256
    for path in (directory,PENDING):
        info=path.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid!=account.pw_uid or info.st_mode&0o077:
            raise Rejected('Private signer-owned pending directory required')
    for item in directory.parents:
        if item!=PENDING: root_path(item,directory=True)
    if actor: private_home(Path(gpg_home),account.pw_uid)
    return config,directory,approval


def manifest_blob(path,stack):
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC)
    try: return stack.enter_context(Blob(fd,1024**2))
    finally: os.close(fd)


def validate_bytes(blob,sha256):
    if blob.sha256!=sha256: raise Rejected('Owner-approved manifest bytes changed')
    from types import SimpleNamespace
    plan,_=approved_manifest(SimpleNamespace(archive=blob)) # schema only, not signature evidence
    if os.pread(blob.fd,blob.size,0)!=canonical(plan):
        raise Rejected('Canonical owner-reviewed manifest bytes required')
    return plan


def validate_authorization(blob,sha256,approval):
    approval_valid(approval,sha256,int(time.time()))
    plan=validate_bytes(blob,sha256)
    if approval['expires']>plan['expires']:
        raise Rejected('Owner approval may not outlive its exact manifest')
    return plan


def sign(sha256):
    with signing_phase('authorization-context'):
        config,directory,approval=context(sha256,actor=True)
    with ExitStack() as stack:
        with signing_phase('policy-load'):
            policy=TrustPolicy.load(POLICY,role='manifest');stack.callback(policy.keyring.close)
            if config['signer'] not in policy.signers: raise Rejected('Signer is not the approved Manifest primary')
        with signing_phase('manifest-authorization'):
            blob=manifest_blob(directory/'manifest.json',stack);validate_authorization(blob,sha256,approval)
        with signing_phase('signature-buffer'):
            target=os.memfd_create('manifest-signature',os.MFD_CLOEXEC|os.MFD_ALLOW_SEALING)
        try:
            with signing_phase('gpg-signing'):
                result=subprocess.run(['/usr/bin/gpg','--no-options','--homedir',config['gnupg_home'],
                    '--batch','--yes','--no-tty','--no-autostart','--no-auto-key-retrieve','--no-auto-key-import',
                    '--pinentry-mode','error','--local-user',config['signer'],'--output',f'/proc/self/fd/{target}',
                    '--detach-sign','--',f'/proc/self/fd/{blob.fd}'],pass_fds=(blob.fd,target),
                    env={'PATH':'/usr/bin','LC_ALL':'C'},stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,timeout=60)
                if result.returncode: raise SigningPhaseRejected('gpg-signing',result.returncode,kind='backend-nonzero')
            with signing_phase('signature-verification'):
                verified=stack.enter_context(SignedArchive(blob.fd,target,policy,archive_limit=1024**2))
                if verified.signer!=config['signer']: raise Rejected('Unexpected manifest primary')
            with signing_phase('authorization-recheck'):
                validate_authorization(blob,sha256,approval)
            with signing_phase('signature-write'):
                write(directory/'manifest.json.sig',os.pread(verified.signature.fd,verified.signature.size,0))
                sync(directory)
        finally: os.close(target)


def publish(sha256):
    if os.getuid()!=0 or os.geteuid()!=0: raise Rejected('Root public manifest publisher required')
    config,directory,approval=context(sha256,actor=False)
    with ExitStack() as stack:
        policy=TrustPolicy.load(POLICY,role='manifest');stack.callback(policy.keyring.close)
        if config['signer'] not in policy.signers: raise Rejected('Unapproved Manifest primary')
        blob=manifest_blob(directory/'manifest.json',stack)
        sig=manifest_blob(directory/'manifest.json.sig',stack)
        verified=stack.enter_context(SignedArchive(blob.fd,sig.fd,policy,archive_limit=1024**2))
        validate_authorization(verified.archive,sha256,approval)
        if verified.signer!=config['signer']: raise Rejected('Unexpected manifest primary')
        approval_valid(approval,sha256,int(time.time()))
        root_path(PUBLIC,directory=True)
        pending=PUBLIC/('manifest-'+sha256+'.pending');pending.mkdir(mode=0o700)
        write(pending/'manifest.json',os.pread(verified.archive.fd,verified.archive.size,0),0o444)
        write(pending/'manifest.json.sig',os.pread(verified.signature.fd,verified.signature.size,0),0o444)
        sync(pending);pending.chmod(0o555)
        validate_authorization(verified.archive,sha256,approval)
        destination=PUBLIC/('manifest-'+sha256)
        if destination.exists(): raise Rejected('Manifest publication already exists; never replace')
        pending.rename(destination);sync(PUBLIC)


def main():
    if len(sys.argv)!=3 or sys.argv[1] not in ('sign','publish'):
        raise Rejected('Explicit sign/publish action and approved manifest SHA256 required')
    {'sign':sign,'publish':publish}[sys.argv[1]](sys.argv[2])


if __name__=='__main__':
    try: main()
    except (OSError,ValueError,TypeError,KeyError,RuntimeError,subprocess.SubprocessError) as error:
        raise SystemExit(failure_record(error,sys.argv[2] if len(sys.argv)==3 else None))
