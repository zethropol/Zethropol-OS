import hashlib,json,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import prepare_post_inert_acceptance as plan
import runtime_acceptance as runtime

from historical_evidence import requires_historical_evidence

@requires_historical_evidence
class CurrentAcceptancePlan(unittest.TestCase):
    def test_current_sources_and_expanded_recovery_evidence_roles_are_pinned(self):
        result=plan.current_plan()
        for name,digest in result['source_sha256'].items():
            self.assertEqual(hashlib.sha256((plan.REPO/name).read_bytes()).hexdigest(),digest)
        cases={c['id']:c for c in result['runtime']['cases']}
        self.assertEqual(set(cases),set(runtime.CASES))
        for name in ('CRASH-ADMISSION','CLOCK-REPLAY'):
            self.assertIn('package-db-before-after',cases[name]['required_evidence_roles'])
            self.assertTrue(cases[name]['package_db_must_remain_unchanged'])
    def test_inert_acceptance_does_not_clear_independent_runtime_gates(self):
        result=plan.current_plan()
        self.assertEqual(result['lifecycle']['current']['inert_installation'],'USER_ACCEPTED_RETAIN')
        for name in ('native','budget','rebuild_receipt','runtime'):self.assertEqual(result[name]['runtime_status'],'BLOCKED')
        self.assertFalse(result['production_authorized']);self.assertTrue(all(v is False for v in result['operations_performed'].values()))
    def test_original_deadline_roles_and_negative_matrix_remain_exact(self):
        result=plan.current_plan();self.assertEqual(result['budget']['limit_seconds'],300)
        self.assertEqual(len(result['budget']['ordered_phases']),5);self.assertFalse(result['budget']['ttl_renewal_permitted'])
        self.assertEqual(result['rebuild_receipt']['role_uids'],{'archive':951,'receipt':950})
        self.assertEqual(len(result['native']['negative_cases']),12)
    def test_privileged_scope_requires_identified_guest_and_excludes_host_signing_activation(self):
        stage=plan.current_plan()['next_privileged_stage']
        self.assertTrue(stage['status'].startswith('BLOCKED'))
        for forbidden in ('any host change','agent start','GPG socket mutation','signing','systemd enable'):
            self.assertIn(forbidden,stage['excluded'])
        self.assertIn('ONE identified disposable guest only',stage['single_authorization_scope'])
    def test_workspace_artifact_is_reproducible_and_preserves_historical_pins(self):
        with tempfile.TemporaryDirectory() as d:
            plan.prepare(Path(d));before=(Path(d)/'current-acceptance-plan.json').read_bytes()
            plan.prepare(Path(d));self.assertEqual(before,(Path(d)/'current-acceptance-plan.json').read_bytes())
            data=json.loads(before);self.assertTrue(all(data['historical_pins']['matches'].values()))

if __name__=='__main__':unittest.main()
