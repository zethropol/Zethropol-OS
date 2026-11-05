"""Read-only review of the four public payloads. Never installs or executes them."""
import hashlib
import json
import os
import re
from pathlib import Path
import stat

REPO = Path(__file__).resolve().parents[3]
BASE = REPO / 'desktop/control-center/build/aur-chain-continuation-post949-20261101'
POLICY = REPO / 'desktop/control-center/build/aur-yay-offline-20261101/generic-distribution'
SPECS = {
    '/usr/lib/zethropol/aur-root-stage': (BASE/'aur-root-stage.CANDIDATE', '0755'),
    '/usr/lib/zethropol/aur-quota-mount': (BASE/'aur-quota-mount.CANDIDATE.r2', '0755'),
    '/etc/zethropol-update/aur-trust/distribution.gpg': (POLICY/'distribution.gpg', '0644'),
    '/etc/zethropol-update/aur-trust/distribution.json': (POLICY/'distribution.json', '0644'),
}


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def stamp(s):
    return (s.st_dev,s.st_ino,s.st_mode,s.st_uid,s.st_gid,s.st_nlink,s.st_size,s.st_mtime_ns,s.st_ctime_ns)


def read_regular(path):
    path = str(path)
    require(Path(path).is_absolute() and str(Path(path)) == path and '..' not in Path(path).parts,
            'Canonical absolute source required')
    directory = os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
    try:
        for name in Path(path).parts[1:-1]:
            child = os.open(name,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW,dir_fd=directory)
            os.close(directory); directory = child
        pin = os.open(Path(path).name,os.O_PATH|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=directory)
    finally:
        os.close(directory)
    try:
        before = os.fstat(pin)
        require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1 and 0 <= before.st_size <= 16*1024**2,
                'Bounded single-link regular source required')
        fd = os.open('/proc/'+str(os.getpid())+'/fd/'+str(pin),os.O_RDONLY|os.O_CLOEXEC|os.O_NONBLOCK)
        with os.fdopen(fd,'rb') as stream:
            require(stamp(before)==stamp(os.fstat(stream.fileno())),'Source changed before read')
            data = stream.read(16*1024**2+1)
            require(len(data)<=16*1024**2 and stamp(before)==stamp(os.fstat(stream.fileno())),
                    'Source changed during read')
            require('security.capability' not in os.listxattr(stream.fileno()),'Source file capabilities prohibited')
        return data
    finally:
        os.close(pin)


def unique(pairs):
    result = {}
    for key,value in pairs:
        require(key not in result,'Duplicate JSON field')
        result[key] = value
    return result


def validate(plan, reader=read_regular):
    require(plan.get('schema')==2 and plan.get('purpose')=='zethropol-four-public-files-review', 'Explicit review schema required')
    require(plan.get('approved') is False and plan.get('applied') is False,'Review cannot grant deployment')
    require(plan.get('directories_to_create')==[], 'Permanent scope may not grow')
    rows = plan['files']
    require(len(rows)==4 and [r['target'] for r in rows]==list(SPECS),'Exact ordered four-file scope required')
    require(plan['publish_order']==list(SPECS),'Policy must be published last')
    blobs = {}; result = []
    for row in rows:
        source,mode = SPECS[row['target']]
        require(row['source']==str(source) and row['owner']=='root:root' and row['mode']==mode,
                'Source/target/owner/mode drift')
        data = reader(source)
        require(hashlib.sha256(data).hexdigest()==row['sha256'],'Source hash mismatch: '+str(source))
        blobs[row['target']] = data
        if mode=='0755':
            require(data[:6]==b'\x7fELF\x02\x01' and int.from_bytes(data[18:20],'little')==62,
                    'x86_64 ELF helper required')
        result.append({'target':row['target'],'source':str(source),'sha256':row['sha256'],'owner':row['owner'],'mode':mode})
    config = json.loads(blobs['/etc/zethropol-update/aur-trust/distribution.json'],object_pairs_hook=unique)
    require(set(config)=={'keyring','keyring_sha256','signers'} and config['keyring']=='distribution.gpg',
            'Policy-local fixed keyring required')
    require(config['keyring_sha256']==hashlib.sha256(blobs['/etc/zethropol-update/aur-trust/distribution.gpg']).hexdigest(),
            'Policy/keyring digest mismatch')
    values = config['signers']
    require(isinstance(values,list) and 1 <= len(values) <= 32 and
            all(isinstance(f,str) and re.fullmatch(r'(?:[A-F0-9]{40}|[A-F0-9]{64})',f) for f in values) and
            len(set(values))==len(values),'Explicit bounded unique distribution primaries required')
    signers = set(values)
    require(sorted(signers)==plan['distribution_primary_signers'],'Signer allowlist drift')
    project_raw = reader(Path('/etc/zethropol-update/aur-trust/policy.json'))
    require(hashlib.sha256(project_raw).hexdigest()==plan['project_policy_sha256'],'Project policy changed')
    project_policy = json.loads(project_raw,object_pairs_hook=unique)
    observed = {role:project_policy[role]['signers'] for role in ('manifest','archive','receipt')}
    require(observed==plan['project_role_signers'] and all(observed.values()),'Project role mapping drift')
    project = {f for values in observed.values() for f in values}
    require(not signers.intersection(project),'Distribution policy may not authorize project role keys')
    return {'status':'READY FOR OWNER REVIEW','deployment_authorized':False,'files':result,
            'distribution_primary_count':len(signers),'manifest_used':False}


if __name__=='__main__':
    plan = json.loads(read_regular(BASE/'next-public-deployment-plan.json'),object_pairs_hook=unique)
    print(json.dumps(validate(plan),indent=2))
