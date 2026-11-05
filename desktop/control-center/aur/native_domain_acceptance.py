"""Offline native-domain acceptance contract; never creates trust or authority.

Reads only named public snapshots/source/evidence. No GPG, ALPM transaction,
key import, service, PolicyKit action or helper execution. Contract PASS is a
specification check, never production integration acceptance.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import uuid

REPO = Path(__file__).resolve().parents[3]
ARCHIVE_PRIMARY = '2C0BE07D3A3C4E73B45186738840E6F9BC0AB7D1'
PROBE_REL = 'desktop/control-center/build/aur-native-verification-20261101-r2/result.json'
PROBE_SHA = '34bba183d66a289968a07c2bc52d97b4bba2fa86e263e2b976c73770d808e595'
PUBLIC_PATHS = {
    'pubring.kbx': Path('/etc/pacman.d/gnupg/pubring.kbx'),
    'trustdb.gpg': Path('/etc/pacman.d/gnupg/trustdb.gpg'),
}
NEGATIVES = frozenset({
    'missing-signature', 'tampered-signature', 'unknown-primary', 'revoked-primary',
    'expired-primary', 'marginal-trust', 'unknown-trust', 'client-gpgdir',
    'same-process-domain-switch', 'archive-signs-official-package',
    'official-domain-changed', 'trust-snapshot-changed-under-lock',
})
RUNTIME_SOURCES = {
    'desktop/control-center/aur/RECOVERY_VALIDATION.json': frozenset({
        'desktop/control-center/src/UpdateService.cpp',
        'desktop/control-center/tests/UpdateServiceTest.cpp',
        'services/update/src/update_service.rs',
        'desktop/control-center/build/control_center',
    }),
    'desktop/control-center/aur/VM_HOST_INVENTORY.json': frozenset(),
}


def evidence_document(raw):
    """Public historical records cannot carry duplicate/ambiguous JSON fields."""
    def unique(pairs):
        result = {}
        for key,value in pairs:
            if key in result:
                raise ValueError('duplicate evidence field')
            result[key] = value
        return result
    def invalid_constant(value):
        raise ValueError('non-finite evidence value')
    result = json.loads(raw, object_pairs_hook=unique, parse_constant=invalid_constant)
    if not isinstance(result,dict):
        raise ValueError('evidence object required')
    return result


def read_public(path, limit=16*1024**2):
    """Pin a bounded single-link regular file before data access."""
    path = Path(path)
    if not path.is_absolute() or '..' in path.parts:
        raise ValueError('absolute public path required')
    directory = os.open('/', os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
    try:
        for part in path.parts[1:-1]:
            child = os.open(part, os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW, dir_fd=directory)
            os.close(directory); directory = child
        pin = os.open(path.name, os.O_PATH|os.O_NOFOLLOW|os.O_CLOEXEC, dir_fd=directory)
    finally:
        os.close(directory)
    try:
        before = os.fstat(pin)
        if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or not 0 <= before.st_size <= limit:
            raise ValueError('bounded single-link public regular file required')
        fd = os.open('/proc/'+str(os.getpid())+'/fd/'+str(pin), os.O_RDONLY|os.O_NONBLOCK|os.O_CLOEXEC)
        with os.fdopen(fd,'rb') as stream:
            first = os.fstat(stream.fileno())
            data = stream.read(limit+1)
            after = os.fstat(stream.fileno())
        stamp = lambda s:(s.st_dev,s.st_ino,s.st_uid,s.st_gid,s.st_mode,s.st_nlink,s.st_size,s.st_mtime_ns,s.st_ctime_ns)
        if len(data)>limit or stamp(before)!=stamp(first) or stamp(before)!=stamp(after):
            raise ValueError('public snapshot changed during read')
        return data
    finally:
        os.close(pin)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def gate(identifier, passed, reason):
    return {'id':identifier,'status':'PASS' if passed else 'BLOCKED','reason':reason}


def default_contract():
    return {
        'schema':1, 'purpose':'zethropol-native-domain-offline-contract',
        'scope':'design-only', 'architecture_mode':'existing-single-domain',
        'official_gpgdir':'/etc/pacman.d/gnupg',
        'aur_gpgdir':'/etc/pacman.d/gnupg',
        'required_package_signatures':True, 'allow_optional':False,
        'allow_marginal':False, 'allow_unknown':False,
        'allow_client_gpgdir':False, 'allow_process_domain_switch':False,
        'archive_may_authorize_official_packages':False,
        'official_domain_independently_authorizes_dependencies':True,
        'separate_worker_from_process_start':False,
        'negative_cases':sorted(NEGATIVES),
    }


def validate_contract(contract):
    """Specification-only validation; does not certify installed implementation."""
    expected = default_contract()
    checks = []
    shape = isinstance(contract,dict) and set(contract)==set(expected)
    checks.append(gate('contract-shape',shape,'Exact bounded schema required; extra fields cannot carry commands or authority'))
    if not shape:
        return {'status':'BLOCKED','production_authorized':False,'checks':checks}
    fixed = type(contract['schema']) is int and contract['schema']==1 and contract['purpose']==expected['purpose'] and contract['scope']=='design-only'
    checks.append(gate('contract-scope',fixed,'Contract remains offline/design-only and cannot authorize deployment'))
    required = contract['required_package_signatures'] is True and all(contract[k] is False for k in ('allow_optional','allow_marginal','allow_unknown'))
    checks.append(gate('required-trusted-only',required,'Native PackageRequired/TrustedOnly is mandatory; role pins do not substitute'))
    controls = contract['allow_client_gpgdir'] is False and contract['allow_process_domain_switch'] is False
    checks.append(gate('domain-confusion-rejected',controls,'No client-selected GPGDir or process-local GPGME domain switch'))
    authority = contract['archive_may_authorize_official_packages'] is False and contract['official_domain_independently_authorizes_dependencies'] is True
    checks.append(gate('official-authority-boundary',authority,'Archive authority must not authenticate official repository packages/dependencies'))
    mode = contract['architecture_mode']
    if mode=='existing-single-domain':
        process = contract['official_gpgdir']=='/etc/pacman.d/gnupg' and contract['aur_gpgdir']=='/etc/pacman.d/gnupg' and contract['separate_worker_from_process_start'] is False
    elif mode=='proposed-fixed-separate-domain':
        process = contract['official_gpgdir']=='/etc/pacman.d/gnupg' and contract['aur_gpgdir']=='/etc/zethropol-update/aur-native-trust' and contract['separate_worker_from_process_start'] is True
    else:
        process = False
    checks.append(gate('fixed-process-contract',process,'Only current fixed domain or explicit design-only separate-process proposal is representable'))
    negative = isinstance(contract['negative_cases'],list) and all(isinstance(x,str) for x in contract['negative_cases']) and len(contract['negative_cases'])==len(NEGATIVES) and set(contract['negative_cases'])==NEGATIVES
    checks.append(gate('negative-matrix-complete',negative,'Missing/tampered/unknown/revoked/expired/trust/domain/official-authority/snapshot drift rejections required'))
    return {'status':'PASS' if all(c['status']=='PASS' for c in checks) else 'BLOCKED','scope':'offline contract only','production_authorized':False,'checks':checks}


def validate_admission_capture(capture, expected):
    """Check a future no-install native observation against independent pins.

    This is offline harness preparation, not an admission path in the broker.
    Both documents remain assertions until a real native process and protected
    evidence are independently reviewed. In particular a project signature or
    role fingerprint cannot turn unknown native trust into full trust.
    """
    def exact(value, fields, label):
        if not isinstance(value,dict) or set(value)!=set(fields):
            raise ValueError('exact '+label+' fields required')
    def digest(value):
        return isinstance(value,str) and re.fullmatch('[0-9a-f]{64}',value) is not None
    fields={'schema','scope','operation_id','package_sha256','process_domain','domain_bound_at_start',
            'domain_switched','siglevel','signature','aur_snapshot_sha256',
            'official_dependency_snapshot_sha256','official_repository_metadata_sha256',
            'native_transaction_started','installation_performed'}
    exact(capture,fields,'native capture')
    exact(expected,{'operation_id','package_sha256','aur_snapshot_sha256',
                    'official_dependency_snapshot_sha256','official_repository_metadata_sha256'},'native expectation')
    if type(capture['schema']) is not int or capture['schema']!=1 or capture['scope']!='disposable-native-loader-no-transaction':
        raise ValueError('disposable no-transaction native capture required')
    if capture['native_transaction_started'] is not False or capture['installation_performed'] is not False:
        raise ValueError('native capture must not start a transaction or install')
    for value in (capture['operation_id'],expected['operation_id']):
        try: valid=isinstance(value,str) and str(uuid.UUID(value))==value
        except (ValueError,TypeError,AttributeError):valid=False
        if not valid:raise ValueError('canonical native operation UUID required')
    for name in ('package_sha256','official_repository_metadata_sha256'):
        if not digest(expected[name]) or capture[name]!=expected[name]:
            raise ValueError('native package/repository identity changed')
    if capture['operation_id']!=expected['operation_id']:
        raise ValueError('native observation belongs to a different operation')
    for name in ('aur_snapshot_sha256','official_dependency_snapshot_sha256'):
        for source in (capture,expected):
            exact(source[name],{'pubring.kbx','trustdb.gpg'},name)
            if not all(digest(value) for value in source[name].values()):
                raise ValueError('typed native snapshot digests required')
        if capture[name]!=expected[name]:raise ValueError('native snapshot drift')
    if expected['aur_snapshot_sha256']==expected['official_dependency_snapshot_sha256']:
        raise ValueError('shared native trust snapshot cannot establish authority separation')
    if (capture['process_domain']!='/etc/zethropol-update/aur-native-trust' or
            capture['domain_bound_at_start'] is not True or capture['domain_switched'] is not False):
        raise ValueError('fixed separate native process from startup required; no shared/client-switched domain')
    level=capture['siglevel']
    exact(level,{'package_required','package_optional','marginal_ok','unknown_ok'},'native signature policy')
    if level['package_required'] is not True or any(level[name] is not False for name in ('package_optional','marginal_ok','unknown_ok')):
        raise ValueError('native PackageRequired/TrustedOnly required')
    signature=capture['signature']
    exact(signature,{'status','trust','primary','expired','revoked'},'native signature result')
    if (signature['status']!='valid' or signature['trust'] not in ('full','ultimate') or
            signature['primary']!=ARCHIVE_PRIMARY or signature['expired'] is not False or
            signature['revoked'] is not False):
        raise ValueError('valid current native fully trusted Archive signature required')
    return {'semantic_status':'PASS','native_acceptance':'BLOCKED','production_authorized':False,
            'scope':'offline native observation consistency only; separate-domain design not implemented',
            'requires_independent_review':True,
            'reason':'JSON assertions cannot prove native verification, official dependency authority or process integration'}


def evaluate_cached_probe(raw, current_hashes):
    """Reuse only the exact historical non-installing probe and exact public data."""
    digest_ok = sha(raw)==PROBE_SHA
    checks = [gate('native-probe-evidence-integrity',digest_ok,'Only the pinned non-installing native probe record can be reused')]
    if not digest_ok:
        return checks + [
            gate('native-public-snapshot-reuse',False,'Unpinned evidence cannot be reused, regardless of its claimed snapshots'),
            gate('official-native-loader-mechanism',False,'No authenticated cached loader result available'),
            gate('archive-native-trust',False,'No current exact Archive native package/full-trust evidence; project pin is insufficient'),
        ]
    probe = evidence_document(raw)
    unchanged = current_hashes==probe['public_snapshot_sha256']
    checks.append(gate('native-public-snapshot-reuse',unchanged,'Cached native loader result applies only while pubring.kbx and trustdb.gpg hashes match exactly'))
    loader = unchanged and probe.get('installation_performed') is False and probe.get('native_transaction_started') is False and probe['sample']['valid_returncode']==0 and probe['tampered_signature_returncode']!=0 and probe['missing_signature_returncode']!=0
    checks.append(gate('official-native-loader-mechanism',loader,'Same public snapshot: valid distro package accepted; missing and tampered signatures rejected; no transaction'))
    present = probe['archive_primary_present_in_native_snapshot'].get(ARCHIVE_PRIMARY)
    # Presence alone would still not establish native full trust, package
    # signature admission, official authority separation or integration.
    why = ('Archive primary is absent from the unchanged native public snapshot' if unchanged and present is False else 'No current exact Archive native package/full-trust/authority-isolation acceptance evidence')
    checks.append(gate('archive-native-trust',False,why+'; project Archive pin is insufficient'))
    return checks


def runtime_inventory(repo=REPO):
    """Keep laboratory evidence distinct from actual system acceptance."""
    records = []
    for rel,allowed_sources in RUNTIME_SOURCES.items():
        try:
            raw = read_public(repo/rel,1024**2); doc = evidence_document(raw)
            declared = doc.get('source_sha256',{})
            if not isinstance(declared,dict) or len(declared)>len(allowed_sources):
                raise ValueError('bounded historical source map required')
            matches = {}
            rejected = {}
            for name,digest in declared.items():
                # Historical user-owned JSON may not select a root, private
                # key, arbitrary absolute file or a new acceptance source.
                if name not in allowed_sources:
                    matches[name]=False; rejected[name]='source outside fixed historical allowlist';continue
                if not isinstance(digest,str) or not re.fullmatch('[0-9a-f]{64}',digest):
                    matches[name]=False; rejected[name]='invalid historical source digest';continue
                try: matches[name]=sha(read_public(repo/name,64*1024**2))==digest
                except (OSError,ValueError): matches[name]=False
            records.append({'path':rel,'sha256':sha(raw),'scope':'historical unit/private-root or passive inventory only','source_hash_matches':matches,'rejected_source_entries':rejected,'usable_as_real_host_acceptance':False})
        except (OSError,ValueError) as error:
            records.append({'path':rel,'error':str(error),'usable_as_real_host_acceptance':False})
    return records


def report(repo=REPO):
    checks=[]; sources={}
    contract=validate_contract(default_contract())
    checks.append(gate('native-offline-contract',contract['status']=='PASS','Existing-domain contract and required negative matrix are represented without weakening policy'))
    try:
        raw=read_public(repo/PROBE_REL)
        current={name:sha(read_public(path)) for name,path in PUBLIC_PATHS.items()}
        checks.extend(evaluate_cached_probe(raw,current))
    except (OSError,ValueError,KeyError,json.JSONDecodeError) as error:
        checks.append(gate('native-public-evidence',False,str(error)))
        checks.append(gate('archive-native-trust',False,'No current verified public native evidence; unknown blocks'))
    for rel in ('services/update/src/aur/adapter.rs','services/update/src/aur/intake.rs','services/update/src/update_service.rs'):
        try:sources[rel]=sha(read_public(repo/rel,64*1024**2))
        except (OSError,ValueError) as error:sources[rel]={'error':str(error)}
    checks.extend([
        gate('native-production-integration',False,'Current AUR preparation reuses the official Alpm handle; no separately integrated AUR domain/process. Shared native trust changes require separate trust/official-authority review; a separate-domain decision and integration remain unaccepted. No native domain or trust change is authorized by this inventory'),
        gate('runtime-polkit-recovery-acceptance',False,'Canonical and candidate production installation gates remain closed. Public prerequisites are not runtime acceptance. Real system-bus PolicyKit/session, worker, persistent journal, snapshots, boot/recovery and failure acceptance evidence is missing; unit/private-root success cannot replace it'),
        gate('disposable-system-acceptance-infrastructure',False,'Approved disposable VM/image/guest identity and reviewed guest-only capability/harness remain required; no host installation or gate bypass is authorized'),
    ])
    return {'schema':1,'scope':'offline readiness only; no deployment/activation authority','status':'BLOCKED','production_authorized':False,'checks':checks,'source_sha256':sources,'cached_runtime_records':runtime_inventory(repo),'contract':contract,'operations_performed':{'key_operation':False,'native_transaction':False,'service_change':False,'policykit_action':False,'host_write':False}}


if __name__=='__main__':
    print(json.dumps(report(),indent=2))
    raise SystemExit(2)
