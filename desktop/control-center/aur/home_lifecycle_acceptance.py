"""Offline acceptance of the user's verified inert installation; no host probe.

User acceptance is retained as such, never manufactured as authenticated runtime
or inode-recovery authority. This module does not deploy or contact any service.
"""
import hashlib
from pathlib import Path

TRANSACTION='57652e06b38ac2bd6d153ce5d217b3d9'
MANIFEST_SHA='c1c90688a452cb8e74871f44696afdfe1299d290a12ec73b53421e8fe4503992'
BUNDLE_SHA='e9325b0b8b49d53bf5528526963cf1cde56ca2a75007e15dbe4ab01cff9746d9'
ACCEPTED_REVIEW={
    'status':'READONLY_PARTIAL_DEPLOYMENT_INVENTORY_CAPTURED',
    'recovery_decision':'VERIFY_EXISTING_INERT_INSTALLATION_RETAIN_ALL_NO_REPLAY',
    'audit_postflight':'PASS_INERT_FILE_DEPLOYMENT',
    'manifest_sha256':MANIFEST_SHA,'protected_preservation_matches_original':True,
    'stability_pass':True,'host_changed_during_review':False,'systemd_active_state':'inactive',
    'agent_started':False,'activation_approved':False,'quarantine_authorized':False,
    'current_inode_transaction_ownership_proven':False,
}

class AcceptanceRejected(ValueError):pass

def validate_review(review):
    """Strict summary semantics only. No JSON assertion grants runtime authority."""
    if not isinstance(review,dict) or set(review)!=set(ACCEPTED_REVIEW):raise AcceptanceRejected('exact accepted review fields required')
    for name,expected in ACCEPTED_REVIEW.items():
        if type(review[name]) is not type(expected) or review[name]!=expected:
            raise AcceptanceRejected('accepted review drift: '+name)
    return {'inert_installation':'USER_ACCEPTED_RETAIN','basis':'explicit user acceptance of supplied root readonly inventory',
            'transaction_id':TRANSACTION,'manifest_sha256':MANIFEST_SHA,'old_transaction_reusable':False,
            'redeployment_required':False,'rollback_authorized':False,'runtime_acceptance':'NOT_ESTABLISHED',
            'activation_approved':False,'quarantine_authorized':False,'credential_readiness':'NOT_PROBED',
            'current_inode_transaction_ownership_proven':False,'inode_mutation_authorized':False,
            'production_authorized':False}

def local_historical_pins(control_center):
    """Check workspace historical bytes, not installed host state."""
    bundle=Path(control_center)/'build/aur-agent-home-lifecycle-v2-files-transaction-20261103'
    expected={'deployment-manifest.INACTIVE.json':MANIFEST_SHA,'file-transaction.pinned.py':BUNDLE_SHA}
    results={name:hashlib.sha256((bundle/name).read_bytes()).hexdigest()==digest for name,digest in expected.items()}
    if not all(results.values()):raise AcceptanceRejected('historical inert bundle or manifest changed')
    return {'scope':'workspace historical byte pins only','matches':results}

def validation_plan():
    """Separate current readonly acceptance from future forbidden live startup."""
    return {
        'schema':1,'purpose':'home-lifecycle-v2-readonly-and-future-runtime-plan',
        'current':validate_review(dict(ACCEPTED_REVIEW)),
        'readonly_acceptance':{'status':'COMPLETE_USER_ACCEPTED','further_host_scan_required':False,
                              'installed_files_preserved':True,'historical_nonce_consumed':True,
                              'inert_state_is_not_agent_runtime_acceptance':True},
        'future_runtime_stages':[
            {'id':'controlled-maintenance-design','status':'BLOCKED','host_operations_performed':False,
             'required':['trusted rescue/early-boot exclusive local filesystem and root maintenance window',
                         'UID949 admissions inhibited; unknown namespace/SCM_RIGHTS origin addressed architecturally',
                         'new four-socket receipt schema and inode/ctime-bound one-shot maintenance executor reviewed/tested',
                         'separate explicit maintenance authorization; no reuse of deployment transaction'],
             'current_scope_permits_execution':False},
            {'id':'manual-activation','status':'BLOCKED','host_operations_performed':False,
             'required':['separately verified complete quarantine receipt and restored parent metadata',
                         'fresh policy/code/unit/config/binary pins, new single-use activation_id',
                         'separate explicit manual activation authorization'],
             'current_scope_permits_execution':False},
            {'id':'post-activation-readonly-runtime','status':'WAITING_FOR_SEPARATE_ACTIVATION',
             'required':['unit cgroup and adopted child pidfd/startticks/kernel IDs match',
                         'primary/SSH kernel endpoint FD ownership matches the adopted process',
                         'exact native route; primary/SSH metadata0700 or0600/no ACL/single link',
                         'no extra/browser listeners; existing four archived originals retained',
                         'config and13 public signer files unchanged; no credential probe',
                         'no unexpected restart; credential_readiness=NOT_PROBED'],
             'current_scope_permits_execution':False},
            {'id':'stop-crash-and-restart-denial','status':'SEPARATE_DISPOSABLE_GUEST_AUTHORIZATION_REQUIRED',
             'required':['owned-process-only clean stop/failure pidfd cleanup and no automatic retry',
                         'retain new stale sockets and consumed claims; unexpected exit2 is failure',
                         'same activation ID replay is denied without socket cleanup'],
             'current_scope_permits_execution':False},
        ],
        'admission_boundaries':{'inert_deployment_authorizes_maintenance':False,
                                'maintenance_authorizes_activation':False,
                                'activation_authorizes_signing_or_production':False},
        'current_prohibitions':['socket mutation','agent start','credential probe','signing','systemd enable',
                                'redeploy accepted files','old transaction replay','automatic rollback'],
    }
