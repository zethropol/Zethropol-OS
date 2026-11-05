"""Offline runtime evidence contract. Never executes a probe or grants authority.

Capture integrity is not proof that PolicyKit, reboot, native commit or recovery
occurred. Positive claims still require independently reviewed raw system evidence.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import time
import uuid

from native_domain_acceptance import read_public

REPO=Path(__file__).resolve().parents[3]
SOURCES=(
    'desktop/control-center/aur/update-reuse/src/production_authority.rs',
    'services/update/src/aur/receipt.rs',
    'services/update/src/broker_protocol.rs',
    'services/update/deployment/polkit/org.zethropol.update.policy',
)
CASES={
    'ELIGIBILITY-REFUSAL':('eligibility-prerequisites','package-db-before-after'),
    'POLKIT-ALLOW':('polkit-decisions','peer-session','consent-nonce'),
    'POLKIT-DENY':('polkit-decisions','peer-session','package-db-before-after'),
    'POLKIT-FOREIGN-PEER':('peer-session','package-db-before-after'),
    'POLKIT-REMOTE-INACTIVE':('peer-session','package-db-before-after'),
    'JOURNAL-ISOLATION':('mount-layout','worker-namespace','journal-fsync'),
    'COMMIT-SUCCESS':('native-outcome','snapshot-results','package-db-before-after'),
    'TRUST-DRIFT':('signed-context','package-db-before-after'),
    'PLAN-DRIFT':('locked-native-plan','package-db-before-after'),
    'WORKER-CANCEL-TIMEOUT':('cgroup-processes','journal-cleanup'),
    'CANCEL-BEFORE':('commit-checkpoint','snapshot-results','package-db-before-after'),
    'CANCEL-AFTER':('commit-checkpoint','cancel-refused','native-outcome'),
    'SNAPSHOT-FAIL':('snapshot-results','package-db-before-after'),
    'BOOT-BLOCK':('boot-verifier','package-db-before-after'),
    'CRASH-ADMISSION':('receipt-tombstone','restart-replay-denial','package-db-before-after'),
    'CRASH-BEFORE-COMMIT':('commit-checkpoint','restart-replay-denial','package-db-before-after'),
    'PARTIAL-KILL':('commit-checkpoint','partial-extraction','unknown-outcome'),
    'POWER-LOSS':('commit-checkpoint','partial-extraction','serial-power-cycle','unknown-outcome'),
    'CLOCK-REPLAY':('boot-epoch','receipt-tombstone','restart-replay-denial','package-db-before-after'),
    'POST-SNAPSHOT-FAIL':('native-outcome','snapshot-results'),
    'POST-BOOT':('native-outcome','boot-verifier','serial-power-cycle'),
    'JOURNAL-DAMAGE-SPACE':('journal-fsync','corruption-space-denial'),
    'ROOT-RECOVERY':('operator-recovery','journal-survives-rollback','boot-verifier'),
    'OFFICIAL-REGRESSION':('official-update-regression','source-consent-separation'),
}
NO_MUTATION=frozenset({'POLKIT-DENY','POLKIT-FOREIGN-PEER','POLKIT-REMOTE-INACTIVE',
                       'SNAPSHOT-FAIL','BOOT-BLOCK','CRASH-BEFORE-COMMIT','ELIGIBILITY-REFUSAL',
                       'TRUST-DRIFT','PLAN-DRIFT','CANCEL-BEFORE','CRASH-ADMISSION','CLOCK-REPLAY'})


class CaptureRejected(ValueError): pass


def require(ok,reason):
    if not ok: raise CaptureRejected(reason)


def sha(data): return hashlib.sha256(data).hexdigest()


def digest(value): return isinstance(value,str) and re.fullmatch('[0-9a-f]{64}',value) is not None


def unique(pairs):
    result={}
    for key,value in pairs:
        require(key not in result,'Duplicate runtime capture field')
        result[key]=value
    return result


def exact(value,fields,label):
    require(isinstance(value,dict) and set(value)==set(fields),'Exact '+label+' fields required')


def plan():
    return {'schema':1,'purpose':'zethropol-runtime-acceptance-plan','scope':'preparation-only',
            'production_authorized':False,'runtime_acceptance':'BLOCKED',
            'prerequisites':['approved disposable VM identity/image/layout',
                'approved guest-only capability without modifying shipping eligibility checks',
                'real signed root/review/independent receipt inputs',
                'operator authorization for each mutating/fault guest case'],
            'cases':[{'id':case,'status':'BLOCKED','required_evidence_roles':list(roles),
                      'package_db_must_remain_unchanged':case in NO_MUTATION}
                     for case,roles in CASES.items()],
            'source_bindings':list(SOURCES),
            'capture_limits':{'bytes':1024**2,'cases':len(CASES),'artifacts_per_case':16,
                              'artifact_bytes':16*1024**2,'aggregate_artifact_bytes':64*1024**2},
            'operations_performed':{'policykit_action':False,'service_change':False,'native_transaction':False,
                                    'snapshot':False,'boot':False,'signing':False}}


def validate_capture(capture,evidence_root,current_sources):
    """Validate exact context/byte bindings, never promote caller assertions."""
    exact(capture,{'schema','purpose','scope','vm_id','boot_id','source_sha256','cases'},'runtime capture')
    require(type(capture['schema']) is int and capture['schema']==1 and
            capture['purpose']=='zethropol-runtime-capture' and capture['scope']=='disposable-vm',
            'Disposable runtime capture scope required')
    for name in ('vm_id','boot_id'):
        try: valid=isinstance(capture[name],str) and str(uuid.UUID(capture[name]))==capture[name]
        except (ValueError,TypeError,AttributeError):valid=False
        require(valid,'Canonical '+name+' required')
    bindings=capture['source_sha256']
    exact(bindings,SOURCES,'runtime source binding')
    require(all(digest(value) for value in bindings.values()) and bindings==current_sources,
            'Runtime implementation changed; capture cannot certify current source')
    records=capture['cases']
    require(isinstance(records,list) and len(records)==len(CASES),'Complete runtime case set required')
    seen=set();hashes={};blocked=[];total_bytes=0
    root=Path(evidence_root).absolute()
    for case in records:
        exact(case,{'id','status','before_package_db_sha256','after_package_db_sha256','evidence'},'runtime case')
        name=case['id']
        require(isinstance(name,str) and name in CASES and name not in seen,'Unique known runtime case required')
        seen.add(name)
        require(case['status'] in ('PASS','BLOCKED'),'Explicit runtime observation status required')
        require(digest(case['before_package_db_sha256']) and digest(case['after_package_db_sha256']),
                'Measured before/after package database fingerprints required')
        evidence=case['evidence']
        require(isinstance(evidence,list) and len(evidence)<=16,'Bounded runtime artifacts required')
        roles=set()
        for item in evidence:
            exact(item,{'role','path','sha256'},'runtime artifact')
            require(isinstance(item['role'],str) and item['role'] in CASES[name] and item['role'] not in roles,
                    'Unique required runtime evidence role expected')
            roles.add(item['role']);relative=item['path']
            require(isinstance(relative,str) and 0<len(relative)<=4096 and '\x00' not in relative and
                    not relative.startswith('/') and str(PurePosixPath(relative))==relative and
                    '..' not in PurePosixPath(relative).parts and relative!='.',
                    'Relative canonical runtime evidence path required')
            require(digest(item['sha256']),'Pinned runtime artifact SHA256 required')
            if relative in hashes:
                require(hashes[relative]==item['sha256'],'Conflicting runtime artifact binding')
            else:
                raw=read_public(root/relative,16*1024**2)
                require(bool(raw),'Empty runtime artifact cannot establish required evidence')
                total_bytes+=len(raw)
                require(total_bytes<=64*1024**2,'Aggregate runtime evidence byte quota exceeded')
                require(sha(raw)==item['sha256'],'Runtime evidence bytes changed')
            hashes[relative]=item['sha256']
        if case['status']=='PASS':
            require(roles==set(CASES[name]),'Passing case lacks required raw evidence roles')
            if name in NO_MUTATION:
                require(case['before_package_db_sha256']==case['after_package_db_sha256'],
                        'Denied/precommit case changed package database')
        else:blocked.append(name)
    return {'capture_integrity':'PASS','scope':'offline byte/context binding only',
            'runtime_acceptance':'BLOCKED','production_authorized':False,'requires_independent_review':True,
            'blocked_observations':blocked,'artifact_sha256':hashes,
            'reason':'Raw JSON PASS claims and artifact hashes do not prove actual system actions or acceptance'}


def source_hashes(repo=REPO):
    return {name:sha(read_public(Path(repo)/name,16*1024**2)) for name in SOURCES}


def passive_preflight(output,repo=REPO):
    """Capture real passive observations, not active PolicyKit/recovery tests.

    Fixed public paths only. No subprocess, bus, agent, daemon or transaction is
    contacted. /proc snapshots describe THIS process's namespace, not an
    asserted production worker or a caller-supplied guest identity.
    """
    output=Path(output)
    output.mkdir(mode=0o700,parents=False,exist_ok=False)
    paths={
        'uid-map':Path(f'/proc/{os.getpid()}/uid_map'),
        'gid-map':Path(f'/proc/{os.getpid()}/gid_map'),
        'mountinfo':Path(f'/proc/{os.getpid()}/mountinfo'),
        'cgroup':Path(f'/proc/{os.getpid()}/cgroup'),
        'process-status':Path(f'/proc/{os.getpid()}/status'),
        'boot-id':Path('/proc/sys/kernel/random/boot_id'),
        'installed-update-polkit':Path('/usr/share/polkit-1/actions/org.zethropol.update.policy'),
    }
    before={'wall_unix_ns':time.time_ns(),'monotonic_ns':time.monotonic_ns(),
            'boottime_ns':time.clock_gettime_ns(time.CLOCK_BOOTTIME)}
    snapshots={};observed={}
    for name,path in paths.items():
        try:
            # Small proc/sys handlers may allocate the requested read buffer;
            # asking megabytes for a UUID can fail with ENOMEM. Keep each
            # public metadata type's budget finite and proportionate.
            limit={'mountinfo':4*1024**2,'process-status':64*1024,
                   'installed-update-polkit':1024**2}.get(name,4096)
            raw=read_public(path,limit)
            target=output/(name+'.raw')
            # Restrict output artifacts; never chmod or touch source paths.
            fd=os.open(target,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o600)
            with os.fdopen(fd,'wb') as stream:stream.write(raw)
            observed[name]=raw
            snapshots[name]={'status':'PASS','scope':'passive snapshot only','source':str(path),
                'artifact':target.name,'sha256':sha(raw),'bytes':len(raw)}
        except (OSError,ValueError) as error:
            snapshots[name]={'status':'BLOCKED','source':str(path),'reason':type(error).__name__+': '+str(error)}
    after={'wall_unix_ns':time.time_ns(),'monotonic_ns':time.monotonic_ns(),
           'boottime_ns':time.clock_gettime_ns(time.CLOCK_BOOTTIME)}
    initial=observed.get('uid-map',b'').split()==[b'0',b'0',b'4294967295']
    boot=None
    try:
        candidate=observed.get('boot-id',b'').decode('ascii').strip()
        if str(uuid.UUID(candidate))==candidate:boot=candidate
    except (ValueError,UnicodeDecodeError):pass
    result={'schema':1,'purpose':'zethropol-passive-runtime-observations',
        'scope':'current process namespace only; not guest/worker or production runtime acceptance',
        'runtime_acceptance':'BLOCKED','production_authorized':False,
        'uid':os.getuid(),'effective_uid':os.geteuid(),'initial_uid_namespace_observed':initial,
        'boot_id_observed':boot,'clock_before':before,'clock_after':after,
        'snapshots':snapshots,'source_sha256':source_hashes(repo),
        'not_established':['effective PolicyKit authorization','worker cgroup isolation/cleanup',
            'journal fsync and snapshot rollback isolation','five-minute admission path',
            'snapshot/boot/recovery/fault acceptance'],
        'operations_performed':{'policykit_action':False,'service_change':False,'native_transaction':False,
                               'snapshot':False,'boot':False,'signing':False,'agent_contact':False}}
    with (output/'passive-preflight.json').open('x') as stream:
        json.dump(result,stream,indent=2);stream.write('\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture',type=Path);parser.add_argument('--evidence-root',type=Path)
    parser.add_argument('--passive-preflight',action='store_true',help='fixed public metadata snapshots only; output is a NEW directory')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if bool(args.capture)!=bool(args.evidence_root):parser.error('--capture and --evidence-root must be paired')
    if args.passive_preflight:
        if args.capture or args.evidence_root:parser.error('--passive-preflight does not accept capture/guest paths')
        result=passive_preflight(args.output)
        print(json.dumps({'runtime_acceptance':result['runtime_acceptance'],'production_authorized':False},indent=2))
        raise SystemExit(0)
    if args.capture:
        data=read_public(args.capture.absolute(),1024**2)
        result=validate_capture(json.loads(data,object_pairs_hook=unique),args.evidence_root,source_hashes())
    else:
        result=plan();result['source_sha256']=source_hashes()
    with args.output.open('x') as stream:json.dump(result,stream,indent=2);stream.write('\n')
    print(json.dumps({'runtime_acceptance':'BLOCKED','production_authorized':False},indent=2))
