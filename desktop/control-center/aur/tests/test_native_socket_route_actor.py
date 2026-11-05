import json,sys,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import native_socket_route_readonly as route

class Actor(unittest.TestCase):
    def test_wrong_actor_identity_never_spawns_query(self):
        with patch.object(route.os,'getresuid',return_value=(0,)*3):
            result=route.probe_actor('pinned',execute=lambda *a,**k:self.fail())
        self.assertEqual(result['native_route_probe'],'BLOCKED_ACTOR_ROUTE')
    def test_actor_uses_pinned_source_without_trying_root_uid_drop(self):
        ready={'status':'QUERY_CONTEXT_READY','identity':'PASS_REAL_EFFECTIVE_SAVED_FS_UID949_GID948','capabilities':'ZERO','supplementary_groups':[948],'no_new_privs':True,'landlock_abi':3,'seccomp_notification':True}
        calls=[]
        def execute(argv,**kwargs):
            calls.append(argv);self.assertIn('child(actor=True)',argv[-1]);self.assertIn('PINNED_SOURCE',argv[-1]);self.assertEqual(kwargs['env'],route.ENV)
            return SimpleNamespace(returncode=0,stdout=(route.CANDIDATE+'\n').encode(),stderr=(route.MARKER+json.dumps(ready)+'\n').encode(),denials=[])
        with patch.object(route.os,'getresuid',return_value=(949,)*3),patch.object(route.os,'getresgid',return_value=(948,)*3),patch.object(route.os,'getgroups',return_value=[948]):
            result=route.probe_actor('# PINNED_SOURCE',execute)
        self.assertEqual(result['signer_service_path_equality'],'MATCH');self.assertEqual(len(calls),1)
        self.assertEqual(result['socket_path'],route.HOME+'/S.gpg-agent')
    def test_actor_supervision_errno_is_preserved(self):
        def execute(*a,**k):raise PermissionError(1,'sensitive')
        with patch.object(route.os,'getresuid',return_value=(949,)*3),patch.object(route.os,'getresgid',return_value=(948,)*3),patch.object(route.os,'getgroups',return_value=[948]):r=route.probe_actor('fixed',execute)
        self.assertEqual(r['errno'],1);self.assertNotIn('sensitive',json.dumps(r))
