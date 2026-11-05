"""Capture checks and provider consumption of the unchanged 300-second budget.

The RuntimeBudget is used only after provider.py reads a root-owned version-2
broker job. Offline JSON assertions never establish authority. This module
does not build, sign, access keys or change system configuration.
"""
import argparse
import hashlib
import json
import os
import stat
import sys
from pathlib import Path, PurePosixPath
import re
import uuid
import time

LIMIT_SECONDS = 300
NS = 1_000_000_000
ROLE_UIDS = {'archive': 951, 'receipt': 950}
PHASES = ('archive_rebuild', 'archive_sign_and_attach', 'attestor_rebuild',
          'receipt_sign_and_publish', 'broker_admission')
CONTEXT = {'operation_id', 'uid', 'bus_name', 'session_id', 'source_package',
           'source_version', 'architecture', 'source_plan_sha256', 'build_root_sha256'}
EXPECTED = CONTEXT | {'outputs', 'root_tree_sha256'}


class CaptureRejected(ValueError):
    pass


def runtime_clock():
    """Real no-agent clock sample; CLOCK_BOOTTIME includes suspend time."""
    return (time.time_ns(),time.clock_gettime_ns(time.CLOCK_BOOTTIME),
            Path('/proc/sys/kernel/random/boot_id').read_text().strip())


class RuntimeBudget:
    """Consume a root broker-issued absolute deadline; never start a fresh TTL.

    Authentication is supplied by the root-owned job reader, not this class.
    Test clock injection does not create broker authority or signed evidence.
    """
    def __init__(self,frame,clock=None):
        exact(frame,{'schema','purpose','boot_id','issued_boottime_ns','deadline_boottime_ns',
                     'captured_boottime_ns','captured_wall_ns'},'server provider budget')
        require(type(frame['schema']) is int and frame['schema']==1 and
                frame['purpose']=='zethropol-provider-reservation-budget','Server budget schema required')
        for field in ('issued_boottime_ns','deadline_boottime_ns','captured_boottime_ns','captured_wall_ns'):
            uint(frame[field],field)
        try:valid=str(uuid.UUID(frame['boot_id']))==frame['boot_id']
        except (ValueError,TypeError,AttributeError):valid=False
        require(valid,'Canonical budget boot required')
        start=frame['issued_boottime_ns'];end=frame['deadline_boottime_ns'];sample=frame['captured_boottime_ns']
        require(end-start==LIMIT_SECONDS*NS and start<=sample<end and frame['captured_wall_ns']>0,
                'Exact original 300-second broker reservation required')
        self.frame=dict(frame);self.clock=clock or runtime_clock
        self.wall_deadline=frame['captured_wall_ns']+(end-sample)
        self._last_wall=frame['captured_wall_ns']
        self._last_boot=frame['captured_boottime_ns']
        uint(self.wall_deadline,'derived wall deadline')
        self.checkpoint()

    def checkpoint(self):
        wall,boot,boot_id=self.clock()
        uint(wall,'runtime wall sample');uint(boot,'runtime boottime sample')
        f=self.frame
        require(boot_id==f['boot_id'],'Provider reservation boot changed')
        require(wall>=self._last_wall and boot>=self._last_boot,
                'Provider clock regressed since previous checkpoint')
        require(f['captured_boottime_ns']<=boot<f['deadline_boottime_ns'] and
                f['captured_wall_ns']<=wall<self.wall_deadline,'Provider reservation expired or clock rolled back')
        require(abs((wall-f['captured_wall_ns'])-(boot-f['captured_boottime_ns']))<=NS,
                'Provider wall/boottime drift detected')
        self._last_wall=wall;self._last_boot=boot
        return min(f['deadline_boottime_ns']-boot,self.wall_deadline-wall)

    def timeout_seconds(self):
        remaining=self.checkpoint()//NS
        require(remaining>0,'Insufficient provider reservation budget')
        return min(remaining,LIMIT_SECONDS)


def require(ok, reason):
    if not ok:
        raise CaptureRejected(reason)


def exact(value, fields, label):
    require(isinstance(value, dict) and set(value) == fields, 'Exact ' + label + ' fields required')


def uint(value, label):
    require(type(value) is int and 0 <= value <= 2**64-1, 'Unsigned integer ' + label + ' required')
    return value


def sha(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None


def identity(value):
    return isinstance(value, str) and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9@._+:~\-]{0,199}', value) is not None


def unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate capture JSON field')
        result[key] = value
    return result


def validate_expected(expected):
    exact(expected, EXPECTED, 'server expectation')
    try:
        require(str(uuid.UUID(expected['operation_id'])) == expected['operation_id'], 'Canonical operation UUID required')
    except (ValueError, TypeError, AttributeError) as error:
        raise CaptureRejected('Canonical operation UUID required') from error
    require(uint(expected['uid'], 'owner UID') > 0 and expected['uid'] != ROLE_UIDS['receipt'],
            'Original owner cannot be the independent attestor')
    require(isinstance(expected['bus_name'], str) and expected['bus_name'].startswith(':') and
            isinstance(expected['session_id'], str) and bool(expected['session_id']), 'Bound server caller context required')
    require(identity(expected['source_package']) and identity(expected['source_version']), 'Bound package identity required')
    require(expected['architecture'] in ('x86_64', 'aarch64'), 'Supported target architecture required')
    require(all(sha(expected[k]) for k in ('source_plan_sha256', 'build_root_sha256', 'root_tree_sha256')),
            'Pinned source plan, signed manifest and measured tree required')
    outputs = expected['outputs']
    require(isinstance(outputs, list) and 1 <= len(outputs) <= 64, 'Bounded selected outputs required')
    for output in outputs:
        exact(output, {'name', 'version', 'filename'}, 'selected output')
        require(all(identity(output[k]) for k in output) and '.pkg.tar' in output['filename'] and
                not output['filename'].endswith('.sig'), 'Full package archive identity required')
    require([o['name'] for o in outputs] == sorted({o['name'] for o in outputs}) and
            len({o['filename'] for o in outputs}) == len(outputs), 'Unique name-sorted selected outputs required')
    return expected


def validate_rebuild(capture, expected):
    """Validate capture semantics only; deliberately does not authenticate claims."""
    validate_expected(expected)
    exact(capture, {'schema', 'purpose', 'scope', 'context', 'root_provenance', 'builds'}, 'capture')
    require(type(capture['schema']) is int and capture['schema'] == 1 and
            capture['purpose'] == 'zethropol-independent-rebuild-capture', 'Explicit capture schema/purpose required')
    require(capture['scope'] in ('synthetic-fixture', 'unverified-host-capture'), 'Known non-authoritative evidence scope required')
    exact(capture['context'], CONTEXT, 'capture caller context')
    uint(capture['context']['uid'], 'capture owner UID')
    require(capture['context'] == {k: expected[k] for k in CONTEXT}, 'Stale or changed server review/caller context')
    root = capture['root_provenance']
    exact(root, {'scope', 'manifest_sha256', 'measured_tree_sha256', 'signature_verified'}, 'root provenance claim')
    require(root['scope'] == 'production-policy-only' and root['manifest_sha256'] == expected['build_root_sha256'] and
            root['measured_tree_sha256'] == expected['root_tree_sha256'] and root['signature_verified'] is True,
            'Unsigned/historical or changed root provenance cannot stand in for signed production root')
    exact(capture['builds'], set(ROLE_UIDS), 'provider pair')
    all_outputs = []
    for role, pinned_uid in ROLE_UIDS.items():
        build = capture['builds'][role]
        exact(build, {'uid', 'real_uid', 'effective_uid', 'method', 'fresh_work', 'fresh_cache',
                      'state', 'cleanup', 'workdir', 'cachedir', 'outputs'}, role + ' build')
        require(all(type(build[k]) is int and build[k] == pinned_uid for k in ('uid','real_uid','effective_uid')),
                'Independent actual real/effective provider UID must match pinned role: ' + role)
        require(build['method'] == 'pipeline.run/makepkg-package' and build['state'] == 'built-untrusted',
                'Complete isolated makepkg package build required; diagnostic binary is insufficient')
        require(build['fresh_work'] is True and build['fresh_cache'] is True, 'Fresh work and cache capture required')
        require(build['cleanup'] == 'verified', 'Interrupted/unknown cleanup cannot authorize receipt')
        home = '/var/lib/zethropol-aur-' + ('archive' if role == 'archive' else 'attestor') + '/work/' + expected['operation_id'] + '/'
        for field in ('workdir', 'cachedir'):
            path = build[field]
            require(isinstance(path, str) and '\x00' not in path and len(path.encode()) <= 4096 and
                    path.startswith(home) and str(PurePosixPath(path)) == path and
                    '..' not in PurePosixPath(path).parts, 'Fixed private provider workspace path required')
        require(build['workdir'] != build['cachedir'], 'Independent fresh cache may not alias workspace')
        outputs = build['outputs']
        require(isinstance(outputs, list) and len(outputs) == len(expected['outputs']), 'Complete selected package output set required')
        observed = {}
        for output in outputs:
            exact(output, {'filename','sha256','metadata'}, 'measured package output')
            require(identity(output['filename']) and output['filename'] not in observed and sha(output['sha256']),
                    'Unique bounded output filename/hash required')
            exact(output['metadata'], {'pkgname','pkgver','pkgbase','arch'}, 'native package metadata')
            observed[output['filename']] = output
        require(set(observed) == {o['filename'] for o in expected['outputs']}, 'Exact selected output filenames required')
        for output in expected['outputs']:
            measured = observed[output['filename']]['metadata']
            require((measured['pkgname'], measured['pkgver'], measured['pkgbase']) ==
                    (output['name'], output['version'], expected['source_package']) and
                    measured['arch'] in (expected['architecture'], 'any'), 'Measured native package identity mismatch')
        all_outputs.append(observed)
    require(all_outputs[0] == all_outputs[1], 'Independent rebuild output hash or metadata differs')
    return {'semantic_status':'PASS', 'evidence_scope':capture['scope'], 'outputs_verified':len(all_outputs[0]),
            'runtime_status':'BLOCKED', 'reason':'Raw capture claims are not authenticated provider/broker evidence; existing signature, native, context/nonce/replay verification remains required',
            'production_authorized':False}


def validate_budget(timing, now_wall, now_monotonic_ns, boot_id):
    """Check the existing critical path under both clocks, without extending TTL."""
    exact(timing, {'reservation','receipt','manifest_expires_at','segments'}, 'timing capture')
    wall = uint(now_wall, 'current wall time'); mono = uint(now_monotonic_ns, 'current monotonic time')
    reservation = timing['reservation']
    exact(reservation, {'issued_at','expires_at','issued_monotonic_ns','deadline_monotonic_ns','boot_id'}, 'reservation timing')
    issued = uint(reservation['issued_at'], 'reservation issue'); expires = uint(reservation['expires_at'], 'reservation expiry')
    start = uint(reservation['issued_monotonic_ns'], 'reservation monotonic issue')
    deadline = uint(reservation['deadline_monotonic_ns'], 'reservation monotonic deadline')
    try:
        require(isinstance(boot_id,str) and str(uuid.UUID(boot_id)) == boot_id and reservation['boot_id'] == boot_id,
                'Same canonical boot identity required')
    except (ValueError, TypeError, AttributeError) as error:
        raise CaptureRejected('Same canonical boot identity required') from error
    require(0 < issued <= wall < expires and 0 < expires-issued <= LIMIT_SECONDS and
            start <= mono < deadline and 0 < deadline-start <= LIMIT_SECONDS*NS,
            'Reservation expired, rolled back or exceeds unchanged 300-second budget')
    require(abs((wall-issued)*NS-(mono-start)) <= NS, 'Wall/monotonic clock drift or rollback detected')
    receipt = timing['receipt']; exact(receipt, {'issued_at','expires_at'}, 'receipt timing')
    ri = uint(receipt['issued_at'], 'receipt issue'); rexp = uint(receipt['expires_at'], 'receipt expiry')
    manifest_expiry = uint(timing['manifest_expires_at'], 'manifest expiry')
    require(issued <= ri <= wall < rexp and 0 < rexp-ri <= LIMIT_SECONDS, 'Receipt future/expired or lifetime exceeds 300 seconds')
    require(wall < manifest_expiry <= wall+86400, 'Current nonexpired Manifest required')
    segments = timing['segments']
    require(isinstance(segments,list) and len(segments)==len(PHASES), 'All current sequential handoff phases must be measured')
    previous_wall, previous_mono = issued,start
    durations = {}
    for phase,segment in zip(PHASES,segments):
        exact(segment, {'phase','start_wall','end_wall','start_monotonic_ns','end_monotonic_ns'}, 'phase timing')
        require(segment['phase']==phase, 'Current provider dependency order cannot be skipped or overlapped')
        sw=uint(segment['start_wall'],'phase wall start'); ew=uint(segment['end_wall'],'phase wall end')
        sm=uint(segment['start_monotonic_ns'],'phase monotonic start'); em=uint(segment['end_monotonic_ns'],'phase monotonic end')
        require(previous_wall <= sw <= ew <= wall and previous_mono <= sm <= em <= mono,
                'Missing, future, overlapping or reversed phase measurement')
        require(abs((ew-sw)*NS-(em-sm)) <= NS, 'Phase dual-clock mismatch')
        durations[phase]=em-sm;previous_wall,previous_mono=ew,em
    require(segments[3]['start_wall'] <= receipt['issued_at'] <= segments[3]['end_wall'],
            'Receipt issue time must occur after independent rebuild during signing/publishing phase')
    effective_wall = min(expires,rexp,manifest_expiry)
    require(wall < effective_wall, 'Effective admission deadline expired')
    return {'semantic_status':'PASS','reservation_lifetime_limit_seconds':LIMIT_SECONDS,
            'critical_path_elapsed_ns':mono-start,'remaining_monotonic_ns':deadline-mono,
            'remaining_wall_seconds':effective_wall-wall,'phase_durations_ns':durations,
            'production_authorized':False,'runtime_status':'BLOCKED',
            'scope':'Timing capture semantics only; timestamps must still be measured by authenticated runtime'}


def read_public_json(path):
    """Bounded no-follow snapshot of workspace public evidence; never executable."""
    path=Path(path).absolute()
    require('..' not in path.parts,'Canonical evidence path required')
    directory=os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
    try:
        for part in path.parts[1:-1]:
            child=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW,dir_fd=directory)
            os.close(directory);directory=child
        pin=os.open(path.name,os.O_PATH|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=directory)
    finally:os.close(directory)
    try:
        before=os.fstat(pin)
        require(stat.S_ISREG(before.st_mode) and before.st_nlink==1 and before.st_size<=1024**2,
                'Bounded single-link public evidence file required')
        fd=os.open('/proc/'+str(os.getpid())+'/fd/'+str(pin),os.O_RDONLY|os.O_CLOEXEC)
        with os.fdopen(fd,'rb') as stream:
            data=stream.read(1024**2+1);after=os.fstat(stream.fileno())
            fields=lambda s:(s.st_dev,s.st_ino,s.st_mode,s.st_uid,s.st_gid,s.st_nlink,s.st_size,s.st_mtime_ns,s.st_ctime_ns)
            require(len(data)<=1024**2 and fields(before)==fields(after),'Evidence changed during read')
        return json.loads(data,object_pairs_hook=unique),hashlib.sha256(data).hexdigest()
    finally:os.close(pin)


def current_readiness(repo=None):
    """Reuse existing historical evidence by digest; never execute an old build."""
    repo = Path(repo) if repo is not None else Path(__file__).resolve().parents[3]
    historical = repo/'desktop/control-center/build/aur-yay-offline-20261101/diagnostic-rebuild.json'
    diagnostic,evidence_sha256=read_public_json(historical)
    checks = [
        {'gate':'independent-package-rebuild','status':'BLOCKED','reason':'Only same-user diagnostic binary evidence exists; full package archive and independent attestor are explicitly false',
         'reused_evidence_sha256':evidence_sha256},
        {'gate':'authenticated-signed-root','status':'BLOCKED','reason':'Fresh root Manifest signature, trusted provisioning provenance and current source-plan/root measurement are missing'},
        {'gate':'independent-signed-receipt','status':'BLOCKED','reason':'No real receipt from independent UID 950 with observed package/signature bytes and sealed broker admission'},
        {'gate':'measured-five-minute-budget','status':'BLOCKED','reason':'No wall+monotonic capture of Archive rebuild/sign+attach, Attestor rebuild, receipt sign+publish and broker admission inside existing 300-second reservation'},
    ]
    require(diagnostic.get('signed_build_root_used') is False and diagnostic.get('independent_attestor') is False and
            diagnostic.get('package_archive_created') is False, 'Historical evidence scope changed; review before reuse')
    return {'overall':'BLOCKED','production_authorized':False,'execution_performed':False,'checks':checks,
            'existing_provider_uids':ROLE_UIDS,'reservation_limit_seconds':LIMIT_SECONDS,
            'runtime_observations_required':list(PHASES),'existing_diagnostic_scope_preserved':True}


def preparation_spec():
    return {'schema':1,'purpose':'zethropol-independent-rebuild-preparation-only','approved':False,'executed':False,
            'roles':{'archive':{'uid':951,'home':'/var/lib/zethropol-aur-archive'},'receipt':{'uid':950,'home':'/var/lib/zethropol-aur-attestor'}},
            'inputs_required':['current separately signed Manifest','root-owned signed provisioning provenance','new measured root/source-plan review','fixed provider config and helpers','exact broker caller/context/nonce','complete selected output set'],
            'sequential_critical_path':list(PHASES),'reservation_limit_seconds':LIMIT_SECONDS,
            'capture_required':['real/effective UIDs','fresh work/cache identities','full package hashes and native metadata','verified cleanup','root Manifest/tree/source-plan bindings','boot ID and wall/monotonic boundaries for every phase'],
            'permitted_preparation':['schema/context/output/metadata/cleanup and dual-clock negative tests','reuse unchanged historical evidence hashes without treating diagnostic builds as independent acceptance'],
            'not_authorized':['Manifest or Archive/Receipt signing','owner approval creation','private key or agent use','root provisioning','package installation','service changes','production enablement'],
            'timing_decision':'Version-2 server jobs export the original persisted boottime reservation, not a new lifetime. RuntimeBudget bounds provider checkpoints, pipeline timeout and signing timeout under boottime/wall samples. Both provider rebuilds remain inside the original reservation. Real complete path acceptance is still required; never extend TTL or fabricate timestamps.',
            'evidence_boundary':'This spec and validator never promote synthetic or user-owned JSON to production evidence. Existing provider and broker authentication remains mandatory.'}


def clock_probe():
    """Real local clock/checkpoint prefix only; no fake receipt or provider run."""
    wall,boot,identity=runtime_clock();start=boot-boot%NS
    frame={'schema':1,'purpose':'zethropol-provider-reservation-budget','boot_id':identity,
           'issued_boottime_ns':start,'deadline_boottime_ns':start+LIMIT_SECONDS*NS,
           'captured_boottime_ns':boot,'captured_wall_ns':wall}
    budget=RuntimeBudget(frame);before=budget.checkpoint()
    h=hashlib.sha256()
    for _ in range(32):h.update(b'local unsigned clock-checkpoint probe'*32768)
    after=budget.checkpoint()
    return {'mechanism_status':'PASS','scope':'local unsigned clock/checkpoint prefix only; synthetic reservation, no broker authorization',
            'clock':'CLOCK_BOOTTIME + CLOCK_REALTIME','boot_id':identity,'elapsed_ns':before-after,
            'remaining_ns':after,'reservation_restarted':False,'signature_or_receipt_created':False,
            'provider_build_performed':False,'runtime_status':'BLOCKED','production_authorized':False}


def main(argv=None):
    """Offline capture entry point; input hashes never become authentication."""
    parser=argparse.ArgumentParser(description=__doc__)
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument('--readiness',action='store_true')
    mode.add_argument('--spec',action='store_true')
    mode.add_argument('--probe-budget',action='store_true')
    mode.add_argument('--validate-rebuild',type=Path,metavar='CAPTURE')
    mode.add_argument('--validate-budget',type=Path,metavar='TIMING')
    parser.add_argument('--expected',type=Path,metavar='SERVER_EXPECTATION_CAPTURE')
    parser.add_argument('--clock',type=Path,metavar='ADMISSION_CLOCK_CAPTURE')
    args=parser.parse_args(argv)
    if bool(args.expected)!=bool(args.validate_rebuild) or bool(args.clock)!=bool(args.validate_budget):
        parser.error('--validate-rebuild requires only --expected; --validate-budget requires only --clock')
    try:
        if args.validate_rebuild:
            capture,capture_hash=read_public_json(args.validate_rebuild)
            expected,expected_hash=read_public_json(args.expected)
            result=validate_rebuild(capture,expected)
            result['input_sha256']={'capture':capture_hash,'server_expectation_capture':expected_hash}
        elif args.validate_budget:
            timing,timing_hash=read_public_json(args.validate_budget)
            clock,clock_hash=read_public_json(args.clock)
            exact(clock,{'now_wall','now_monotonic_ns','boot_id'},'admission clock capture')
            result=validate_budget(timing,clock['now_wall'],clock['now_monotonic_ns'],clock['boot_id'])
            result['input_sha256']={'timing':timing_hash,'admission_clock_capture':clock_hash}
        elif args.probe_budget:
            result=clock_probe()
        else:
            result=preparation_spec() if args.spec else current_readiness()
        print(json.dumps(result,indent=2));return 0
    except (CaptureRejected,OSError,ValueError,TypeError,KeyError) as error:
        print(json.dumps({'semantic_status':'BLOCKED','runtime_status':'BLOCKED',
                          'production_authorized':False,'reason':str(error)}))
        return 2


if __name__ == '__main__':
    sys.exit(main())
