import sys
from pathlib import Path
from types import SimpleNamespace
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import agent_owner_config_review as review

class MetadataReview(unittest.TestCase):
    def sample(self,mode=0o644,**changes):
        values=dict(st_mode=0o100000|mode,st_uid=949,st_gid=948,st_nlink=1,st_size=42)
        values.update(changes);return SimpleNamespace(**values)
    def test_observed_host_metadata_accepted_for_review_only(self):
        self.assertEqual(review.metadata_rejections(self.sample(),[]),[])
        self.assertEqual(review.metadata_rejections(self.sample(0o600),[]),[])
    def test_write_execute_special_and_unapproved_modes_rejected(self):
        for mode in (0o666,0o664,0o640,0o755,0o4644,0o2644,0o1644):
            with self.subTest(mode=mode):self.assertIn('mode',review.metadata_rejections(self.sample(mode),[]))
    def test_owner_links_acl_type_size_guards_retained(self):
        for changes,acls,label in ((dict(st_uid=0),[],'owner-group'),(dict(st_gid=0),[],'owner-group'),(dict(st_nlink=2),[],'link-count'),({},['system.posix_acl_access'],'acl'),(dict(st_mode=0o120644),[],'not-regular'),(dict(st_size=16385),[],'size')):
            with self.subTest(label=label):self.assertIn(label,review.metadata_rejections(self.sample(**changes),acls))
    def test_accepted_metadata_does_not_accept_unknown_configuration(self):
        self.assertEqual(review.metadata_rejections(self.sample(),[]),[])
        result=review.classify(b'log-file /private/location\n')
        self.assertEqual(result['classification'],'REQUIRES_REVIEW')
        self.assertNotIn('private',str(result))
