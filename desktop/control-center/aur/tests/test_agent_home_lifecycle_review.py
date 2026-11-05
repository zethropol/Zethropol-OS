import contextlib,io,json,sys,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import agent_home_lifecycle_review as review

class Review(unittest.TestCase):
    def test_nonroot_blocks_before_reading_socket_or_config(self):
        with patch.object(review.os,'getresuid',return_value=(1000,)*3),patch.object(review.diagnostic,'inspect_socket') as snap,contextlib.redirect_stdout(io.StringIO()) as output:
            review.main();snap.assert_not_called()
        result=json.loads(output.getvalue());self.assertEqual(result['phase'],'root-host-identity')
        self.assertFalse(result['host_changed']);self.assertFalse(result['agent_started'])
    def exercise(self,changed=False,listener=False):
        before={'device':'1','inode':'2','uid':949,'gid':948,'mode':'0o600','ctime_ns':'3','acl':{}}
        after=dict(before,inode='4') if changed else before
        def report(snap):
            return {'observed':snap,'blockers':[{'field':'listener','expected':'no bound endpoint','observed':'LISTENING'}] if listener else [],
                    'listener_visibility':'COMPLETE_FOR_OBSERVED_PROCESS_NAMESPACES','listener_observation':{'scope':'observed only'},
                    'listener_state':'LISTENING' if listener else 'NO_ENDPOINT_IN_OBSERVED_TABLES','phase':'socket-field-guards'}
        with patch.object(review.os,'getresuid',return_value=(0,)*3),patch.object(Path,'read_text',return_value='0 0 4294967295'),patch.object(review.os,'stat',return_value=SimpleNamespace(st_ino=1)),patch.object(review.os,'lstat',return_value=SimpleNamespace(st_uid=0,st_gid=0,st_mode=0o40755)),patch.object(review.os,'listxattr',return_value=[]),patch.object(review.life,'node'),patch.object(review.diagnostic,'inspect_socket',side_effect=[report(before),report(after)]),patch.object(review.life,'no_listener',return_value=not listener),patch.object(review.life,'any_uid949_agent',return_value=False),patch.object(review,'target_state',return_value={'state':'ABSENT'}),patch.object(review,'config_binding',return_value={'reviewed_hash_match':True}),patch.object(review,'public_binary_hash',return_value='a'*64):
            return review.review()
    def test_metadata_report_does_not_requery_accepted_route_or_authorize_activation(self):
        result=self.exercise()
        self.assertEqual(result['route_evidence'],'USER_CONFIRMED_NOT_REQUERIED')
        self.assertFalse(result['activation_approved']);self.assertEqual(result['old_socket']['inode'],'2')
        self.assertTrue(result['listener_absent'])
        self.assertFalse(result['quarantine_authorized'])
        self.assertIn('NOT_EXCLUDED',result['unobserved_namespace_and_fd_holders'])
    def test_listener_is_reported_without_start_stop_or_connect(self):
        with self.assertRaises(review.diagnostic.BindingBlocked) as caught:self.exercise(listener=True)
        self.assertEqual(caught.exception.report['blockers'][0]['field'],'listener')
    def test_inode_race_blocks_report(self):
        with self.assertRaises(review.diagnostic.BindingBlocked) as caught:self.exercise(changed=True)
        self.assertEqual(caught.exception.report['blockers'][0]['field'],'full_review_inode_ctime_stability')
    def test_reader_has_no_mutation_or_process_execution_calls(self):
        import ast
        tree=ast.parse(Path(review.__file__).read_text())
        calls={node.func.attr for node in ast.walk(tree) if isinstance(node,ast.Call) and isinstance(node.func,ast.Attribute)}
        self.assertTrue(calls.isdisjoint({'unlink','remove','rename','chmod','chown','mkdir','Popen','run','connect','bind','root_claim','main'}))

    def test_existing_unit_preimage_hash_is_bound_without_overwrite(self):
        s=SimpleNamespace(st_dev=1,st_ino=2,st_uid=0,st_gid=0,st_mode=0o100644,st_ctime_ns=3,st_mtime_ns=4,st_nlink=1,st_size=4)
        with patch.object(review.os,'lstat',return_value=s),patch.object(review.life,'root_input',return_value=b'unit'):
            result=review.target_state(review.life.UNIT_PATH)
        import hashlib
        self.assertEqual(result['sha256'],hashlib.sha256(b'unit').hexdigest())

    def test_main_retains_field_values_instead_of_generic_blocked(self):
        data={'status':'BLOCKED_STALE_SOCKET_BINDING','observed':{'mode':'0o700'},'blockers':[{'field':'mode','expected':'0o600','observed':'0o700'}],'phase':'socket-field-guards'}
        with patch.object(review,'review',side_effect=review.diagnostic.BindingBlocked(data)),contextlib.redirect_stdout(io.StringIO()) as output:
            review.main()
        result=json.loads(output.getvalue())
        self.assertEqual(result['observed']['mode'],'0o700');self.assertEqual(result['blockers'],data['blockers'])
