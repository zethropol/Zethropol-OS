"""Pure, bounded decoder for FUTURE reviewed launchers; never starts a process.

Diagnostics are assertions, not key-use evidence or permission. Do not feed
unbounded communicate()/capture_output buffers into a privileged audit path;
the future launcher must drain/discard excess bytes while retaining <=4096.
"""
import json
import re

PREFIX=b'ZETHROPOL_MANIFEST_FAILURE_JSON '
PROTOCOL_ID='zethropol-manifest-diagnostics/1'
PHASES=frozenset(('authorization-context','policy-load','manifest-authorization',
    'signature-buffer','gpg-signing','signature-verification','authorization-recheck','signature-write','unknown'))
KINDS=frozenset(('rejected','permission-denied','os-error','invalid-input','timeout','subprocess-error','backend-nonzero'))
FIELDS={'schema','protocol','purpose','status','manifest_sha256','phase','kind','os_errno','backend_returncode'}


def unique(pairs):
    result={}
    for k,v in pairs:
        if k in result:raise ValueError('Duplicate diagnostic field')
        result[k]=v
    return result


def integer(value,low,high):
    return type(value) is int and low<=value<=high


def invalid_constant(_):
    raise ValueError('Nonfinite diagnostic')


def decode(stderr,expected_digest):
    if not isinstance(expected_digest,str) or not re.fullmatch('[0-9a-f]{64}',expected_digest):return None
    if type(stderr) is not bytes or len(stderr)>4096 or not stderr.startswith(PREFIX):return None
    try:
        record=json.loads(stderr[len(PREFIX):].decode('ascii'),object_pairs_hook=unique,
                          parse_constant=invalid_constant)
        if not isinstance(record,dict) or set(record)!=FIELDS:return None
        if (type(record['schema']) is not int or record['schema']!=1 or
                record['protocol']!=PROTOCOL_ID or record['purpose']!='zethropol-manifest-signing-failure' or record['status']!='BLOCKED' or
                record['manifest_sha256']!=expected_digest or
                type(record['phase']) is not str or record['phase'] not in PHASES or
                type(record['kind']) is not str or record['kind'] not in KINDS):return None
        err=record['os_errno'];rc=record['backend_returncode']
        if err is not None and not integer(err,1,4095):return None
        if rc is not None and not integer(rc,-255,255):return None
        if rc is not None and (record['phase']!='gpg-signing' or record['kind']!='backend-nonzero' or rc==0):return None
        return record
    except (ValueError,UnicodeError,RecursionError):return None


def process_outcome(returncode,stderr,expected_digest,*,timed_out=False,launch_failed=False):
    """Sanitized audit value; no returned field can contain untrusted error text."""
    if not isinstance(expected_digest,str) or not re.fullmatch('[0-9a-f]{64}',expected_digest):
        raise ValueError('Exact expected Manifest digest required')
    if type(timed_out) is not bool or type(launch_failed) is not bool:
        raise ValueError('Typed process status required')
    if timed_out and launch_failed:raise ValueError('Ambiguous process failure')
    if returncode is not None and not integer(returncode,-255,255):
        raise ValueError('Typed process returncode required')
    if returncode is None and not (timed_out or launch_failed):
        raise ValueError('Missing process outcome')
    if returncode is not None and (timed_out or launch_failed):
        raise ValueError('Ambiguous process outcome')
    detail=decode(stderr,expected_digest) if returncode not in (None,0) else None
    return {'schema':1,'purpose':'zethropol-manifest-process-outcome','manifest_sha256':expected_digest,
            'status':'SIGNER_COMPLETED_UNVERIFIED' if returncode==0 else 'BLOCKED',
            'process_result':'timeout' if timed_out else 'launch-failed' if launch_failed else
                             'signal-exit' if returncode<0 else 'nonzero-exit' if returncode else 'zero-exit',
            'child_returncode':returncode,'diagnostic':detail,
            'diagnostic_status':'validated' if detail is not None else 'unavailable',
            'retry_permitted':False,'signature_verified':False,'production_authorized':False}


class BoundedFailureCapture:
    """Feed drained stderr chunks; excess is discarded, never audited verbatim."""
    def __init__(self):
        self._buffer=bytearray()
        self.overflow=False

    def feed(self,chunk):
        if type(chunk) is not bytes:raise ValueError('Byte chunk required')
        room=4096-len(self._buffer)
        self._buffer.extend(chunk[:room])
        if len(chunk)>room:self.overflow=True

    def outcome(self,returncode,expected_digest,**flags):
        return process_outcome(returncode,b'' if self.overflow else bytes(self._buffer),expected_digest,**flags)


class SignerProcessBlocked(RuntimeError):
    def __init__(self,outcome):
        self.outcome=outcome
        super().__init__('Signer process BLOCKED; inspect structured audit outcome')


def audit_process_result(capture,returncode,expected_digest,audit,**flags):
    """Future launcher adapter: persist sanitized outcome before fail-closed stop.

    The injected audit must fsync or raise. Zero exit remains unverified and
    must still pass separate signature/provenance checks. Never resumes/retries.
    """
    outcome=capture.outcome(returncode,expected_digest,**flags)
    audit('signer-process-outcome',outcome=outcome)
    if outcome['status']=='BLOCKED':raise SignerProcessBlocked(outcome)
    return outcome
