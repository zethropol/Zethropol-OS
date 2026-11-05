"""Injected public agent results and dummy FD I/O; no GPG signing/key access."""
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import gpg_backend_readonly as diag
import manifest_clean_transaction as clean
from sealed import Blob


class ReadonlyAgent(unittest.TestCase):
    def setUp(self):
        patches=[patch.object(diag.os,name,return_value=value) for name,value in
                 [('getuid',949),('geteuid',949),('getgid',948),('getegid',948),('getgroups',[948])]]
        patches += [patch.object(diag.pwd,'getpwuid',return_value=SimpleNamespace(pw_name='zethropol-aur-release',pw_dir='/var/lib/zethropol-aur-release',pw_shell='/usr/bin/nologin')),
                    patch.object(diag,'home_metadata',return_value='PASS')]
        for p in patches:p.start();self.addCleanup(p.stop)
        self.calls=[]

    def runner(self,agent_rc=0,agent_bytes=b'D 12345\nOK\n',options=True):
        def run(argv):
            self.calls.append(argv)
            if argv[0]=='/usr/bin/gpg':return SimpleNamespace(returncode=0,stdout=('\n'.join(diag.REQUIRED) if options else '--homedir').encode())
            if '--dump-options' in argv:return SimpleNamespace(returncode=0,stdout=b'--homedir\n--no-autostart\n--no-history\n')
            if argv[0]=='/usr/bin/gpgconf':return SimpleNamespace(returncode=0,stdout=(diag.HOME+'/S.gpg-agent\n').encode())
            return SimpleNamespace(returncode=agent_rc,stdout=agent_bytes)
        return run

    def test_missing_socket_does_not_connect_launch_or_guess_secret_presence(self):
        result=diag.inspect(self.runner(),lambda path:'socket-or-ancestor-absent')
        self.assertEqual(result['result'],'NO_EXISTING_AGENT_SOCKET')
        self.assertEqual(result['secret_key_presence'],'NOT_PROBED')
        self.assertFalse(any('GETINFO pid' in call for call in self.calls))

    def test_safe_existing_agent_query_only_uses_public_getinfo(self):
        with patch.object(diag.Path,'read_text',return_value='Uid:\t949\t949\t949\t949\n'):
            result=diag.inspect(self.runner(),lambda path:'safe-existing-socket')
        self.assertEqual(result['result'],'AGENT_REACHABLE_KEY_AND_CACHE_UNKNOWN')
        query=self.calls[-1]
        self.assertIn('--no-autostart',query);self.assertIn('--no-history',query)
        self.assertEqual(query[-2:],['GETINFO pid','/bye'])
        self.assertNotIn('KEYINFO',' '.join(query))

    def test_unreachable_or_stale_agent_is_reported_without_raw_error(self):
        result=diag.inspect(self.runner(1,b'SECRET private path passphrase'),lambda path:'safe-existing-socket')
        self.assertEqual(result['result'],'EXISTING_AGENT_CONNECTION_FAILED')
        self.assertNotIn('SECRET',json.dumps(result))

    def test_invalid_or_foreign_agent_response_does_not_claim_owner(self):
        result=diag.inspect(self.runner(0,b'D PRIVATE_KEY\nOK\n'),lambda path:'safe-existing-socket')
        self.assertEqual(result['result'],'AGENT_PUBLIC_RESPONSE_INVALID')
        self.assertNotIn('PRIVATE_KEY',json.dumps(result))
        with patch.object(diag.Path,'read_text',return_value='Uid:\t1000\t1000\t1000\t1000\n'):
            result=diag.inspect(self.runner(),lambda path:'safe-existing-socket')
        self.assertEqual(result['result'],'AGENT_OWNER_UNVERIFIED')

    def test_unsafe_home_socket_or_unsupported_options_never_connect(self):
        with patch.object(diag,'home_metadata',return_value='BLOCKED'):
            self.assertEqual(diag.inspect(self.runner())['result'],'HOME_METADATA_BLOCKED')
            self.assertEqual(self.calls,[])
        self.assertEqual(diag.inspect(self.runner(options=False))['result'],'SIGNING_OPTIONS_UNSUPPORTED')
        result=diag.inspect(self.runner(),lambda path:'unsafe-socket-metadata')
        self.assertEqual(result['result'],'SOCKET_ACCESS_OR_METADATA_BLOCKED')
        self.assertFalse(any('GETINFO pid' in call for call in self.calls))

    def test_wrong_uid_never_runs_diagnostic_commands(self):
        with patch.object(diag.os,'getuid',return_value=0):
            with self.assertRaises(ValueError):diag.inspect(self.runner())
        self.assertEqual(self.calls,[])

    def test_consumed_manifest_is_denied_in_future_coordinators(self):
        self.assertIn('bdd617a78db3bf3bda1ea073f11e585a753352bf5a3da86eee6941a0b4f2c8fe',clean.OLD_HASHES)


class PublicFdModel(unittest.TestCase):
    def test_same_uid_sealed_input_and_writable_output_proc_fds_work(self):
        with tempfile.TemporaryFile() as source:
            source.write(b'PUBLIC_DUMMY_INPUT');source.flush()
            with Blob(source.fileno()) as blob:
                target=os.memfd_create('public-dummy-output',os.MFD_CLOEXEC|os.MFD_ALLOW_SEALING)
                try:
                    code="import os,sys;assert open('/proc/self/fd/'+sys.argv[1],'rb').read()==b'PUBLIC_DUMMY_INPUT';f=os.open('/proc/self/fd/'+sys.argv[2],os.O_WRONLY|os.O_TRUNC);os.write(f,b'PUBLIC_DUMMY_OUTPUT');os.close(f)"
                    result=subprocess.run([sys.executable,'-I','-S','-B','-c',code,str(blob.fd),str(target)],
                        pass_fds=(blob.fd,target),stdin=subprocess.DEVNULL,capture_output=True,env=diag.ENV,timeout=5)
                    self.assertEqual(result.returncode,0)
                    self.assertEqual(os.pread(target,100,0),b'PUBLIC_DUMMY_OUTPUT')
                finally:os.close(target)


if __name__=='__main__':unittest.main()
