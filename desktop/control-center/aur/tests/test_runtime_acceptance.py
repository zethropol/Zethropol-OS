"""Offline fixture checks; no PolicyKit, guest, signing, native or service call."""
import copy
import json
import os
import stat
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import runtime_acceptance as runtime


class RuntimeAcceptance(unittest.TestCase):
    def test_passive_collector_writes_real_snapshot_bindings_without_authority(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'passive';reads=[]
            def read(path,limit):
                reads.append(str(path))
                if path.name=='uid_map':return b'0 0 4294967295\n'
                if path.name=='boot_id':
                    self.assertEqual(limit,4096)
                    return b'22222222-2222-4222-8222-222222222222\n'
                return b'EXPLICIT PASSIVE FIXTURE ONLY\n'
            with patch('runtime_acceptance.read_public',side_effect=read),patch('runtime_acceptance.source_hashes',return_value={}):
                report=runtime.passive_preflight(out)
            self.assertEqual(len(reads),7)
            self.assertTrue(all(path.startswith('/proc/') or path=='/usr/share/polkit-1/actions/org.zethropol.update.policy' for path in reads))
            self.assertTrue(report['initial_uid_namespace_observed'])
            self.assertEqual(report['runtime_acceptance'],'BLOCKED');self.assertFalse(report['production_authorized'])
            self.assertFalse(any(report['operations_performed'].values()))
            self.assertLessEqual(report['clock_before']['monotonic_ns'],report['clock_after']['monotonic_ns'])
            for artifact in report['snapshots'].values():
                p=out/artifact['artifact'];self.assertEqual(runtime.sha(p.read_bytes()),artifact['sha256'])
                self.assertEqual(stat.S_IMODE(p.stat().st_mode),0o600)

    def test_passive_collector_denial_and_nonhost_namespace_stay_explicit(self):
        with tempfile.TemporaryDirectory() as d:
            def read(path,limit):
                if path.name=='mountinfo':raise PermissionError('fixture access denied')
                return b'1000 0 1\n'
            with patch('runtime_acceptance.read_public',side_effect=read),patch('runtime_acceptance.source_hashes',return_value={}):
                result=runtime.passive_preflight(Path(d)/'passive')
            self.assertFalse(result['initial_uid_namespace_observed'])
            self.assertEqual(result['snapshots']['mountinfo']['status'],'BLOCKED')
            self.assertIsNone(result['boot_id_observed'])
            self.assertEqual(result['runtime_acceptance'],'BLOCKED')

    def test_passive_collector_never_reuses_existing_evidence_directory(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'passive';out.mkdir();sentinel=out/'existing';sentinel.write_bytes(b'unchanged')
            with patch('runtime_acceptance.read_public') as read,self.assertRaises(FileExistsError):runtime.passive_preflight(out)
            read.assert_not_called();self.assertEqual(sentinel.read_bytes(),b'unchanged')

    def capture(self,root):
        raw=b'UNTRUSTED FIXTURE; NOT A REAL RUNTIME ACCEPTANCE'
        (root/'fixture').write_bytes(raw)
        records=[]
        for name,roles in runtime.CASES.items():
            records.append({'id':name,'status':'PASS','before_package_db_sha256':'a'*64,
                'after_package_db_sha256':'a'*64,'evidence':[{'role':role,'path':'fixture','sha256':runtime.sha(raw)} for role in roles]})
        sources={name:'b'*64 for name in runtime.SOURCES}
        return {'schema':1,'purpose':'zethropol-runtime-capture','scope':'disposable-vm',
                'vm_id':'11111111-1111-4111-8111-111111111111',
                'boot_id':'22222222-2222-4222-8222-222222222222','source_sha256':sources,'cases':records},sources

    def test_plan_does_not_run_or_authorize_acceptance(self):
        plan=runtime.plan()
        self.assertEqual(plan['runtime_acceptance'],'BLOCKED')
        self.assertFalse(plan['production_authorized'])
        self.assertFalse(any(plan['operations_performed'].values()))
        self.assertTrue(all(case['status']=='BLOCKED' for case in plan['cases']))

    def test_positive_json_and_matching_artifacts_never_prove_runtime(self):
        with tempfile.TemporaryDirectory() as d:
            capture,sources=self.capture(Path(d));result=runtime.validate_capture(capture,d,sources)
            self.assertEqual(result['capture_integrity'],'PASS')
            self.assertEqual(result['runtime_acceptance'],'BLOCKED')
            self.assertFalse(result['production_authorized']);self.assertTrue(result['requires_independent_review'])

    def test_changed_sources_missing_duplicate_cases_and_boolean_schema(self):
        with tempfile.TemporaryDirectory() as d:
            capture,sources=self.capture(Path(d))
            variants=[dict(capture,schema=True),dict(capture,scope='host'),dict(capture,cases=capture['cases'][:-1]),
                      dict(capture,cases=[capture['cases'][0]]*len(runtime.CASES)),dict(capture,command='sudo')]
            for bad in variants:
                with self.assertRaises(runtime.CaptureRejected):runtime.validate_capture(bad,d,sources)
            changed=dict(sources);changed[next(iter(changed))]='c'*64
            with self.assertRaises(runtime.CaptureRejected):runtime.validate_capture(capture,d,changed)

    def test_denied_case_mutation_and_missing_artifact_roles_reject(self):
        with tempfile.TemporaryDirectory() as d:
            capture,sources=self.capture(Path(d))
            bad=copy.deepcopy(capture);case=next(v for v in bad['cases'] if v['id']=='POLKIT-DENY')
            case['after_package_db_sha256']='f'*64
            with self.assertRaisesRegex(runtime.CaptureRejected,'changed package'):runtime.validate_capture(bad,d,sources)
            bad=copy.deepcopy(capture);bad['cases'][0]['evidence']=[]
            with self.assertRaisesRegex(runtime.CaptureRejected,'lacks'):runtime.validate_capture(bad,d,sources)

    def test_admission_crash_and_clock_replay_cannot_claim_package_mutation(self):
        with tempfile.TemporaryDirectory() as d:
            capture,sources=self.capture(Path(d))
            for name in ('CRASH-ADMISSION','CLOCK-REPLAY'):
                bad=copy.deepcopy(capture);case=next(v for v in bad['cases'] if v['id']==name)
                case['after_package_db_sha256']='f'*64
                with self.assertRaisesRegex(runtime.CaptureRejected,'changed package'):
                    runtime.validate_capture(bad,d,sources)
                plan_case=next(v for v in runtime.plan()['cases'] if v['id']==name)
                self.assertTrue(plan_case['package_db_must_remain_unchanged'])

    def test_replay_and_admission_crash_require_raw_package_database_evidence(self):
        with tempfile.TemporaryDirectory() as d:
            capture,sources=self.capture(Path(d))
            for name in ('CRASH-ADMISSION','CLOCK-REPLAY'):
                bad=copy.deepcopy(capture);case=next(v for v in bad['cases'] if v['id']==name)
                case['evidence']=[v for v in case['evidence'] if v['role']!='package-db-before-after']
                with self.assertRaisesRegex(runtime.CaptureRejected,'lacks required'):
                    runtime.validate_capture(bad,d,sources)

    def test_empty_raw_evidence_is_not_a_passing_recovery_observation(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);capture,sources=self.capture(root);(root/'empty').write_bytes(b'')
            item=next(v for v in capture['cases'] if v['id']=='ROOT-RECOVERY')['evidence'][0]
            item.update(path='empty',sha256=runtime.sha(b''))
            with self.assertRaisesRegex(runtime.CaptureRejected,'Empty runtime artifact'):
                runtime.validate_capture(capture,d,sources)

    def test_traversal_symlink_and_modified_raw_evidence_reject(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);capture,sources=self.capture(root)
            for name in ('../fixture','/tmp/fixture','./fixture'):
                bad=copy.deepcopy(capture);bad['cases'][0]['evidence'][0]['path']=name
                with self.assertRaises(runtime.CaptureRejected):runtime.validate_capture(bad,d,sources)
            (root/'link').symlink_to('fixture')
            bad=copy.deepcopy(capture);bad['cases'][0]['evidence'][0]['path']='link'
            with self.assertRaises(ValueError):runtime.validate_capture(bad,d,sources)
            (root/'link').unlink();os.link(root/'fixture',root/'hardlink')
            with self.assertRaises(ValueError):runtime.validate_capture(capture,d,sources)
            (root/'hardlink').unlink()
            (root/'fixture').write_bytes(b'changed')
            with self.assertRaisesRegex(runtime.CaptureRejected,'bytes changed'):runtime.validate_capture(capture,d,sources)

    def test_duplicate_json_and_unsafe_source_binding_keys_reject(self):
        with self.assertRaises(runtime.CaptureRejected):json.loads('{"schema":1,"schema":1}',object_pairs_hook=runtime.unique)
        with tempfile.TemporaryDirectory() as d:
            capture,sources=self.capture(Path(d));capture['source_sha256']['/etc/shadow']='a'*64
            with self.assertRaises(runtime.CaptureRejected):runtime.validate_capture(capture,d,sources)

    def test_aggregate_evidence_budget_is_finite(self):
        with tempfile.TemporaryDirectory() as d:
            capture,sources=self.capture(Path(d));data=b'x'*(16*1024**2)
            i=0
            for case in capture['cases']:
                for item in case['evidence']:
                    item['path']=f'large-{i}';item['sha256']=runtime.sha(data);i+=1
            with patch('runtime_acceptance.read_public',return_value=data),self.assertRaisesRegex(runtime.CaptureRejected,'Aggregate'):
                runtime.validate_capture(capture,d,sources)


if __name__=='__main__':unittest.main()
