"""Producer unit/filesystem checks; never production authorization or activation.

Real temporary sockets and O_EXCL records exercise Linux mutation semantics.
Authority-boundary mocks below are only unit inputs, never installed or published.
"""
from contextlib import ExitStack
import time
import concurrent.futures
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import agent_lifecycle_949 as life

SHA='a'*64
AID='b'*32

class Producer(unittest.TestCase):
    def test_live_process_cannot_enter_maintenance_or_touch_policy(self):
        with patch.object(life,'root_host_identity'),patch.object(life.os,'getpid',return_value=123),patch.object(life,'owner_authorization') as authorize,patch.object(life,'root_input') as read:
            with self.assertRaises(life.Refused):life.produce_activation(SHA)
            self.assertEqual(life.PHASE,'exclusive-rescue-init-required')
            authorize.assert_not_called();read.assert_not_called()

    def test_rescue_wrapper_refuses_live_root_without_exec(self):
        with patch.object(life,'root_host_identity'),patch.object(life.os,'getpid',return_value=123),patch.object(life.os,'execve') as execute:
            with self.assertRaises(life.Refused):life.main('rescue-init')
            execute.assert_not_called()

    def test_boot_selection_requires_both_exact_operation_scopes_once(self):
        activate='zethropol.uid949.activate='+SHA
        quarantine='zethropol.uid949.quarantine='+SHA
        for words in (activate,quarantine,activate+' '+quarantine+' '+activate,
                      activate+' '+quarantine.replace(SHA,'c'*64),activate+' '+quarantine.replace(SHA,'bad')):
            with self.subTest(words=words),patch.object(Path,'read_text',return_value=words):
                with self.assertRaises(life.Refused):life.boot_selection()
        with patch.object(Path,'read_text',return_value=activate+' '+quarantine):self.assertEqual(life.boot_selection(),SHA)

    def test_missing_owner_authority_cannot_reserve_or_mutate(self):
        with patch.object(life,'exclusive_boot_window'),patch.object(life,'owner_authorization',side_effect=life.Refused()),patch.object(life,'durable_create') as create,patch.object(life,'rename_socket') as rename:
            with self.assertRaises(life.Refused):life.produce_activation(SHA)
            create.assert_not_called();rename.assert_not_called()

    def test_inactive_policy_drift_is_rejected_before_consumption(self):
        with patch.object(life,'exclusive_boot_window'),patch.object(life,'owner_authorization',return_value={}),patch.object(life,'root_input',return_value=b'{}'),patch.object(life,'durable_create') as create:
            with self.assertRaises(life.Refused):life.produce_activation(SHA)
            create.assert_not_called()

    def test_durable_intent_has_exactly_one_winner_under_race(self):
        with tempfile.TemporaryDirectory() as directory:
            path=directory+'/intent'
            def attempt(_):
                try:life.durable_create(path,b'consumed');return True
                except FileExistsError:return False
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                results=list(pool.map(attempt,range(24)))
            self.assertEqual(sum(results),1)
            self.assertEqual(Path(path).read_bytes(),b'consumed')
            self.assertEqual(os.stat(path).st_mode&0o777,0o644)

    def test_write_crash_leaves_poisoned_intent_and_denies_replay(self):
        with tempfile.TemporaryDirectory() as directory:
            path=directory+'/intent'
            with patch.object(life.os,'write',side_effect=OSError('injected write failure')):
                with self.assertRaises(OSError):life.durable_create(path,b'consumed')
            self.assertTrue(Path(path).exists())
            with self.assertRaises(FileExistsError):life.durable_create(path,b'retry')
            self.assertEqual(Path(path).read_bytes(),b'')

    def test_partial_writes_are_finished_before_commit(self):
        with tempfile.TemporaryDirectory() as directory:
            original=os.write
            with patch.object(life.os,'write',side_effect=lambda fd,data:original(fd,data[:2])):
                life.durable_create(directory+'/record',b'abcdefg')
            self.assertEqual(Path(directory+'/record').read_bytes(),b'abcdefg')

    def test_four_real_socket_renames_preserve_identity_and_never_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'source';target=Path(directory)/'quarantine'
            source.mkdir();target.mkdir()
            for name in life.SOCKET_NAMES:
                path=source/name
                sock=socket.socket(socket.AF_UNIX);sock.bind(str(path));sock.close();os.chmod(path,0o600)
                before=life.socket_snapshot(path)
                life.rename_socket(path,target/name)
                after=life.socket_snapshot(target/name)
                self.assertFalse(path.exists())
                self.assertEqual({k:v for k,v in before.items() if k!='ctime_ns'},
                                 {k:v for k,v in after.items() if k!='ctime_ns'})
                sock=socket.socket(socket.AF_UNIX);sock.bind(str(path));sock.close()
                replacement=life.socket_snapshot(path)
                with self.assertRaises(FileExistsError):life.rename_socket(path,target/name)
                self.assertEqual(life.socket_snapshot(path),replacement)
                self.assertEqual(life.socket_snapshot(target/name),after)

    def operation(self,directory,fail_rename=None):
        """Exercise real files; substitute authority checks ONLY in this unit test."""
        home=Path(directory)/'home';gnupg=home/'gnupg';state=Path(directory)/'state'
        gnupg.mkdir(parents=True);(state/'quarantine').mkdir(parents=True,mode=0o700)
        policy_path=Path(directory)/'policy.json';config=gnupg/'gpg-agent.conf'
        config.write_text('pinentry-program /unit-test/pinentry\n')
        before={}
        for name in life.SOCKET_NAMES:
            path=gnupg/name
            with socket.socket(socket.AF_UNIX) as sock:sock.bind(str(path))
            os.chmod(path,0o600);before[name]=life.socket_snapshot(path)
        policy={'schema':2,'purpose':'zethropol-agent-home-lifecycle-949','generation':life.GENERATION,
                'uid':949,'gid':948,'gnupg_home':str(gnupg),'socket_path':str(gnupg/'S.gpg-agent'),
                'activation_approved':False,'activation_id':None,'quarantine_receipt':None,
                'old_socket':before['S.gpg-agent']}
        raw=life.canonical(policy);policy_path.write_bytes(raw)
        authorization={'manifest_sha256':SHA,'approval_sha256':'f'*64,'expires':int(time.time())+600}
        actual_rename=life.rename_socket
        count=0
        def rename(source,target):
            nonlocal count
            count+=1
            if count==fail_rename:raise OSError('injected quarantine interruption')
            return actual_rename(source,target)
        def read(path,limit=65536):
            if path==str(policy_path) or str(path).startswith(str(state)):return Path(path).read_bytes()
            return b'unit code pin'
        with ExitStack() as stack:
            for name,value in {'HOME':str(home),'GNUPG':str(gnupg),'SOCKET':str(gnupg/'S.gpg-agent'),
                               'STATE':str(state),'POLICY':str(policy_path),'CONFIG':str(config),
                               'INACTIVE_POLICY_SHA':life.sha(raw)}.items():
                stack.enter_context(patch.object(life,name,value))
            for name in ('exclusive_boot_window','node','verified_config','validate_old_socket_binding','common_checks'):
                stack.enter_context(patch.object(life,name))
            stack.enter_context(patch.object(life,'owner_configuration_presence',return_value=[]))
            stack.enter_context(patch.object(life,'owner_authorization',return_value=authorization))
            stack.enter_context(patch.object(life,'root_input',side_effect=read))
            stack.enter_context(patch.object(life,'pin_binary',return_value='e'*64))
            stack.enter_context(patch.object(life,'actor_lock',side_effect=lambda **kw:os.open(directory,os.O_RDONLY)))
            stack.enter_context(patch.object(life,'load_policy',side_effect=lambda:json.loads(policy_path.read_bytes())))
            stack.enter_context(patch.object(life,'check_quarantine_receipt'))
            stack.enter_context(patch.object(life,'rename_socket',side_effect=rename))
            # Old-boot constraint is a separate security check. These sockets are
            # freshly created LOCAL unit objects, not historical production proof.
            stat_read=Path.read_text
            def text(path,*args,**kwargs):
                if str(path)=='/proc/stat':return 'btime '+str(int(time.time())+60)+'\n'
                return stat_read(path,*args,**kwargs)
            stack.enter_context(patch.object(Path,'read_text',text))
            return life.produce_activation(SHA)

    def test_full_producer_publishes_bound_receipt_after_all_four_renames(self):
        with tempfile.TemporaryDirectory() as directory:
            result=self.operation(directory)
            state=Path(directory)/'state'
            policy=json.loads((Path(directory)/'policy.json').read_bytes())
            receipt=json.loads(Path(policy['quarantine_receipt']).read_bytes())
            self.assertTrue(policy['activation_approved'])
            self.assertEqual(result['policy_sha256'],life.sha((Path(directory)/'policy.json').read_bytes()))
            self.assertEqual(result['receipt_sha256'],life.sha(Path(policy['quarantine_receipt']).read_bytes()))
            self.assertEqual(result['activation_id'],receipt['activation_id'])
            self.assertNotEqual(receipt['activation_id'],receipt['maintenance_id'])
            self.assertEqual(set(receipt['sockets']),set(life.SOCKET_NAMES))
            for name,pair in receipt['sockets'].items():
                archived=state/'quarantine'/receipt['maintenance_id']/name
                self.assertEqual(life.socket_snapshot(archived),pair['after'])
                self.assertFalse((Path(directory)/'home'/'gnupg'/name).exists())
            self.assertTrue((state/('production.'+SHA+'.used')).exists())

    def test_interrupted_second_rename_preserves_evidence_and_never_approves(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(OSError):self.operation(directory,fail_rename=2)
            state=Path(directory)/'state'
            policy=json.loads((Path(directory)/'policy.json').read_bytes())
            self.assertFalse(policy['activation_approved'])
            self.assertIsNone(policy['activation_id'])
            intent=state/('production.'+SHA+'.used')
            self.assertTrue(intent.exists())
            with self.assertRaises(FileExistsError):life.durable_create(str(intent),b'retry')
            self.assertEqual(len(list((state/'quarantine').glob('*/S.gpg-agent'))),1)
            self.assertTrue((Path(directory)/'home'/'gnupg'/'S.gpg-agent.ssh').exists())
            self.assertEqual(list(state.glob('activation.*.json')),[])
            self.assertEqual(list(state.glob('quarantine.*.json')),[])

    def test_policy_publish_crash_retains_closed_policy_and_consumed_intent(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(life.os,'replace',side_effect=OSError('injected commit interruption')):
                with self.assertRaises(OSError):self.operation(directory)
            state=Path(directory)/'state'
            policy_path=Path(directory)/'policy.json'
            self.assertFalse(json.loads(policy_path.read_bytes())['activation_approved'])
            self.assertEqual(len(list(state.glob('activation.*.json'))),1)
            self.assertEqual(len(list(state.glob('quarantine.*.json'))),1)
            self.assertTrue((state/('production.'+SHA+'.used')).exists())
            record=json.loads(next(state.glob('activation.*.json')).read_bytes())
            self.assertNotEqual(record['policy_sha256'],life.sha(policy_path.read_bytes()))

    def test_nested_pid1_cannot_satisfy_host_maintenance(self):
        from types import SimpleNamespace
        with patch.object(life,'root_host_identity'),patch.object(life.os,'getpid',return_value=1),patch.object(life.os,'stat',return_value=SimpleNamespace(st_ino=123)),patch.object(life,'boot_selection') as selection:
            with self.assertRaises(life.Refused):life.exclusive_boot_window(SHA)
            selection.assert_not_called()

    def record(self):
        approval={'schema':1,'purpose':'zethropol-approved-root-manifest','manifest_sha256':SHA,'expires':1000}
        receipt=b'public receipt unit bytes'
        policy={'activation_id':AID,'_policy_sha256':'c'*64,'quarantine_receipt':'receipt'}
        record={'schema':1,'purpose':'zethropol-lifecycle-activation','activation_id':AID,
                'policy_sha256':'c'*64,'receipt_sha256':life.sha(receipt),'boot_id':'boot',
                'namespace_inodes':{'pid':1},'manifest_sha256':SHA,'approval_sha256':life.sha(life.canonical(approval)),'expires':1000}
        return policy,record,approval,receipt

    def verify(self,policy,record,approval,receipt):
        def read(path):
            if path=='receipt':return receipt
            if path.endswith(SHA+'.json'):return life.canonical(approval)
            return life.canonical(record)
        with patch.object(life,'root_input',side_effect=read),patch.object(life,'boot_id',return_value='boot'),patch.object(life,'namespace_inodes',return_value={'pid':1}),patch.object(life.time,'time',return_value=500):
            return life.verify_activation(policy)

    def test_commit_binds_exact_policy_receipt_approval_boot_and_expiry(self):
        policy,record,approval,receipt=self.record()
        self.assertEqual(self.verify(policy,record,approval,receipt),record)
        for changes in ({'policy_sha256':'d'*64},{'receipt_sha256':'d'*64},{'approval_sha256':'d'*64},
                        {'activation_id':'d'*32},{'boot_id':'other'},{'expires':499},
                        {'namespace_inodes':{'pid':2}},{'manifest_sha256':'invalid'}):
            with self.subTest(changes=changes),self.assertRaises(life.Refused):
                self.verify(policy,dict(record,**changes),approval,receipt)
        with self.assertRaises(life.Refused):self.verify(policy,record,dict(approval,expires=1001),receipt)
        with self.assertRaises(life.Refused):self.verify(policy,record,approval,receipt+b'changed')

    def test_no_producer_record_means_no_claim_or_handoff(self):
        with patch.object(life,'verify_activation',side_effect=FileNotFoundError()),patch.object(life.os,'open') as opened:
            for method in (life.root_claim,life.verify_claim):
                with self.assertRaises(FileNotFoundError):method({'activation_id':AID})
            opened.assert_not_called()

if __name__=='__main__':unittest.main()
