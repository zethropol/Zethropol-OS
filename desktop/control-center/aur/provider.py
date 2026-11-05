"""Existing AUR provider workflow. No key generation, trust bootstrap or installer.

Root only provisions signed files/publishes verified handoffs. Separate unprivileged
accounts sign developer output and independently rebuild/attest it.
"""
import json
import fcntl
import os
from pathlib import Path
import pwd
import grp
import signal
import stat
import subprocess
import sys
import time
import uuid
from contextlib import ExitStack

CONFIG = Path('/etc/zethropol-update/aur-providers.json')
DELIVERIES = Path('/var/lib/zethropol/aur-deliveries')
JOBS = Path('/var/lib/zethropol/aur-provider-jobs')
REVIEWS = Path('/var/lib/zethropol/aur-approved-reviews')
USERS = {'archive':'zethropol-aur-archive','receipt':'zethropol-aur-attestor'}
HOMES = {'archive': Path('/var/lib/zethropol-aur-archive'), 'receipt': Path('/var/lib/zethropol-aur-attestor')}


def root_path(path, directory=False):
    path = Path(path)
    for item in [path, *path.parents]:
        info = item.lstat()
        if info.st_uid != 0 or info.st_mode & 0o022 or stat.S_ISLNK(info.st_mode):
            raise RuntimeError('Root-owned immutable provider input required')
    if directory and not path.is_dir(): raise RuntimeError('Provider directory required')
    return path


# Isolated Python must not execute caller-owned modules as a privileged provider.
if __name__ == '__main__':
    for name in ('provider','pipeline','provision','worker','trust','sealed','quota','journal','scoped','root_limits','staged_root','rebuild_acceptance'):
        root_path(Path(__file__).resolve().parent / (name+'.py'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from worker import Rejected, canonical, digest, tree, matches_root
from trust import TrustPolicy, SignedArchive
from root_limits import root_limits
from sealed import Blob, SEALS
from rebuild_acceptance import RuntimeBudget, CaptureRejected, ROLE_UIDS


def unique(pairs):
    result={}
    for key,value in pairs:
        if key in result: raise Rejected('Duplicate provider JSON field')
        result[key]=value
    return result


def read(path, limit=1024**2, trusted=True):
    if trusted: root_path(path)
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC)
    try:
        with Blob(fd,limit) as blob: return os.pread(blob.fd,blob.size,0)
    finally: os.close(fd)


def document(path): return json.loads(read(path),object_pairs_hook=unique)


def identifier(value):
    if str(uuid.UUID(value))!=value: raise Rejected('Canonical operation UUID required')
    return value


def public_roles(stack):
    policies={}
    for role in ('manifest','archive','receipt'):
        policy=TrustPolicy.load('/etc/zethropol-update/aur-trust/policy.json',role=role)
        stack.callback(policy.keyring.close);policies[role]=policy
    if policies['receipt'].signers & (policies['archive'].signers|policies['manifest'].signers):
        raise Rejected('Independent receipt keys must differ from developer/manifest keys')
    return policies


def configuration():
    config=document(CONFIG)
    if set(config)!={'schema','purpose','archive','receipt'} or config['schema']!=1 or config['purpose']!='zethropol-aur-providers':
        raise Rejected('Exact production provider configuration required')
    accounts={}
    for role in ('archive','receipt'):
        item=config[role]
        if set(item)!={'user','signer'} or not isinstance(item['signer'],str) or not item['signer']:
            raise Rejected('Real public signing fingerprint and provider account required')
        if item['user']!=USERS[role]: raise Rejected('Provider account must match the packaged role unit')
        account=pwd.getpwnam(item['user']);accounts[role]=account.pw_uid
        if account.pw_uid==0: raise Rejected('Signing/build providers must never be root')
    if accounts['archive']==accounts['receipt']: raise Rejected('Independent attestor requires a separate operating account')
    if any(accounts[role] != ROLE_UIDS[role] for role in ROLE_UIDS):
        raise Rejected('Production provider accounts must use Archive UID 951 and Attestor UID 950')
    return config,accounts


def signed(stack,path,signature,policy,trusted=True,archive_limit=256*1024**2):
    # Snapshot both inputs before verification; a later writer cannot swap bytes.
    if trusted: root_path(path);root_path(signature)
    archive=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC)
    try:
        sig=os.open(signature,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC)
        try: return stack.enter_context(SignedArchive(archive,sig,policy,archive_limit=archive_limit))
        finally: os.close(sig)
    finally: os.close(archive)


def write(path,data,mode=0o600,gid=None):
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,mode)
    try:
        os.fchmod(fd,mode)
        if gid is not None: os.fchown(fd,0,gid)
        with os.fdopen(os.dup(fd),'wb') as stream: stream.write(data);stream.flush();os.fsync(stream.fileno())
    finally: os.close(fd)


def sync(path):
    fd=os.open(path,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
    try: os.fsync(fd)
    finally: os.close(fd)


def job(operation):
    value=document(JOBS/identifier(operation)/'job.json')
    if value.get('operation_id')!=operation or value.get('purpose')!='zethropol-untrusted-attestation-intake' or type(value.get('schema')) is not int or value['schema']!=2:
        raise Rejected('Original caller-bound broker job required')
    RuntimeBudget(value['budget']).checkpoint()
    review=value['review'];request=value['request']
    if digest(canonical(request['plan']))!=review['source_plan_sha256'] or request['plan_sha256']!=review['source_plan_sha256']:
        raise Rejected('Independent approved source plan changed')
    expected={p['filename'] for p in review['outputs']}
    if len(expected)!=len(review['outputs']) or (len(value['archives'])!=len(expected) or expected!={p['filename'] for p in value['archives']}):
        raise Rejected('Complete unique reviewed output set required')
    return value


def actor(role):
    config,accounts=configuration()
    if os.getuid()!=accounts[role] or os.geteuid()!=accounts[role]: raise Rejected('Wrong provider operating account')
    home=HOMES[role]
    for directory in (home,home/'work',home/'gnupg'):
        info=directory.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid!=os.getuid() or stat.S_IMODE(info.st_mode)!=0o700:
            raise Rejected('Private provider work and software-key home required')
    return config[role]['signer'],home


def sign(data,role,stack,policies,timeout=60):
    fingerprint,home=actor(role)
    if fingerprint not in policies[role].signers: raise Rejected('Signing identity is not pinned for this role')
    input_fd=os.memfd_create('aur-sign-input',os.MFD_CLOEXEC|os.MFD_ALLOW_SEALING)
    output_fd=os.memfd_create('aur-sign-output',os.MFD_CLOEXEC|os.MFD_ALLOW_SEALING)
    try:
        with os.fdopen(os.dup(input_fd),'wb') as stream: stream.write(data);stream.flush()
        os.lseek(input_fd,0,0)
        fcntl.fcntl(input_fd,fcntl.F_ADD_SEALS,SEALS)
        # Private material is accessed only by gpg-agent in the account's own
        # key home. No passphrase/key command argument, export or logging.
        completed=subprocess.run(['/usr/bin/gpg','--no-options','--homedir',str(home/'gnupg'),'--batch','--yes','--no-tty',
            '--no-autostart','--no-auto-key-retrieve','--no-auto-key-import','--pinentry-mode','error',
            '--local-user',fingerprint,'--output',f'/proc/self/fd/{output_fd}','--detach-sign',f'/proc/self/fd/{input_fd}'],
            pass_fds=(input_fd,output_fd),stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
            env={'PATH':'/usr/bin','LC_ALL':'C'},timeout=min(60,timeout))
        if completed.returncode: raise Rejected('Provider signing failed; unlock the approved software key through its agent')
        size=os.fstat(output_fd).st_size
        if not 0<size<=1024**2: raise Rejected('Bounded detached signature required')
        # Independently verify the actual produced signature and approved primary.
        verified=stack.enter_context(SignedArchive(input_fd,output_fd,policies[role]))
        if verified.archive.sha256!=digest(data) or verified.signer!=fingerprint:
            raise Rejected('Provider signature differs from exact input/configured primary')
        return os.pread(verified.signature.fd,verified.signature.size,0)
    finally: os.close(input_fd);os.close(output_fd)


def public_input(path):
    path=Path(path)
    base=Path('/var/lib/zethropol/aur-provider-inputs')
    if not path.is_relative_to(base): raise Rejected('Signed provider inputs must use the fixed public input directory')
    root_path(path)
    if not path.stat().st_mode&0o004 or any(not p.stat().st_mode&0o001 for p in path.parents):
        raise Rejected('Signed public inputs must be readable by isolated build accounts')
    return path


def provision(review_hash):
    if os.getuid()!=0 or os.geteuid()!=0: raise Rejected('Root provisioning publisher required')
    if len(review_hash)!=64 or any(c not in '0123456789abcdef' for c in review_hash): raise Rejected('Reviewed digest required')
    request=document(REVIEWS/(review_hash+'.build.json'))
    if digest(canonical(request['plan']))!=request['plan_sha256']: raise Rejected('Provisioning plan changed')
    public_input(request['manifest']);public_input(request['manifest_signature'])
    if not isinstance(request['packages'], list) or not 1 <= len(request['packages']) <= 256:
        raise Rejected('Bounded signed root package set required')
    for package in request['packages']: public_input(package['archive']);public_input(package['signature'])
    with ExitStack() as stack:
        public_roles(stack);configuration()
        manifest_policy=TrustPolicy.load('/etc/zethropol-update/aur-trust/policy.json',role='manifest');stack.callback(manifest_policy.keyring.close)
        distribution=TrustPolicy.load('/etc/zethropol-update/aur-trust/distribution.json');stack.callback(distribution.keyring.close)
        manifest=signed(stack,request['manifest'],request['manifest_signature'],manifest_policy)
        from trust import approved_manifest
        root_plan,root_id=approved_manifest(manifest)
        if len(request['packages']) != len(root_plan['packages']):
            raise Rejected('Signed root package count mismatch')
        _, archive_limit, _, _ = root_limits(root_plan['schema'])
        packages=[]; total=0
        for item in request['packages']:
            verified=signed(stack,item['archive'],item['signature'],distribution,
                            archive_limit=archive_limit-total)
            total+=verified.archive.size;packages.append(verified)
        root=Path('/var/lib/zethropol/aur-build-roots')/manifest.archive.sha256
        if str(root)!=request['build_root']: raise Rejected('Fixed signed build root required')
        root_path(root.parent,directory=True)
        if not root.exists():
            from provision import provision as extract
            extract(root,manifest,packages)
        expected=[{'name':p['name'],'sha256':archive.archive.sha256,'signer':archive.signer} for p,archive in zip(root_plan['packages'],packages)]
        if len(packages)!=len(root_plan['packages']) or any(p['sha256']!=archive.archive.sha256 for p,archive in zip(root_plan['packages'],packages)):
            raise Rejected('Distribution closure mismatch')
        if document(root/'.provenance.json')!={'manifest_sha256':root_id,'scope':'production-policy-only','packages':expected} or not matches_root(request['plan'], tree(root,True)):
            raise Rejected('Immutable root/provenance differs from approved plan')


def prepare(operation):
    if os.geteuid()!=0: raise Rejected('Root job publisher required')
    configuration()
    source=DELIVERIES/identifier(operation)
    ready=document(source/'untrusted-build/ready.json');request=document(source/'build.json')
    if ready.get('operation_id')!=operation: raise Rejected('Intake UUID changed')
    if type(ready.get('schema')) is not int or ready['schema']!=2: raise Rejected('Deadline-bearing broker intake required')
    budget=RuntimeBudget(ready['budget'])
    gid=grp.getgrnam('zethropol-aur-providers').gr_gid
    root_path(JOBS,directory=True)
    directory=JOBS/operation;directory.mkdir(mode=0o750);directory.chmod(0o750);os.chown(directory,0,gid);sync(JOBS)
    aggregate=0
    for archive in ready['archives']:
        name=archive['filename']
        if Path(name).name!=name or name in ('.','..'): raise Rejected('Invalid reviewed filename')
        data=read(source/'untrusted-build'/name,256*1024**2)
        aggregate+=len(data)
        if aggregate>256*1024**2: raise Rejected('Provider output quota exceeded')
        if digest(data)!=archive['sha256']: raise Rejected('Original untrusted intake changed')
        write(directory/name,data,0o640,gid)
    # Copy only the already hash-bound recipe; never execute it as root.
    recipe=Path(request['recipe']);expected=request['plan']['recipe']
    if tree(recipe)!=expected: raise Rejected('Reviewed recipe changed before provider delivery')
    copied=directory/'recipe';copied.mkdir(mode=0o750);os.chown(copied,0,gid)
    for name,entry in expected.items():
        if name=='.': continue
        if Path(name).is_absolute() or '..' in Path(name).parts: raise Rejected('Recipe path escape')
        destination=copied/name
        if entry[0]=='dir': destination.mkdir(mode=0o750,parents=True,exist_ok=True);os.chown(destination,0,gid)
        elif entry[0]=='file':
            data=read(recipe/name,256*1024**2,trusted=False)
            if digest(data)!=entry[2]: raise Rejected('Recipe copy changed')
            write(destination,data,entry[1]&0o755,gid)
        else: raise Rejected('Unreviewed recipe entry')
    # Transport copies are group-readable. The independent account restores
    # exact approved modes in its private workspace before worker revalidation.
    for name,entry in expected.items():
        if entry[1]&0o022: raise Rejected('Approved recipe is writable by others')
        (copied/name).chmod(0o750 if entry[0]=='dir' else ((entry[1]|0o440)&~0o022))
    request['recipe']=str(copied)
    ready['request']=request
    write(directory/'job.json',canonical(ready),0o640,gid);sync(directory)
    budget.checkpoint()
    job(operation)


def sign_build(operation):
    actor('archive');value=job(operation)
    budget=RuntimeBudget(value['budget'])
    with ExitStack() as stack:
        policies=public_roles(stack)
        directory,_,cancelled=measured_build(value,'archive',operation)
        for item in value['archives']:
            budget.checkpoint()
            data=read(JOBS/operation/item['filename'],256*1024**2)
            if digest(data)!=item['sha256']: raise Rejected('Developer intake hash changed')
            signature=sign(data,'archive',stack,policies,timeout=budget.timeout_seconds())
            budget.checkpoint()
            if cancelled[0]: raise Rejected('Cancelled; no developer signature publication')
            write(directory/(item['filename']+'.sig'),signature)
        sync(directory)


def attach(operation):
    if os.geteuid()!=0: raise Rejected('Root signature publisher required')
    value=job(operation);gid=grp.getgrnam('zethropol-aur-providers').gr_gid
    budget=RuntimeBudget(value['budget'])
    with ExitStack() as stack:
        policies=public_roles(stack)
        for item in value['archives']:
            budget.checkpoint()
            signature=HOMES['archive']/'work'/operation/(item['filename']+'.sig')
            verified=signed(stack,JOBS/operation/item['filename'],signature,policies['archive'],trusted=False)
            if verified.archive.sha256!=item['sha256']: raise Rejected('Developer output identity changed')
            write(JOBS/operation/(item['filename']+'.sig'),os.pread(verified.signature.fd,verified.signature.size,0),0o640,gid)
        sync(JOBS/operation)


def compare_rebuild(value,result):
    if result.get('state')!='built-untrusted' or result.get('cleanup')!='verified': raise Rejected('Independent isolated rebuild or cleanup failed')
    expected={p['filename']:p['sha256'] for p in value['archives']}
    observed={Path(p['path']).name:p['sha256'] for p in result['archives']}
    if len(observed)!=len(result['archives']) or expected!=observed: raise Rejected('Independent build differs; developer signature alone cannot authorize receipt')


def measured_build(value,role,operation):
    _,home=actor(role)
    if role=='receipt' and value.get('uid')==os.getuid():
        raise Rejected('Original build owner cannot be its independent attestor')
    budget=RuntimeBudget(value['budget'])
    directory=home/'work'/operation;directory.mkdir(mode=0o700)
    store=directory/'journal';store.mkdir(mode=0o700)
    request=dict(value['request']);request['store']=str(store)
    epoch=request['plan']['identity'].get('source_date_epoch')
    if type(epoch) is not int or not 0<epoch<=int(time.time()): raise Rejected('Approved deterministic source epoch required for independent rebuild')
    recipe=directory/'recipe';recipe.mkdir(mode=0o700)
    expected=request['plan']['recipe']
    for name,entry in expected.items():
        if name=='.': continue
        if Path(name).is_absolute() or '..' in Path(name).parts: raise Rejected('Recipe path escape')
        target=recipe/name
        if entry[0]=='dir': target.mkdir(mode=0o700,parents=True,exist_ok=True)
        elif entry[0]=='file':
            data=read(Path(request['recipe'])/name,256*1024**2)
            if digest(data)!=entry[2]: raise Rejected('Independent recipe transport changed')
            write(target,data)
        else: raise Rejected('Unsupported independent recipe entry')
    for name,entry in expected.items(): (recipe/name).chmod(entry[1])
    request['recipe']=str(recipe)

    cancelled=[False]
    for sig in (signal.SIGTERM,signal.SIGINT): signal.signal(sig,lambda *_:cancelled.__setitem__(0,True))
    from pipeline import run
    def cancel_or_expire():
        try: budget.checkpoint()
        except CaptureRejected as error: raise Rejected(str(error)) from error
        return cancelled[0]
    result=run(request,cancel=cancel_or_expire,require_metadata=True,timeout=budget.timeout_seconds())
    budget.checkpoint()
    compare_rebuild(value,result)
    if cancelled[0]: raise Rejected('Cancelled; no signature')
    metadata={Path(p['path']).name:p['metadata'] for p in result['archives']}
    for output in value['review']['outputs']:
        info=metadata[output['filename']]
        if info['pkgname']!=output['name'] or info['pkgver']!=output['version'] or info['pkgbase']!=value['review']['source_package'] or info['arch'] not in (value['review']['architecture'],'any'):
            raise Rejected('Measured package identity mismatch')
    return directory,result,cancelled


def validate_receipt(receipt,value,observed,now):
    """Publisher admission only; does not replace native broker verification.

    observed is measured from sealed, signature-verified archives/signatures.
    Signing a JSON assertion alone never supplies these observations.
    """
    fields={'schema','purpose','operation_id','uid','bus_name','session_id','source_package',
            'source_version','architecture','build_root_sha256','source_plan_sha256',
            'issued_at','expires_at','packages'}
    if (not isinstance(receipt,dict) or set(receipt)!=fields or
            type(receipt['schema']) is not int or receipt['schema']!=1 or
            receipt['purpose']!='zethropol-aur-build-receipt'):
        raise Rejected('Exact receipt schema/purpose required')
    for key in ('operation_id','uid','bus_name','session_id'):
        if receipt[key]!=value[key]: raise Rejected('Receipt owner/context changed')
    if type(receipt['uid']) is not int or receipt['uid']<1: raise Rejected('Real receipt owner required')
    for key in ('source_package','source_version','architecture','build_root_sha256','source_plan_sha256'):
        if receipt[key]!=value['review'][key]: raise Rejected('Receipt reviewed context changed')
    issued=receipt['issued_at'];expires=receipt['expires_at']
    if (type(issued) is not int or type(expires) is not int or
            not 0<issued<=now<expires or not 0<expires-issued<=300):
        raise Rejected('Invalid independent receipt lifetime')
    outputs=value['review']['outputs'];packages=receipt['packages']
    if (not isinstance(packages,list) or not 1<=len(packages)<=64 or len(packages)!=len(outputs) or
            len(observed)!=len(outputs) or
            [p['name'] for p in outputs]!=sorted(set(p['name'] for p in outputs)) or
            len({p['filename'] for p in outputs})!=len(outputs)):
        raise Rejected('Exact sorted complete receipt outputs required')
    for package,output in zip(packages,outputs):
        if not isinstance(package,dict) or set(package)!={'name','version','architecture','filename','sha256','signature_sha256'}:
            raise Rejected('Exact receipt package fields required')
        for key in ('name','version','filename'):
            if package[key]!=output[key]: raise Rejected('Receipt package identity changed')
        if package['architecture'] not in (value['review']['architecture'],'any'):
            raise Rejected('Receipt package architecture changed')
        actual=observed.get(package['filename'])
        if actual is None or (package['sha256'],package['signature_sha256'])!=actual:
            raise Rejected('Receipt package/signature bytes changed')


def attest(operation):
    actor('receipt');value=job(operation)
    budget=RuntimeBudget(value['budget'])
    with ExitStack() as stack:
        policies=public_roles(stack)
        signatures={}
        for item in value['archives']:
            archive=signed(stack,JOBS/operation/item['filename'],JOBS/operation/(item['filename']+'.sig'),policies['archive'])
            if archive.archive.sha256!=item['sha256']: raise Rejected('Signed developer output changed')
            signatures[item['filename']]=archive.signature.sha256
        directory,result,cancelled=measured_build(value,'receipt',operation)
        review=value['review'];now=int(time.time())
        receipt={'schema':1,'purpose':'zethropol-aur-build-receipt','operation_id':operation,'uid':value['uid'],'bus_name':value['bus_name'],
            'session_id':value['session_id'],'source_package':review['source_package'],'source_version':review['source_version'],
            'architecture':review['architecture'],'build_root_sha256':review['build_root_sha256'],'source_plan_sha256':review['source_plan_sha256'],
            'issued_at':now,'expires_at':now+240,'packages':[]}
        hashes={p['filename']:p['sha256'] for p in value['archives']}
        metadata={Path(p['path']).name:p['metadata'] for p in result['archives']}
        for output in review['outputs']:
            info=metadata[output['filename']]
            if info['pkgname']!=output['name'] or info['pkgver']!=output['version'] or info['pkgbase']!=review['source_package'] or info['arch'] not in (review['architecture'],'any'):
                raise Rejected('Independently measured package identity mismatch')
            receipt['packages'].append({'name':output['name'],'version':output['version'],'architecture':info['arch'],
                'filename':output['filename'],'sha256':hashes[output['filename']],'signature_sha256':signatures[output['filename']]})
        budget.checkpoint()
        data=canonical(receipt);signature=sign(data,'receipt',stack,policies,timeout=budget.timeout_seconds())
        budget.checkpoint()
        if cancelled[0]: raise Rejected('Cancelled; no receipt publication')
        write(directory/'receipt.json',data);write(directory/'receipt.json.sig',signature);sync(directory)


def publish(operation):
    if os.geteuid()!=0: raise Rejected('Root attestation publisher required')
    value=job(operation);directory=DELIVERIES/operation
    budget=RuntimeBudget(value['budget'])
    with ExitStack() as stack:
        policies=public_roles(stack)
        signed_receipt=signed(stack,HOMES['receipt']/'work'/operation/'receipt.json',HOMES['receipt']/'work'/operation/'receipt.json.sig',policies['receipt'],trusted=False,archive_limit=1024**2)
        data=os.pread(signed_receipt.archive.fd,signed_receipt.archive.size,0);receipt=json.loads(data,object_pairs_hook=unique)
        verified_archives=[];observed={};total=0
        for item in value['archives']:
            archive=signed(stack,JOBS/operation/item['filename'],JOBS/operation/(item['filename']+'.sig'),policies['archive'])
            if archive.archive.sha256!=item['sha256']: raise Rejected('Archive changed before publish')
            total+=archive.archive.size
            if total>256*1024**2 or item['filename'] in observed: raise Rejected('Bounded unique publisher outputs required')
            observed[item['filename']]=(archive.archive.sha256,archive.signature.sha256)
            verified_archives.append((item,archive))
        validate_receipt(receipt,value,observed,int(time.time()))
        budget.checkpoint()
        manifest=signed(stack,value['request']['manifest'],value['request']['manifest_signature'],policies['manifest'],archive_limit=1024**2)
        from trust import approved_manifest
        approved_manifest(manifest)
        if manifest.archive.sha256!=value['review']['build_root_sha256']: raise Rejected('Manifest changed before publish')
        pending=directory/'attestation.pending';pending.mkdir(mode=0o700)
        write(pending/'receipt.json',data);write(pending/'receipt.json.sig',os.pread(signed_receipt.signature.fd,signed_receipt.signature.size,0))
        for item,archive in verified_archives:
            write(pending/item['filename'],os.pread(archive.archive.fd,archive.archive.size,0))
            write(pending/(item['filename']+'.sig'),os.pread(archive.signature.fd,archive.signature.size,0))
        write(pending/'manifest.json',os.pread(manifest.archive.fd,manifest.archive.size,0));write(pending/'manifest.json.sig',os.pread(manifest.signature.fd,manifest.signature.size,0))
        sync(pending)
        validate_receipt(receipt,value,observed,int(time.time()))
        approved_manifest(manifest) # Expiry may have elapsed while copying outputs.
        budget.checkpoint()
        if (directory/'attestation').exists(): raise Rejected('Attestation already published; never replace')
        pending.rename(directory/'attestation');sync(directory)


def recover(operation,role='receipt'):
    # Cleanup remains possible after key revocation or configuration removal.
    if os.getuid()!=pwd.getpwnam(USERS[role]).pw_uid: raise Rejected('Wrong cleanup account')
    home=HOMES[role]
    store=home/'work'/identifier(operation)/'journal'
    if store.is_dir():
        from journal import Journal
        journal=Journal(store)
        try: journal.recover()
        finally: journal.close()


def main():
    try:
        if len(sys.argv)!=3: raise Rejected('One fixed provider action and reviewed identifier required')
        action,value=sys.argv[1:]
        if action=='provision': provision(value)
        else:
            identifier(value)
            actions={'prepare':prepare,'sign-build':sign_build,'attach':attach,'attest':attest,'publish':publish,'recover':recover,'recover-archive':lambda value:recover(value,'archive')}
            if action not in actions: raise Rejected('Unknown provider action')
            actions[action](value)
        return 0
    except (OSError,ValueError,KeyError,TypeError,RuntimeError,subprocess.SubprocessError):
        # Do not forward GPG/agent output or private configuration into journals.
        print('AUR provider rejected: production identity/input/verification unavailable; no installation grant',file=sys.stderr)
        return 1


if __name__=='__main__': raise SystemExit(main())
