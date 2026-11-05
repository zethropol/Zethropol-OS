"""Unprivileged temporary trees and public synthetic child output only."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import manifest_diagnostic_rollout as deploy
import manifest_bundle_launcher as launch
import manifest_workflow as producer
import manifest_failure_capture as consumer


class Rollout(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.path=Path(self.temp.name)
        self.fd=os.open(self.path,deploy.DFLAGS);self.events=[];self.protected=[]
        self.patches=[patch.object(deploy,'ROOT_UID',os.getuid()),patch.object(deploy,'ROOT_GID',os.getgid())]
        for p in self.patches:p.start()
        self.payload={'a.py':b'pass\n','bundle.json':b'{}'}
        self.tx=deploy.Transaction(self.fd,'a'*64,self.payload,
            lambda event,**fields:self.events.append(event),lambda:self.protected.append(True),lambda name:None)

    def tearDown(self):
        os.close(self.fd)
        for p in reversed(self.patches):p.stop()
        self.temp.cleanup()

    def test_atomic_directory_publish_and_protected_checks(self):
        self.tx.apply()
        self.assertTrue(self.tx.published)
        self.assertEqual(deploy.checktree(self.fd,self.tx.final,self.payload),self.tx.owned)
        self.assertFalse((self.path/self.tx.stage).exists())
        self.assertEqual(self.events,['preflight-pass','staging-complete','published','success'])
        self.assertEqual(len(self.protected),3)

    def test_existing_final_never_replaced(self):
        (self.path/self.tx.final).mkdir();(self.path/self.tx.final/'foreign').write_text('preserve')
        with self.assertRaises(RuntimeError):self.tx.apply()
        self.assertEqual((self.path/self.tx.final/'foreign').read_text(),'preserve')
        self.assertFalse((self.path/self.tx.stage).exists())

    def test_existing_pending_or_quarantine_block_without_reuse(self):
        for name in (self.tx.stage,self.tx.quarantine):
            p=self.path/name;p.mkdir()
            with self.assertRaises(RuntimeError):self.tx.apply()
            self.assertEqual(self.events,[]);p.rmdir()

    def test_commit_collision_preserves_foreign_final_and_own_stage(self):
        def race(event,**fields):
            self.events.append(event)
            if event=='staging-complete':(self.path/self.tx.final).mkdir()
        self.tx.audit=race
        with self.assertRaises(OSError):self.tx.apply()
        self.assertTrue((self.path/self.tx.stage/'a.py').exists())
        self.assertEqual(list((self.path/self.tx.final).iterdir()),[])

    def test_postflight_failure_quarantines_only_owned_release(self):
        self.tx.postflight=lambda _:(_ for _ in ()).throw(RuntimeError('injected'))
        with self.assertRaises(RuntimeError):self.tx.apply()
        self.assertFalse((self.path/self.tx.final).exists())
        self.assertTrue((self.path/self.tx.quarantine/'a.py').exists())
        self.assertEqual(self.events[-1],'rollback-quarantined')

    def test_foreign_final_inode_stops_rollback_and_keeps_evidence(self):
        self.tx.apply();os.rename(self.path/self.tx.final,self.path/'retained-original')
        (self.path/self.tx.final).mkdir()
        with self.assertRaises(RuntimeError):self.tx.rollback()
        self.assertTrue((self.path/'retained-original'/'a.py').exists())
        self.assertTrue((self.path/self.tx.final).exists())

    def test_changed_member_stops_rollback(self):
        self.tx.apply();(self.path/self.tx.final/'a.py').write_text('changed')
        with self.assertRaises(RuntimeError):self.tx.rollback()
        self.assertTrue((self.path/self.tx.final).exists())

    def test_audit_failure_after_publish_rolls_back(self):
        def audit(event,**fields):
            if event=='published':raise OSError('ENOSPC injected')
            self.events.append(event)
        self.tx.audit=audit
        with self.assertRaises(OSError):self.tx.apply()
        self.assertFalse((self.path/self.tx.final).exists())
        self.assertTrue((self.path/self.tx.quarantine).exists())

    def test_protected_drift_before_commit_prevents_publication(self):
        calls=[]
        def protected():
            calls.append(True)
            if len(calls)==2:raise RuntimeError('protected drift')
        self.tx.protected=protected
        with self.assertRaises(RuntimeError):self.tx.apply()
        self.assertFalse((self.path/self.tx.final).exists())
        self.assertTrue((self.path/self.tx.stage).exists())

    def test_symlink_hardlink_mode_and_acl_members_rejected(self):
        self.tx.apply();file=self.path/self.tx.final/'a.py'
        file.chmod(0o666)
        with self.assertRaises(RuntimeError):deploy.checktree(self.fd,self.tx.final,self.payload)
        file.chmod(0o644);os.link(file,self.path/'extra-link')
        with self.assertRaises(RuntimeError):deploy.checktree(self.fd,self.tx.final,self.payload)
        (self.path/'extra-link').unlink()
        with patch.object(deploy.os,'listxattr',return_value=['system.posix_acl_default']):
            with self.assertRaises(RuntimeError):deploy.checktree(self.fd,self.tx.final,self.payload)
        file.unlink();file.symlink_to(self.path/'missing')
        with self.assertRaises(OSError):deploy.checktree(self.fd,self.tx.final,self.payload)

    def test_no_renameat2_fallback(self):
        with patch.object(deploy.ctypes,'CDLL',return_value=SimpleNamespace()):
            with self.assertRaises(RuntimeError):deploy.noreplace(self.fd,'x',self.fd,'y')

    def test_protected_same_bytes_inode_replacement_is_rejected(self):
        body=b'unchanged';plan={'protected':[{'path':'/usr/lib/zethropol/aur/worker.py','sha256':deploy.sha(body)}]}
        old=SimpleNamespace(st_dev=1,st_ino=1,st_mode=0o100644,st_uid=0,st_gid=0,st_size=len(body),st_mtime_ns=1,st_ctime_ns=1)
        new=SimpleNamespace(**{**vars(old),'st_ino':2})
        with patch.object(deploy,'readpath',return_value=body),patch.object(deploy.Path,'lstat',return_value=old):
            baseline=deploy.protected_check(plan)
        with patch.object(deploy,'readpath',return_value=body),patch.object(deploy.Path,'lstat',return_value=new):
            with self.assertRaises(RuntimeError):deploy.protected_check(plan,baseline)

    def test_duplicate_protected_set_cannot_replace_accepted_thirteen(self):
        row={'path':'/usr/lib/zethropol/aur/worker.py','sha256':'a'*64}
        plan={'schema':1,'purpose':'zethropol-manifest-diagnostics-rollout','release_sha256':'a'*64,
              'files':[],'protected':[row]*13,'protected_plan_sha256':deploy.PROTECTED_PLAN}
        with self.assertRaises(RuntimeError):deploy.loadplan(deploy.canonical(plan))


class Protocol(unittest.TestCase):
    def test_current_producer_consumer_handshake(self):launch.protocol_check(producer,consumer)

    def test_old_or_mixed_producer_consumer_block_before_child(self):
        with self.assertRaises(RuntimeError):launch.protocol_check(SimpleNamespace(DIAGNOSTIC_PROTOCOL='old'),consumer)
        with self.assertRaises(RuntimeError):launch.protocol_check(producer,SimpleNamespace(PROTOCOL_ID='old'))
        fake=SimpleNamespace(PROTOCOL_ID=consumer.PROTOCOL_ID,process_outcome=lambda *a:{'diagnostic_status':'unavailable'})
        with self.assertRaises(RuntimeError):launch.protocol_check(producer,fake)

    def test_legacy_or_wrong_version_marker_rejected(self):
        digest='a'*64
        record=json.loads(producer.failure_record(producer.SigningPhaseRejected('policy-load'),digest).split(' ',1)[1])
        record.pop('protocol')
        self.assertIsNone(consumer.decode(consumer.PREFIX+json.dumps(record).encode(),digest))
        record['protocol']='zethropol-manifest-diagnostics/0'
        self.assertIsNone(consumer.decode(consumer.PREFIX+json.dumps(record).encode(),digest))

    def test_bundle_file_mixing_and_extra_members_block(self):
        with tempfile.TemporaryDirectory() as temporary:
            files={name:b'# fixture\n' for name in ('manifest_bundle_launcher.py','manifest_workflow.py','manifest_failure_capture.py')}
            description={'schema':1,'protocol':launch.PROTOCOL,'files':[{'name':name,'sha256':hashlib.sha256(raw).hexdigest(),'mode':'0644'} for name,raw in sorted(files.items())]}
            raw=launch.canonical(description);directory=Path(temporary)/('aur-manifest-diagnostics-'+hashlib.sha256(raw).hexdigest());directory.mkdir()
            for name,body in files.items():(directory/name).write_bytes(body);(directory/name).chmod(0o644)
            (directory/'bundle.json').write_bytes(raw)
            with patch.object(launch,'read_fixed',side_effect=lambda p:Path(p).read_bytes()):
                launch.verify_bundle(directory)
                (directory/'manifest_workflow.py').write_text('# old version\n')
                with self.assertRaises(RuntimeError):launch.verify_bundle(directory)
                (directory/'manifest_workflow.py').write_bytes(files['manifest_workflow.py'])
                (directory/'foreign').write_text('extra')
                with self.assertRaises(RuntimeError):launch.verify_bundle(directory)

    def synthetic_popen(self,command,**kwargs):
        kwargs.pop('preexec_fn')
        return subprocess.Popen(command,**kwargs)

    def test_bounded_drain_handles_large_synthetic_stderr_and_records_exit(self):
        events=[]
        with self.assertRaises(consumer.SignerProcessBlocked) as caught:
            launch.observe_process([sys.executable,'-I','-S','-B','-c',"import os;os.write(2,b'SECRET'*12000);raise SystemExit(2)"],
                consumer,'a'*64,lambda name,**fields:events.append(fields),popen=self.synthetic_popen)
        self.assertEqual(caught.exception.outcome['child_returncode'],2)
        self.assertIsNone(caught.exception.outcome['diagnostic'])
        self.assertNotIn('SECRET',json.dumps(events))

    def test_synthetic_zero_exit_remains_unverified(self):
        result=launch.observe_process([sys.executable,'-I','-S','-B','-c','pass'],consumer,'a'*64,
                                     lambda *a,**k:None,popen=self.synthetic_popen)
        self.assertFalse(result['signature_verified'])

    def test_launch_failure_and_timeout_are_audited_without_secret(self):
        events=[]
        def denied(*a,**k):raise PermissionError('SECRET')
        with self.assertRaises(consumer.SignerProcessBlocked) as caught:
            launch.observe_process([],consumer,'a'*64,lambda name,**fields:events.append(fields),popen=denied)
        self.assertEqual(caught.exception.outcome['process_result'],'launch-failed')
        with self.assertRaises(consumer.SignerProcessBlocked) as caught:
            launch.observe_process([sys.executable,'-I','-S','-B','-c','import time;time.sleep(5)'],consumer,'a'*64,
                lambda name,**fields:events.append(fields),popen=self.synthetic_popen,timeout=0.05)
        self.assertEqual(caught.exception.outcome['process_result'],'timeout')
        self.assertNotIn('SECRET',json.dumps(events))


if __name__=='__main__':unittest.main()
