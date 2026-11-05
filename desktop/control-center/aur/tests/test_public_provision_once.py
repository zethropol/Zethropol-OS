"""Four-file transaction/fault tests. All writes use unprivileged /tmp fixtures.

UID/GID and directory-policy facts are modeled for fixtures; this is not host
root ownership/provisioning evidence. The actual root entry point is never run.
"""
import contextlib
import copy
import errno
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('public_once',Path(__file__).resolve().parents[1]/'public_provision_once.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
ORIGINAL_DIRECTORY=m.directory


class PublicProvisionTests(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='public-provision-test-')
        self.addCleanup(self.temporary.cleanup)
        self.base=Path(self.temporary.name);self.base.chmod(0o755)
        saved={sig:signal.getsignal(sig) for sig in m.SIGNALS}
        self.addCleanup(lambda:[signal.signal(sig,handler) for sig,handler in saved.items()])
        previous_umask=os.umask(0o077);self.addCleanup(lambda:os.umask(previous_umask))
        self.stack=contextlib.ExitStack();self.addCleanup(self.stack.close)
        original_stat,original_fstat=os.stat,os.fstat
        def mapped(s):
            fields={n:getattr(s,n) for n in dir(s) if n.startswith('st_')}
            fields.update(st_uid=0,st_gid=0)
            return SimpleNamespace(**fields)
        self.stack.enter_context(patch.object(m.os,'stat',side_effect=lambda *a,**k:mapped(original_stat(*a,**k))))
        self.stack.enter_context(patch.object(m.os,'fstat',side_effect=lambda *a,**k:mapped(original_fstat(*a,**k))))
        self.stack.enter_context(patch.object(m.os,'fchown',lambda fd,uid,gid:None))
        def fixture_directory(fd,uid=0,gid=0,mode=None):
            s=os.fstat(fd)
            m.require(stat.S_ISDIR(s.st_mode),'Fixture directory required')
            if mode is not None:m.require(stat.S_IMODE(s.st_mode)==mode,'Fixture directory mode')
            m.acl(fd);return s
        self.stack.enter_context(patch.object(m,'directory',fixture_directory))
        self.stack.enter_context(patch.object(m,'identity_guard',lambda apply:None))
        self.sources=self.base/'sources';self.sources.mkdir()
        self.targets=self.base/'targets';self.targets.mkdir();self.targets.chmod(0o755)
        self.protected=self.base/'protected';self.protected.mkdir();self.protected.chmod(0o755)
        original_plan=json.loads(Path(m.PLAN).read_bytes())
        self.plan=copy.deepcopy(original_plan);self.rows=[];self.specs={}
        names=['root-helper','quota-helper','distribution.gpg','distribution.json']
        for row,name in zip(original_plan['files'],names):
            source=self.sources/name;source.write_bytes(Path(row['source']).read_bytes())
            target=self.targets/name
            new={'source':str(source),'target':str(target),'mode':row['mode'],'owner':'root:root','sha256':m.sha(source.read_bytes())}
            self.rows.append(new);self.specs[str(target)]=(str(source),new['mode'],new['sha256'])
        self.config=str(self.targets/'distribution.json');self.ring=str(self.targets/'distribution.gpg')
        self.txroot=self.base/'transaction'
        self.plan.update(files=self.rows,publish_order=list(self.specs))
        self.plan['transaction_layout']['root']=str(self.txroot)
        self.project=self.sources/'project.json';self.project.write_bytes(Path(m.PROJECT).read_bytes());self.project.chmod(0o644)
        accepted=[]
        for i in range(13):
            path=self.protected/f'p{i}.py';path.write_bytes(f'protected-{i}'.encode());path.chmod(0o644)
            accepted.append({'target':str(path),'sha256':m.sha(path.read_bytes())})
        self.accepted=self.sources/'accepted.json';self.accepted.write_text(json.dumps({'files':accepted}))
        self.accepted_bytes={r['target']:Path(r['target']).read_bytes() for r in accepted}
        self.planpath=self.sources/'plan.json';self.planpath.write_text(json.dumps(self.plan))
        self.stack.enter_context(patch.multiple(m,PLAN=str(self.planpath),PLAN_SHA=m.sha(self.planpath.read_bytes()),
            ACCEPTED_PLAN=str(self.accepted),ACCEPTED_SHA=m.sha(self.accepted.read_bytes()),SPECS=self.specs,
            PROJECT=str(self.project),CONFIG=self.config,RING=self.ring,TX=str(self.txroot)))
        self.output=io.StringIO();self.stack.enter_context(contextlib.redirect_stdout(self.output))
        self.stack.enter_context(contextlib.redirect_stderr(self.output))

    def run_apply(self):return m.run(apply=True)
    def no_targets(self):return all(not Path(r['target']).exists() for r in self.rows)
    def protected_unchanged(self):return all(Path(p).read_bytes()==v for p,v in self.accepted_bytes.items())
    def tx(self):
        result=m.Transaction();result.open();self.addCleanup(result.close);return result

    def test_full_transaction_four_modes_policy_last_and_protected_unchanged(self):
        self.assertEqual(self.run_apply(),0)
        for row in self.rows:
            self.assertEqual(m.sha(Path(row['target']).read_bytes()),row['sha256'])
            self.assertEqual(stat.S_IMODE(Path(row['target']).stat().st_mode),int(row['mode'],8))
        events=[json.loads(x) for x in (self.txroot/'audit.jsonl').read_text().splitlines()]
        self.assertEqual([e['path'] for e in events if e['event']=='publish-complete'],list(self.specs))
        self.assertEqual(events[-1]['event'],'success')
        self.assertEqual(events[-1]['protected_signer_files_verified'],13)
        self.assertTrue(self.protected_unchanged())
        self.assertFalse(list(self.targets.glob('*.tmp')))
        self.assertEqual(len(json.loads((self.txroot/'postflight.json').read_bytes())['targets']),4)

    def test_default_check_is_read_only(self):
        self.assertEqual(m.run(),0);self.assertTrue(self.no_targets());self.assertFalse(self.txroot.exists())
        self.assertIn('read-only preflight',self.output.getvalue())

    def test_any_matching_preexisting_target_refuses_without_transaction(self):
        row=self.rows[0];Path(row['target']).write_bytes(Path(row['source']).read_bytes())
        with self.assertRaises(m.Refused):self.run_apply()
        self.assertFalse(self.txroot.exists())

    def test_dangling_target_symlink_refuses(self):
        Path(self.config).symlink_to(self.base/'missing')
        with self.assertRaises(m.Refused):self.run_apply()
        self.assertFalse(self.txroot.exists())

    def test_reserved_temporary_conflict_refuses_without_removing_it(self):
        path=self.targets/'.zethropol-aur-public-20261101-2.tmp';path.write_bytes(b'foreign')
        with self.assertRaises(m.Refused):self.run_apply()
        self.assertEqual(path.read_bytes(),b'foreign');self.assertFalse(self.txroot.exists())

    def test_plan_hash_change_refuses(self):
        self.planpath.write_bytes(self.planpath.read_bytes()+b' ')
        with self.assertRaises(m.Refused):self.run_apply()
        self.assertFalse(self.txroot.exists())

    def test_source_hash_change_refuses(self):
        Path(self.rows[0]['source']).write_bytes(b'unapproved')
        with self.assertRaises(m.Refused):self.run_apply()
        self.assertFalse(self.txroot.exists())

    def test_source_symlink_and_hardlink_refuse(self):
        source=Path(self.rows[0]['source']);saved=source.with_name('saved');source.rename(saved);source.symlink_to(saved)
        with self.assertRaises(m.Refused):self.run_apply()
        source.unlink();os.link(saved,source)
        with self.assertRaises(m.Refused):self.run_apply()
        self.assertFalse(self.txroot.exists())

    def test_capability_xattr_rejected(self):
        with patch.object(m.os,'listxattr',return_value=['security.capability']),self.assertRaises(m.Refused):self.run_apply()
        self.assertFalse(self.txroot.exists())

    def test_scope_or_mode_drift_rejected(self):
        for field,value in [('target','/etc/sudoers'),('source','/etc/shadow'),('mode','4755'),('owner','949:948')]:
            changed=copy.deepcopy(self.plan);changed['files'][0][field]=value
            with self.subTest(field=field),self.assertRaises(m.Refused):m.validate_plan(changed)

    def test_failure_after_policy_rolls_back_four_in_reverse_order(self):
        publish=m.Transaction.publish
        def failed(tx,row,data):
            publish(tx,row,data)
            if row['target']==self.config:raise OSError('injected after config')
        with patch.object(m.Transaction,'publish',failed):self.assertEqual(self.run_apply(),2)
        self.assertTrue(self.no_targets());self.assertTrue(self.protected_unchanged())
        q=self.txroot/'quarantine';self.assertEqual(len(list(q.iterdir())),4)
        self.assertEqual((q/'00-distribution.json').read_bytes(),Path(self.rows[-1]['source']).read_bytes())
        self.assertEqual(stat.S_IMODE((q/'00-distribution.json').stat().st_mode),0o600)

    def test_failure_before_policy_does_not_publish_policy(self):
        publish=m.Transaction.publish
        def failed(tx,row,data):
            if row['target']==self.ring:raise OSError('injected before ring')
            publish(tx,row,data)
        with patch.object(m.Transaction,'publish',failed):self.assertEqual(self.run_apply(),2)
        self.assertTrue(self.no_targets());self.assertEqual(len(list((self.txroot/'quarantine').iterdir())),2)

    def test_uncertain_policy_rename_resolved_by_inode_before_rollback(self):
        move=m.Transaction.move
        def commit_then_error(tx,sfd,src,dfd,dst):
            move(tx,sfd,src,dfd,dst)
            if dst=='distribution.json':raise OSError(errno.EIO,'uncertain rename')
        with patch.object(m.Transaction,'move',commit_then_error):self.assertEqual(self.run_apply(),2)
        self.assertTrue(self.no_targets());self.assertTrue((self.txroot/'quarantine/00-distribution.json').exists())

    def test_unknown_policy_outcome_preserves_dependencies(self):
        move=m.Transaction.move;original_stat=os.stat;uncertain=False
        def commit_then_error(tx,sfd,src,dfd,dst):
            nonlocal uncertain
            move(tx,sfd,src,dfd,dst)
            if dst=='distribution.json':uncertain=True;raise OSError(errno.EIO,'uncertain rename')
        def fail_stat(path,*a,**k):
            if uncertain and path=='distribution.json':raise OSError(errno.EIO,'cannot inspect config')
            return original_stat(path,*a,**k)
        with patch.object(m.Transaction,'move',commit_then_error),patch.object(m.os,'stat',side_effect=fail_stat):self.assertEqual(self.run_apply(),3)
        self.assertTrue(all(Path(r['target']).exists() for r in self.rows));self.assertTrue(self.protected_unchanged())

    def test_foreign_policy_before_our_config_preserves_dependencies(self):
        tx=self.tx()
        for row in self.rows[:-1]:tx.publish(row,Path(row['source']).read_bytes())
        foreign=Path(self.config);foreign.write_bytes(Path(self.rows[-1]['source']).read_bytes())
        self.assertTrue(tx.rollback())
        self.assertTrue(all(Path(row['target']).exists() for row in self.rows))
        self.assertTrue(self.protected_unchanged())

    def test_config_rename_collision_preserves_dependencies_and_foreign_policy(self):
        move=m.Transaction.move
        def collide(tx,sfd,src,dfd,dst):
            if dst=='distribution.json':
                Path(self.config).write_bytes(b'foreign policy')
                raise OSError(errno.EEXIST,'competing root publication')
            return move(tx,sfd,src,dfd,dst)
        with patch.object(m.Transaction,'move',collide):self.assertEqual(self.run_apply(),3)
        self.assertTrue(all(Path(row['target']).exists() for row in self.rows))
        self.assertEqual(Path(self.config).read_bytes(),b'foreign policy')
        self.assertTrue(self.protected_unchanged())

    def test_replaced_policy_inode_prevents_dependency_withdrawal(self):
        tx=self.tx()
        for row in self.rows:tx.publish(row,Path(row['source']).read_bytes())
        Path(self.config).unlink();Path(self.config).write_bytes(Path(self.rows[-1]['source']).read_bytes())
        self.assertTrue(tx.rollback())
        self.assertTrue(all(Path(row['target']).exists() for row in self.rows))

    def test_prepared_audit_failure_prevents_publication(self):
        event=m.Transaction.event
        def fail_prepared(tx,event_name,**data):
            if event_name=='publish-prepared':raise OSError(errno.ENOSPC,'journal full')
            event(tx,event_name,**data)
        with patch.object(m.Transaction,'event',fail_prepared):self.assertEqual(self.run_apply(),2)
        self.assertTrue(self.no_targets());self.assertTrue(self.protected_unchanged())

    def test_cross_filesystem_quarantine_preserves_all_bytes(self):
        tx=self.tx()
        for row in self.rows:tx.publish(row,Path(row['source']).read_bytes())
        with patch.object(tx,'move',side_effect=OSError(errno.EXDEV,'cross device')):self.assertEqual(tx.rollback(),[])
        self.assertTrue(self.no_targets())
        self.assertEqual(len(list((self.txroot/'quarantine').iterdir())),4)

    def test_destination_directory_fsync_precedes_source_withdrawal_commit(self):
        tx=self.tx();row=self.rows[0];tx.publish(row,Path(row['source']).read_bytes())
        real=os.fsync;calls=[]
        def fsync(fd):calls.append(fd);return real(fd)
        with patch.object(m.os,'fsync',side_effect=fsync):self.assertEqual(tx.rollback(),[])
        self.assertLess(calls.index(tx.quarantine),calls.index(tx.objects[0]['pfd']))

    def test_changed_published_helper_is_preserved(self):
        tx=self.tx();row=self.rows[0];tx.publish(row,Path(row['source']).read_bytes())
        Path(row['target']).write_bytes(b'foreign edit')
        self.assertTrue(tx.rollback());self.assertEqual(Path(row['target']).read_bytes(),b'foreign edit')

    def test_protected_payload_drift_aborts_before_policy_without_restoring_foreign_edit(self):
        publish=m.Transaction.publish;victim=next(iter(self.accepted_bytes))
        def tamper(tx,row,data):
            publish(tx,row,data)
            if row['target']==self.ring:Path(victim).write_bytes(b'external edit')
        with patch.object(m.Transaction,'publish',tamper):self.assertEqual(self.run_apply(),2)
        self.assertTrue(self.no_targets());self.assertEqual(Path(victim).read_bytes(),b'external edit')

    def test_policy_keyring_project_role_drift_rejected(self):
        content={r['target']:Path(r['source']).read_bytes() for r in self.rows}
        config=json.loads(content[self.config]);config['keyring']='../manifest.gpg';content[self.config]=json.dumps(config).encode()
        with self.assertRaises(m.Refused):m.validate_policy(self.plan,content)

    def test_elf_static_checks_reject_executable_stack_and_bad_loader(self):
        data=Path(self.rows[0]['source']).read_bytes();m.elf_valid(data)
        bad=bytearray(data);bad[16:18]=(2).to_bytes(2,'little')
        with self.assertRaises(m.Refused):m.elf_valid(bad)
        with self.assertRaises(m.Refused):m.elf_valid(data.replace(b'/lib64/ld-linux-x86-64.so.2',b'/tmp/xx/ld-linux-x86-64.so.2'))

    def test_early_main_requires_isolation_before_local_imports(self):
        script=self.base/'installer.py';script.write_bytes(Path(spec.origin).read_bytes())
        (self.base/'contextlib.py').write_text('raise RuntimeError("UNTRUSTED_MODULE_LOADED")')
        result=subprocess.run([sys.executable,'-S','-B',str(script)],capture_output=True,text=True,timeout=10)
        self.assertNotEqual(result.returncode,0);self.assertIn('Python -I -S -B required',result.stderr)
        self.assertNotIn('UNTRUSTED_MODULE_LOADED',result.stderr)


if __name__=='__main__':unittest.main()
