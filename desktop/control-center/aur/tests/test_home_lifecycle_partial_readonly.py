import ast,base64,contextlib,io,json,os,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import home_lifecycle_file_transaction as tx
import home_lifecycle_partial_readonly as review
import prepare_home_lifecycle_partial_review as build

from historical_evidence import requires_historical_evidence

@requires_historical_evidence
class PartialReview(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.root=Path(cls.temp.name);cls.bound=build.prepare(cls.root)
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def audit(self):
        b=self.bound;env=b['envelope']
        claim={'transaction_id':env['transaction_id'],'manifest_sha256':b['manifest_sha256'],'bundle_sha256':b['bundle_sha256'],'capture_sha256':tx.sha(tx.canonical(env))}
        post={'status':'PASS_INERT_FILE_DEPLOYMENT','manifest_sha256':b['manifest_sha256'],
              'protected_capture_sha256':tx.sha(tx.canonical(env['capture']['preservation'])),'agent_started':False,'activation_approved':False,'daemon_reloaded':False}
        return {'claim.json':{'data':claim},'manifest.json':{'data':b['manifest'],'metadata':{'sha256':b['manifest_sha256']}},
                'preflight.json':{'data':env},'postflight.json':{'data':post}}
    def test_audit_checks_require_exact_original_capture_claim_and_manifest(self):
        with patch.object(review,'BOUND',self.bound,create=True),patch.object(review,'tx',tx,create=True):
            records=self.audit();self.assertTrue(all(review.audit_checks(records).values()))
            records['claim.json']['data']['transaction_id']='b'*32
            self.assertFalse(review.audit_checks(records)['claim_bound_to_original'])
    def test_rollback_or_missing_postflight_is_not_completed_installation(self):
        with patch.object(review,'BOUND',self.bound,create=True),patch.object(review,'tx',tx,create=True):
            records=self.audit();records.pop('postflight.json')
            self.assertFalse(review.audit_checks(records)['successful_postflight_bound'])
            records['rollback.json']={'data':{'status':'INCOMPLETE_RETAINED'}}
            self.assertFalse(review.audit_checks(records)['rollback_absent'])
    def test_matching_bytes_alone_do_not_authorize_adoption_or_recovery(self):
        obj={'state':'EXISTS','type':'regular','manifest_metadata_match':True,'manifest_hash_match':True}
        self.assertTrue(review.recommend({'claim':False},[obj],True,True).startswith('BLOCKED'))
        self.assertTrue(review.recommend({'claim':True},[dict(obj,manifest_metadata_match=False)],True,True).startswith('BLOCKED'))
        self.assertTrue(review.recommend({'claim':True},[obj],False,True).startswith('BLOCKED'))
        self.assertIn('NO_REPLAY',review.recommend({'claim':True},[obj],True,True))
    def test_nofollow_inventory_captures_existing_inode_and_hash(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'public';path.write_bytes(b'public');os.chmod(path,0o644)
            spec={'type':'regular','uid':os.getuid(),'gid':os.getgid(),'mode':'0644','nlink':1,'sha256':tx.sha(b'public')}
            with patch.object(review,'tx',tx,create=True),patch.object(tx,'walk',side_effect=lambda p:os.open(p,tx.D)):
                r=review.inspect(str(path),spec)
                self.assertEqual(r['metadata']['inode'],path.stat().st_ino);self.assertTrue(r['manifest_hash_match'])
                os.symlink('public',Path(d)/'alias');r=review.inspect(str(Path(d)/'alias'),spec)
                self.assertEqual(r['type'],'symlink');self.assertFalse(r['manifest_metadata_match'])
                self.assertNotIn('sha256',r)
    def test_missing_and_unreadable_objects_remain_distinct(self):
        with tempfile.TemporaryDirectory() as d,patch.object(review,'tx',tx,create=True),patch.object(tx,'walk',side_effect=lambda p:os.open(p,tx.D)):
            self.assertEqual(review.inspect(d+'/missing')['state'],'ABSENT')
        with patch.object(review,'tx',tx,create=True),patch.object(tx,'walk',side_effect=PermissionError(13,'redacted')):
            r=review.inspect('/protected/file');self.assertEqual(r['state'],'BLOCKED_INVENTORY');self.assertEqual(r['errno'],13)
            self.assertNotIn('redacted',json.dumps(r))
    def test_reader_uses_no_mutation_activation_or_borrow_calls(self):
        tree=ast.parse(Path(review.__file__).read_text())
        calls={n.func.attr for n in ast.walk(tree) if isinstance(n,ast.Call) and isinstance(n.func,ast.Attribute)}
        self.assertTrue(calls.isdisjoint({'apply','write_new','mkdir_new','ensure_directory','noreplace','unlink','rename','chmod','chown','Popen','run','connect','setns','pidfd_getfd'}))
    def test_audit_is_read_before_target_inventory_and_errors_block(self):
        order=[]
        def listing(path):order.append('audit-list' if path.endswith(build.TXID) else 'extra-list');return ['claim.json'] if path.endswith(build.TXID) else []
        def audit(path):order.append('audit-read');raise PermissionError(13,'hidden')
        def inspecting(path,spec=None):order.append('target');return {'state':'ABSENT','path':path}
        with patch.object(review,'BOUND',self.bound,create=True),patch.object(review,'tx',tx,create=True),patch.object(tx,'host_context',return_value={}),patch.object(review,'names',side_effect=listing),patch.object(review,'read_audit',side_effect=audit),patch.object(review,'inspect',side_effect=inspecting),patch.object(tx,'preservation',return_value={}),patch.object(tx,'systemd_collisions',return_value={}):
            r=review.review()
        self.assertLess(order.index('audit-read'),order.index('target'))
        self.assertTrue(r['recovery_decision'].startswith('BLOCKED'));self.assertFalse(r['host_changed']);self.assertFalse(r['old_transaction_reusable'])
    def test_original_bundle_capture_and_reader_pins_are_preserved(self):
        pins=json.loads((self.root/'review-pins.json').read_bytes())
        self.assertEqual(pins['historical_manifest_sha256'],build.MANIFEST)
        self.assertEqual(pins['historical_bundle_sha256'],build.BUNDLE_SHA)
        self.assertEqual(pins['original_transaction_id'],build.TXID)
        raw=(self.root/'partial-review.pinned.py').read_bytes();self.assertEqual(pins['reader_sha256'],tx.sha(raw));ast.parse(raw)
        self.assertIn(pins['reader_sha256'],(self.root/'review-partial.fish').read_text())
        before=raw;build.prepare(self.root);self.assertEqual(before,(self.root/'partial-review.pinned.py').read_bytes())

class CollisionProgress(unittest.TestCase):
    def test_initial_release_collision_cannot_enter_write_phase(self):
        progress={}
        with patch.object(tx,'bundle_data',return_value=({},{})),patch.object(tx,'capture',side_effect=tx.Blocked('deployment collision: '+tx.RELEASE)),patch.object(tx,'ensure_directory') as mkdir,patch.object(tx,'write_new') as write,patch.object(tx,'noreplace') as rename:
            with self.assertRaises(tx.Blocked):tx.apply({}, {},progress)
            mkdir.assert_not_called();write.assert_not_called();rename.assert_not_called()
        self.assertEqual(progress,{'stage':'preflight-revalidation','write_phase_entered':False})
    def test_apply_error_report_distinguishes_prior_artifacts_from_current_writes(self):
        with patch.object(tx,'bundle_data',return_value=({},{})),patch.object(tx,'capture',side_effect=tx.Blocked('deployment collision: '+tx.RELEASE)),contextlib.redirect_stdout(io.StringIO()) as output:
            with self.assertRaises(SystemExit):tx.main({},'apply',base64.b64encode(b'{}').decode())
        r=json.loads(output.getvalue());self.assertFalse(r['this_invocation_write_phase_entered']);self.assertFalse(r['partial_artifacts_may_exist'])
        self.assertTrue(r['prior_artifacts_not_excluded']);self.assertEqual(r['failure_stage'],'preflight-revalidation')
    def test_write_phase_failure_remains_conservative(self):
        with patch.object(tx,'bundle_data',return_value=({'directories':[]},{})),patch.object(tx,'capture',return_value={}),patch.object(tx,'validate_capture'),contextlib.redirect_stdout(io.StringIO()) as output:
            # Missing STATE spec fails after the write-phase boundary; conservative,
            # never claim exact mutation inventory from a generic error flag.
            with self.assertRaises(SystemExit):tx.main({},'apply',base64.b64encode(tx.canonical({'transaction_id':'a'*32})).decode())
        r=json.loads(output.getvalue());self.assertTrue(r['this_invocation_write_phase_entered']);self.assertTrue(r['partial_artifacts_may_exist'])

if __name__=='__main__':unittest.main()
