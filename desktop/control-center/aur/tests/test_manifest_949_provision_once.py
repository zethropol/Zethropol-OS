"""Transaction fault tests in /tmp only. No sudo, host target or signing calls."""
import contextlib
import errno
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import signal
import sys
import subprocess
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('once', Path(__file__).resolve().parents[1] / 'manifest_949_provision_once.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
REAL_FILECHECK = m.filecheck


from historical_evidence import requires_historical_evidence

class DeploymentTests(unittest.TestCase):
    def setUp(self):
        saved_signals = {sig: signal.getsignal(sig) for sig in m.SIGNALS}
        self.addCleanup(lambda: [signal.signal(sig, handler) for sig, handler in saved_signals.items()])
        self.temp = tempfile.TemporaryDirectory(prefix='manifest-949-installer-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.uid, self.gid = os.getuid(), os.getgid()
        # Fixture operations remain unprivileged; production requires uid/gid 0.
        def fixture_directory(fd, uid=0, gid=0, mode=None):
            s = os.fstat(fd)
            m.require(stat.S_ISDIR(s.st_mode), 'Not directory')
            if mode is not None:
                m.require(stat.S_IMODE(s.st_mode) == mode, 'Fixture mode conflict')
            m.acl(fd)
            return s
        self.directory_patch = patch.object(m, 'directory', fixture_directory)
        self.directory_patch.start()
        self.addCleanup(self.directory_patch.stop)
        self.chown = patch.object(m.os, 'fchown', lambda fd, uid, gid: None)
        self.chown.start()
        self.addCleanup(self.chown.stop)
        # Transaction filecheck is independently tested with real owner values.
        def fixture_check(pfd, name, expected):
            fd = os.open(name, m.FLAGS, dir_fd=pfd)
            try:
                s = os.fstat(fd)
                m.require(stat.S_IMODE(s.st_mode) == 0o644, 'File mode conflict')
                m.acl(fd)
                m.require(m.sha(m.readfd(fd)) == expected, 'Hash mismatch')
                return s
            finally:
                os.close(fd)
        self.check_patch = patch.object(m, 'filecheck', fixture_check)
        self.check_patch.start()
        self.addCleanup(self.check_patch.stop)
        self.txpatch = patch.object(m, 'TX', str(self.root / 'transaction'))
        self.txpatch.start()
        self.addCleanup(self.txpatch.stop)
        self.tx = m.Transaction()
        self.tx.open()
        self.addCleanup(self.tx.close)
        self.destination = self.root / 'destination'
        self.destination.mkdir(mode=0o755)
        self.data = b'approved public bytes'
        self.row = {'target': str(self.destination / 'module.py'), 'sha256': m.sha(self.data)}

    def test_publication_is_complete_and_temp_is_removed(self):
        self.tx.publish(self.row, None, self.data)
        self.assertEqual(Path(self.row['target']).read_bytes(), self.data)
        self.assertEqual(stat.S_IMODE(Path(self.row['target']).stat().st_mode), 0o644)
        self.assertEqual([p.name for p in self.destination.iterdir()], ['module.py'])

    def test_collision_preserves_preexisting_target(self):
        Path(self.row['target']).write_bytes(b'foreign')
        with self.assertRaises(FileExistsError):
            self.tx.publish(self.row, None, self.data)
        self.assertEqual(Path(self.row['target']).read_bytes(), b'foreign')

    def test_symlink_ancestor_is_rejected(self):
        link = self.root / 'link'
        link.symlink_to(self.destination)
        with self.assertRaises(OSError):
            m.parent(str(link / 'module.py'))

    def test_changed_file_stops_rollback(self):
        self.tx.publish(self.row, None, self.data)
        Path(self.row['target']).write_bytes(b'changed')
        # This is a metadata-neutral fixture owner mapping, not root emulation.
        with self.fixture_root_metadata():
            issues = self.tx.rollback()
        self.assertTrue(issues)
        self.assertEqual(Path(self.row['target']).read_bytes(), b'changed')

    @contextlib.contextmanager
    def fixture_root_metadata(self):
        original_stat, original_fstat = os.stat, os.fstat
        def mapped(s):
            fields = {name: getattr(s, name) for name in dir(s) if name.startswith('st_')}
            fields.update(st_uid=0, st_gid=0)
            return SimpleNamespace(**fields)
        with patch.object(m.os, 'stat', side_effect=lambda *a, **k: mapped(original_stat(*a, **k))), patch.object(m.os, 'fstat', side_effect=lambda *a, **k: mapped(original_fstat(*a, **k))):
            yield

    def test_rollback_quarantines_config_before_modules(self):
        self.tx.publish(self.row, None, self.data)
        config = {'target': str(self.destination / 'config.json'), 'sha256': m.sha(b'config')}
        self.tx.publish(config, None, b'config')
        with self.fixture_root_metadata():
            self.assertEqual(self.tx.rollback(), [])
        self.assertEqual(list(self.destination.iterdir()), [])
        q = self.root / 'transaction/quarantine'
        self.assertEqual((q / '00-config.json').read_bytes(), b'config')
        self.assertEqual((q / '01-module.py').read_bytes(), self.data)
        self.assertEqual(stat.S_IMODE((q / '00-config.json').stat().st_mode), 0o600)

    def test_cross_filesystem_quarantine_preserves_bytes(self):
        self.tx.publish(self.row, None, self.data)
        with self.fixture_root_metadata(), patch.object(self.tx, 'move', side_effect=OSError(errno.EXDEV, 'test cross-device')):
            self.assertEqual(self.tx.rollback(), [])
        self.assertFalse(Path(self.row['target']).exists())
        self.assertEqual((self.root / 'transaction/quarantine/00-module.py').read_bytes(), self.data)

    def test_failed_publication_quarantines_only_our_temporary(self):
        Path(self.row['target']).write_bytes(b'preexisting')
        with self.assertRaises(FileExistsError):
            self.tx.publish(self.row, None, self.data)
        with self.fixture_root_metadata():
            self.assertEqual(self.tx.rollback(), [])
        self.assertEqual(Path(self.row['target']).read_bytes(), b'preexisting')
        self.assertEqual([p.name for p in self.destination.iterdir()], ['module.py'])

    def test_config_quarantine_error_preserves_dependencies(self):
        self.tx.publish(self.row, None, self.data)
        config = {'target': str(self.destination / 'config.json'), 'sha256': m.sha(b'config')}
        self.tx.publish(config, None, b'config')
        with self.fixture_root_metadata(), patch.object(m, 'CONFIG', config['target']), patch.object(self.tx, 'move', side_effect=OSError(errno.EIO, 'injected')):
            self.assertTrue(self.tx.rollback())
        self.assertEqual(Path(self.row['target']).read_bytes(), self.data)
        self.assertEqual(Path(config['target']).read_bytes(), b'config')

    def test_rename_committed_but_reported_error_withdraws_config_first(self):
        self.tx.publish(self.row, None, self.data)
        config = {'target': str(self.destination / 'config.json'), 'sha256': m.sha(b'config')}
        original_move = self.tx.move
        def commit_then_error(srcfd, src, dstfd, dst):
            original_move(srcfd, src, dstfd, dst)
            raise OSError(errno.EIO, 'uncertain rename outcome')
        with patch.object(self.tx, 'move', side_effect=commit_then_error), self.assertRaises(OSError):
            self.tx.publish(config, None, b'config')
        self.assertTrue(Path(config['target']).exists())
        with self.fixture_root_metadata(), patch.object(m, 'CONFIG', config['target']):
            self.assertEqual(self.tx.rollback(), [])
        self.assertFalse(Path(config['target']).exists())
        self.assertFalse(Path(self.row['target']).exists())
        self.assertEqual((self.root / 'transaction/quarantine/00-config.json').read_bytes(), b'config')

    def test_unknown_config_outcome_preserves_dependencies(self):
        self.tx.publish(self.row, None, self.data)
        config = {'target': str(self.destination / 'config.json'), 'sha256': m.sha(b'config')}
        original_move = self.tx.move
        def commit_then_error(srcfd, src, dstfd, dst):
            original_move(srcfd, src, dstfd, dst)
            raise OSError(errno.EIO, 'uncertain rename outcome')
        with patch.object(self.tx, 'move', side_effect=commit_then_error), self.assertRaises(OSError):
            self.tx.publish(config, None, b'config')
        original_stat = os.stat
        def fail_config_stat(path, *args, **kwargs):
            if path == 'config.json':
                raise OSError(errno.EIO, 'cannot determine config state')
            return original_stat(path, *args, **kwargs)
        with patch.object(m, 'CONFIG', config['target']), patch.object(m.os, 'stat', side_effect=fail_config_stat):
            self.assertTrue(self.tx.rollback())
        self.assertEqual(Path(config['target']).read_bytes(), b'config')
        self.assertEqual(Path(self.row['target']).read_bytes(), self.data)

    def test_launcher_isolation_is_required_before_input_reads(self):
        with patch.object(m.sys, 'flags', SimpleNamespace(isolated=0, no_site=0)), patch.object(m, 'readpath', side_effect=AssertionError('input must not be read')), self.assertRaises(m.Refused):
            m.run()

    def test_main_rejects_missing_isolation_before_workspace_import(self):
        script = self.root / 'installer.py'
        script.write_bytes(Path(spec.origin).read_bytes())
        (self.root / 'contextlib.py').write_text('raise RuntimeError("UNTRUSTED_MODULE_LOADED")')
        result = subprocess.run([sys.executable, '-S', '-B', str(script)],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Python -I -S -B required', result.stderr)
        self.assertNotIn('UNTRUSTED_MODULE_LOADED', result.stderr)

    def test_path_traversal_rejected(self):
        for value in ('relative', '/tmp/../etc/x', '/tmp//x', '/tmp/./x'):
            with self.subTest(value=value), self.assertRaises(m.Refused):
                m.canonical(value)

    def test_read_rejects_hardlink_and_dangling_symlink(self):
        path = self.root / 'source'; path.write_bytes(b'a')
        alias = self.root / 'alias'; os.link(path, alias)
        with self.assertRaises(m.Refused): m.readpath(str(path))
        alias.unlink(); alias.symlink_to(self.root / 'absent')
        with self.assertRaises(m.Refused): m.readpath(str(alias))

    def test_fifo_is_rejected_before_any_data_open(self):
        source = self.root / 'fifo'; os.mkfifo(source)
        original_open = os.open
        def guarded_open(path, flags, *args, **kwargs):
            if path == source.name:
                self.assertTrue(flags & os.O_PATH, 'Unvalidated source opened for data access')
            if not flags & os.O_PATH and isinstance(path, str) and path.isdecimal():
                self.fail('FIFO must not be reopened for I/O through procfd')
            return original_open(path, flags, *args, **kwargs)
        with patch.object(m.os, 'open', side_effect=guarded_open), self.assertRaises(m.Refused):
            m.readpath(str(source))

    def test_swapped_source_name_never_selects_replacement_inode(self):
        source = self.root / 'source'; source.write_bytes(b'approved')
        previous = self.root / 'previous'
        original_open = os.open
        swapped = False
        def swap_after_pin(path, flags, *args, **kwargs):
            nonlocal swapped
            fd = original_open(path, flags, *args, **kwargs)
            if path == source.name and flags & os.O_PATH and not swapped:
                swapped = True
                source.rename(previous)
                source.write_bytes(b'replacement')
            return fd
        with patch.object(m.os, 'open', side_effect=swap_after_pin):
            self.assertEqual(m.readpath(str(source)), b'approved')
        self.assertEqual(source.read_bytes(), b'replacement')

    def test_hardlink_added_during_read_is_rejected(self):
        source = self.root / 'source'; source.write_bytes(b'approved')
        alias = self.root / 'alias'
        original_read = os.read
        def link_during_read(fd, count):
            data = original_read(fd, count)
            if data and not alias.exists():
                os.link(source, alias)
            return data
        with patch.object(m.os, 'read', side_effect=link_during_read), self.assertRaises(m.Refused):
            m.readpath(str(source))

    def test_real_filecheck_rejects_permission_change_during_read(self):
        source = self.destination / 'module.py'; source.write_bytes(self.data); source.chmod(0o644)
        pfd = os.open(self.destination, m.DIRFLAGS)
        original_read = os.read
        def chmod_during_read(fd, count):
            data = original_read(fd, count)
            if data:
                source.chmod(0o600)
            return data
        try:
            with self.fixture_root_metadata(), patch.object(m.os, 'read', side_effect=chmod_during_read), self.assertRaises(m.Refused):
                REAL_FILECHECK(pfd, source.name, m.sha(self.data))
        finally:
            os.close(pfd)

    def test_prepared_inode_record_precedes_atomic_rename(self):
        original_move = self.tx.move
        def verify_journal(srcfd, src, dstfd, dst):
            events = [json.loads(line) for line in (self.root / 'transaction/audit.jsonl').read_text().splitlines()]
            prepared = events[-1]
            self.assertEqual(prepared['event'], 'publish-prepared')
            self.assertEqual((prepared['dev'], prepared['ino']), m.identity(os.stat(src, dir_fd=srcfd, follow_symlinks=False)))
            return original_move(srcfd, src, dstfd, dst)
        with patch.object(self.tx, 'move', side_effect=verify_journal):
            self.tx.publish(self.row, None, self.data)

    def test_prepared_audit_failure_prevents_final_publication(self):
        original_event = self.tx.event
        def fail_prepared(event, **data):
            if event == 'publish-prepared':
                raise OSError(errno.ENOSPC, 'injected journal failure')
            original_event(event, **data)
        with patch.object(self.tx, 'event', side_effect=fail_prepared), self.assertRaises(OSError):
            self.tx.publish(self.row, None, self.data)
        self.assertFalse(Path(self.row['target']).exists())
        self.assertFalse(self.tx.objects[-1]['rename_attempted'])
        with self.fixture_root_metadata():
            self.assertEqual(self.tx.rollback(), [])
        self.assertFalse(list(self.destination.iterdir()))

    def test_destination_directory_is_synced_before_source_on_rollback(self):
        self.tx.publish(self.row, None, self.data)
        calls = []
        original_fsync = os.fsync
        def record_fsync(fd):
            calls.append(fd)
            return original_fsync(fd)
        with self.fixture_root_metadata(), patch.object(m.os, 'fsync', side_effect=record_fsync):
            self.assertEqual(self.tx.rollback(), [])
        self.assertLess(calls.index(self.tx.quarantine), calls.index(self.tx.objects[0]['pfd']))

    def test_audit_failure_does_not_prevent_withdrawal(self):
        self.tx.publish(self.row, None, self.data)
        with self.fixture_root_metadata(), patch.object(self.tx, 'event', side_effect=OSError(errno.ENOSPC, 'injected audit disk full')):
            issues = self.tx.rollback()
        self.assertTrue(issues)
        self.assertFalse(Path(self.row['target']).exists())
        self.assertEqual((self.root / 'transaction/quarantine/00-module.py').read_bytes(), self.data)

    def test_broken_console_does_not_fail_persistent_audit(self):
        with patch('builtins.print', side_effect=BrokenPipeError()):
            self.tx.event('console-closed')
        lines = (self.root / 'transaction/audit.jsonl').read_text().splitlines()
        self.assertEqual(json.loads(lines[-1])['event'], 'console-closed')

    def fixture_plan(self):
        sources = self.root / 'sources'; sources.mkdir()
        directories = [{'path': str(self.root / ('d' + str(i))), 'owner': 'root:root', 'mode': '0755'} for i in range(5)]
        files = []
        for i in range(12):
            source = sources / ('m' + str(i) + '.py'); source.write_bytes(b'public module ' + str(i).encode())
            files.append({'source': str(source), 'target': directories[0]['path'] + '/' + source.name, 'sha256': m.sha(source.read_bytes()), 'mode': '0644'})
        config = sources / 'config.json'
        config.write_bytes(Path(spec.origin).parent.parent.joinpath('build/aur-owner-949-preprovision-20261101/aur-manifest-signer.json.CANDIDATE').read_bytes())
        target_config = directories[1]['path'] + '/config.json'
        files.append({'source': str(config), 'target': target_config, 'sha256': m.sha(config.read_bytes()), 'mode': '0644'})
        raw = json.dumps({'files': files, 'directories': directories}).encode()
        plan = sources / 'plan.json'; plan.write_bytes(raw)
        return plan, raw, files, directories, target_config

    @contextlib.contextmanager
    def fixture_run(self, plan, raw, config):
        previous_umask = os.umask(0o022)
        os.umask(previous_umask)
        original_read = m.readpath
        def fixture_read(path):
            if path == '/proc/' + str(os.getpid()) + '/uid_map':
                return b'0 0 4294967295\n'
            return original_read(path)
        try:
            with patch.object(m.sys, 'flags', SimpleNamespace(**{**{n: getattr(sys.flags, n) for n in dir(sys.flags) if not n.startswith('_')}, 'isolated': 1, 'no_site': 1})), patch.object(m.sys, 'dont_write_bytecode', True), patch.multiple(m, PLAN=str(plan), PLAN_SHA=m.sha(raw), CONFIG=config, TX=str(self.root / 'run-transaction')), self.fixture_root_metadata(), patch.object(m, 'readpath', side_effect=fixture_read), patch.object(m.os, 'getuid', return_value=0), patch.object(m.os, 'geteuid', return_value=0), patch.object(m.os, 'getgid', return_value=0), patch.object(m.os, 'getegid', return_value=0), contextlib.redirect_stdout(io.StringIO()):
                yield
        finally:
            os.umask(previous_umask)

    @requires_historical_evidence
    def test_modified_plan_is_rejected_without_transaction_creation(self):
        plan, raw, files, directories, config = self.fixture_plan()
        plan.write_bytes(raw + b' ')
        with self.fixture_run(plan, raw, config), self.assertRaises(m.Refused):
            m.run()
        self.assertFalse((self.root / 'run-transaction').exists())
        self.assertTrue(all(not Path(row['path']).exists() for row in directories))

    @requires_historical_evidence
    def test_modified_source_is_rejected_without_transaction_creation(self):
        plan, raw, files, directories, config = self.fixture_plan()
        Path(files[0]['source']).write_bytes(b'unapproved')
        with self.fixture_run(plan, raw, config), self.assertRaises(m.Refused):
            m.run()
        self.assertFalse((self.root / 'run-transaction').exists())
        self.assertTrue(all(not Path(row['path']).exists() for row in directories))

    @requires_historical_evidence
    def test_full_transaction_postflight_and_config_last(self):
        plan, raw, files, directories, config = self.fixture_plan()
        with self.fixture_run(plan, raw, config):
            self.assertEqual(m.run(), 0)
        events = [json.loads(line) for line in (self.root / 'run-transaction/audit.jsonl').read_text().splitlines()]
        published = [e['path'] for e in events if e['event'] == 'publish-complete']
        self.assertEqual(len(published), 13)
        self.assertEqual(published[-1], config)
        after = json.loads((self.root / 'run-transaction/postflight.json').read_text())
        self.assertEqual(len(after), 18)
        self.assertTrue(all(v is not None for v in after.values()))
        self.assertFalse(list(self.root.rglob('*.tmp')))

    @requires_historical_evidence
    def test_failure_after_config_publication_rolls_back_complete_set(self):
        plan, raw, files, directories, config = self.fixture_plan()
        original_publish = m.Transaction.publish
        def fail_after_config(tx, row, before, data):
            original_publish(tx, row, before, data)
            if row['target'] == config:
                raise OSError('injected post-config failure')
        with self.fixture_run(plan, raw, config), patch.object(m.Transaction, 'publish', fail_after_config), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(m.run(), 2)
        self.assertTrue(all(not Path(row['target']).exists() for row in files))
        self.assertTrue(all(not Path(row['path']).exists() for row in directories))
        q = self.root / 'run-transaction/quarantine'
        self.assertEqual(len(list(q.iterdir())), 13)
        self.assertTrue((q / '00-config.json').exists())

    def test_existing_transaction_is_never_reused(self):
        other = m.Transaction()
        with self.assertRaises(FileExistsError): other.open()


if __name__ == '__main__':
    unittest.main()
