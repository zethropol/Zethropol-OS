"""Key-free composition guards; never invokes a worker, signer or broker."""
import sys
from pathlib import Path
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pipeline import run
from worker import Rejected, canonical, digest


class PipelineGuards(unittest.TestCase):
    def test_provider_budget_cancellation_stops_before_loading_trust(self):
        with patch('pipeline.os.getuid',return_value=1000),patch('pipeline.os.geteuid',return_value=1000),patch('pipeline.TrustPolicy.load') as load:
            with self.assertRaisesRegex(Rejected,'Cancelled'):run(self.request(),cancel=lambda:True)
            load.assert_not_called()
    def request(self):
        plan = {'fixture': 'untrusted source plan'}
        return {'store':'/unused', 'recipe':'/unused', 'plan':plan,
                'plan_sha256':digest(canonical(plan)), 'manifest':'/unused',
                'manifest_signature':'/unused', 'build_root':'/unused',
                'packages':[{'archive':'/unused', 'signature':'/unused'}]}

    def test_root_cannot_enter_build_pipeline(self):
        with patch('pipeline.os.getuid', return_value=0), patch('pipeline.TrustPolicy.load') as load:
            with self.assertRaisesRegex(Rejected, 'never root'): run(self.request())
            load.assert_not_called()

    def test_changed_plan_is_rejected_before_loading_trust_or_running_worker(self):
        request=self.request(); request['plan_sha256']='0'*64
        with patch('pipeline.os.getuid', return_value=1000), patch('pipeline.os.geteuid', return_value=1000), patch('pipeline.TrustPolicy.load') as load:
            with self.assertRaisesRegex(Rejected, 'changed'): run(request)
            load.assert_not_called()

    def test_missing_production_policy_never_falls_back_to_test_or_host_tools(self):
        with patch('pipeline.os.getuid', return_value=1000), patch('pipeline.os.geteuid', return_value=1000), patch('pipeline.TrustPolicy.load', side_effect=Rejected('Missing project role')) as load, patch('pipeline.QuotaWorker') as worker:
            with self.assertRaisesRegex(Rejected, 'Missing project role'): run(self.request())
            load.assert_called_once_with('/etc/zethropol-update/aur-trust/policy.json', role='manifest')
            worker.assert_not_called()

    def test_incomplete_closure_or_extra_command_cannot_run(self):
        for request in [dict(self.request(), packages=[]), dict(self.request(), command='anything')]:
            with patch('pipeline.os.getuid', return_value=1000), patch('pipeline.os.geteuid', return_value=1000), patch('pipeline.QuotaWorker') as worker:
                with self.assertRaises(Rejected): run(request)
                worker.assert_not_called()

class ManagedLifecycle(unittest.TestCase):
    def test_cancel_is_bound_to_execution_and_parent_disconnect_cancels(self):
        import io,os,json,uuid
        from pipeline import ManagedChannel
        read,write=os.pipe();self.addCleanup(os.close,read);self.addCleanup(os.close,write)
        execution=str(uuid.uuid4());other=str(uuid.uuid4())
        os.write(write,(json.dumps({'execution':execution,'request':{}})+'\n').encode())
        channel=ManagedChannel(read,io.StringIO());self.assertEqual(channel.initial(),{})
        os.write(write,(json.dumps({'cancel':other})+'\n').encode());self.assertFalse(channel.cancelled())
        os.write(write,(json.dumps({'cancel':execution})+'\n').encode());self.assertTrue(channel.cancelled())

    def test_supervisor_cancel_kills_real_child_group_and_persists_terminal(self):
        import os,tempfile,time
        from journal import Supervisor
        from worker import Worker
        with tempfile.TemporaryDirectory() as temporary:
            store=Path(temporary);store.chmod(0o700)
            started=store/'child'
            class LabWorker(Worker):
                def run(self,plan,approved,recipe,root,cancel,timeout,operation):
                    self.report('building')
                    script="import subprocess,time,pathlib; p=subprocess.Popen(['%s','-c','import time;time.sleep(30)']);pathlib.Path(%r).write_text(str(p.pid));time.sleep(30)"%(sys.executable,str(started))
                    try: self.execute([sys.executable,'-c',script],store,cancel,timeout)
                    except Rejected as error:
                        return {'state':'cancelled','operation':operation,'reason':str(error),'archives':[]}
                    raise AssertionError('fixture should be cancelled')
            supervisor=Supervisor(store);self.addCleanup(supervisor.journal.close)
            events=[]
            result=supervisor.run(LabWorker(store),{},'a'*64,store,store,cancel=started.exists,notify=events.append)
            self.assertEqual(result['state'],'cancelled');self.assertEqual(result['cleanup'],'verified')
            self.assertEqual(supervisor.journal.status(result['operation'])['state'],'cancelled')
            self.assertEqual(events[0]['event'],'started');self.assertEqual(events[1]['stage'],'building')
            child=int(started.read_text());status=Path('/proc')/str(child)/'stat'
            self.assertTrue(not status.exists() or status.read_text().split()[2]=='Z')
            with self.assertRaises(Rejected),supervisor.journal.lease(result['operation']): pass

    def test_scope_cleanup_failure_never_becomes_successful_cancellation(self):
        import tempfile
        from journal import Supervisor
        with tempfile.TemporaryDirectory() as temporary:
            store=Path(temporary);store.chmod(0o700)
            class LabWorker:
                def run(self,plan,approved,recipe,root,cancel,timeout,operation):
                    self.journal.scope(operation,'zethropol-aur-'+operation+'-1.scope')
                    return {'state':'cancelled','operation':operation,'reason':'Cancelled','archives':[]}
            supervisor=Supervisor(store);self.addCleanup(supervisor.journal.close)
            with patch('scoped.stop_scope',side_effect=Rejected('Cannot verify cgroup stop')):
                result=supervisor.run(LabWorker(),{},'a'*64,store,store)
            self.assertEqual(result['state'],'interrupted');self.assertEqual(result['cleanup'],'unknown')
            self.assertTrue(result['scopes'])

    def test_parent_pipe_disconnect_is_a_cancellation_request(self):
        import io,os,json,uuid
        from pipeline import ManagedChannel
        read,write=os.pipe()
        with os.fdopen(read,'rb',buffering=0) as parent,os.fdopen(write,'wb',buffering=0) as writer:
            writer.write((json.dumps({'execution':str(uuid.uuid4()),'request':{}})+'\n').encode())
            channel=ManagedChannel(parent.fileno(),io.StringIO());channel.initial()
            writer.close();self.assertTrue(channel.cancelled())

    def test_cancel_wins_build_result_publication_without_artifact_handoff(self):
        import tempfile
        from journal import Journal
        with tempfile.TemporaryDirectory() as temporary:
            store=Path(temporary);store.chmod(0o700)
            journal=Journal(store)
            try:
                operation=journal.create('a'*64,30)
                with journal.lease(operation):
                    journal.cancel(operation)
                    result=journal.finish(operation,{'state':'built-untrusted','archives':[{'untrusted':'fixture'}]})
                self.assertEqual(result['state'],'cancelled');self.assertEqual(result['archives'],[])
                self.assertEqual(journal.status(operation)['state'],'cancelled')
            finally: journal.close()
