"""Read-only measurement from REAL signed/provisioned roots to a new review.

Never extracts, signs, approves, executes a recipe or installs anything. An
unsigned historical tree cannot enter this path. Output is an unapproved
source-plan candidate, never independent attestation or production acceptance.
"""
import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import sys

if __name__=='__main__':
    if os.getuid()==0 or os.geteuid()==0:
        raise SystemExit('Signed-root review must never run as root')
    sys.path.insert(0,str(Path(__file__).resolve().parent))

from worker import Rejected, canonical, digest, review, tree
from trust import SignedArchive, TrustPolicy, approved_manifest
from root_limits import root_limits
from provider import root_path, unique
from native_domain_acceptance import read_public

POLICY='/etc/zethropol-update/aur-trust/policy.json'
DISTRIBUTION='/etc/zethropol-update/aur-trust/distribution.json'
ROOTS=Path('/var/lib/zethropol/aur-build-roots')


def expected_provenance(plan,manifest_hash,verified):
    if len(verified)!=len(plan['packages']): raise Rejected('Complete verified root closure required')
    records=[]
    for package,observed in zip(plan['packages'],verified):
        if observed['sha256']!=package['sha256'] or observed['scope']!='production-policy-only':
            raise Rejected('Production signed package identity required')
        records.append({'name':package['name'],'sha256':observed['sha256'],'signer':observed['signer']})
    return {'manifest_sha256':manifest_hash,'scope':'production-policy-only','packages':records}


def check_provenance(actual,expected):
    if actual!=expected: raise Rejected('New manifest/provenance mismatch; historical roots cannot be reused')


def measure(request):
    if os.getuid()==0 or os.geteuid()==0: raise Rejected('Review measurement must be non-root')
    if (not isinstance(request,dict) or set(request)!={'manifest','manifest_signature','packages','build_root',
                                                    'recipe','identity','sources','dependencies'}):
        raise Rejected('Exact signed-root measurement request required')
    if not isinstance(request['packages'],list) or not 1<=len(request['packages'])<=256:
        raise Rejected('Bounded signed package set required')
    with ExitStack() as stack:
        manifest_policy=TrustPolicy.load(POLICY,role='manifest');stack.callback(manifest_policy.keyring.close)
        distribution=TrustPolicy.load(DISTRIBUTION);stack.callback(distribution.keyring.close)
        if any(p.deployment!='production-policy-only' for p in (manifest_policy,distribution)):
            raise Rejected('Real production public trust policies required')
        def signed(path,signature,policy,limit):
            # Every input is snapshotted before public verification.
            fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC)
            try:
                sig=os.open(signature,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC)
                try:return SignedArchive(fd,sig,policy,archive_limit=limit)
                finally:os.close(sig)
            finally:os.close(fd)
        manifest=stack.enter_context(signed(request['manifest'],request['manifest_signature'],manifest_policy,1024**2))
        if manifest.scope!='production-policy-only':raise Rejected('Real Manifest signature required')
        plan,root_id=approved_manifest(manifest)
        count,compressed,_,_=root_limits(plan['schema'])
        if len(request['packages'])!=len(plan['packages']) or len(request['packages'])>count:
            raise Rejected('Signed package count changed')
        verified=[];total=0
        for item in request['packages']:
            if not isinstance(item,dict) or set(item)!={'archive','signature'}:
                raise Rejected('Exact public archive/signature pair required')
            with signed(item['archive'],item['signature'],distribution,compressed-total) as archive:
                total+=archive.archive.size
                verified.append({'sha256':archive.archive.sha256,'signer':archive.signer,'scope':archive.scope})
        expected=expected_provenance(plan,root_id,verified)
        root=Path(request['build_root'])
        if root!=ROOTS/manifest.archive.sha256:raise Rejected('Fixed new-manifest root path required')
        root_path(root,directory=True)
        root_path(root/'.provenance.json')
        provenance=json.loads(read_public(root/'.provenance.json',1024**2),object_pairs_hook=unique)
        check_provenance(provenance,expected)
        measured,plan_hash=review(request['recipe'],root,request['identity'],request['sources'],request['dependencies'],schema=2)
        # Recheck after the complete measurement, including provenance and expiry.
        from worker import matches_root
        if not matches_root(measured,tree(root,True)):
            raise Rejected('Root changed during review measurement')
        check_provenance(json.loads(read_public(root/'.provenance.json',1024**2),object_pairs_hook=unique),expected)
        approved_manifest(manifest)
        return {'schema':1,'purpose':'zethropol-signed-root-review-candidate','approved':False,
                'production_authorized':False,'independent_attestation':False,
                'signed_manifest_sha256':manifest.archive.sha256,'manifest_primary':manifest.signer,
                'source_plan':measured,'source_plan_sha256':plan_hash,
                'reviewed_root_commitment':measured['rootfs'],
                'staged_execution_tree_sha256':None,
                'staged_measurement':'not performed; execution backend must freeze/remeasure independently',
                'signature_verified':True,'root_provisioning_performed':False,
                'requires_new_executable_owner_review':True}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--request',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        request=json.loads(read_public(args.request.absolute(),1024**2),object_pairs_hook=unique)
        result=measure(request)
        with args.output.open('xb') as stream:stream.write(canonical(result))
    except (OSError,ValueError,KeyError,TypeError,Rejected):
        raise SystemExit('Signed-root review BLOCKED; no approval, root provisioning or signature produced')
