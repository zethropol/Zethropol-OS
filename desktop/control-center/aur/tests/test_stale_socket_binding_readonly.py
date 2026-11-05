import contextlib,errno,io,json,os,stat,sys,tempfile,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import stale_socket_binding_readonly as probe

class Binding(unittest.TestCase):
    def inspect(self,mode=0o600,uid=949,gid=948,acl=None,inventory=None,changed=None,kind=stat.S_IFSOCK,acl_race=False,nlink=1):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'S.gpg-agent';path.write_bytes(b'untouched test fixture')
            original_stat=os.stat;original_fstat=os.fstat;original_open=os.open
            s=path.stat();leaf=[None];stat_calls=[0];acl_calls=[0]
            base=dict(st_dev=s.st_dev,st_ino=s.st_ino,st_mode=kind|mode,st_uid=uid,st_gid=gid,st_nlink=nlink,st_size=s.st_size,st_mtime_ns=s.st_mtime_ns,st_ctime_ns=s.st_ctime_ns)
            def capture(p,*a,**kw):
                if p=='S.gpg-agent':
                    stat_calls[0]+=1;values=dict(base)
                    if changed and stat_calls[0]>1:values.update(changed)
                    return SimpleNamespace(**values)
                return original_stat(p,*a,**kw)
            def opening(p,*a,**kw):
                fd=original_open(p,*a,**kw)
                if p=='S.gpg-agent':leaf[0]=fd
                return fd
            def fstat(fd):return SimpleNamespace(**base) if fd==leaf[0] else original_fstat(fd)
            def xattrs(*a,**kw):
                acl_calls[0]+=1
                return ['system.posix_acl_access'] if acl or (acl_race and acl_calls[0]>1) else []
            inventory=inventory or {'visibility':'COMPLETE_FOR_OBSERVED_PROCESS_NAMESPACES','entries':[],'errors':[]}
            with patch.object(probe,'GNUPG',d),patch.object(probe,'SOCKET',str(path)),patch.object(probe.os,'stat',side_effect=capture),patch.object(probe.os,'fstat',side_effect=fstat),patch.object(probe.os,'open',side_effect=opening),patch.object(probe.os,'listxattr',side_effect=xattrs),patch.object(probe.os,'getxattr',return_value=b'ACL bytes'),patch.object(probe,'listener_observation',return_value=inventory):
                try:r=probe.inspect_socket()
                except probe.BindingBlocked as error:r=error.report
            self.assertEqual(path.read_bytes(),b'untouched test fixture')
            return r
    def test_matching_metadata_is_capture_not_cleanup_authorization(self):
        r=self.inspect();self.assertEqual(r['status'],'FRESH_METADATA_CAPTURED_NOT_MAINTENANCE_AUTHORITY')
        self.assertFalse(r['quarantine_authorized']);self.assertFalse(r['activation_approved']);self.assertFalse(r['host_changed'])
    def test_0700_is_explicitly_accepted_without_authority(self):
        r=self.inspect(mode=0o700)
        self.assertEqual(r['observed']['mode'],'0o700')
        self.assertEqual(r['blockers'],[])
        self.assertFalse(r['quarantine_authorized']);self.assertFalse(r['activation_approved'])
    def test_other_owner_only_and_special_modes_are_rejected(self):
        for mode in (0,0o100,0o200,0o300,0o400,0o500,0o1600,0o2600,0o4600,0o660,0o701):
            self.assertIn('mode',[b['field'] for b in self.inspect(mode=mode)['blockers']])
    def test_multiple_links_are_rejected(self):
        self.assertIn('nlink',[b['field'] for b in self.inspect(nlink=2)['blockers']])
    def test_each_uid_gid_mode_acl_mismatch_is_reported(self):
        for kwargs,field in (({'uid':1000},'uid'),({'gid':999},'gid'),({'mode':0o660},'mode'),({'acl':True},'acl')):
            r=self.inspect(**kwargs);self.assertIn(field,[b['field'] for b in r['blockers']])
    def test_all_known_field_mismatches_survive_together(self):
        r=self.inspect(uid=1,gid=2,mode=0o777,acl=True)
        self.assertTrue({'uid','gid','mode','acl'}<={b['field'] for b in r['blockers']})
    def test_wrong_type_and_symlink_are_not_followed(self):
        for kind in (stat.S_IFREG,stat.S_IFLNK):
            r=self.inspect(kind=kind);self.assertEqual(r['blockers'][0]['field'],'type')
            self.assertEqual(r['listener_state'],'NOT_PROBED')
    def test_inode_device_or_nanosecond_ctime_change_blocks(self):
        for changed in ({'st_ino':9},{'st_dev':9},{'st_ctime_ns':1}):
            r=self.inspect(changed=changed)
            self.assertIn('socket_identity_ctime_stability',[b['field'] for b in r['blockers']])
    def test_acl_change_during_observation_blocks(self):
        self.assertIn('acl_stability',[b['field'] for b in self.inspect(acl_race=True)['blockers']])
    def test_fingerprint_ignores_atime_but_preserves_exact_ctime(self):
        data=dict(st_dev=1,st_ino=2,st_mode=0o140600,st_uid=949,st_gid=948,st_nlink=1,st_size=0,st_mtime_ns=5,st_ctime_ns=1_793_532_307_947_935_714)
        self.assertEqual(probe.fingerprint(SimpleNamespace(**data,st_atime_ns=1)),probe.fingerprint(SimpleNamespace(**data,st_atime_ns=999)))
        self.assertNotEqual(probe.fingerprint(SimpleNamespace(**data)),probe.fingerprint(SimpleNamespace(**dict(data,st_ctime_ns=data['st_ctime_ns']+1))))
    def test_listener_or_nonlistening_bound_endpoint_blocks(self):
        for listening,state in ((True,'LISTENING'),(False,'BOUND_ENDPOINT_PRESENT')):
            r=self.inspect(inventory={'visibility':'COMPLETE_FOR_OBSERVED_PROCESS_NAMESPACES','entries':[{'kernel_inode':'123','listening':listening}]})
            self.assertEqual(r['listener_state'],state);self.assertIn('listener',[b['field'] for b in r['blockers']])
    def test_incomplete_visibility_cannot_be_promoted_to_no_listener(self):
        r=self.inspect(inventory={'visibility':'INCOMPLETE','entries':[],'errors':[{'errno':13}]})
        self.assertEqual(r['listener_state'],'UNKNOWN');self.assertIn('listener_visibility',[b['field'] for b in r['blockers']])
    def test_oserror_retains_field_phase_errno_and_partial_metadata(self):
        with patch.object(probe.os,'open',side_effect=PermissionError(errno.EACCES,'SECRET')):
            r=probe.inspect_socket()
        self.assertEqual(r['blockers'][0]['errno'],errno.EACCES)
        self.assertEqual(r['phase'],'socket-parent-nofollow-open');self.assertNotIn('SECRET',json.dumps(r))
    def test_missing_historical_values_are_never_synthesized(self):
        c=self.inspect()['historical_comparison']
        for field in ('device','inode','ctime_ns'):self.assertEqual(c[field],'OLD_VALUE_NOT_RETAINED')
        self.assertIn('ACTOR_GID',c['gid']);self.assertIn('NOT_CURRENT_PROOF',c['listener'])
    def test_nonroot_main_never_probes_socket(self):
        with patch.object(probe.os,'getresuid',return_value=(1000,)*3),patch.object(probe,'inspect_socket') as inspect,contextlib.redirect_stdout(io.StringIO()) as output:
            probe.main();inspect.assert_not_called()
        r=json.loads(output.getvalue());self.assertEqual(r['blockers'][0]['field'],'real_effective_saved_uid')
    def test_no_gpg_process_or_mutation_calls_exist(self):
        import ast
        tree=ast.parse(Path(probe.__file__).read_text());calls={n.func.attr for n in ast.walk(tree) if isinstance(n,ast.Call) and isinstance(n.func,ast.Attribute)}
        self.assertTrue(calls.isdisjoint({'connect','bind','Popen','run','unlink','remove','rename','chmod','chown','mkdir','write','root_claim'}))

    def test_listener_tables_are_matched_by_path_not_filesystem_inode(self):
        def read(path):
            return 'header\n000: 2 0 00010000 0001 01 777 '+probe.SOCKET+'\n' if str(path).startswith('/proc/123/') else 'header\n'
        with patch.object(Path,'iterdir',return_value=iter([Path('/proc/123')])),patch.object(probe.os,'stat',side_effect=lambda p:SimpleNamespace(st_ino=2 if str(p).startswith('/proc/123/') else 1)),patch.object(Path,'read_text',autospec=True,side_effect=read):
            result=probe.listener_observation()
        self.assertEqual(result['table_count'],2);self.assertTrue(result['entries'][0]['listening'])
        self.assertEqual(result['entries'][0]['kernel_inode'],'777')
    def test_permission_denied_network_table_is_not_negative_evidence(self):
        def read(path):
            if str(path).startswith('/proc/123/'):raise PermissionError(errno.EACCES,'SECRET')
            return 'header\n'
        with patch.object(Path,'iterdir',return_value=iter([Path('/proc/123')])),patch.object(probe.os,'stat',side_effect=lambda p:SimpleNamespace(st_ino=2 if str(p).startswith('/proc/123/') else 1)),patch.object(Path,'read_text',autospec=True,side_effect=read):r=probe.listener_observation()
        self.assertEqual(r['visibility'],'INCOMPLETE');self.assertEqual(r['errors'][0]['errno'],errno.EACCES)
        self.assertNotIn('SECRET',json.dumps(r))
    def test_disappearing_process_keeps_listener_visibility_incomplete(self):
        def st(path):
            if str(path).startswith('/proc/123/'):raise FileNotFoundError(errno.ENOENT,'gone')
            return SimpleNamespace(st_ino=1)
        with patch.object(Path,'iterdir',return_value=iter([Path('/proc/123')])),patch.object(probe.os,'stat',side_effect=st),patch.object(Path,'read_text',return_value='header\n'):r=probe.listener_observation()
        self.assertEqual(r['visibility'],'INCOMPLETE');self.assertEqual(r['errors'][0]['kind'],'process-disappeared')
