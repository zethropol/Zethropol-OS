import ast,contextlib,io,json,os,stat,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import home_socket_safety_readonly as reader
import agent_home_lifecycle_review as lifecycle

class Safety(unittest.TestCase):
    def test_exact_types_include_socket_and_every_collision_kind(self):
        for mode,expected in ((stat.S_IFSOCK,'socket'),(stat.S_IFREG,'regular'),(stat.S_IFDIR,'directory'),
                              (stat.S_IFLNK,'symlink'),(stat.S_IFIFO,'fifo'),(stat.S_IFCHR,'character'),(stat.S_IFBLK,'block')):
            self.assertEqual(reader.kind(mode|0o700),expected)
    def test_auxiliary_report_retains_exact_path_and_socket_type(self):
        from types import SimpleNamespace
        s=SimpleNamespace(st_dev=38,st_ino=1936365,st_uid=949,st_gid=948,st_mode=stat.S_IFSOCK|0o700,st_ctime_ns=1)
        with patch.object(lifecycle.os,'lstat',return_value=s):r=lifecycle.target_state(lifecycle.life.AUX_SOCKET)
        self.assertEqual(r['path'],lifecycle.life.GNUPG+'/S.gpg-agent.ssh')
        self.assertEqual(r['type'],'socket');self.assertEqual(r['state'],'EXISTS_REQUIRES_COLLISION_REVIEW')
    def test_table_uses_kernel_inode_and_keeps_nonlistener(self):
        raw='Num RefCount Protocol Flags Type St Inode Path\n0: 2 0 00000000 0001 01 777 /fixed\n'
        r=reader.parse_table(raw,'2',{'/fixed'})
        self.assertEqual(r[0]['kernel_inode'],'777');self.assertFalse(r[0]['listening'])
        self.assertEqual(reader.parse_table(raw,'2',{'/other'}),[])
    def test_bad_tables_cannot_be_absence_evidence(self):
        for raw in ('','header\n','Num\nmalformed','Num\n0: 2 0 invalid 0001 01 2 /fixed'):
            with self.assertRaises(ValueError):reader.parse_table(raw,'1',{'/fixed'})
    def scan_fixture(self,denied=False,bad=False,churn=False):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);task=root/'123/task/124'
            for name in ('ns','net','fd'):(task/name).mkdir(parents=True)
            (task/'stat').write_text('124 (name with ) spaces) '+' '.join(['0']*19+['321']))
            for name in ('net','mnt'):(task/'ns'/name).write_text('')
            (task/'net/unix').write_text('bad' if bad else 'Num RefCount Protocol Flags Type St Inode Path\n0: 2 0 00010000 0001 01 777 /fixed\n')
            os.symlink('socket:[777]',task/'fd/4');os.symlink('net:[999]',task/'fd/5')
            real_readlink=os.readlink
            def link(p):
                if denied:raise PermissionError(13,'private detail')
                return real_readlink(p)
            def path(p):return root if p=='/proc' else Path(p)
            with patch.object(reader,'Path',side_effect=path),patch.object(reader.os,'readlink',side_effect=link):
                if churn:
                    with patch.object(reader,'starttime',side_effect=['321','322']):return reader.proc_scan({'/fixed'})
                return reader.proc_scan({'/fixed'})
    def test_cross_thread_fd_candidate_is_not_global_absence_proof(self):
        r=self.scan_fixture();self.assertEqual(r['candidate_fd_holders'][0]['tid'],'124')
        self.assertTrue(r['entries'][0]['listening']);self.assertFalse(r['global_holder_absence_proven'])
        self.assertEqual(r['unrepresented_net_namespace_fds'][0]['net_namespace'],'999')
        self.assertEqual(r['visibility'],'COMPLETE_FOR_OBSERVED_TASKS_ONLY')
    def test_fd_denial_is_incomplete_and_redacted(self):
        r=self.scan_fixture(denied=True);self.assertEqual(r['visibility'],'INCOMPLETE')
        self.assertNotIn('private detail',json.dumps(r));self.assertFalse(r['global_holder_absence_proven'])
    def test_bad_table_or_pid_churn_is_incomplete(self):
        for args in ({'bad':True},{'churn':True}):self.assertEqual(self.scan_fixture(**args)['visibility'],'INCOMPLETE')
    def test_nofollow_snapshot_preserves_symlink_and_regular_fixture(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'file').write_text('untouched');os.symlink('file',p/'S.gpg-agent.ssh')
            fd=os.open(d,os.O_RDONLY|os.O_DIRECTORY)
            try:
                with patch.object(reader,'GNUPG',d):
                    r=reader.snapshot(fd,'S.gpg-agent.ssh');self.assertEqual(r['type'],'symlink')
                    self.assertEqual(reader.snapshot(fd,'missing')['state'],'ABSENT')
                    self.assertEqual(reader.snapshot(fd,'file')['type'],'regular')
            finally:os.close(fd)
            self.assertEqual((p/'file').read_text(),'untouched')
    def test_nonisolated_main_fails_before_host_reads(self):
        with patch.object(reader.binding,'root_context') as probe,contextlib.redirect_stdout(io.StringIO()) as out:
            reader.main();probe.assert_not_called()
        r=json.loads(out.getvalue());self.assertFalse(r['quarantine_authorized']);self.assertFalse(r['host_changed'])
    def test_reader_never_mutates_connects_or_borrows_fds(self):
        tree=ast.parse(Path(reader.__file__).read_text())
        calls={n.func.attr for n in ast.walk(tree) if isinstance(n,ast.Call) and isinstance(n.func,ast.Attribute)}
        self.assertTrue(calls.isdisjoint({'connect','bind','setns','pidfd_getfd','Popen','run','unlink','rename','chmod','chown','mkdir','write_text','write_bytes'}))

if __name__=='__main__':unittest.main()
