import ast,base64,contextlib,hashlib,io,json,os,stat,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import home_lifecycle_file_transaction as tx
import prepare_home_lifecycle_transaction as build

from historical_evidence import requires_historical_evidence

@requires_historical_evidence
class Transaction(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.root=Path(cls.temp.name);cls.bundle=build.prepare(cls.root)
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def envelope(self):return {'schema':1,'transaction_id':'a'*32,'issued_boottime_ns':100,'capture':{'pins':'fixed'}}
    def manager(self,**fields):
        base={'LoadState':'not-found','ActiveState':'inactive','SubState':'dead','UnitFileState':'','FragmentPath':'','DropInPaths':'','Job':''};base.update(fields)
        return '\n'.join(k+'='+v for k,v in base.items())
    def test_manifest_digest_is_exact_disk_bytes_and_payload_is_inactive(self):
        m,payload=tx.bundle_data(self.bundle)
        self.assertEqual(self.bundle['manifest_sha256'],hashlib.sha256((self.root/'deployment-manifest.INACTIVE.json').read_bytes()).hexdigest())
        self.assertFalse(json.loads(payload[tx.RELEASE+'/policy.json'])['activation_approved'])
        self.assertEqual(len(m['files']),5)
    def test_payload_or_manifest_tampering_fails(self):
        for change in ('payload','manifest','activation'):
            b=json.loads(json.dumps(self.bundle))
            if change=='payload':b['payload'][tx.UNIT]=base64.b64encode(b'foreign').decode()
            elif change=='manifest':b['manifest']['host_applied']=True
            else:
                path=tx.RELEASE+'/policy.json';p=json.loads(base64.b64decode(b['payload'][path]));p['activation_approved']=True;raw=tx.canonical(p)
                b['payload'][path]=base64.b64encode(raw).decode()
                for f in b['manifest']['files']:
                    if f['target']==path:f['sha256']=tx.sha(raw)
                b['manifest_sha256']=tx.sha(tx.canonical(b['manifest']))
            with self.assertRaises(tx.Blocked):tx.bundle_data(b)
    def test_only_five_exact_targets_can_be_written(self):
        b=json.loads(json.dumps(self.bundle));b['payload']['/tmp/foreign']=''
        with self.assertRaises(tx.Blocked):tx.bundle_data(b)
    def test_capture_binding_rejects_future_observation_drift_and_bad_ids(self):
        e=self.envelope();tx.validate_capture(e,{'pins':'fixed'},100)
        for envelope,current,now in ((e,{'pins':'drift'},100),(e,{'pins':'fixed'},99),
                                     (dict(e,transaction_id='../bad'),{'pins':'fixed'},100)):
            with self.assertRaises(tx.Blocked):tx.validate_capture(envelope,current,now)
    def test_manager_not_found_inactive_no_jobs_passes(self):
        with patch.object(tx,'systemd_output',side_effect=[self.manager(),'']):self.assertEqual(tx.manager_state()['ActiveState'],'inactive')
    def test_manager_active_loaded_dropin_enabled_or_queued_blocks(self):
        for fields in ({'ActiveState':'active'},{'LoadState':'loaded'},{'DropInPaths':'/bad'},{'UnitFileState':'enabled'},{'Job':'42'}):
            with patch.object(tx,'systemd_output',return_value=self.manager(**fields)):
                with self.assertRaises(tx.Blocked):tx.manager_state()
        with patch.object(tx,'systemd_output',side_effect=[self.manager(),'42 '+tx.UNIT_NAME+' start waiting']):
            with self.assertRaises(tx.Blocked):tx.manager_state()
    def test_postpublication_loaded_inactive_static_unit_does_not_imply_start(self):
        raw=self.manager(LoadState='loaded',FragmentPath=tx.UNIT,UnitFileState='static')
        with patch.object(tx,'systemd_output',side_effect=[raw,'']):tx.manager_state(allow_published=True)
        with patch.object(tx,'systemd_output',side_effect=[self.manager(LoadState='loaded',FragmentPath='/foreign'),'']):
            with self.assertRaises(tx.Blocked):tx.manager_state(allow_published=True)
    def test_systemctl_invocations_are_readonly_and_bounded(self):
        from types import SimpleNamespace
        with patch.object(tx.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout=b'ok')) as run:
            tx.systemd_output(['show',tx.UNIT_NAME])
        args,kw=run.call_args;self.assertEqual(args[0][:2],['/usr/bin/systemctl','show']);self.assertEqual(kw['timeout'],15)
        with patch.object(tx.subprocess,'run',return_value=SimpleNamespace(returncode=1,stdout=b'')):
            with self.assertRaises(tx.Blocked):tx.systemd_output(['show'])
    def test_dropin_enable_and_alias_collisions_block(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            with patch.object(tx,'SYSTEMD_ROOTS',(d,)),patch.object(tx,'walk',side_effect=lambda p:os.open(p,tx.D)),patch.object(tx,'manager_state',return_value={}):
                os.symlink(tx.UNIT_NAME,root/'alias.service')
                with self.assertRaises(tx.Blocked):tx.systemd_collisions()
                (root/'alias.service').unlink();(root/'service.d').mkdir()
                with self.assertRaises(tx.Blocked):tx.systemd_collisions()
                (root/'service.d').rmdir();(root/'multi-user.target.wants').mkdir();os.symlink('../'+tx.UNIT_NAME,root/'multi-user.target.wants'/tx.UNIT_NAME)
                with self.assertRaises(tx.Blocked):tx.systemd_collisions()
    def test_unreadable_systemd_inventory_is_not_absence(self):
        with patch.object(tx,'SYSTEMD_ROOTS',('/unreadable',)),patch.object(tx.os,'lstat',side_effect=PermissionError(13,'denied')),patch.object(tx,'manager_state') as manager:
            with self.assertRaises(PermissionError):tx.systemd_collisions()
            manager.assert_not_called()

    def test_preflight_has_no_writes_and_generates_separate_bound_apply(self):
        with patch.object(tx,'capture',return_value={'preservation':{'pins':'fixed'}}),patch.object(tx,'BUNDLE_PATH',str(self.root/'file-transaction.pinned.py'),create=True),patch.object(tx,'BUNDLE_SHA','a'*64,create=True),patch.object(tx,'apply') as apply,contextlib.redirect_stdout(io.StringIO()) as out:
            tx.main(self.bundle,'preflight');apply.assert_not_called()
        r=json.loads(out.getvalue().splitlines()[0]);self.assertEqual(r['status'],'PASS_FILE_DEPLOYMENT_PREFLIGHT');self.assertFalse(r['host_changed'])
        self.assertTrue(r['separate_single_use_apply_command_follows']);self.assertIn('aaaaaaaaaaaaaaaa',out.getvalue());self.assertIn('apply',out.getvalue())
    def test_failed_preflight_never_prints_apply_command(self):
        with patch.object(tx,'capture',side_effect=tx.Blocked('protected drift')),contextlib.redirect_stdout(io.StringIO()) as out:
            with self.assertRaises(SystemExit):tx.main(self.bundle,'preflight')
        self.assertNotIn('next_files_only_apply_fish_command',json.loads(out.getvalue()))
    def test_apply_revalidates_before_first_write(self):
        with patch.object(tx,'capture',return_value={'pins':'changed'}),patch.object(tx,'ensure_directory') as write:
            with self.assertRaises(tx.Blocked):tx.apply(self.bundle,self.envelope())
            write.assert_not_called()
    def test_capture_requires_two_equal_preservation_samples(self):
        with patch.object(tx,'host_context',return_value={}),patch.object(tx,'preservation',side_effect=[{'x':1},{'x':2}]),patch.object(tx,'collisions',return_value={}),patch.object(tx,'systemd_collisions',return_value={}):
            with self.assertRaises(tx.Blocked):tx.capture(self.bundle)
    def test_write_new_is_exclusive_and_checks_real_bytes(self):
        with tempfile.TemporaryDirectory() as d:
            parent=os.open(d,tx.D)
            try:
                tx.write_new(parent,'claim',b'bound',os.getuid(),os.getgid(),0o600)
                with self.assertRaises(FileExistsError):tx.write_new(parent,'claim',b'replay',os.getuid(),os.getgid(),0o600)
                self.assertEqual((Path(d)/'claim').read_bytes(),b'bound')
            finally:os.close(parent)
    def test_public_reader_rejects_symlink_and_preserves_regular_metadata(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);f=p/'public';f.write_bytes(b'public fixture');os.chmod(f,0o644);os.symlink('public',p/'alias')
            before=os.stat(f)
            with patch.object(tx,'walk',side_effect=lambda path:os.open(path,tx.D)):
                item,raw=tx.public_file(str(f),os.getuid(),os.getgid(),0o644)
                self.assertEqual(raw,b'public fixture');self.assertEqual(item['sha256'],tx.sha(raw))
                with self.assertRaises(OSError):tx.public_file(str(p/'alias'),os.getuid(),os.getgid(),0o644)
            self.assertEqual(os.stat(f),before)

    def test_noreplace_never_overwrites_existing_destination(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'source').write_text('new');(p/'destination').write_text('keep');fd=os.open(d,tx.D)
            try:
                with self.assertRaises(FileExistsError):tx.noreplace(fd,'source',fd,'destination')
                self.assertEqual((p/'destination').read_text(),'keep');self.assertTrue((p/'source').exists())
                tx.noreplace(fd,'source',fd,'fresh');self.assertEqual((p/'fresh').read_text(),'new')
            finally:os.close(fd)
    def test_actor_staging_is_under_root_state_not_uid_owned_home(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'home').mkdir();(p/'state').mkdir()
            opened=[]
            def walk(path):return os.open(p/('state' if path.startswith(tx.STATE) else 'home'),tx.D)
            def mkdir(parent,name,*a,**kw):
                opened.append(os.readlink('/proc/self/fd/'+str(parent)));os.mkdir(name,0o700,dir_fd=parent);return os.open(name,tx.D,dir_fd=parent)
            with patch.object(tx,'walk',side_effect=walk),patch.object(tx,'mkdir_new',side_effect=mkdir),patch.object(tx.os,'fchown'),patch.object(tx,'reachable'):
                tx.publish_directory(tx.ACTOR,[],'a'*32,949,948,0o700)
            self.assertEqual(opened,[str(p/'state')]);self.assertTrue((p/'home'/Path(tx.ACTOR).name).is_dir())
    def fixture_apply(self,fail_postflight=False):
        # Real disposable filesystem publication/rollback; host identity and
        # ownership conversion are isolated mocks, never host installation.
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            for path in ('var/lib','usr/lib/zethropol','etc/systemd/system',tx.HOME.lstrip('/')):
                (root/path).mkdir(parents=True,exist_ok=True)
            local=lambda path:root/path.lstrip('/')
            original_write=tx.write_new;original_mkdir=tx.mkdir_new
            writes=[]
            def writing(parent,name,raw,uid=0,gid=0,mode=0o600):
                writes.append(name);return original_write(parent,name,raw,os.getuid(),os.getgid(),mode)
            def making(parent,name,uid=0,gid=0,mode=0o700):return original_mkdir(parent,name,os.getuid(),os.getgid(),mode)
            envelope=self.envelope();envelope['capture']={'preservation':{'baseline':'untouched'}}
            def verify(manifest,baseline):
                for f in manifest['files']:self.assertEqual(tx.sha(local(f['target']).read_bytes()),f['sha256'])
                if fail_postflight:raise tx.Blocked('injected postflight failure')
            with patch.object(tx,'capture',return_value=envelope['capture']),patch.object(tx.time,'clock_gettime_ns',return_value=100),patch.object(tx,'walk',side_effect=lambda path:os.open(local(path),tx.D)),patch.object(tx,'reachable'),patch.object(tx,'mkdir_new',side_effect=making),patch.object(tx,'write_new',side_effect=writing),patch.object(tx.os,'fchown'),patch.object(tx,'preservation',return_value={'baseline':'untouched'}),patch.object(tx,'systemd_collisions',return_value={}),patch.object(tx,'manager_state',return_value={}),patch.object(tx,'public_file',return_value=({'sha256':tx.sha(base64.b64decode(self.bundle['payload'][tx.UNIT]))},b'')),patch.object(tx,'postflight',side_effect=verify),patch.object(tx,'BUNDLE_SHA','a'*64,create=True):
                if fail_postflight:
                    with self.assertRaises(tx.Blocked):tx.apply(self.bundle,envelope)
                    self.assertFalse(local(tx.UNIT).exists())
                    self.assertTrue(local(tx.ROLLBACK+'/'+envelope['transaction_id']+'.unit').exists())
                    rollback=json.loads(local(tx.STATE+'/deployment/'+envelope['transaction_id']+'/rollback.json').read_bytes())
                    self.assertTrue(rollback['unit_withdrawn'])
                else:
                    result=tx.apply(self.bundle,envelope);self.assertEqual(result['status'],'PASS_INERT_FILE_DEPLOYMENT')
                    self.assertTrue(local(tx.UNIT).exists());self.assertLess(writes.index('lifecycle.lock'),writes.index('.'+tx.UNIT_NAME+'.'+envelope['transaction_id']+'.pending'))
                with self.assertRaises(tx.Blocked):tx.apply(self.bundle,envelope)
            self.assertTrue(local(tx.RELEASE).exists());self.assertTrue(local(tx.ACTOR+'/lifecycle.lock').exists())
    def test_disposable_end_to_end_unit_last_and_replay_blocked(self):self.fixture_apply()
    def test_disposable_postflight_failure_withdraws_only_owned_unit(self):self.fixture_apply(fail_postflight=True)

    def test_socket_inventory_pins_all_four_and_rejects_metadata_drift(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as d:
            for name in tx.SOCKET_INODES:(Path(d)/name).write_bytes(b'')
            original_stat=tx.os.stat;original_fstat=tx.os.fstat;original_open=tx.os.open
            leaf={};drift=[False]
            def data(name):
                return SimpleNamespace(st_dev=38,st_ino=tx.SOCKET_INODES[name],st_mode=stat.S_IFSOCK|0o700,
                        st_uid=949,st_gid=948,st_nlink=2 if drift[0] else 1,st_size=0,st_mtime_ns=1,st_ctime_ns=1793481059412486406)
            def opened(path,*a,**kw):
                fd=original_open(path,*a,**kw)
                if path in tx.SOCKET_INODES:leaf[fd]=path
                return fd
            def stated(path,*a,**kw):return data(path) if path in tx.SOCKET_INODES else original_stat(path,*a,**kw)
            def fstated(fd):return data(leaf[fd]) if fd in leaf else original_fstat(fd)
            # Avoid FD-number reuse from the fixture root pin being classified as a leaf.
            parent=original_open(d,tx.D)
            with patch.object(tx,'walk',return_value=parent),patch.object(tx,'reachable'),patch.object(tx.os,'open',side_effect=opened),patch.object(tx.os,'stat',side_effect=stated),patch.object(tx.os,'fstat',side_effect=fstated),patch.object(tx.os,'listxattr',return_value=[]):
                result=tx.socket_inventory();self.assertEqual(set(result),set(tx.SOCKET_INODES))
            parent=original_open(d,tx.D);leaf.clear();drift[0]=True
            with patch.object(tx,'walk',return_value=parent),patch.object(tx,'reachable'),patch.object(tx.os,'open',side_effect=opened),patch.object(tx.os,'stat',side_effect=stated),patch.object(tx.os,'fstat',side_effect=fstated),patch.object(tx.os,'listxattr',return_value=[]):
                with self.assertRaises(tx.Blocked):tx.socket_inventory()

    def test_socket_expected_inodes_cover_all_four_without_fd_absence_gate(self):
        self.assertEqual(set(tx.SOCKET_INODES.values()),{1936362,1936363,1936364,1936365})
        source=Path(tx.__file__).read_text();tree=ast.parse(source)
        attrs={n.func.attr for n in ast.walk(tree) if isinstance(n,ast.Call) and isinstance(n.func,ast.Attribute)}
        self.assertTrue(attrs.isdisjoint({'unlink','remove','connect','kill','pidfd_getfd','setns'}))
        self.assertNotIn('candidate_fd_holders',source)
    def test_pinned_bundle_and_bootstrap_are_reproducible(self):
        before=(self.root/'file-transaction.pinned.py').read_bytes();build.prepare(self.root)
        self.assertEqual(before,(self.root/'file-transaction.pinned.py').read_bytes())
        ast.parse(before);ast.parse(tx.bootstrap(str(self.root/'file-transaction.pinned.py'),tx.sha(before),'preflight'))
        self.assertIn(tx.sha(before),(self.root/'preflight-files.fish').read_text())
    def test_nonisolated_execution_stops_before_host_probe(self):
        with patch.object(tx.os,'getresuid') as uid:
            with self.assertRaises(tx.Blocked):tx.host_context()
            uid.assert_not_called()

if __name__=='__main__':unittest.main()
