"""Signature-policy parsing regressions; mocked status is not crypto evidence."""
import json
from pathlib import Path
import stat
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from trust import TrustPolicy
from worker import Rejected


class TrustStatus(unittest.TestCase):
    primary = 'A' * 40
    valid = ('[GNUPG:] VALIDSIG ' + 'B' * 40 + ' 2026-11-05 1 0 4 0 22 8 00 ' + primary + '\n').encode()

    def verify(self, output, returncode=0):
        policy = TrustPolicy(SimpleNamespace(fd=3), frozenset([self.primary]), 'production-policy-only')
        with patch('trust.subprocess.run', return_value=SimpleNamespace(returncode=returncode, stdout=output)):
            return policy.verify(SimpleNamespace(fd=4), SimpleNamespace(fd=5))

    def test_primary_is_pinned_instead_of_subkey(self):
        self.assertEqual(self.verify(self.valid), self.primary)
        with self.assertRaises(Rejected): self.verify(self.valid.replace(self.primary.encode(), b'C' * 40))

    def test_expiry_revocation_and_failure_override_validsig(self):
        for token in ('BADSIG','ERRSIG','EXPSIG','EXPKEYSIG','REVKEYSIG','KEYEXPIRED','SIGEXPIRED','KEYREVOKED','NO_PUBKEY','FAILURE','NODATA'):
            with self.subTest(token=token), self.assertRaises(Rejected):
                self.verify(self.valid + ('[GNUPG:] ' + token + ' untrusted\n').encode())

    def test_duplicate_truncated_or_oversized_status_is_rejected(self):
        for output in (self.valid * 2, b'[GNUPG:] VALIDSIG A', b'x' * (1024**2 + 1)):
            with self.subTest(size=len(output)), self.assertRaises(Rejected): self.verify(output)
        with self.assertRaises(Rejected): self.verify(self.valid, returncode=1)

    def test_boolean_and_float_project_schema_cannot_become_production_policy(self):
        for schema in (True, 1.0):
            with self.subTest(schema=schema), tempfile.TemporaryDirectory() as directory:
                path = Path(directory)/'policy.json'
                path.write_text(json.dumps({'schema':schema,'purpose':'zethropol-aur-trust','manifest':{},'receipt':{},'archive':{}}))
                root_metadata = SimpleNamespace(st_mode=stat.S_IFDIR | 0o755, st_uid=0)
                with patch('trust.Path.lstat', return_value=root_metadata), self.assertRaisesRegex(Rejected, 'versioned project trust role'):
                    TrustPolicy.load(path, role='manifest')
