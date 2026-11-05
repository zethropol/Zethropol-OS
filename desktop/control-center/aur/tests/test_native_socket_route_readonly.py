import contextlib,ctypes,errno,io,json,os,subprocess,sys,tempfile,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch,mock_open
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import native_socket_route_readonly as route

class Route(unittest.TestCase):
    def ready(self):
        return dict(status='QUERY_CONTEXT_READY',identity='PASS_REAL_EFFECTIVE_SAVED_FS_UID949_GID948',capabilities='ZERO',supplementary_groups=[948],no_new_privs=True,landlock_abi=3,seccomp_notification=True)
    def answer(self,path=None,record=None,rc=0,stderr=b''):
        return SimpleNamespace(returncode=rc,stdout=((path or route.HOME+'/S.gpg-agent')+'\n').encode(),stderr=(route.MARKER+json.dumps(record or self.ready())+'\n').encode()+stderr)
    def test_accepted_home_route_matches_new_service_without_active_route_claim(self):
        r=route.evaluate(self.answer())
        self.assertEqual(r['signer_service_path_equality'],'MATCH')
        self.assertEqual(r['service_design'],'NATIVE_HOME_SOCKET_CANDIDATE')
        self.assertEqual(r['route_binding'],'NOT_VERIFIED')
        self.assertFalse(r['agent_started']);self.assertFalse(r['signing_attempted'])
    def test_home_match_never_proves_active_service(self):
        r=route.evaluate(self.answer(route.CANDIDATE))
        self.assertEqual(r['signer_service_path_equality'],'MATCH')
        self.assertEqual(r['service_context_equality'],'NOT_PROVEN')
    def test_bad_context_never_promoted(self):
        for field,value in (('identity','MAPPED_UID'),('capabilities','NONZERO'),('supplementary_groups',[948,0]),('no_new_privs',False),('landlock_abi',2)):
            record=dict(self.ready(),**{field:value})
            self.assertEqual(route.evaluate(self.answer(record=record))['native_route_probe'],'BLOCKED_CHILD_CONTEXT_INVALID')
    def test_errno_phase_preserved_without_raw_stderr(self):
        r=route.evaluate(self.answer(record={'status':'BLOCKED','phase':'landlock-restrict-self','errno':errno.EPERM,'errno_name':'EPERM'},rc=1,stderr=b'SECRET\n'))
        self.assertEqual((r['phase'],r['errno']),('landlock-restrict-self',errno.EPERM))
        self.assertNotIn('SECRET',json.dumps(r))
    def test_nonzero_and_unknown_path_do_not_invent_errno(self):
        r=route.evaluate(self.answer(rc=2,stderr=b'SECRET'))
        self.assertEqual(r['native_route_probe'],'BLOCKED_GPGCONF');self.assertIsNone(r['errno'])
        r=route.evaluate(self.answer('/SECRET'))
        self.assertEqual(r['native_route_probe'],'BLOCKED_UNEXPECTED_NATIVE_ROUTE');self.assertNotIn('SECRET',json.dumps(r))
    def test_denied_mkdir_cannot_be_hidden_by_successful_fallback(self):
        answer=self.answer();answer.denials=[{'syscall':'mkdir','number':83,'errno':errno.EPERM}]
        r=route.evaluate(answer)
        self.assertEqual(r['native_route_probe'],'BLOCKED_QUERY_SIDE_EFFECT_OR_POLICY_SYSCALL')
        self.assertEqual(r['errno'],errno.EPERM)
        self.assertNotIn('socket_path',r)
        self.assertNotIn('signer_service_path_equality',r)
    def test_missing_attestation_cannot_pass(self):
        r=route.evaluate(SimpleNamespace(stdout=b'/foo\n',stderr=b'',returncode=-31))
        self.assertEqual(r['native_route_probe'],'BLOCKED_CHILD_CONTEXT_UNATTESTED')
        self.assertEqual(r['child_returncode'],-31)
    def test_mapping_rejects_uid_only_namespace_illusion(self):
        self.assertFalse(route.mapped('949 1000 1\n',949))
        self.assertTrue(route.mapped('0 0 4294967295\n',949))
    def test_signer_drop_order_and_no_new_privileges(self):
        calls=[]
        libc=SimpleNamespace(capset=lambda *a:calls.append('capset') or 0,prctl=lambda *a:calls.append('nnp') or 0)
        with patch.object(route.os,'setgroups',side_effect=lambda x:calls.append(('groups',x))),patch.object(route.os,'setgid',side_effect=lambda x:calls.append(('gid',x))),patch.object(route.os,'setuid',side_effect=lambda x:calls.append(('uid',x))):
            route.drop_identity(libc)
        self.assertEqual(calls,[('groups',[948]),('gid',948),('uid',949),'capset','nnp'])
    def test_drop_failure_never_executes_query(self):
        with patch.object(route,'TRANSPORT_FD',-1,create=True),patch.object(route.socket,'socket',return_value=SimpleNamespace(set_inheritable=lambda x:None)),patch.object(route.ctypes,'CDLL',return_value=object()),patch.object(route,'drop_identity',side_effect=PermissionError(errno.EPERM,'SECRET')),patch.object(route.os,'execve') as execute,contextlib.redirect_stderr(io.StringIO()) as output:
            self.assertEqual(route.child(),1);execute.assert_not_called()
        self.assertNotIn('SECRET',output.getvalue());self.assertIn('"errno": 1',output.getvalue())
    def test_process_context_checks_saved_fs_ids_caps_maps(self):
        status='Uid:\t949 949 949 949\nGid:\t948 948 948 948\nCapEff:\t0\nCapPrm:\t0\nCapInh:\t0\nCapAmb:\t0\nNoNewPrivs:\t1\nSeccomp:\t0\n'
        for wrong in ('', 'Uid:', 'CapEff:', 'map', 'inherited-seccomp'):
            def read(path):
                body=status.replace('Uid:\t949 949 949 949','Uid:\t949 949 0 949') if wrong=='Uid:' else status.replace('CapEff:\t0','CapEff:\t1') if wrong=='CapEff:' else status.replace('Seccomp:\t0','Seccomp:\t2') if wrong=='inherited-seccomp' else status
                return io.StringIO(body if path.endswith('status') else '949 1000 1\n' if wrong=='map' else '0 0 4294967295\n')
            with patch.object(route.os,'getresuid',return_value=(949,)*3),patch.object(route.os,'getresgid',return_value=(948,)*3),patch.object(route.os,'getgroups',return_value=[948]),patch.object(route.os,'getcwd',return_value='/'),patch('builtins.open',side_effect=read):
                if wrong:
                    with self.assertRaises(OSError):route.process_context()
                else:self.assertIn('identity',route.process_context())
    def test_no_namespace_or_agent_syscalls_in_allowlist(self):
        denied=('clone','clone3','fork','vfork','unshare','setns','mount','socket','connect','io_uring_setup','chmod','chown','unlink','mkdir','truncate','ptrace','kill')
        self.assertTrue(set(denied).isdisjoint(route.ALLOW_SYSCALLS))
        self.assertEqual(route.ARGV,['/usr/bin/gpgconf','--homedir',route.HOME,'--list-dirs','agent-socket'])
    def test_nonroot_preflight_and_exact_os_failure(self):
        with patch.object(route.os,'getresuid',return_value=(1000,)*3):
            r=route.probe(lambda *a,**kw:self.fail())
        self.assertEqual((r['phase'],r['errno']),('root-entry',errno.EPERM))
    def test_spawn_never_uses_namespace_tools_and_launch_errno_is_exact(self):
        def execute(args,**kwargs):
            self.assertEqual(args[:5],['/usr/bin/python3','-I','-S','-B','-c'])
            self.assertEqual(kwargs['env'],route.ENV);self.assertTrue(kwargs['close_fds'])
            raise PermissionError(errno.EACCES,'SECRET')
        with patch.object(route.os,'getresuid',return_value=(0,)*3),patch('builtins.open',mock_open(read_data='0 0 4294967295\n')),patch('agent_pinentry_route_review.trusted'),patch.object(route,'CHILD_SOURCE','fixed-child'):
            r=route.probe(execute)
        self.assertEqual((r['phase'],r['errno']),('child-launch',errno.EACCES))
        self.assertNotIn('SECRET',json.dumps(r))

    def test_supervisor_rejects_wrong_saved_or_fs_child_identity(self):
        with patch.object(route.os,'geteuid',return_value=0),patch('builtins.open',mock_open(read_data='Uid: 949 949 0 949\nGid: 948 948 948 948\n')),patch.object(route,'drop_identity') as drop:
            with self.assertRaises(OSError):route.drop_supervisor_if_root(123)
            drop.assert_not_called()
    def test_supervisor_also_gives_up_root_after_child_attestation(self):
        with patch.object(route.os,'geteuid',return_value=0),patch('builtins.open',mock_open(read_data='Uid: 949 949 949 949\nGid: 948 948 948 948\n')),patch.object(route,'drop_identity') as drop,patch.object(route.os,'getresuid',return_value=(949,)*3),patch.object(route.os,'getresgid',return_value=(948,)*3),patch.object(route.os,'getgroups',return_value=[948]):
            route.drop_supervisor_if_root(123)
            drop.assert_called_once()

    def test_fixed_home_checks_root_ancestors_and_private_metadata(self):
        for bad in ('','root-ancestor','private-owner','acl'):
            def metadata(path):
                private=path in ('/var/lib/zethropol-aur-release',route.HOME)
                return SimpleNamespace(st_uid=0 if bad=='private-owner' else 949 if private else 0,st_gid=948 if private else 0,st_mode=0o040000|(0o700 if private else 0o777 if bad=='root-ancestor' else 0o755))
            with patch.object(route.os,'lstat',side_effect=metadata),patch.object(route.os,'access',return_value=True),patch.object(route.os,'listxattr',return_value=['system.posix_acl_access'] if bad=='acl' else []):
                if bad:
                    with self.assertRaises(OSError):route.home_context()
                else:route.home_context()
    def test_other_supported_runtime_root_is_reported_without_guessing_service_binding(self):
        path='/run/gnupg/user/949/d.native/S.gpg-agent'
        r=route.evaluate(self.answer(path))
        self.assertEqual(r['socket_path'],path)
        self.assertEqual(r['service_design'],'BLOCKED_SERVICE_PATH_MUST_FOLLOW_NATIVE_ROUTE')
        self.assertEqual(r['signer_service_path_equality'],'MISMATCH')
        for bad in ('/run/user/949/gnupg/../S.gpg-agent','/run/user/949/gnupg//S.gpg-agent','/run/user/1000/gnupg/S.gpg-agent'):
            self.assertFalse(route.valid_socket_path(bad))

class ConfinementLive(unittest.TestCase):
    def test_namespace_free_kernel_denies_mutation_network_and_other_exec(self):
        # Child applies confinement to itself as the current development user.
        # No host UID949 claim; all fixtures belong to this temporary test dir.
        with tempfile.TemporaryDirectory(prefix='aur-route-test-') as d:
            fixture=Path(d)/'private-key-sentinel';fixture.write_text('test fixture')
            code=f'''
import sys,os,ctypes,json,socket,errno
sys.path.insert(0,{str(Path(route.__file__).parent)!r})
import native_socket_route_readonly as r
libc=ctypes.CDLL(None,use_errno=True);lib=ctypes.CDLL(r.SECCOMP,use_errno=True)
r.HOME={d!r}
r.kernel_ok(libc.prctl(38,1,0,0,0))
r.landlock(libc);r.seccomp(lib)
checks={{
'file-write':lambda:os.open({str(fixture)!r},os.O_WRONLY),
'private-read':lambda:os.open({str(fixture)!r},os.O_RDONLY),
'mkdir':lambda:os.mkdir({str(Path(d)/'new')!r}),
'unlink':lambda:os.unlink({str(fixture)!r}),
'chmod':lambda:os.chmod({str(fixture)!r},0o600),
'socket':lambda:socket.socket(),
'fork':lambda:os.fork(),
'other-executable':lambda:os.execve('/usr/bin/true',['true'],{{}}),
}}
result={{}}
for name,call in checks.items():
 try:call();raise RuntimeError(name+' unexpectedly allowed')
 except OSError as e:
  assert e.errno in (errno.EPERM,errno.EACCES),(name,e.errno)
  result[name]=e.errno
print(json.dumps(result))
'''
            p=subprocess.run([sys.executable,'-I','-S','-B','-c',code],capture_output=True,timeout=10)
            self.assertEqual(p.returncode,0,p.stderr.decode())
            self.assertEqual(len(json.loads(p.stdout)),8)
            self.assertEqual(fixture.read_text(),'test fixture')
    def test_fixed_gpgconf_query_can_exec_under_namespace_free_confinement(self):
        with tempfile.TemporaryDirectory(prefix='aur-route-home-') as d:
            code=f'''
import sys,os,ctypes
sys.path.insert(0,{str(Path(route.__file__).parent)!r})
import native_socket_route_readonly as r
r.HOME={d!r}
libc=ctypes.CDLL(None,use_errno=True);lib=ctypes.CDLL(r.SECCOMP,use_errno=True)
r.kernel_ok(libc.prctl(38,1,0,0,0));r.landlock(libc);r.seccomp(lib)
os.execve(r.GPGCONF,[r.GPGCONF,'--homedir',r.HOME,'--list-dirs','agent-socket'],r.ENV)
'''
            p=subprocess.run([sys.executable,'-I','-S','-B','-c',code],capture_output=True,timeout=10)
            self.assertEqual(p.returncode,0,p.stderr.decode())
            self.assertTrue(p.stdout.startswith(b'/'))
            self.assertEqual(list(Path(d).iterdir()),[])

    def test_supervisor_records_exact_denials_and_never_continues_them(self):
        with tempfile.TemporaryDirectory(prefix='aur-route-notify-') as d:
            code=f'''
import sys,os,ctypes,socket,array,json,errno
sys.path.insert(0,{str(Path(route.__file__).parent)!r})
import native_socket_route_readonly as r
r.HOME={d!r}
transport=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM,0,fileno=TRANSPORT_FD)
libc=ctypes.CDLL(None,use_errno=True);lib=ctypes.CDLL(r.SECCOMP,use_errno=True)
r.kernel_ok(libc.prctl(38,1,0,0,0));r.landlock(libc)
listener=r.seccomp(lib,transport.fileno())
transport.sendmsg([b'R'],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',[listener]))])
os.close(listener);transport.close()
for call in (lambda:os.mkdir({str(Path(d)/'new')!r}),lambda:socket.socket(),lambda:os.open({str(Path(d)/'file')!r},os.O_CREAT|os.O_WRONLY,0o600)):
 try:call();raise RuntimeError('policy bypass')
 except OSError as e:assert e.errno==errno.EPERM,e.errno
print('denials enforced')
'''
            p=route.supervise([sys.executable,'-I','-S','-B','-c',code],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=route.ENV,cwd='/',timeout=5,close_fds=True)
            self.assertEqual(p.returncode,0,p.stderr.decode())
            observed={x['syscall'] for x in p.denials}
            self.assertTrue({'mkdir','openat'}<=observed)
            self.assertTrue(observed<={'mkdir','socket','openat'})  # outer sandbox can deny sockets first
            self.assertEqual(list(Path(d).iterdir()),[])

    def test_supervised_real_query_cannot_certify_a_write_denied_fallback(self):
        with tempfile.TemporaryDirectory(prefix='aur-route-query-') as d:
            ready=Route().ready()
            code=f'''
import sys,os,ctypes,socket,array
sys.path.insert(0,{str(Path(route.__file__).parent)!r})
import native_socket_route_readonly as r
r.HOME={d!r}
transport=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM,0,fileno=TRANSPORT_FD)
libc=ctypes.CDLL(None,use_errno=True);lib=ctypes.CDLL(r.SECCOMP,use_errno=True)
r.kernel_ok(libc.prctl(38,1,0,0,0));r.landlock(libc)
listener=r.seccomp(lib,transport.fileno())
transport.sendmsg([b'R'],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',[listener]))])
os.close(listener);transport.close()
r.emit({ready!r})
os.execve(r.GPGCONF,[r.GPGCONF,'--homedir',r.HOME,'--list-dirs','agent-socket'],r.ENV)
'''
            p=route.supervise([sys.executable,'-I','-S','-B','-c',code],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=route.ENV,cwd='/',timeout=5,close_fds=True)
            self.assertEqual(p.returncode,0,p.stderr.decode())
            self.assertTrue(p.stdout.startswith(b'/'))
            self.assertEqual(list(Path(d).iterdir()),[])
            if p.denials:
                self.assertEqual(route.evaluate(p)['native_route_probe'],'BLOCKED_QUERY_SIDE_EFFECT_OR_POLICY_SYSCALL')
