import hashlib,sys,json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import agent_pinentry_route_review as review

class PinentryRoute(unittest.TestCase):
    def test_exact_hash_and_absolute_normalized_path(self):
        with self.assertRaises(ValueError):review.target(b'pinentry-program /SECRET\n')
        for path in ('relative','/a/../b','/a//b'):
            raw=('pinentry-program '+path+'\n').encode()
            with patch.object(review,'EXPECTED',hashlib.sha256(raw).hexdigest()):
                with self.assertRaises(ValueError):review.target(raw)
    def test_target_never_executed_and_values_redacted(self):
        raw=b'pinentry-program /SECRET/program\n'
        with patch.object(review,'EXPECTED',hashlib.sha256(raw).hexdigest()),patch.object(review,'trusted',return_value={'uid949_dac_access':'PASS'}),patch.object(review,'query',return_value=SimpleNamespace(returncode=0,stdout=b'pinentry\n')) as query:
            result=review.inspect(raw)
            self.assertNotIn('SECRET',json.dumps(result))
            self.assertFalse(result['pinentry_executed']);self.assertFalse(result['headless_pinentry_required'])
            self.assertTrue(all(call.args[0][0]=='/usr/bin/pacman' for call in query.call_args_list))
    def test_route_delegates_to_host_identity_checked_probe(self):
        with patch('native_socket_route_readonly.probe',return_value={'route_binding':'NOT_VERIFIED'}) as probe:
            self.assertEqual(review.route(),{'route_binding':'NOT_VERIFIED'})
            probe.assert_called_once_with()
    def test_untrusted_target_blocks_without_execution(self):
        with patch.object(review,'target',return_value='/SECRET'),patch.object(review,'trusted',side_effect=ValueError('SECRET')),patch.object(review,'query') as query:
            result=review.inspect(b'');query.assert_not_called();self.assertNotIn('SECRET',json.dumps(result))
