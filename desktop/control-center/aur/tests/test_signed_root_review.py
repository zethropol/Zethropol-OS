"""No signing, trust bootstrap, root extraction or key operations."""
from pathlib import Path
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import signed_root_review as r
from worker import Rejected


class SignedRootReview(unittest.TestCase):
    def test_manifest_expiry_change_invalidates_historical_provenance(self):
        plan={'packages':[{'name':'compiler','sha256':'a'*64}]}
        verified=[{'sha256':'a'*64,'signer':'B'*40,'scope':'production-policy-only'}]
        new=r.expected_provenance(plan,'c'*64,verified)
        r.check_provenance(new,new)
        with self.assertRaises(Rejected):r.check_provenance(dict(new,manifest_sha256='d'*64),new)
        with self.assertRaises(Rejected):r.check_provenance(dict(new,scope='isolated-test'),new)
        for rows in ([],[dict(verified[0],scope='isolated-test')],[dict(verified[0],sha256='e'*64)]):
            with self.assertRaises(Rejected):r.expected_provenance(plan,'c'*64,rows)

    def test_measurement_rejects_root_and_arbitrary_request_before_trust(self):
        with patch('signed_root_review.os.getuid',return_value=0),patch('signed_root_review.TrustPolicy.load') as trust:
            with self.assertRaises(Rejected):r.measure({})
            trust.assert_not_called()
        with patch('signed_root_review.os.getuid',return_value=1000),patch('signed_root_review.os.geteuid',return_value=1000),patch('signed_root_review.TrustPolicy.load') as trust:
            with self.assertRaises(Rejected):r.measure({'root':'historical'})
            trust.assert_not_called()

    def test_missing_manifest_signature_cannot_reach_root_or_recipe(self):
        request={k:'/unused' for k in ('manifest','manifest_signature','build_root','recipe')}
        request.update(identity={},sources=[],dependencies=[],packages=[{'archive':'/unused','signature':'/unused'}])
        policy=SimpleNamespace(deployment='production-policy-only',keyring=SimpleNamespace(close=lambda:None))
        with patch('signed_root_review.os.getuid',return_value=1000),patch('signed_root_review.os.geteuid',return_value=1000),\
             patch('signed_root_review.TrustPolicy.load',return_value=policy),patch('signed_root_review.os.open',side_effect=FileNotFoundError),\
             patch('signed_root_review.review') as review,patch('signed_root_review.root_path') as root:
            with self.assertRaises(FileNotFoundError):r.measure(request)
            review.assert_not_called();root.assert_not_called()

    def test_test_policy_never_certifies_a_root(self):
        request={k:'/unused' for k in ('manifest','manifest_signature','build_root','recipe')}
        request.update(identity={},sources=[],dependencies=[],packages=[{}])
        policy=SimpleNamespace(deployment='isolated-test',keyring=SimpleNamespace(close=lambda:None))
        with patch('signed_root_review.os.getuid',return_value=1000),patch('signed_root_review.os.geteuid',return_value=1000),\
             patch('signed_root_review.TrustPolicy.load',return_value=policy),patch('signed_root_review.os.open') as opening:
            with self.assertRaises(Rejected):r.measure(request)
            opening.assert_not_called()


if __name__=='__main__':unittest.main()
