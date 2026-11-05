"""Synthetic failure injection only: no signer process, key, agent or host writes."""
import errno
import json
from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import manifest_workflow as flow
import manifest_failure_capture as capture

D='a'*64


class FailureCapture(unittest.TestCase):
    def message(self,phase='gpg-signing',rc=2):
        return flow.failure_record(flow.SigningPhaseRejected(phase,rc,kind='backend-nonzero'),D).encode()

    def test_structured_record_roundtrip_binds_manifest_and_backend_code(self):
        result=capture.process_outcome(1,self.message(),D)
        self.assertEqual(result['child_returncode'],1)
        self.assertEqual(result['diagnostic']['backend_returncode'],2)
        self.assertEqual(result['diagnostic']['phase'],'gpg-signing')
        self.assertFalse(result['retry_permitted'])

    def test_phase_and_errno_injection_are_public_and_separate(self):
        for phase,error,kind in (
            ('authorization-context',PermissionError(errno.EACCES,'SECRET /private/key'),'permission-denied'),
            ('policy-load',FileNotFoundError(errno.ENOENT,'SECRET /policy'),'os-error'),
            ('gpg-signing',subprocess.TimeoutExpired(['SECRET'],1,stderr=b'SECRET'),'timeout'),
            ('signature-verification',flow.Rejected('SECRET signer mismatch'),'rejected'),
            ('signature-write',OSError(errno.ENOSPC,'SECRET /pending'),'os-error')):
            with self.subTest(phase=phase):
                try:
                    with flow.signing_phase(phase):raise error
                except flow.SigningPhaseRejected as rejected:
                    message=flow.failure_record(rejected,D).encode()
                self.assertNotIn(b'SECRET',message)
                decoded=capture.decode(message,D)
                self.assertEqual(decoded['phase'],phase)
                self.assertEqual(decoded['kind'],kind)
                self.assertEqual(decoded['os_errno'],error.errno if isinstance(error,OSError) else None)

    def test_legacy_generic_rejection_and_traceback_do_not_leak(self):
        for raw in (b'Manifest workflow rejected; no signing/publication authority inferred',
                    b'Traceback /private/key SECRET',b'\x1b[31mSECRET'):
            result=capture.process_outcome(1,raw,D)
            self.assertIsNone(result['diagnostic'])
            self.assertNotIn('SECRET',json.dumps(result))

    def test_malformed_extra_duplicate_mismatched_and_untyped_rejected(self):
        message=self.message();record=json.loads(message[len(capture.PREFIX):])
        bad=[]
        for field,value in (('schema',True),('phase',[]),('kind','SECRET'),('backend_returncode',True),
                            ('backend_returncode',0),('backend_returncode',999),('os_errno',True),
                            ('os_errno',0),('manifest_sha256','b'*64),('status','PASS')):
            bad.append(capture.PREFIX+json.dumps(dict(record,**{field:value})).encode())
        bad.extend([message+b'\n'+message,message+b' SECRET',b'SECRET\n'+message,
                    capture.PREFIX+json.dumps(dict(record,private='SECRET')).encode(),
                    capture.PREFIX+b'{"schema":1,"schema":1}',
                    capture.PREFIX+b'{"os_errno":NaN}'])
        for raw in bad:self.assertIsNone(capture.decode(raw,D))

    def test_zero_exit_never_becomes_signature_verification(self):
        result=capture.process_outcome(0,self.message(),D)
        self.assertEqual(result['status'],'SIGNER_COMPLETED_UNVERIFIED')
        self.assertFalse(result['signature_verified'])
        self.assertFalse(result['production_authorized'])
        self.assertIsNone(result['diagnostic'])

    def test_launch_timeout_signal_and_exit_are_distinct(self):
        for rc,flags,label in ((None,{'launch_failed':True},'launch-failed'),
                              (None,{'timed_out':True},'timeout'),(-9,{},'signal-exit'),(1,{},'nonzero-exit')):
            self.assertEqual(capture.process_outcome(rc,b'',D,**flags)['process_result'],label)
        for rc,flags in ((True,{}),(None,{}),(1,{'timed_out':True}),
                         (None,{'timed_out':True,'launch_failed':True})):
            with self.assertRaises(ValueError):capture.process_outcome(rc,b'',D,**flags)

    def test_capture_is_bounded_and_truncation_never_accepts_prefix(self):
        sink=capture.BoundedFailureCapture();sink.feed(self.message());sink.feed(b' '*8192)
        self.assertEqual(len(sink._buffer),4096)
        self.assertTrue(sink.overflow)
        self.assertIsNone(sink.outcome(1,D)['diagnostic'])
        self.assertIsNone(capture.decode(b'x'*4097,D))

    def test_chunked_capture_accepts_complete_record_without_raw_output(self):
        sink=capture.BoundedFailureCapture();message=self.message()
        for i in range(0,len(message),7):sink.feed(message[i:i+7])
        self.assertEqual(sink.outcome(1,D)['diagnostic']['phase'],'gpg-signing')

    def test_nonzero_exit_without_detail_never_invents_backend_cause(self):
        for child_rc in (1,2,126,-9):
            result=capture.process_outcome(child_rc,b'',D)
            self.assertEqual(result['child_returncode'],child_rc)
            self.assertIsNone(result['diagnostic'])
            self.assertEqual(result['status'],'BLOCKED')

    def test_adapter_persists_sanitized_detail_before_raising(self):
        sink=capture.BoundedFailureCapture();sink.feed(self.message());events=[]
        with self.assertRaises(capture.SignerProcessBlocked) as caught:
            capture.audit_process_result(sink,1,D,lambda name,**fields:events.append((name,fields)))
        self.assertEqual(events[0][0],'signer-process-outcome')
        self.assertEqual(events[0][1]['outcome'],caught.exception.outcome)
        self.assertEqual(caught.exception.outcome['diagnostic']['backend_returncode'],2)

    def test_adapter_audit_failure_is_not_swallowed_or_promoted(self):
        def failed_audit(*args,**kwargs):raise OSError(errno.ENOSPC,'audit full')
        with self.assertRaises(OSError):
            capture.audit_process_result(capture.BoundedFailureCapture(),0,D,failed_audit)

    def test_adapter_zero_exit_still_requires_independent_verification(self):
        events=[]
        result=capture.audit_process_result(capture.BoundedFailureCapture(),0,D,
                    lambda name,**fields:events.append(name))
        self.assertEqual(events,['signer-process-outcome'])
        self.assertEqual(result['status'],'SIGNER_COMPLETED_UNVERIFIED')
        self.assertFalse(result['signature_verified'])


if __name__=='__main__':unittest.main()
