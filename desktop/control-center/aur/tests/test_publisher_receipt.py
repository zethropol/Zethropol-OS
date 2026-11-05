"""Publisher context/bytes gates with explicit data fixtures, not attestations."""
import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from provider import validate_receipt, measured_build
from worker import Rejected


class PublisherReceipt(unittest.TestCase):
    def fixture(self):
        context={'operation_id':'11111111-1111-4111-8111-111111111111','uid':1000,'bus_name':':1.22','session_id':'c2'}
        review={'source_package':'demo','source_version':'1-1','architecture':'x86_64',
                'build_root_sha256':'a'*64,'source_plan_sha256':'b'*64,
                'outputs':[{'name':'demo','version':'1-1','filename':'demo.pkg.tar.zst'}]}
        package={'name':'demo','version':'1-1','filename':'demo.pkg.tar.zst','architecture':'any','sha256':'c'*64,'signature_sha256':'d'*64}
        receipt=dict(context,**{k:v for k,v in review.items() if k!='outputs'},schema=1,purpose='zethropol-aur-build-receipt',issued_at=1000,expires_at=1240,packages=[package])
        return receipt,dict(context,review=review),{'demo.pkg.tar.zst':('c'*64,'d'*64)}

    def test_valid_fixture_and_strict_lifetime(self):
        receipt,value,observed=self.fixture();validate_receipt(receipt,value,observed,1010)
        for fields in ({'schema':True},{'issued_at':1011},{'issued_at':True},{'expires_at':1010},
                       {'issued_at':1},{'expires_at':1500},{'purpose':'worker-result'},{'extra':1},{'uid':True}):
            with self.subTest(fields=fields),self.assertRaises(Rejected):
                validate_receipt(dict(receipt,**fields),value,observed,1010)

    def test_all_output_bytes_and_identity_are_required(self):
        receipt,value,observed=self.fixture()
        for key,changed in [('sha256','e'*64),('signature_sha256','e'*64),('architecture','arm'),('name','other'),('version','2-1'),('filename','other.pkg.tar')]:
            bad=copy.deepcopy(receipt);bad['packages'][0][key]=changed
            with self.assertRaises(Rejected):validate_receipt(bad,value,observed,1010)
        for packages in ([],receipt['packages']*2,[dict(receipt['packages'][0],extra=1)]):
            with self.assertRaises(Rejected):validate_receipt(dict(receipt,packages=packages),value,observed,1010)
        with self.assertRaises(Rejected):validate_receipt(receipt,value,{},1010)

    def test_original_uid_cannot_attest_to_own_build(self):
        with patch('provider.actor',return_value=('unused',Path('/unused'))),patch('provider.os.getuid',return_value=1000):
            with self.assertRaisesRegex(Rejected,'independent attestor'):
                measured_build({'uid':1000},'receipt','unused')


if __name__=='__main__':unittest.main()
