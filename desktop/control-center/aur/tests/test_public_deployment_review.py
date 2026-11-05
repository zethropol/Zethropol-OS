"""Reject public-plan scope/hash/role drift. No installation or key operation."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('public_review',Path(__file__).resolve().parents[1]/'public_deployment_review.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


from historical_evidence import requires_historical_evidence

@requires_historical_evidence
class PublicPlanReview(unittest.TestCase):
    def setUp(self):
        self.plan=json.loads(m.read_regular(m.BASE/'next-public-deployment-plan.json'))
        self.blobs={str(path):m.read_regular(path) for path,_ in m.SPECS.values()}
        self.blobs['/etc/zethropol-update/aur-trust/policy.json']=m.read_regular('/etc/zethropol-update/aur-trust/policy.json')

    def review(self):return m.validate(self.plan,reader=lambda path:self.blobs[str(path)])

    def changed_policy(self,config):
        key='/etc/zethropol-update/aur-trust/distribution.json'
        row=next(r for r in self.plan['files'] if r['target']==key)
        data=json.dumps(config).encode();self.blobs[row['source']]=data;row['sha256']=hashlib.sha256(data).hexdigest()

    def test_exact_plan_has_only_four_reviewed_files(self):
        result=self.review();self.assertEqual(result['status'],'READY FOR OWNER REVIEW')
        self.assertFalse(result['deployment_authorized']);self.assertEqual(len(result['files']),4)

    def test_scope_target_and_owner_mode_drift_rejected(self):
        original=copy.deepcopy(self.plan)
        for field,value in [('target','/etc/sudoers'),('source','/etc/shadow'),('owner','949:948'),('mode','4755')]:
            self.plan=copy.deepcopy(original);self.plan['files'][0][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):self.review()
        self.plan=copy.deepcopy(original);self.plan['files'].append(copy.deepcopy(original['files'][0]))
        with self.assertRaises(ValueError):self.review()

    def test_policy_cannot_publish_before_keyring_or_helpers(self):
        self.plan['files']=list(reversed(self.plan['files']))
        with self.assertRaises(ValueError):self.review()

    def test_new_directory_or_deployment_authority_rejected(self):
        for field,value in [('directories_to_create',['/etc/new']),('approved',True),('applied',True)]:
            with self.subTest(field=field):
                previous=self.plan[field];self.plan[field]=value
                with self.assertRaises(ValueError):self.review()
                self.plan[field]=previous

    def test_modified_binary_hash_rejected(self):
        row=self.plan['files'][0];self.blobs[row['source']]+=b'changed'
        with self.assertRaises(ValueError):self.review()

    def test_ring_replacement_cannot_match_fixed_policy_digest(self):
        row=next(r for r in self.plan['files'] if r['target'].endswith('.gpg'))
        data=b'replacement';self.blobs[row['source']]=data;row['sha256']=hashlib.sha256(data).hexdigest()
        with self.assertRaises(ValueError):self.review()

    def test_policy_local_basename_required(self):
        source=next(r['source'] for r in self.plan['files'] if r['target'].endswith('.json'))
        config=json.loads(self.blobs[source]);config['keyring']='../manifest.gpg';self.changed_policy(config)
        with self.assertRaises(ValueError):self.review()

    def test_project_archive_key_cannot_be_distribution_authority(self):
        source=next(r['source'] for r in self.plan['files'] if r['target'].endswith('.json'))
        config=json.loads(self.blobs[source]);config['signers'].append(self.plan['project_role_signers']['archive'][0])
        self.plan['distribution_primary_signers']=sorted(config['signers']);self.changed_policy(config)
        with self.assertRaises(ValueError):self.review()

    def test_source_symlink_and_hardlink_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            source=Path(temporary)/'source';source.write_bytes(b'public')
            alias=Path(temporary)/'alias';alias.symlink_to(source)
            with self.assertRaises(ValueError):m.read_regular(alias)
            alias.unlink();os.link(source,alias)
            with self.assertRaises(ValueError):m.read_regular(source)

    def test_source_name_swap_reads_pinned_bytes(self):
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as temporary:
            source=Path(temporary)/'source';source.write_bytes(b'approved')
            original=os.open
            def swap(name,flags,*args,**kwargs):
                fd=original(name,flags,*args,**kwargs)
                if name=='source' and flags&os.O_PATH:
                    source.rename(source.with_name('original'));source.write_bytes(b'changed')
                return fd
            with patch.object(m.os,'open',side_effect=swap):self.assertEqual(m.read_regular(source),b'approved')


if __name__=='__main__':unittest.main()
