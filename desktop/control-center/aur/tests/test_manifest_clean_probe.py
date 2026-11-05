"""Injected UID949 public-probe outcomes only; no subprocess or key access."""
import sys
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import manifest_clean_transaction as clean


class PublicProbe(unittest.TestCase):
    def inject(self,returncode,stdout):
        plan={'public_probe':{key:{'path':'/public/'+key,'sha256':'a'*64}
                              for key in ('archive','signature','keyring')}}
        plan['public_probe']['primary']='A'*40
        launcher=SimpleNamespace(drop_identity=lambda:None)
        with patch.object(clean,'read_checked',return_value=b'public'),\
             patch.object(clean,'sealed_public',side_effect=[30,31,32]),\
             patch.object(clean.os,'close'),\
             patch.object(clean.subprocess,'run',return_value=SimpleNamespace(returncode=returncode,stdout=stdout)) as run:
            clean.public_probe(plan,launcher)
            return run.call_args

    def test_success_checks_uid_gpgv_and_file_access_without_private_probe(self):
        args=self.inject(0,b'PASS uid949=verified gid948=verified public-gpgv=verified file-access=verified key-agent=not-probed\n')
        self.assertEqual(args.kwargs['pass_fds'],(30,31,32))
        self.assertEqual(args.kwargs['stdin'],clean.subprocess.DEVNULL)
        self.assertIn('/usr/bin/gpgv',clean.PUBLIC_PROBE)
        self.assertNotIn('--detach-sign',clean.PUBLIC_PROBE)
        self.assertNotIn('private-keys',clean.PUBLIC_PROBE)

    def test_public_failure_phase_is_allowlisted(self):
        clean.phase('uid949-public-verification-probe')
        with self.assertRaises(clean.Blocked):self.inject(1,b'BLOCKED uid949-public-preflight phase=public-file-access\n')
        self.assertEqual(clean.CURRENT_PHASE,'uid949-public-file-access')

    def test_untrusted_child_output_is_never_used_as_phase(self):
        clean.phase('uid949-public-verification-probe')
        with self.assertRaises(clean.Blocked):self.inject(1,b'BLOCKED /secret passphrase agent socket')
        self.assertEqual(clean.CURRENT_PHASE,'uid949-public-verification-probe')

    def test_owner_authorization_cannot_allow_manifest_beyond_one_day(self):
        plan={'schema':1,'purpose':'zethropol-clean-manifest-transaction','manifest_sha256':'b'*64,
              'expires':87401,'manifest_path':'/review/new.json','nonce':'c'*32,'inputs':[{}]*294,
              'bundle_files':{str(i):'d'*64 for i in range(16)},'config':{},'public_probe':{},'source_bindings':{}}
        with self.assertRaises(clean.Blocked):clean.validate_plan(plan,'b'*64,True,now=1000)


if __name__=='__main__':unittest.main()
