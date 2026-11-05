"""Synthetic semantic tests only; none creates production proof or signatures."""
import copy
import importlib.util
import json
from pathlib import Path
import os
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
import io

spec=importlib.util.spec_from_file_location('rebuild_acceptance',Path(__file__).resolve().parents[1]/'rebuild_acceptance.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


class RebuildAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.operation='11111111-1111-4111-8111-111111111111'
        self.boot='22222222-2222-4222-8222-222222222222'
        self.expected={'operation_id':self.operation,'uid':1000,'bus_name':':1.42','session_id':'session-1',
                       'source_package':'yay','source_version':'13.1.0-1','architecture':'x86_64',
                       'source_plan_sha256':'a'*64,'build_root_sha256':'b'*64,'root_tree_sha256':'c'*64,
                       'outputs':[{'name':'yay','version':'13.1.0-1','filename':'yay-13.1.0-1-x86_64.pkg.tar.zst'}]}
        self.capture={'schema':1,'purpose':'zethropol-independent-rebuild-capture','scope':'synthetic-fixture',
                      'context':{k:self.expected[k] for k in m.CONTEXT},
                      'root_provenance':{'scope':'production-policy-only','manifest_sha256':'b'*64,'measured_tree_sha256':'c'*64,'signature_verified':True},
                      'builds':{}}
        output={'filename':self.expected['outputs'][0]['filename'],'sha256':'d'*64,
                'metadata':{'pkgname':'yay','pkgver':'13.1.0-1','pkgbase':'yay','arch':'x86_64'}}
        for role,uid in m.ROLE_UIDS.items():
            home='/var/lib/zethropol-aur-'+('archive' if role=='archive' else 'attestor')+'/work/'+self.operation
            self.capture['builds'][role]={'uid':uid,'real_uid':uid,'effective_uid':uid,'method':'pipeline.run/makepkg-package',
                'fresh_work':True,'fresh_cache':True,'state':'built-untrusted','cleanup':'verified',
                'workdir':home+'/journal','cachedir':home+'/clean-cache','outputs':[copy.deepcopy(output)]}
        self.timing={'reservation':{'issued_at':1000,'expires_at':1300,'issued_monotonic_ns':10*m.NS,
                                   'deadline_monotonic_ns':310*m.NS,'boot_id':self.boot},
                     'receipt':{'issued_at':1110,'expires_at':1350},'manifest_expires_at':2000,'segments':[]}
        lengths=[30,10,60,20,10];wall=1000;mono=10*m.NS
        for phase,length in zip(m.PHASES,lengths):
            self.timing['segments'].append({'phase':phase,'start_wall':wall,'end_wall':wall+length,
                'start_monotonic_ns':mono,'end_monotonic_ns':mono+length*m.NS})
            wall+=length;mono+=length*m.NS
        self.wall=1130;self.mono=140*m.NS

    def rebuild(self):return m.validate_rebuild(self.capture,self.expected)
    def budget(self):return m.validate_budget(self.timing,self.wall,self.mono,self.boot)

    def test_valid_synthetic_capture_never_becomes_runtime_authority(self):
        result=self.rebuild();self.assertEqual(result['semantic_status'],'PASS')
        self.assertEqual(result['runtime_status'],'BLOCKED');self.assertFalse(result['production_authorized'])

    def test_same_uid_and_wrong_real_effective_identity_rejected(self):
        for field,value in [('uid',951),('real_uid',0),('effective_uid',951),('uid',True)]:
            with self.subTest(field=field,value=value):
                old=self.capture['builds']['receipt'][field];self.capture['builds']['receipt'][field]=value
                with self.assertRaises(m.CaptureRejected):self.rebuild()
                self.capture['builds']['receipt'][field]=old

    def test_capture_context_numeric_alias_does_not_match_uid(self):
        self.capture['context']['uid']=1000.0
        with self.assertRaises(m.CaptureRejected):self.rebuild()
        self.expected['uid']=1;self.capture['context']['uid']=True
        with self.assertRaises(m.CaptureRejected):self.rebuild()

    def test_malformed_output_filename_rejected_as_capture_error(self):
        for filename in ([],{},None,'bad\x00name'):
            self.capture['builds']['receipt']['outputs'][0]['filename']=filename
            with self.assertRaises(m.CaptureRejected):self.rebuild()

    def test_nul_and_oversized_private_workspace_paths_rejected(self):
        current=self.capture['builds']['receipt']['cachedir']
        for path in (current+'\x00',current+'x'*4096):
            self.capture['builds']['receipt']['cachedir']=path
            with self.assertRaises(m.CaptureRejected):self.rebuild()

    def test_original_owner_cannot_attest_own_build(self):
        self.expected['uid']=950;self.capture['context']['uid']=950
        with self.assertRaises(m.CaptureRejected):self.rebuild()

    def test_stale_source_plan_context_rejected(self):
        self.capture['context']['source_plan_sha256']='e'*64
        with self.assertRaises(m.CaptureRejected):self.rebuild()

    def test_unsigned_historical_root_cannot_substitute(self):
        self.capture['root_provenance']['scope']='isolated-test'
        with self.assertRaises(m.CaptureRejected):self.rebuild()

    def test_boolean_signature_claim_is_strict(self):
        self.capture['root_provenance']['signature_verified']=1
        with self.assertRaises(m.CaptureRejected):self.rebuild()

    def test_missing_full_package_and_duplicate_outputs_rejected(self):
        for value in ([],self.capture['builds']['receipt']['outputs']*2):
            self.capture['builds']['receipt']['outputs']=value
            with self.assertRaises(m.CaptureRejected):self.rebuild()

    def test_metadata_difference_rejected(self):
        for field,value in [('pkgbase','other'),('arch','aarch64'),('pkgver','old')]:
            with self.subTest(field=field):
                metadata=self.capture['builds']['receipt']['outputs'][0]['metadata'];old=metadata[field];metadata[field]=value
                with self.assertRaises(m.CaptureRejected):self.rebuild()
                metadata[field]=old

    def test_equal_identity_but_different_archive_hash_rejected(self):
        self.capture['builds']['receipt']['outputs'][0]['sha256']='e'*64
        with self.assertRaises(m.CaptureRejected):self.rebuild()

    def test_binary_only_build_does_not_count(self):
        self.capture['builds']['receipt']['method']='make build binary only'
        with self.assertRaises(m.CaptureRejected):self.rebuild()

    def test_interrupted_cleanup_blocks_receipt(self):
        self.capture['builds']['receipt']['cleanup']='interrupted'
        with self.assertRaises(m.CaptureRejected):self.rebuild()

    def test_fresh_cache_and_job_path_binding_required(self):
        self.capture['builds']['receipt']['fresh_cache']=False
        with self.assertRaises(m.CaptureRejected):self.rebuild()
        self.capture['builds']['receipt']['fresh_cache']=True
        self.capture['builds']['receipt']['cachedir']='/var/lib/zethropol-aur-attestor/work/old-job/cache'
        with self.assertRaises(m.CaptureRejected):self.rebuild()

    def test_extra_capture_field_cannot_inject_authority(self):
        self.capture['production_authorized']=True
        with self.assertRaises(m.CaptureRejected):self.rebuild()

    def test_budget_includes_both_rebuilds_and_returns_margin(self):
        result=self.budget();self.assertEqual(result['semantic_status'],'PASS')
        self.assertEqual(result['critical_path_elapsed_ns'],130*m.NS)
        self.assertEqual(result['remaining_monotonic_ns'],170*m.NS)
        self.assertFalse(result['production_authorized'])
        self.assertEqual(result['runtime_status'],'BLOCKED')

    def test_malformed_boot_identity_rejected_as_capture_error(self):
        for boot in ('not-a-uuid',None,17):
            with self.assertRaises(m.CaptureRejected):
                m.validate_budget(self.timing,self.wall,self.mono,boot)

    def test_expiry_boundary_and_no_lifetime_extension(self):
        self.wall=1300;self.mono=310*m.NS
        with self.assertRaises(m.CaptureRejected):self.budget()
        self.wall=1130;self.mono=140*m.NS;self.timing['reservation']['expires_at']=1301
        with self.assertRaises(m.CaptureRejected):self.budget()

    def test_wall_rollback_cannot_extend_monotonic_deadline(self):
        self.wall=1100;self.mono=310*m.NS
        with self.assertRaises(m.CaptureRejected):self.budget()

    def test_clock_drift_and_other_boot_rejected(self):
        self.wall=1120
        with self.assertRaises(m.CaptureRejected):self.budget()
        self.wall=1130;self.boot='33333333-3333-4333-8333-333333333333'
        with self.assertRaises(m.CaptureRejected):self.budget()

    def test_skipped_or_overlapping_provider_phases_rejected(self):
        self.timing['segments'][2]['start_monotonic_ns']=11*m.NS
        with self.assertRaises(m.CaptureRejected):self.budget()
        self.timing['segments']=self.timing['segments'][:-1]
        with self.assertRaises(m.CaptureRejected):self.budget()

    def test_receipt_cannot_precede_attestor_or_be_future(self):
        self.timing['receipt']['issued_at']=1001
        with self.assertRaises(m.CaptureRejected):self.budget()
        self.timing['receipt']['issued_at']=1140
        with self.assertRaises(m.CaptureRejected):self.budget()

    def test_expired_manifest_and_receipt_refused(self):
        self.timing['manifest_expires_at']=1130
        with self.assertRaises(m.CaptureRejected):self.budget()
        self.timing['manifest_expires_at']=2000;self.timing['receipt']['expires_at']=1130
        with self.assertRaises(m.CaptureRejected):self.budget()

    def test_boolean_and_negative_times_rejected(self):
        for value in (True,-1,1.5):
            self.timing['reservation']['issued_at']=value
            with self.assertRaises(m.CaptureRejected):self.budget()

    def test_duplicate_json_and_source_link_rejected(self):
        with self.assertRaises(m.CaptureRejected):json.loads('{"schema":1,"schema":1}',object_pairs_hook=m.unique)
        with tempfile.TemporaryDirectory() as temporary:
            source=Path(temporary)/'source';source.write_text('{}');link=Path(temporary)/'link';link.symlink_to(source)
            with self.assertRaises(m.CaptureRejected):m.read_public_json(link)
            link.unlink();os.link(source,link)
            with self.assertRaises(m.CaptureRejected):m.read_public_json(source)

    def test_offline_cli_binds_input_hashes_without_promoting_authority(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            capture=root/'capture';expected=root/'expected';timing=root/'timing';clock=root/'clock'
            capture.write_text(json.dumps(self.capture));expected.write_text(json.dumps(self.expected))
            timing.write_text(json.dumps(self.timing));clock.write_text(json.dumps({'now_wall':self.wall,'now_monotonic_ns':self.mono,'boot_id':self.boot}))
            for argv in (['--validate-rebuild',str(capture),'--expected',str(expected)],
                         ['--validate-budget',str(timing),'--clock',str(clock)]):
                output=io.StringIO()
                with redirect_stdout(output):self.assertEqual(m.main(argv),0)
                result=json.loads(output.getvalue())
                self.assertEqual(result['semantic_status'],'PASS');self.assertEqual(result['runtime_status'],'BLOCKED')
                self.assertFalse(result['production_authorized']);self.assertEqual(len(result['input_sha256']),2)

    def test_offline_cli_rejects_duplicate_fields_without_authority(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);capture=root/'capture';expected=root/'expected'
            expected.write_text(json.dumps(self.expected));capture.write_text('{"schema":1,"schema":1}')
            output=io.StringIO()
            with redirect_stdout(output):self.assertEqual(m.main(['--validate-rebuild',str(capture),'--expected',str(expected)]),2)
            result=json.loads(output.getvalue());self.assertEqual(result['runtime_status'],'BLOCKED')
            self.assertFalse(result['production_authorized'])

    def test_offline_cli_requires_complete_exclusive_input_pairs(self):
        for argv in (['--validate-rebuild','unused'],['--spec','--expected','unused'],
                     ['--validate-budget','unused','--expected','other','--clock','clock']):
            with self.assertRaises(SystemExit),redirect_stdout(io.StringIO()),redirect_stderr(io.StringIO()):m.main(argv)

    def test_runtime_budget_consumes_original_deadline_and_detects_suspend_rollback_and_boot_change(self):
        frame={'schema':1,'purpose':'zethropol-provider-reservation-budget','boot_id':self.boot,
               'issued_boottime_ns':10*m.NS,'deadline_boottime_ns':310*m.NS,
               'captured_boottime_ns':100*m.NS,'captured_wall_ns':1000*m.NS}
        sample=[1001*m.NS,101*m.NS,self.boot]
        budget=m.RuntimeBudget(frame,clock=lambda:tuple(sample))
        self.assertEqual(budget.timeout_seconds(),209)
        sample[:2]=[1002*m.NS,102*m.NS];self.assertEqual(budget.timeout_seconds(),208)
        for wall,boot,identity in ((1210*m.NS,310*m.NS,self.boot),(999*m.NS,102*m.NS,self.boot),
                                  (1002*m.NS,104*m.NS,self.boot),(1002*m.NS,102*m.NS,'other-boot')):
            sample[:]=[wall,boot,identity]
            with self.assertRaises(m.CaptureRejected):budget.checkpoint()

    def test_runtime_budget_refuses_extension_missing_fields_and_subsecond_build_budget(self):
        frame={'schema':1,'purpose':'zethropol-provider-reservation-budget','boot_id':self.boot,
               'issued_boottime_ns':10*m.NS,'deadline_boottime_ns':310*m.NS,
               'captured_boottime_ns':100*m.NS,'captured_wall_ns':1000*m.NS}
        sample=[1209*m.NS+500_000_000,309*m.NS+500_000_000,self.boot]
        budget=m.RuntimeBudget(frame,clock=lambda:tuple(sample))
        with self.assertRaises(m.CaptureRejected):budget.timeout_seconds()
        for bad in (dict(frame,deadline_boottime_ns=311*m.NS),dict(frame,schema=True),{},dict(frame,extra=1)):
            with self.assertRaises(m.CaptureRejected):m.RuntimeBudget(bad,clock=lambda:tuple(sample))

    def test_runtime_budget_refuses_regression_after_accepted_checkpoint(self):
        frame={'schema':1,'purpose':'zethropol-provider-reservation-budget','boot_id':self.boot,
               'issued_boottime_ns':10*m.NS,'deadline_boottime_ns':310*m.NS,
               'captured_boottime_ns':100*m.NS,'captured_wall_ns':1000*m.NS}
        for rollback in ('both','wall','boot'):
            with self.subTest(rollback=rollback):
                sample=[1010*m.NS,110*m.NS,self.boot]
                budget=m.RuntimeBudget(frame,clock=lambda:tuple(sample))
                sample[:2]=[1020*m.NS,120*m.NS];budget.checkpoint()
                # All three still exceed the original capture and fit the old
                # one-second drift allowance. None may undo consumed time.
                if rollback=='both':sample[:2]=[1019*m.NS,119*m.NS]
                elif rollback=='wall':sample[:2]=[1020*m.NS-1,120*m.NS+1]
                else:sample[:2]=[1020*m.NS+1,120*m.NS-1]
                with self.assertRaisesRegex(m.CaptureRejected,'previous checkpoint'):budget.checkpoint()

    def test_runtime_budget_equal_samples_do_not_renew_deadline_or_remaining_time(self):
        frame={'schema':1,'purpose':'zethropol-provider-reservation-budget','boot_id':self.boot,
               'issued_boottime_ns':10*m.NS,'deadline_boottime_ns':310*m.NS,
               'captured_boottime_ns':100*m.NS,'captured_wall_ns':1000*m.NS}
        sample=[1010*m.NS,110*m.NS,self.boot]
        budget=m.RuntimeBudget(frame,clock=lambda:tuple(sample));remaining=budget.checkpoint()
        self.assertEqual(budget.checkpoint(),remaining)
        sample[:2]=[1020*m.NS,120*m.NS]
        self.assertEqual(budget.checkpoint(),remaining-10*m.NS)
        self.assertEqual(budget.frame,frame)
        self.assertEqual(budget.wall_deadline,1210*m.NS)

    def test_real_clock_probe_never_claims_rebuild_or_receipt(self):
        result=m.clock_probe()
        self.assertEqual(result['mechanism_status'],'PASS');self.assertGreater(result['elapsed_ns'],0)
        self.assertFalse(result['signature_or_receipt_created']);self.assertFalse(result['provider_build_performed'])
        self.assertEqual(result['runtime_status'],'BLOCKED')


if __name__=='__main__':unittest.main()
