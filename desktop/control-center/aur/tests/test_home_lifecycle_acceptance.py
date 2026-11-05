import ast,copy,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import home_lifecycle_acceptance as acceptance

from historical_evidence import requires_historical_evidence

class InertAcceptance(unittest.TestCase):
    def test_user_acceptance_retains_files_without_runtime_or_recovery_authority(self):
        result=acceptance.validate_review(copy.deepcopy(acceptance.ACCEPTED_REVIEW))
        self.assertEqual(result['inert_installation'],'USER_ACCEPTED_RETAIN')
        for flag in ('old_transaction_reusable','redeployment_required','rollback_authorized','activation_approved',
                     'quarantine_authorized','inode_mutation_authorized','production_authorized'):
            self.assertIs(result[flag],False)
        self.assertEqual(result['runtime_acceptance'],'NOT_ESTABLISHED')
    def test_missing_inode_provenance_does_not_block_nonmutating_inert_acceptance(self):
        result=acceptance.validate_review(dict(acceptance.ACCEPTED_REVIEW))
        self.assertIs(result['current_inode_transaction_ownership_proven'],False)
        self.assertEqual(result['credential_readiness'],'NOT_PROBED')
    def test_drift_and_truthy_values_cannot_inject_authority(self):
        for change in ({'activation_approved':True},{'agent_started':1},{'systemd_active_state':'active'},
                       {'manifest_sha256':'0'*64},{'stability_pass':1},{'protected_preservation_matches_original':False},
                       {'current_inode_transaction_ownership_proven':True},{'command':'start'}):
            with self.subTest(change=change),self.assertRaises(acceptance.AcceptanceRejected):
                acceptance.validate_review(dict(acceptance.ACCEPTED_REVIEW,**change))
    def test_runtime_plan_does_not_repeat_host_scan_or_allow_any_live_stage(self):
        plan=acceptance.validation_plan();self.assertFalse(plan['readonly_acceptance']['further_host_scan_required'])
        self.assertTrue(all(stage['current_scope_permits_execution'] is False for stage in plan['future_runtime_stages']))
        self.assertTrue(all(v is False for v in plan['admission_boundaries'].values()))
    @requires_historical_evidence
    def test_historical_workspace_pins_still_match_without_host_contact(self):
        result=acceptance.local_historical_pins(Path(acceptance.__file__).resolve().parents[1])
        self.assertTrue(all(result['matches'].values()))
    def test_historical_drift_is_rejected_without_repair(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'build/aur-agent-home-lifecycle-v2-files-transaction-20261103';p.mkdir(parents=True)
            for name in ('deployment-manifest.INACTIVE.json','file-transaction.pinned.py'):(p/name).write_bytes(b'drift')
            with self.assertRaises(acceptance.AcceptanceRejected):acceptance.local_historical_pins(d)
            self.assertEqual((p/'file-transaction.pinned.py').read_bytes(),b'drift')
    def test_no_host_or_mutation_call_paths(self):
        calls={n.func.attr for n in ast.walk(ast.parse(Path(acceptance.__file__).read_text())) if isinstance(n,ast.Call) and isinstance(n.func,ast.Attribute)}
        self.assertTrue(calls.isdisjoint({'run','Popen','connect','bind','chmod','chown','unlink','rename','write_bytes','write_text','apply','systemd_output'}))

if __name__=='__main__':unittest.main()
