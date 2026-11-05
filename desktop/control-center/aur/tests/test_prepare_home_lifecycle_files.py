import ast,hashlib,json,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import prepare_home_lifecycle_files as build

from historical_evidence import requires_historical_evidence

@requires_historical_evidence
class Manifest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.manifest=build.prepare(self.root)
    def test_payload_has_exact_four_lifecycle_files_plus_empty_lock(self):
        files=self.manifest['files'];self.assertEqual(len(files),5)
        self.assertEqual({f['target'] for f in files}, {build.RELEASE+'/'+n for n in ('agent_lifecycle_949.py','native_socket_route_readonly.py','policy.json')}|{build.UNIT,build.ACTOR+'/lifecycle.lock'})
        for f in files:
            self.assertEqual(f['sha256'],hashlib.sha256(Path(f['source']).read_bytes()).hexdigest())
            self.assertEqual(f['nlink'],1);self.assertEqual(f['acl'],{});self.assertEqual(f['preimage'],'ABSENT_REQUIRED')
    def test_policy_cannot_admit_activation(self):
        p=json.loads((self.root/'policy.INACTIVE.json').read_text())
        self.assertIs(p['activation_approved'],False);self.assertIsNone(p['activation_id']);self.assertIsNone(p['quarantine_receipt'])
        for field,name in (('helper_sha256','agent_lifecycle_949.py'),('route_reader_sha256','native_socket_route_readonly.py'),('unit_sha256',Path(build.UNIT).name)):
            self.assertEqual(p[field],build.sha((self.root/name).read_bytes()))
    def test_protected_thirteen_and_socket_paths_are_outside_payload(self):
        protected={f['target'] for f in self.manifest['protected_signer_baseline']}
        self.assertEqual(len(protected),13)
        self.assertTrue(protected.isdisjoint(f['target'] for f in self.manifest['files']))
        self.assertFalse(any('/gnupg/' in f['target'] for f in self.manifest['files']+self.manifest['directories']))
    def test_exact_actor_and_root_metadata_and_no_future_authority_files(self):
        for f in self.manifest['files']+self.manifest['directories']:
            actor=f['target'].startswith(build.ACTOR)
            self.assertEqual((f['uid'],f['gid']),(949,948) if actor else (0,0))
            if f['type']=='regular':self.assertEqual(f['mode'],'0600' if actor else '0644')
        self.assertEqual((self.root/'lifecycle.lock.EMPTY').read_bytes(),b'')
        self.assertIn('NONE',self.manifest['future_records']['root_claim_and_receipt'])
    def test_reproducible_manifest_and_pinned_reader_sources(self):
        before={p.name:p.read_bytes() for p in self.root.iterdir()};build.prepare(self.root)
        self.assertEqual(before,{p.name:p.read_bytes() for p in self.root.iterdir()})
        raw=(self.root/'socket-safety-reader.pinned.py').read_text();ast.parse(raw)
        self.assertIn(build.sha(raw.encode()),(self.root/'check-socket-safety.fish').read_text())
        chunks=[ast.literal_eval(n.args[0]) for n in ast.walk(ast.parse(raw)) if isinstance(n,ast.Call) and isinstance(n.func,ast.Name) and n.func.id=='compile']
        self.assertEqual(chunks,[(build.SOURCE/(n+'.py')).read_text() for n in ('stale_socket_binding_readonly','home_socket_safety_readonly')])
    def test_manual_unit_no_enable_reload_restart_or_cleanup(self):
        unit=(self.root/Path(build.UNIT).name).read_text()
        for text in ('Restart=no',' root-claim',' run','NoNewPrivileges=yes'):self.assertIn(text,unit)
        for text in ('[Install]','ExecStopPost','--steal-socket','daemon-reload'):self.assertNotIn(text,unit)

if __name__=='__main__':unittest.main()
