"""Prepare current workspace acceptance plan; never executes host/guest probes."""
import hashlib
import json
from pathlib import Path

import home_lifecycle_acceptance as lifecycle
import native_domain_acceptance as native
import rebuild_acceptance as rebuild
import runtime_acceptance as runtime

SOURCE=Path(__file__).resolve().parent
REPO=SOURCE.parents[2]
DESTINATION=SOURCE.parent/'build/aur-post-inert-acceptance-20261103'
SOURCES=(
    'desktop/control-center/aur/home_lifecycle_acceptance.py',
    'desktop/control-center/aur/prepare_post_inert_acceptance.py',
    'desktop/control-center/aur/native_archive_loader.c',
    'desktop/control-center/aur/native_domain_acceptance.py',
    'desktop/control-center/aur/rebuild_acceptance.py',
    'desktop/control-center/aur/runtime_acceptance.py',
    'desktop/control-center/aur/provider.py',
    'desktop/control-center/aur/pipeline.py',
    'desktop/control-center/aur/update-reuse/src/delivery.rs',
    *runtime.SOURCES,
)

def current_plan():
    result={'schema':1,'purpose':'accepted-inert-installation-independent-acceptance-preparation',
        'scope':'workspace plan and tests only; no capture/authentication/runtime authority',
        'lifecycle':lifecycle.validation_plan(),'historical_pins':lifecycle.local_historical_pins(SOURCE.parent),
        'native':{'runtime_status':'BLOCKED','negative_cases':sorted(native.NEGATIVES),
                  'requires':['fixed-domain process-start native loader integrated into worker admission',
                              'separate public trust snapshots and existing valid Archive-signed package/signature fixtures',
                              'observed full native trust and official dependency authority separation'],
                  'new_signature_generation_permitted':False},
        'budget':{'runtime_status':'BLOCKED','limit_seconds':rebuild.LIMIT_SECONDS,'clock':'CLOCK_BOOTTIME + CLOCK_REALTIME',
                  'ordered_phases':list(rebuild.PHASES),'original_frame_and_deadline_must_survive_every_handoff':True,
                  'same_instance_clock_regression_rejected':True,'ttl_renewal_permitted':False},
        'rebuild_receipt':{'runtime_status':'BLOCKED','role_uids':rebuild.ROLE_UIDS,
                          'requires':['authenticated signed-root/source-plan/operation inputs',
                                      'real independent full package outputs/native metadata and observed cleanup for UID951/950',
                                      'existing signed receipt bytes and protected broker admission evidence'],
                          'current_scope':'synthetic semantics/denial/consumer tests only'},
        'runtime':{'runtime_status':'BLOCKED','cases':[{'id':case,'required_evidence_roles':list(roles),
                          'package_db_must_remain_unchanged':case in runtime.NO_MUTATION} for case,roles in runtime.CASES.items()],
                   'real_vm_or_PolicyKit_actions_performed':False},
        'next_privileged_stage':{
            'status':'BLOCKED_MISSING_APPROVED_DISPOSABLE_GUEST_AND_PUBLIC_FIXTURES',
            'single_authorization_scope':'provision the reviewed no-signing acceptance fixture environment in ONE identified disposable guest only',
            'preconditions':['operator-approved disposable VM UUID/image digest/boot identity and rollback layout',
                             'current reviewed source/binary hashes and bounded protected evidence sink',
                             'public pre-existing package/signature/trust fixtures with trusted provenance; no new signatures',
                             'guest-only package-db/trust/journal fixtures separated from host and production',
                             'privilege/action allowlist and expected preimages bound to the same guest plan'],
            'excluded':['any host change','agent start','GPG socket mutation','credential probe','signing',
                        'systemd enable','production enable','replay of old deployment or Manifest operation',
                        'automatic native transaction, guest power loss, boot or fault experiment'],
            'authority_boundary':'fixture provisioning alone does not authorize later mutating/fault cases or live lifecycle activation',
        },
        'historical_sidecars':'older acceptance-stage/preflight maps retain old hashes/roles; use this current source-pinned snapshot',
        'operations_performed':{'host_change':False,'guest_execution':False,'agent_start':False,'signing':False,
                                'PolicyKit_action':False,'native_transaction':False,'old_transaction_replay':False},
        'production_authorized':False,
    }
    result['source_sha256']={name:hashlib.sha256((REPO/name).read_bytes()).hexdigest() for name in dict.fromkeys(SOURCES)}
    return result

def prepare(destination=DESTINATION):
    destination.mkdir(exist_ok=True)
    result=current_plan()
    (destination/'current-acceptance-plan.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
    (destination/'home-lifecycle-accepted.json').write_text(json.dumps(result['lifecycle'],indent=2,sort_keys=True)+'\n')
    return result

if __name__=='__main__':prepare();print(DESTINATION)
