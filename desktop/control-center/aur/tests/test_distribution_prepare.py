"""Negative trust gates; no private keys, signing, installation or host writes."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from distribution_prepare import primary, allowlist
from trust import approved_signers
from worker import Rejected


class DistributionRules(unittest.TestCase):
    fingerprint = 'A' * 40

    def status(self, suffix=''):
        return ('[GNUPG:] VALIDSIG ' + 'B' * 40 + ' 2026-01-01 1 0 4 0 1 8 00 ' +
                self.fingerprint + '\n' + suffix)

    def test_signing_subkey_maps_to_primary(self):
        self.assertEqual(primary(self.status()), self.fingerprint)

    def test_crypto_validity_does_not_imply_native_trust(self):
        for status in (self.status(), self.status('[GNUPG:] TRUST_MARGINAL 0 pgp')):
            with self.assertRaises(ValueError): primary(status, require_trust=True)
        self.assertEqual(primary(self.status('[GNUPG:] TRUST_FULLY 0 pgp'), True), self.fingerprint)

    def test_revoked_expired_and_multiple_signatures_fail_closed(self):
        for token in ('REVKEYSIG', 'EXPKEYSIG', 'BADSIG', 'KEYEXPIRED', 'NO_PUBKEY'):
            with self.subTest(token=token), self.assertRaises(ValueError):
                primary(self.status('[GNUPG:] ' + token + ' deadbeef'))
        with self.assertRaises(ValueError): primary(self.status() * 2)

    def test_full_fingerprint_and_32_signer_bound(self):
        values = [f'{i:040X}' for i in range(33)]
        self.assertEqual(len(allowlist(values[:32])), 32)
        for value in ([], values, ['abc'], ['a' * 40]):
            with self.subTest(value=value), self.assertRaises(ValueError): allowlist(value)

    def test_runtime_policy_rejects_duplicate_overflow_and_wrong_json_type(self):
        for value in ([self.fingerprint] * 33, [self.fingerprint] * 2,
                      self.fingerprint, {self.fingerprint: True}, [None], []):
            with self.subTest(value=value), self.assertRaises(Rejected): approved_signers(value)
        self.assertEqual(approved_signers([self.fingerprint]), frozenset([self.fingerprint]))


if __name__ == '__main__': unittest.main()
