import json
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import agent_owner_config_review as review

class ConfigReview(unittest.TestCase):
    def test_safe_subset(self):
        result=review.classify(b'# private comment\nno-allow-loopback-pinentry\nno-allow-external-cache\ndefault-cache-ttl 0\nmax-cache-ttl 0\n')
        self.assertEqual(result['classification'],'SAFE_SUBSET_CANDIDATE')
        self.assertNotIn('private',json.dumps(result))
    def test_unknown_option_values_and_comments_never_emitted(self):
        for raw in (b'log-file /sensitive/path\n',b'pinentry-program SECRET\n',b'include SECRET\n',b'--homedir SECRET\n',b'passphrase SECRET\n',b'default-cache-ttl SECRET\n'):
            result=review.classify(raw)
            self.assertEqual(result['classification'],'REQUIRES_REVIEW')
            self.assertNotIn('SECRET',json.dumps(result));self.assertNotIn('/sensitive',json.dumps(result))
    def test_duplicates_and_bounds_block(self):
        for raw in (b'max-cache-ttl 0\nmax-cache-ttl 1',b'max-cache-ttl 99999999',b'\xff',b'x'*16385):
            self.assertEqual(review.classify(raw)['classification'],'REQUIRES_REVIEW')
    def test_parser_has_no_side_effect_calls(self):
        from unittest.mock import patch
        with patch('os.open',side_effect=AssertionError()):
            self.assertEqual(review.classify(b'')['classification'],'SAFE_SUBSET_CANDIDATE')
