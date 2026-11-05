"""New coordinator guards only. No signer, real UID drop, key, or host writes."""
import json
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from contextlib import redirect_stdout

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import manifest_clean_transaction as clean


class CleanTransaction(unittest.TestCase):
    def plan(self):
        return {'schema':1,'purpose':'zethropol-clean-manifest-transaction','manifest_sha256':'b'*64,
                'expires':2000,'manifest_path':'/review/new.json','nonce':'c'*32,'inputs':[{}]*294,
                'bundle_files':{str(i):'d'*64 for i in range(16)},'config':{},'public_probe':{},'source_bindings':{}}

    def test_new_exact_authorization_is_required_before_any_access(self):
        plan=self.plan()
        with patch.object(clean,'preflight',side_effect=AssertionError('No access permitted')),patch.object(clean.time,'time',return_value=1000):
            for authorization in (None,'a'*64):
                with self.assertRaises(clean.Blocked):clean.main(plan,'--apply',authorization)
        clean.validate_plan(plan,'b'*64,apply=True,now=1000)

    def test_expired_candidate_rejected_even_with_matching_authorization(self):
        plan=self.plan()
        with self.assertRaises(clean.Blocked):clean.validate_plan(plan,'b'*64,True,now=2000)

    def test_old_failed_hashes_rejected(self):
        for old in clean.OLD_HASHES:
            plan=self.plan();plan['manifest_sha256']=old
            with self.assertRaises(clean.Blocked):clean.validate_plan(plan,old,True,now=1000)

    def test_new_names_are_exact_and_independent(self):
        plan=self.plan();names=clean.names(plan)
        self.assertEqual(names[0],'b'*64+'.json')
        self.assertEqual(names[1],'b'*64)
        self.assertTrue(all('c'*32 in name for name in names[2:]))
        changed=dict(plan,nonce='d'*32)
        self.assertNotEqual(clean.names(changed)[2:],names[2:])
        self.assertNotIn('4597d7',json.dumps(names))

    def test_nonce_schema_bool_expiry_and_path_injection_rejected(self):
        for field,value in (('schema',True),('expires',True),('nonce','../audit'),('manifest_sha256','../key')):
            plan=self.plan();plan[field]=value
            with self.assertRaises(clean.Blocked):clean.validate_plan(plan,now=1000)

    def test_mutating_entrypoint_has_explicit_mode_guard(self):
        with self.assertRaises(clean.Blocked):clean.main(self.plan(),'--retry','b'*64)

    def test_read_only_public_snapshot_checks_digest_symlinks_and_hardlinks(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'public';path.write_bytes(b'public bytes')
            self.assertEqual(clean.read_checked(str(path),clean.digest(b'public bytes')),b'public bytes')
            with self.assertRaises(clean.Blocked):clean.read_checked(str(path),'a'*64)
            link=Path(temporary)/'link';link.symlink_to(path)
            with self.assertRaises(OSError):clean.read_checked(str(link))
            hard=Path(temporary)/'hard';os.link(path,hard)
            with self.assertRaises(clean.Blocked):clean.read_checked(str(path))

    def test_existing_target_including_symlink_is_never_reused(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary);fd=os.open(path,clean.DFLAGS)
            try:
                clean.absent(fd,'new')
                (path/'new').symlink_to(path/'missing')
                with self.assertRaises(clean.Blocked):clean.absent(fd,'new')
            finally:os.close(fd)

    def test_audit_retains_large_time_as_string_and_structured_child_outcome(self):
        with tempfile.TemporaryDirectory() as temporary:
            file=Path(temporary)/'synthetic-audit';fd=os.open(file,os.O_RDWR|os.O_CREAT|os.O_EXCL,0o600)
            try:
                with patch.object(clean.time,'time_ns',return_value=1793532307947935714):
                    event=clean.event_writer(fd,self.plan());event('signer-process-outcome',outcome={'child_returncode':1,'diagnostic':{'phase':'gpg-signing','backend_returncode':2}})
                row=json.loads(file.read_text())
                self.assertEqual(row['time_ns'],'1793532307947935714')
                self.assertEqual(row['outcome']['diagnostic']['backend_returncode'],2)
                with self.assertRaises(clean.Blocked):event('arbitrary-secret-event')
            finally:os.close(fd)

    def test_read_only_preflight_never_calls_mutator(self):
        plan=self.plan()
        with patch.object(clean.time,'time',return_value=1000),patch.object(clean.os,'getuid',return_value=0),\
             patch.object(clean.os,'geteuid',return_value=0),patch.object(clean.sys,'flags') as flags,\
             patch.object(clean.Path,'read_text',return_value='0 0 4294967295'),\
             patch.object(clean,'preflight',return_value=(b'public',{},(),())),\
             patch.object(clean,'apply_transaction',side_effect=AssertionError('Mutation forbidden')) as apply:
            flags.isolated=True;flags.no_site=True
            with patch.object(clean.sys,'dont_write_bytecode',True),redirect_stdout(io.StringIO()):clean.main(plan,'--check')
            apply.assert_not_called()


if __name__=='__main__':unittest.main()
