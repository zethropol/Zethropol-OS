"""Offline acceptance/data-boundary tests, not native trust establishment."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('native_acceptance',Path(__file__).resolve().parents[1]/'native_domain_acceptance.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


from historical_evidence import requires_historical_evidence

class NativeDomainAcceptance(unittest.TestCase):
    def native_capture(self):
        expected={'operation_id':'11111111-1111-4111-8111-111111111111','package_sha256':'a'*64,
                  'aur_snapshot_sha256':{'pubring.kbx':'b'*64,'trustdb.gpg':'c'*64},
                  'official_dependency_snapshot_sha256':{'pubring.kbx':'d'*64,'trustdb.gpg':'e'*64},
                  'official_repository_metadata_sha256':'f'*64}
        capture=copy.deepcopy(expected)
        capture.update(schema=1,scope='disposable-native-loader-no-transaction',
                       process_domain='/etc/zethropol-update/aur-native-trust',domain_bound_at_start=True,
                       domain_switched=False,native_transaction_started=False,installation_performed=False,
                       siglevel={'package_required':True,'package_optional':False,'marginal_ok':False,'unknown_ok':False},
                       signature={'status':'valid','trust':'full','primary':m.ARCHIVE_PRIMARY,'expired':False,'revoked':False})
        return capture,expected

    def test_native_capture_semantics_never_grant_acceptance(self):
        capture,expected=self.native_capture()
        result=m.validate_admission_capture(capture,expected)
        self.assertEqual(result['semantic_status'],'PASS')
        self.assertEqual(result['native_acceptance'],'BLOCKED')
        self.assertFalse(result['production_authorized'])

    def test_capture_requires_fixed_process_and_distinct_native_authority_snapshots(self):
        capture,expected=self.native_capture()
        for key,value in [('process_domain','/etc/pacman.d/gnupg'),('process_domain','/tmp/client'),
                          ('domain_bound_at_start',False),('domain_switched',True),
                          ('domain_bound_at_start',1),('domain_switched',0)]:
            with self.subTest(key=key,value=value),self.assertRaises(ValueError):
                m.validate_admission_capture(dict(capture,**{key:value}),expected)
        capture['aur_snapshot_sha256']=copy.deepcopy(expected['official_dependency_snapshot_sha256'])
        expected['aur_snapshot_sha256']=copy.deepcopy(expected['official_dependency_snapshot_sha256'])
        with self.assertRaisesRegex(ValueError,'shared native trust'):m.validate_admission_capture(capture,expected)

    def test_capture_rejects_nontrusted_wrong_expired_or_revoked_signature(self):
        capture,expected=self.native_capture()
        for key,value in [('status','invalid'),('trust','marginal'),('trust','unknown'),
                          ('primary','0'*40),('expired',True),('revoked',True),('expired',0),('revoked',0)]:
            bad=copy.deepcopy(capture);bad['signature'][key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError):m.validate_admission_capture(bad,expected)

    def test_capture_rejects_optional_flags_and_transaction_claims(self):
        capture,expected=self.native_capture()
        for key,value in [('package_required',False),('package_required',1),('package_optional',True),
                          ('marginal_ok',True),('unknown_ok',True),('package_optional',0)]:
            bad=copy.deepcopy(capture);bad['siglevel'][key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError):m.validate_admission_capture(bad,expected)
        for key,value in [('native_transaction_started',True),('installation_performed',True),
                          ('native_transaction_started',0),('installation_performed',0)]:
            with self.assertRaises(ValueError):m.validate_admission_capture(dict(capture,**{key:value}),expected)

    def test_capture_rejects_context_hash_drift_and_ambiguous_shape(self):
        capture,expected=self.native_capture()
        invalid=[dict(capture,schema=True),dict(capture,operation_id='22222222-2222-4222-8222-222222222222'),
                 dict(capture,package_sha256='0'*64),dict(capture,official_repository_metadata_sha256='0'*64),
                 dict(capture,aur_snapshot_sha256={'pubring.kbx':'0'*64,'trustdb.gpg':'c'*64}),
                 dict(capture,command='/usr/bin/gpg')]
        for bad in invalid:
            with self.assertRaises(ValueError):m.validate_admission_capture(bad,expected)
        bad=copy.deepcopy(expected);bad['aur_snapshot_sha256']['trustdb.gpg']=True
        with self.assertRaises(ValueError):m.validate_admission_capture(capture,bad)

    def test_existing_contract_pass_is_not_production_authority(self):
        result=m.validate_contract(m.default_contract())
        self.assertEqual(result['status'],'PASS');self.assertFalse(result['production_authorized'])

    def test_optional_marginal_unknown_or_unsigned_are_blocked(self):
        for key,value in [('required_package_signatures',False),('allow_optional',True),('allow_marginal',True),('allow_unknown',True)]:
            with self.subTest(key=key):
                c=m.default_contract();c[key]=value
                self.assertEqual(m.validate_contract(c)['status'],'BLOCKED')

    def test_client_path_process_switch_and_authority_widening_are_blocked(self):
        for key in ('allow_client_gpgdir','allow_process_domain_switch','archive_may_authorize_official_packages'):
            with self.subTest(key=key):
                c=m.default_contract();c[key]=True
                self.assertEqual(m.validate_contract(c)['status'],'BLOCKED')

    def test_separate_domain_remains_design_only_and_needs_separate_process(self):
        c=m.default_contract();c.update(architecture_mode='proposed-fixed-separate-domain',aur_gpgdir='/etc/zethropol-update/aur-native-trust',separate_worker_from_process_start=True)
        self.assertEqual(m.validate_contract(c)['status'],'PASS')
        c['separate_worker_from_process_start']=False
        self.assertEqual(m.validate_contract(c)['status'],'BLOCKED')

    def test_arbitrary_domain_and_missing_official_independence_block(self):
        for key,value in [('aur_gpgdir','/tmp/client-ring'),('official_gpgdir','/etc/zethropol-update/aur-native-trust'),('official_domain_independently_authorizes_dependencies',False)]:
            c=m.default_contract();c[key]=value
            self.assertEqual(m.validate_contract(c)['status'],'BLOCKED')

    def test_incomplete_duplicate_negative_matrix_or_unknown_command_block(self):
        for mutate in ('missing','duplicate','command'):
            c=m.default_contract()
            if mutate=='missing':c['negative_cases'].pop()
            elif mutate=='duplicate':c['negative_cases'][-1]=c['negative_cases'][0]
            else:c['command']='/bin/sh'
            self.assertEqual(m.validate_contract(c)['status'],'BLOCKED')

    def test_schema_and_boolean_types_are_strict(self):
        for key,value in [('schema',True),('schema',1.0),('allow_client_gpgdir',0),('required_package_signatures',1),('scope','production')]:
            c=m.default_contract();c[key]=value
            self.assertEqual(m.validate_contract(c)['status'],'BLOCKED')

    @requires_historical_evidence
    def test_real_probe_is_reused_only_with_exact_snapshot_and_archive_stays_blocked(self):
        raw=m.read_public(m.REPO/m.PROBE_REL);p=json.loads(raw)
        checks=m.evaluate_cached_probe(raw,p['public_snapshot_sha256'])
        result={c['id']:c['status'] for c in checks}
        self.assertEqual(result['official-native-loader-mechanism'],'PASS')
        self.assertEqual(result['archive-native-trust'],'BLOCKED')
        drift=copy.deepcopy(p['public_snapshot_sha256']);drift['pubring.kbx']='0'*64
        result={c['id']:c['status'] for c in m.evaluate_cached_probe(raw,drift)}
        self.assertEqual(result['native-public-snapshot-reuse'],'BLOCKED')
        self.assertEqual(result['official-native-loader-mechanism'],'BLOCKED')

    @requires_historical_evidence
    def test_modified_probe_claim_cannot_authorize_archive(self):
        raw=m.read_public(m.REPO/m.PROBE_REL);p=json.loads(raw)
        p['archive_primary_present_in_native_snapshot'][m.ARCHIVE_PRIMARY]=True
        checks=m.evaluate_cached_probe(json.dumps(p).encode(),p['public_snapshot_sha256'])
        self.assertEqual(checks[0]['status'],'BLOCKED')
        self.assertTrue(all(c['status']!='PASS' for c in checks))
        self.assertEqual({c['id']:c['status'] for c in checks}['archive-native-trust'],'BLOCKED')

    def test_runtime_records_cannot_select_absolute_private_or_unknown_sources(self):
        recovery='desktop/control-center/aur/RECOVERY_VALIDATION.json'
        vm='desktop/control-center/aur/VM_HOST_INVENTORY.json'
        repo=Path('/workspace-fixture')
        for name in ('/private/key','../outside','services/update/src/unreviewed.rs'):
            calls=[]
            def public(path,limit=16*1024**2):
                calls.append(path)
                if path==repo/recovery:return json.dumps({'source_sha256':{name:'a'*64}}).encode()
                if path==repo/vm:return b'{}'
                raise AssertionError('unreviewed path opened: '+str(path))
            with self.subTest(name=name),patch.object(m,'read_public',side_effect=public):
                records=m.runtime_inventory(repo)
            self.assertEqual(calls,[repo/recovery,repo/vm])
            self.assertIn(name,records[0]['rejected_source_entries'])
            self.assertFalse(records[0]['usable_as_real_host_acceptance'])

    def test_bad_historical_digest_is_rejected_before_source_read(self):
        repo=Path('/workspace-fixture');recovery=next(iter(m.RUNTIME_SOURCES))
        name=next(iter(m.RUNTIME_SOURCES[recovery]))
        def public(path,limit=16*1024**2):
            if path==repo/recovery:return json.dumps({'source_sha256':{name:True}}).encode()
            if str(path).endswith('VM_HOST_INVENTORY.json'):return b'{}'
            raise AssertionError('source read despite invalid hash')
        with patch.object(m,'read_public',side_effect=public):records=m.runtime_inventory(repo)
        self.assertEqual(records[0]['rejected_source_entries'][name],'invalid historical source digest')

    def test_ambiguous_or_unbounded_historical_source_metadata_fails_closed(self):
        for raw in (b'{"source_sha256":{},"source_sha256":{}}',b'{"source_sha256":[]}',
                    b'[]',b'{"source_sha256":{},"value":NaN}'):
            with patch.object(m,'read_public',return_value=raw):records=m.runtime_inventory(Path('/fixture'))
            self.assertTrue(all('error' in record for record in records))
            self.assertTrue(all(record['usable_as_real_host_acceptance'] is False for record in records))

    def test_public_source_symlink_and_hardlink_rejected(self):
        import os
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)/'public';p.write_bytes(b'public')
            link=Path(directory)/'link';link.symlink_to(p)
            with self.assertRaises(ValueError):m.read_public(link)
            link.unlink();os.link(p,link)
            with self.assertRaises(ValueError):m.read_public(p)


if __name__=='__main__':unittest.main()
