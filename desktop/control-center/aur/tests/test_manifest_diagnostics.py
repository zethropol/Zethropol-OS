"""Fixed diagnostics only: no host state, GPG, key, agent or real signer execution."""
import sys
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import manifest_workflow as flow


class ManifestDiagnostics(unittest.TestCase):
    def test_each_phase_suppresses_private_exception_details(self):
        for phase in flow.SIGNING_PHASES:
            with self.subTest(phase=phase):
                with self.assertRaises(flow.SigningPhaseRejected) as caught:
                    with flow.signing_phase(phase):
                        raise PermissionError('PRIVATE KEY PATH / secret agent response')
                self.assertEqual(flow.failure_message(caught.exception),
                                 'ZETHROPOL_MANIFEST_FAILURE phase='+phase)

    def test_nested_phase_preserves_original_failure_and_returncode(self):
        with self.assertRaises(flow.SigningPhaseRejected) as caught:
            with flow.signing_phase('signature-verification'):
                with flow.signing_phase('gpg-signing'):
                    raise flow.SigningPhaseRejected('gpg-signing',2)
        self.assertEqual(flow.failure_message(caught.exception),
                         'ZETHROPOL_MANIFEST_FAILURE phase=gpg-signing returncode=2')

    def test_returncode_only_accepts_bounded_actual_integers(self):
        for value in (True,'secret',999999):
            self.assertIsNone(flow.SigningPhaseRejected('gpg-signing',value).returncode)
        with self.assertRaises(ValueError):
            flow.SigningPhaseRejected('untrusted private details')

    def test_timeout_does_not_report_captured_output(self):
        with self.assertRaises(flow.SigningPhaseRejected) as caught:
            with flow.signing_phase('gpg-signing'):
                raise subprocess.TimeoutExpired(['private-command'],60,output=b'secret',stderr=b'secret')
        self.assertEqual(flow.failure_message(caught.exception),
                         'ZETHROPOL_MANIFEST_FAILURE phase=gpg-signing')

    def test_other_failures_remain_generic(self):
        self.assertNotIn('secret',flow.failure_message(RuntimeError('secret')))

    def test_context_denial_never_reaches_gpg(self):
        # Invoke workspace logic with authorization mocked to fail immediately.
        # This does not invoke the installed signer or any host operation.
        with patch.object(flow,'context',side_effect=flow.Rejected('private')) as context, \
             patch.object(flow.subprocess,'run',side_effect=AssertionError('GPG forbidden')) as run:
            with self.assertRaises(flow.SigningPhaseRejected) as caught:
                flow.sign('a'*64)
            self.assertEqual(caught.exception.phase,'authorization-context')
            context.assert_called_once()
            run.assert_not_called()


if __name__=='__main__':unittest.main()
