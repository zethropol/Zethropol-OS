"""Verify real distribution archives and emit an UNSIGNED root manifest candidate.

Public preparation only: no production TrustPolicy.load bypass, key generation,
signing, root extraction, service or install operation. Owner review/signing and
production ownership verification remain separate mandatory steps.
"""
from contextlib import ExitStack
import argparse
import json
import os
from pathlib import Path
import time
import tarfile

from root_limits import root_limits
from sealed import Blob
from trust import TrustPolicy, SignedArchive, approved_manifest, approved_signers
from worker import canonical, digest, Rejected


def prepare(profile_path, distribution_path, extra_cache, output, expires):
    profile = json.loads(Path(profile_path).read_bytes())
    config_path = Path(distribution_path)
    config = json.loads(config_path.read_bytes())
    ring = config_path.parent / config['keyring']
    if ring.parent != config_path.parent or ring.name != config['keyring']:
        raise Rejected('Candidate keyring must be a local basename')
    package_limit, archive_limit, expanded_limit, entry_limit = root_limits(2)
    rows = profile['packages']
    if not isinstance(rows, list) or not 1 <= len(rows) <= package_limit:
        raise Rejected('Bounded candidate package list required')
    if type(expires) is not int or not time.time() < expires <= time.time()+86400:
        raise Rejected('Explicit expiry within one day required')
    packages=[]; verified=[]; total=0; expanded=0; entries=0; files=0
    with ExitStack() as stack:
        fd=os.open(ring,os.O_RDONLY|os.O_NOFOLLOW)
        try: keyring=stack.enter_context(Blob(fd,16*1024**2))
        finally: os.close(fd)
        if keyring.sha256 != config['keyring_sha256']:
            raise Rejected('Candidate distribution keyring pin mismatch')
        # This scope only exercises the real signature verifier. It does not
        # certify root ownership or authorize these user-owned candidate files.
        policy=TrustPolicy(keyring,approved_signers(config['signers']),'isolated-test')
        for row in rows:
            name=row['filename']
            if Path(name).name != name:
                raise Rejected('Invalid distribution archive filename')
            path=Path('/var/cache/pacman/pkg')/name
            if not path.is_file(): path=Path(extra_cache)/name
            archive=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK)
            try:
                signature=os.open(str(path)+'.sig',os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK)
                try:
                    with SignedArchive(archive,signature,policy,archive_limit=archive_limit-total) as signed:
                        if signed.archive.sha256 != row['sha256'] or signed.signer != row['signer']:
                            raise Rejected('Candidate differs from reviewed package/signing identity')
                        stream=os.fdopen(os.open(f'/proc/self/fd/{signed.archive.fd}',os.O_RDONLY),'rb')
                        metadata=[]; package_files={}
                        with stream,tarfile.open(fileobj=stream,mode='r:*') as archive_stream:
                            for member in archive_stream:
                                entries+=1;expanded+=member.size
                                if member.size<0 or entries>entry_limit or expanded>expanded_limit:
                                    raise Rejected('Candidate root exceeds signed expansion/entry budget')
                                if member.isfile():
                                    files+=1
                                    if member.name.removeprefix('./') not in ('.PKGINFO','.MTREE','.INSTALL','.BUILDINFO'):
                                        package_files[member.name.removeprefix('./')]=member.size
                                if member.islnk():
                                    source_name=member.linkname.removeprefix('./')
                                    if source_name not in package_files:
                                        raise Rejected('Hardlink source must be an earlier file in the same package')
                                    size=package_files[source_name];expanded+=size;files+=1
                                    if expanded>expanded_limit: raise Rejected('Hardlink expansion quota exceeded')
                                    package_files[member.name.removeprefix('./')]=size
                                elif not (member.isfile() or member.isdir() or member.issym()):
                                    raise Rejected('Root archive contains unsupported hardlink/device entry')
                                if member.name.removeprefix('./') == '.PKGINFO':
                                    if not member.isfile() or member.size>1024**2:
                                        raise Rejected('Bounded regular package metadata required')
                                    metadata.append(archive_stream.extractfile(member).read().decode())
                        if len(metadata)!=1:
                            raise Rejected('Unique signed package metadata required')
                        fields={}
                        for line in metadata[0].splitlines():
                            if ' = ' in line:
                                key,value=line.split(' = ',1);fields.setdefault(key,[]).append(value)
                        if (fields.get('pkgname') != [row['name']] or fields.get('pkgver') != [row['version']] or
                                fields.get('arch') not in (['x86_64'],['any'])):
                            raise Rejected('Signed package name/version/architecture incompatible with x86_64 root')
                        total+=signed.archive.size
                        packages.append({k:row[k] for k in ('name','version','sha256')})
                        verified.append({'archive':str(path.resolve()),'signature':str(Path(str(path)+'.sig').resolve()),
                                         'signer':signed.signer,'signature_sha256':signed.signature.sha256})
                finally: os.close(signature)
            finally: os.close(archive)
    manifest={'schema':2,'purpose':'zethropol-aur-build-root','architecture':'x86_64',
              'expires':expires,'packages':packages}
    target=Path(output);target.mkdir(mode=0o700,parents=True,exist_ok=False)
    raw=canonical(manifest)
    path=target/'root-manifest.unsigned.json';path.write_bytes(raw)
    fd=os.open(path,os.O_RDONLY)
    try:
        with Blob(fd) as blob:
            from types import SimpleNamespace
            approved_manifest(SimpleNamespace(archive=blob))  # Schema only; NOT a signature check.
    finally: os.close(fd)
    report={'manifest_sha256':digest(raw),'manifest_signature':None,'production_authorized':False,
            'verified_package_count':len(packages),'compressed_bytes':total,
            'expanded_member_bytes':expanded,'archive_entries':entries,'archive_regular_files':files,
            'profile_sha256':digest(Path(profile_path).read_bytes()),
            'distribution_policy_sha256':digest(config_path.read_bytes()),
            'distribution_keyring_sha256':keyring.sha256,'packages':verified}
    (target/'preparation.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile',required=True)
    parser.add_argument('--distribution-policy',required=True)
    parser.add_argument('--extra-cache',required=True)
    parser.add_argument('--output',required=True)
    parser.add_argument('--expires',required=True,type=int)
    args=parser.parse_args()
    report=prepare(args.profile,args.distribution_policy,args.extra_cache,args.output,args.expires)
    print(json.dumps({k:report[k] for k in ('manifest_sha256','verified_package_count','manifest_signature','production_authorized')}))
