"""V2 guards. Fake agent processes only; live confinement uses test files."""
import errno,fcntl,hashlib,json,os,signal,socket,stat,subprocess,sys,tempfile,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch,mock_open
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import agent_lifecycle_949 as life

class Lifecycle(unittest.TestCase):
    def policy(self):
        return {'schema':2,'purpose':'zethropol-agent-home-lifecycle-949','generation':life.GENERATION,'uid':949,'gid':948,
                'gnupg_home':life.GNUPG,'socket_path':life.SOCKET,'old_socket':None,'quarantine_receipt':None,
                'helper_sha256':'a'*64,'route_reader_sha256':'a'*64,'unit_sha256':'a'*64,
                'binary_sha256':{'/usr/bin/gpg-agent':None,'/usr/bin/gpgconf':None},'agent_config_sha256':life.CONFIG_HASH,
                'agent_config_metadata':None,'activation_id':None,'activation_approved':False}
    def test_natural_home_route_and_preserved_config_argv(self):
        argv=life.agent_argv(42)
        self.assertEqual(argv[argv.index('--homedir')+1],life.GNUPG)
        self.assertEqual(argv[argv.index('--options')+1],'/proc/self/fd/42')
        for item in ('--daemon','--no-detach','--no-allow-loopback-pinentry','--no-allow-external-cache'):self.assertIn(item,argv)
        for item in ('--supervised','--steal-socket','--no-options','--enable-ssh-support'):self.assertNotIn(item,argv)
        self.assertEqual(life.SOCKET,life.GNUPG+'/S.gpg-agent')
    def test_unapproved_or_legacy_policy_cannot_start(self):
        for p in (self.policy(),dict(self.policy(),schema=1)):
            with patch.object(life,'root_input',return_value=json.dumps(p).encode()):
                with self.assertRaises(life.Refused):life.load_policy()
    def test_socket_policy_has_only_two_exact_modes_and_keeps_identity_fields(self):
        old={'device':'38','inode':'1936362','uid':949,'gid':948,'mode':'0o700','ctime_ns':'1793481059412486406','acl':{}}
        for mode in ('0o600','0o700'):
            life.validate_old_socket_binding(dict(old,mode=mode))
        for change in ({'mode':'0o500'},{'mode':'0o1700'},{'mode':'0700'},{'uid':0},{'gid':949},{'acl':{'access':'x'}},{'ctime_ns':None},{'inode':None}):
            with self.assertRaises(life.Refused):life.validate_old_socket_binding(dict(old,**change))

    def test_root_cannot_be_actor(self):
        with patch.object(life.os,'getresuid',return_value=(0,)*3):
            with self.assertRaises(life.Refused):life.actor_identity()
    def test_stale_socket_presence_is_rejected_without_modification(self):
        with tempfile.TemporaryDirectory() as d:
            path=d+'/stale';sock=socket.socket(socket.AF_UNIX);sock.bind(path);sock.close()
            before=os.lstat(path)
            with self.assertRaises(life.Refused):life.socket_absent(path)
            self.assertEqual(os.lstat(path),before)
    def test_snapshot_retains_inode_ctime_owner_mode_and_acl(self):
        with tempfile.TemporaryDirectory() as d:
            path=d+'/stale';sock=socket.socket(socket.AF_UNIX);sock.bind(path);sock.close()
            before=os.lstat(path);snap=life.socket_snapshot(path)
            self.assertEqual((snap['inode'],snap['ctime_ns']),(str(before.st_ino),str(before.st_ctime_ns)))
            self.assertEqual((snap['uid'],snap['gid'],snap['acl']),(before.st_uid,before.st_gid,{}))
            self.assertEqual(os.lstat(path),before)
    def test_changed_stale_inode_does_not_pass_pin(self):
        with patch.object(life,'socket_snapshot',return_value={'inode':'3'}):
            with self.assertRaises(life.Refused):life.old_socket_check({'old_socket':{'inode':'2'}})
    def test_listener_or_bound_nonlistener_blocks(self):
        for flags in ('00010000','00000000'):
            row='header\n000: 2 0 '+flags+' 0001 01 123 '+life.SOCKET+'\n'
            with patch.object(Path,'read_text',return_value=row):self.assertFalse(life.no_listener(life.SOCKET))
    def test_existing_stale_blocks_common_checks_before_launch(self):
        with patch.object(life,'node'),patch.object(Path,'lstat',return_value=SimpleNamespace(st_mode=0o40755,st_uid=0,st_gid=0)),patch.object(life.os,'listxattr',return_value=[]),patch.object(life,'owner_configuration_presence',return_value=['owner-agent-config']),patch.object(life,'verified_config'),patch.object(life,'no_listener',return_value=True),patch.object(life,'any_uid949_agent',return_value=False),patch.object(life,'socket_absent',side_effect=life.Refused()),patch.object(life.subprocess,'Popen') as launch:
            with self.assertRaises(life.Refused):life.common_checks({})
            launch.assert_not_called()
    def test_config_is_pinned_noatime_and_not_rewritten(self):
        raw=b'pinentry-program /usr/bin/example-pinentry\n';digest=hashlib.sha256(raw).hexdigest()
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'gpg-agent.conf';path.write_bytes(raw);os.chmod(path,0o600)
            s=path.stat();fake=SimpleNamespace(st_dev=s.st_dev,st_ino=s.st_ino,st_uid=949,st_gid=948,st_mode=s.st_mode,st_ctime_ns=s.st_ctime_ns,st_size=s.st_size,st_nlink=1)
            policy={'agent_config_sha256':digest,'agent_config_metadata':life.config_metadata(fake)}
            with patch.object(life,'CONFIG',str(path)),patch.object(life,'CONFIG_HASH',digest),patch.object(life.os,'lstat',return_value=fake),patch.object(life.os,'fstat',return_value=fake):
                self.assertEqual(life.verified_config(policy),raw)
                with self.assertRaises(life.Refused):life.verified_config(dict(policy,agent_config_sha256='0'*64))
            self.assertEqual(path.read_bytes(),raw)
    def test_route_gate_mismatch_never_seals_config_or_starts_agent(self):
        source=b'def probe_actor(source):return {"native_route_probe":"COMPLETED_READONLY_HOST_UID949","socket_path":"/run/other","signer_service_path_equality":"MISMATCH","agent_started":False}\n'
        with patch.object(life,'actor_identity'),patch.object(life,'common_checks'),patch.object(life,'verify_claim'),patch.object(life,'check_quarantine_receipt'),patch.object(life,'root_input',return_value=source),patch.object(life.os,'memfd_create') as create:
            with self.assertRaises(life.Refused):life.prepare_home({'route_reader_sha256':hashlib.sha256(source).hexdigest()})
            create.assert_not_called()
    def test_route_gate_seals_exact_existing_config(self):
        source=('def probe_actor(source):return '+repr(dict(native_route_probe='COMPLETED_READONLY_HOST_UID949',socket_path=life.SOCKET,signer_service_path_equality='MATCH',agent_started=False))+'\n').encode()
        raw=b'pinentry-program /public/example\n'
        with patch.object(life,'actor_identity'),patch.object(life,'common_checks'),patch.object(life,'verify_claim'),patch.object(life,'check_quarantine_receipt'),patch.object(life,'root_input',return_value=source),patch.object(life,'socket_absent'),patch.object(life,'no_listener',return_value=True),patch.object(life,'verified_config',return_value=raw):
            fd=life.prepare_home({'route_reader_sha256':hashlib.sha256(source).hexdigest()})
        try:
            self.assertEqual(os.pread(fd,4096,0),raw)
            self.assertTrue(fcntl.fcntl(fd,fcntl.F_GET_SEALS)&fcntl.F_SEAL_WRITE)
            with self.assertRaises(OSError):os.write(fd,b'changed')
        finally:os.close(fd)
    def test_root_activation_claim_is_exclusive_and_cannot_be_reused(self):
        with tempfile.TemporaryDirectory() as d,patch.object(life,'STATE',d),patch.object(life,'node'),patch.object(life,'verify_activation'),patch.object(life,'boot_id',return_value='fresh-boot'),patch.object(life,'boottime_ns',return_value=100):
            p={'activation_id':'a'*32,'_policy_sha256':'b'*64};life.root_claim(p)
            path=Path(life.claim_path(p));before=path.read_bytes()
            with self.assertRaises(FileExistsError):life.root_claim(p)
            self.assertEqual(path.read_bytes(),before)
            with patch.object(life,'root_input',return_value=before):life.verify_claim(p)
            with patch.object(life,'root_input',return_value=before),patch.object(life,'boot_id',return_value='different-boot'):
                with self.assertRaises(life.Refused):life.verify_claim(p)
    def test_expired_claim_never_gets_new_budget(self):
        p={'activation_id':'a'*32,'_policy_sha256':'b'*64}
        record=dict(event='root-activation-consumed',activation_id=p['activation_id'],policy_sha256=p['_policy_sha256'],boot_id='boot',start_deadline_boottime_ns='99')
        with patch.object(life,'verify_activation'),patch.object(life,'root_input',return_value=json.dumps(record).encode()),patch.object(life,'boot_id',return_value='boot'),patch.object(life,'boottime_ns',return_value=100):
            with self.assertRaises(life.Refused):life.verify_claim(p)
    def test_actor_used_marker_cannot_be_replaced(self):
        with tempfile.TemporaryDirectory() as d,patch.object(life,'AUDIT',d):
            p={'activation_id':'a'*32,'_policy_sha256':'b'*64};life.consume_actor_start(p)
            with self.assertRaises(FileExistsError):life.consume_actor_start(p)
    def test_lock_is_held_across_route_prepare_and_agent_supervision(self):
        events=[]
        with patch.object(life,'actor_identity'),patch.object(life,'load_policy',return_value={}),patch.object(life,'actor_lock',return_value=42),patch.object(life,'prepare_home',side_effect=lambda p:events.append('prepare') or 43),patch.object(life,'supervise',side_effect=lambda *a:events.append('supervise')),patch.object(life.os,'close',side_effect=lambda fd:events.append(('close',fd))):life.main('run')
        self.assertEqual(events,['prepare','supervise',('close',43),('close',42)])
    def test_root_claim_guard_failure_does_not_consume(self):
        with patch.object(life,'root_host_identity'),patch.object(life,'load_policy',return_value={}),patch.object(life,'actor_lock',return_value=42),patch.object(life.os,'close'),patch.object(life,'common_checks',side_effect=life.Refused()),patch.object(life,'root_claim') as claim:
            with self.assertRaises(life.Refused):life.main('root-claim')
            claim.assert_not_called()
    def test_native_daemon_descendant_is_adopted_with_pidfd_not_launcher_pid(self):
        launcher=SimpleNamespace(pid=10,returncode=0,poll=lambda:0)
        with patch.object(life,'direct_children',return_value=[20]),patch.object(life,'owned_agent_identity',return_value='start-1') as identity,patch.object(life.os,'pidfd_open',return_value=99),patch.object(life.time,'monotonic',return_value=1):
            owned=life.adopt_agent(launcher,2)
        self.assertEqual((owned.pid,owned.pidfd,owned.start_ticks),(20,99,'start-1'));self.assertEqual(identity.call_count,2)
    def test_multiple_descendants_and_changed_start_ticks_refused(self):
        launcher=SimpleNamespace(returncode=0,poll=lambda:0)
        with patch.object(life,'direct_children',return_value=[20,21]),patch.object(life.time,'monotonic',return_value=1):
            with self.assertRaises(life.Refused):life.adopt_agent(launcher,2)
        with patch.object(life,'direct_children',return_value=[20]),patch.object(life.time,'monotonic',return_value=1),patch.object(life,'owned_agent_identity',side_effect=['before','after']),patch.object(life.os,'pidfd_open',return_value=99),patch.object(life.os,'close') as close:
            with self.assertRaises(life.Refused):life.adopt_agent(launcher,2)
            close.assert_called_once_with(99)
    def test_owned_agent_reaps_only_pidfd_child(self):
        owned=life.OwnedAgent(10,99,'start')
        with patch.object(life.os,'waitid',return_value=SimpleNamespace(si_code=os.CLD_EXITED,si_status=2)) as wait:
            self.assertEqual(owned.poll(),2);self.assertEqual(owned.poll(),2)
            wait.assert_called_once_with(os.P_PIDFD,99,os.WEXITED|os.WNOHANG)
    def test_already_exited_owned_pidfd_signal_is_safe(self):
        with patch.object(life.signal,'pidfd_send_signal',side_effect=ProcessLookupError()):life.signal_owned(99,signal.SIGTERM)
    def test_unit_has_no_runtime_creation_socket_activation_or_automatic_restart(self):
        unit=(Path(life.__file__).parent/'release/providers'/life.UNIT).read_text()
        for text in ('Type=notify','NotifyAccess=main','Restart=no','NoNewPrivileges=yes','CapabilityBoundingSet=','KillMode=mixed'):self.assertIn(text,unit)
        for text in ('user-runtime-dir','ReadOnlyPaths','PrivateTmp','[Install]','.socket','ExecStopPost'):self.assertNotIn(text,unit)
        self.assertIn(' root-claim',unit)

    def test_quarantine_receipt_must_bind_original_inode_and_preserve_acl(self):
        original={'device':'1','inode':'2','uid':949,'gid':948,'mode':'0o600','ctime_ns':'3','acl':{}}
        after=dict(original,ctime_ns='4');mid='a'*32
        receipt={'schema':2,'maintenance_id':mid,'activation_id':mid,'manifest_sha256':'f'*64,'before':original,'after':after,'destination':life.STATE+'/quarantine/'+mid+'/S.gpg-agent','source_absent':True,'directories_restored':True,
                 'sockets':{name:{'before':original,'after':after} for name in life.SOCKET_NAMES}}
        policy={'activation_id':mid,'quarantine_receipt':life.STATE+'/quarantine.'+mid+'.json','old_socket':original}
        with patch.object(life,'verify_activation',return_value={'manifest_sha256':'f'*64}),patch.object(life,'root_input',return_value=json.dumps(receipt).encode()),patch.object(life,'socket_absent'),patch.object(life,'node'),patch.object(life,'socket_snapshot',return_value=after),patch.object(life,'no_listener',return_value=True):
            self.assertEqual(life.check_quarantine_receipt(policy,root=True)['after'],after)
        receipt['after']=dict(after,inode='99')
        with patch.object(life,'verify_activation',return_value={'manifest_sha256':'f'*64}),patch.object(life,'root_input',return_value=json.dumps(receipt).encode()),patch.object(life,'socket_absent'):
            with self.assertRaises(life.Refused):life.check_quarantine_receipt(policy)
    def test_root_host_namespace_mismatch_is_refused(self):
        with patch.object(life.os,'getresuid',return_value=(0,)*3),patch.object(Path,'read_text',return_value='0 0 4294967295'),patch.object(life,'namespace_inodes',return_value={'net':1}),patch.object(life.os,'stat',return_value=SimpleNamespace(st_ino=2)):
            with self.assertRaises(life.Refused):life.root_host_identity()
    def test_existing_lock_excludes_second_actor_without_recreating_file(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'lifecycle.lock';path.write_bytes(b'');os.chmod(path,0o600)
            fake=SimpleNamespace(st_mode=0o100600,st_uid=949,st_gid=948,st_nlink=1)
            with patch.object(life,'actor_identity'),patch.object(life,'node'),patch.object(life,'AUDIT',d),patch.object(life.os,'fstat',return_value=fake),patch.object(life.os,'listxattr',return_value=[]):
                lock=life.actor_lock()
                try:
                    with self.assertRaises(BlockingIOError):life.actor_lock()
                finally:os.close(lock)

class Confinement(unittest.TestCase):
    def test_agent_policy_denies_key_file_mutation_and_socket_unlink(self):
        with tempfile.TemporaryDirectory(prefix='aur-lifecycle-test-') as d:
            key=Path(d)/'key-sentinel';key.write_text('fixture');os.chmod(key,0o600)
            stale=Path(d)/'old.socket';sock=socket.socket(socket.AF_UNIX);sock.bind(str(stale));sock.close();before=stale.stat()
            code=f'''
import sys,os,socket,errno,json
sys.path.insert(0,{str(Path(life.__file__).parent)!r})
import agent_lifecycle_949 as r
r.GNUPG={d!r}
r.agent_confinement()
for call in (lambda:os.open({str(key)!r},os.O_WRONLY),lambda:os.chmod({str(key)!r},0o777),lambda:os.unlink({str(key)!r}),lambda:os.unlink({str(stale)!r})):
 try:call();raise RuntimeError('mutation allowed')
 except OSError as e:assert e.errno in (errno.EPERM,errno.EACCES),e.errno
os.umask(0o077)
s=socket.socket(socket.AF_UNIX);s.bind({str(Path(d)/'new.socket')!r});s.close()
print('PASS native socket create; no file write/unlink/chmod')
'''
            result=subprocess.run([sys.executable,'-I','-S','-B','-c',code],capture_output=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr.decode())
            self.assertEqual(stale.stat(),before);self.assertEqual(key.read_text(),'fixture');self.assertEqual(stat.S_IMODE(key.stat().st_mode),0o600)

class Supervision(unittest.TestCase):
    def exercise(self,stop=False,forced=False,endpoint=True):
        with tempfile.TemporaryDirectory() as d,tempfile.TemporaryFile() as dummy:
            handlers={};pidfd=os.dup(dummy.fileno());process=SimpleNamespace(pid=20,pidfd=pidfd,start_ticks='123',returncode=None)
            def poll():
                if process.returncode is not None:return process.returncode
                if stop:handlers[signal.SIGTERM](None,None);return None
                process.returncode=2;return 2
            def wait(timeout=None):
                if forced and timeout==10:raise subprocess.TimeoutExpired('fake',10)
                process.returncode=-signal.SIGKILL if forced else 0;return process.returncode
            process.poll=poll;process.wait=wait
            launcher=SimpleNamespace(pid=10,returncode=0,poll=lambda:0)
            def handler(sig,fn):previous=handlers.get(sig,signal.SIG_DFL);handlers[sig]=fn;return previous
            with patch.object(life,'actor_identity'),patch.object(life,'common_checks'),patch.object(life,'verify_claim'),patch.object(life,'consume_actor_start'),patch.object(life,'node'),patch.object(life,'socket_absent'),patch.object(life,'no_listener',return_value=True),patch.object(life,'enable_subreaper'),patch.object(life,'adopt_agent',return_value=process),patch.object(life,'child_endpoint_ready',return_value=endpoint),patch.object(life,'notify_ready') as ready,patch.object(life,'AUDIT',d),patch.object(life.os,'pidfd_open',side_effect=lambda p:os.dup(dummy.fileno())),patch.object(life.signal,'signal',side_effect=handler),patch.object(life.signal,'pidfd_send_signal') as send:
                failed=False
                try:life.supervise({'_policy_sha256':'a'*64},42,popen=lambda *a,**k:launcher)
                except life.Refused:failed=True
                signals=[c.args[1] for c in send.call_args_list];notified=ready.called
            rows=[json.loads(row) for file in Path(d).glob('*.jsonl') for row in file.read_text().splitlines()]
            return failed,signals,rows,notified
    def test_crash_is_audited_without_restart(self):
        failed,signals,rows,ready=self.exercise()
        self.assertTrue(failed);self.assertEqual(signals,[]);self.assertTrue(ready)
        self.assertEqual(rows[-1]['event'],'lifecycle-blocked')
        self.assertEqual([r['returncode'] for r in rows if r['event']=='agent-process-exited'],[2])
    def test_clean_stop_uses_owned_pidfd(self):
        failed,signals,rows,ready=self.exercise(stop=True)
        self.assertFalse(failed);self.assertEqual(signals,[signal.SIGTERM]);self.assertTrue(ready)
    def test_forced_stop_stays_failed_and_preserves_evidence(self):
        failed,signals,rows,ready=self.exercise(stop=True,forced=True)
        self.assertTrue(failed);self.assertEqual(signals,[signal.SIGTERM,signal.SIGKILL])
    def test_failed_endpoint_never_notifies_ready(self):
        failed,signals,rows,ready=self.exercise(endpoint=False)
        self.assertTrue(failed);self.assertFalse(ready)

if __name__=='__main__':unittest.main()
