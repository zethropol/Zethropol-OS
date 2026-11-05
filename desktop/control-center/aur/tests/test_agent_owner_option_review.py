import hashlib,json,sys
from pathlib import Path
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import agent_owner_option_review as review

class OptionReview(unittest.TestCase):
    def describe(self,raw):return review.describe(raw,hashlib.sha256(raw).hexdigest())
    def test_known_name_only_and_effect_with_no_acceptance(self):
        result=self.describe(b'pinentry-program /private/SECRET\n')
        self.assertEqual(result['options'][0]['option'],'pinentry-program')
        self.assertEqual(result['options'][0]['effect'],'pinentry-execution')
        self.assertEqual(result['options'][0]['policy_acceptance'],'NOT_APPROVED')
        self.assertNotIn('SECRET',json.dumps(result));self.assertNotIn('/private',json.dumps(result))
    def test_arbitrary_tokens_comments_and_values_redacted(self):
        result=self.describe(b'# SECRET\nSECRET SECRET\nlog-file=SECRET\n--homedir SECRET\n')
        self.assertNotIn('SECRET',json.dumps(result))
        self.assertEqual(result['options'][0]['option'],'UNRECOGNIZED_REDACTED')
        self.assertEqual(result['options'][1]['option'],'log-file')
        self.assertEqual(result['options'][2]['option'],'UNRECOGNIZED_REDACTED')
    def test_exact_hash_required_before_any_classification(self):
        with self.assertRaises(ValueError):review.describe(b'pinentry-program SECRET\n')
    def test_review_stays_blocked_even_for_known_restrictions(self):
        result=self.describe(b'no-allow-loopback-pinentry\n')
        self.assertEqual(result['classification'],'REQUIRES_REVIEW')
        self.assertEqual(result['version_support'],'NOT_VERIFIED')
    def test_no_gpg_or_file_operation_in_parser(self):
        with patch('os.open',side_effect=AssertionError()):self.describe(b'disable-check-own-socket\n')
