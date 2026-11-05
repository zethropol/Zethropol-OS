import fcntl
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from worker import Rejected, tree, digest
from sealed import Blob, FrozenTree, beneath, SEALS
from journal import Journal


class SecurityChainTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)

    def test_sealed_fd_rejects_writes_and_survives_path_replacement(self):
        path = self.base/'archive'
        path.write_bytes(b'original')
        fd = os.open(path, os.O_RDONLY)
        self.addCleanup(os.close, fd)
        with Blob(fd) as sealed:
            path.unlink()
            path.write_bytes(b'changed')
            self.assertEqual(os.pread(sealed.fd, 100, 0), b'original')
            self.assertEqual(sealed.sha256, digest(b'original'))
            self.assertEqual(fcntl.fcntl(sealed.fd, fcntl.F_GET_SEALS), SEALS)
            for mutation in (lambda: os.pwrite(sealed.fd, b'X', 0), lambda: os.ftruncate(sealed.fd, 0)):
                with self.assertRaises(OSError): mutation()

    def test_concurrent_modification_during_copy_rejected(self):
        path = self.base/'archive'
        path.write_bytes(b'a'*100)
        fd = os.open(path, os.O_RDONLY)
        original = os.pread
        def raced(source, count, offset):
            data = original(source, count, offset)
            if source == fd:
                path.write_bytes(b'b'*101)
            return data
        try:
            with patch('sealed.os.pread', side_effect=raced), self.assertRaisesRegex(Rejected, 'mutated'):
                Blob(fd)
        finally:
            os.close(fd)

    def test_intermediate_symlink_and_path_escape_rejected(self):
        root = self.base/'root'; root.mkdir()
        (root/'sub').symlink_to(self.base, target_is_directory=True)
        fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
        try:
            for name in ('sub/secret', '../secret', '/etc/passwd'):
                with self.subTest(name=name), self.assertRaises((Rejected, OSError)):
                    beneath(fd, name)
        finally:
            os.close(fd)

    def test_frozen_tree_binds_content_not_original_paths(self):
        root = self.base/'root'; root.mkdir()
        (root/'binary').write_bytes(b'reviewed')
        manifest = tree(root)
        with FrozenTree(root, manifest) as frozen:
            (root/'binary').write_bytes(b'changed')
            self.assertEqual(os.pread(frozen.fds[0], 100, 0), b'reviewed')
            self.assertNotIn(str(root), frozen.mounts)
        with self.assertRaisesRegex(Rejected, 'changed'):
            FrozenTree(root, manifest)

    def journal(self):
        store = self.base/'store'; store.mkdir(mode=0o700, exist_ok=True)
        journal = Journal(store)
        self.addCleanup(journal.close)
        return journal

    def test_journal_cancel_deadline_replay_and_foreign_id(self):
        journal = self.journal()
        operation = journal.create('a'*64, 10)
        with journal.lease(operation):
            self.assertEqual(journal.recover(), [])
            journal.cancel(operation)
            with self.assertRaisesRegex(Rejected, 'Cancelled'):
                journal.check(operation)
            journal.finish(operation, {'state': 'cancelled'})
            with self.assertRaises(Rejected):
                journal.finish(operation, {'state': 'built-untrusted'})
        with self.assertRaises(Rejected), journal.lease(operation): pass
        operation = journal.create('b'*64, 0)
        with journal.lease(operation):
            with self.assertRaisesRegex(Rejected, 'timed out'):
                journal.check(operation)
        with self.assertRaises(Rejected): journal.status('00000000-0000-0000-0000-000000000000')

    def test_real_process_crash_recovery_preserves_completed_result(self):
        journal = self.journal()
        operation = journal.create('c'*64, 30)
        script = '''import sys,time
from journal import Journal
j=Journal(sys.argv[1])
with j.lease(sys.argv[2]):
 print('leased',flush=True)
 time.sleep(30)
'''
        env = dict(os.environ, PYTHONPATH=str(Path(__file__).resolve().parents[1]), PYTHONDONTWRITEBYTECODE='1')
        child = subprocess.Popen([sys.executable, '-c', script, str(journal.path), operation], stdout=subprocess.PIPE, text=True, env=env)
        try:
            self.assertEqual(child.stdout.readline().strip(), 'leased')
            self.assertEqual(journal.recover(), [])
            child.kill(); child.wait(timeout=5)
            self.assertEqual(journal.recover(), [operation])
            self.assertEqual(journal.status(operation)['state'], 'interrupted')
            self.assertEqual(journal.recover(), [])
            completed = journal.create('d'*64, 30)
            with journal.lease(completed): journal.finish(completed, {'state': 'built-untrusted'})
            self.assertEqual(journal.recover(), [])
            self.assertEqual(journal.status(completed)['state'], 'built-untrusted')
        finally:
            if child.poll() is None: child.kill(); child.wait()
            child.stdout.close()


class AlpmPrepareTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.build = Path(cls.temp.name)
        source = Path(__file__).resolve().parents[1]/'prepare_only.c'
        for name, extra in [('signed', []), ('fixture', ['-DZETHROPOL_TEST_UNSIGNED=1'])]:
            subprocess.run(['/usr/bin/cc', '-O2', '-Wall', '-Wextra', '-Werror', *extra, str(source), '-lalpm', '-o', str(cls.build/name)], check=True)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base/'root'; self.root.mkdir(mode=0o700)
        self.db = self.root/'db'; self.db.mkdir(mode=0o700)
        (self.db/'local').mkdir()
        (self.db/'local/ALPM_DB_VERSION').write_text('9\n')
        (self.db/'sync').mkdir()

    def archive(self, name='demo', depends=(), conflicts=(), provides=(), arch='any'):
        info = f'pkgname = {name}\npkgbase = {name}\npkgver = 2-1\npkgdesc = Fixture\nbuilddate = 1\npackager = Test\nsize = 0\narch = {arch}\nlicense = MIT\n'
        for key, values in [('depend', depends), ('conflict', conflicts), ('provides', provides)]:
            info += ''.join(f'{key} = {value}\n' for value in values)
        path = self.base/(name+'.pkg.tar')
        with tarfile.open(path, 'w') as tar:
            data = info.encode(); entry = tarfile.TarInfo('.PKGINFO'); entry.size = len(data)
            tar.addfile(entry, io.BytesIO(data))
        return path

    def local(self, name, version='1-1', provides=(), depends=()):
        directory = self.db/'local'/f'{name}-{version}'; directory.mkdir()
        text = f'%NAME%\n{name}\n\n%VERSION%\n{version}\n\n%ARCH%\nx86_64\n\n'
        for key, values in [('PROVIDES', provides), ('DEPENDS', depends)]:
            if values: text += '%'+key+'%\n'+'\n'.join(values)+'\n\n'
        (directory/'desc').write_text(text)
        (directory/'files').write_text('%FILES%\n\n')

    def repo(self, name, version='2-1'):
        text = f'%FILENAME%\n{name}-{version}-x86_64.pkg.tar.zst\n\n%NAME%\n{name}\n\n%VERSION%\n{version}\n\n%DESC%\nFixture\n\n%CSIZE%\n1\n\n%ISIZE%\n1\n\n%SHA256SUM%\n'+('a'*64)+'\n\n%ARCH%\nx86_64\n\n'
        with tarfile.open(self.db/'sync/core.db', 'w:gz') as tar:
            data = text.encode(); entry = tarfile.TarInfo(f'{name}-{version}/desc'); entry.size = len(data)
            tar.addfile(entry, io.BytesIO(data))

    def prepare(self, path, signed=False, repos='-'):
        fd = os.open(path, os.O_RDONLY)
        try:
            with Blob(fd) as blob:
                return subprocess.run([str(self.build/('signed' if signed else 'fixture')), str(self.root), str(self.db), 'x86_64', repos, str(blob.fd)],
                    pass_fds=(blob.fd,), capture_output=True, text=True, timeout=10)
        finally:
            os.close(fd)

    def test_real_metadata_and_versioned_provides(self):
        self.local('provider', provides=('virtual=3',))
        result = self.prepare(self.archive(depends=('virtual>=2',)))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('ADD\tdemo\t2-1\tany\tarchive', result.stdout)
        self.assertFalse((self.root/'usr').exists())

    def test_missing_versioned_dependency(self):
        self.local('provider', provides=('virtual=1',))
        result = self.prepare(self.archive(depends=('virtual>=2',)))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('prepare', result.stderr)

    def test_conflict_and_reverse_dependency_breakage(self):
        self.local('old')
        result = self.prepare(self.archive(conflicts=('old',)))
        self.assertNotEqual(result.returncode, 0)
        self.local('demo', provides=('virtual=1',))
        self.local('consumer', depends=('virtual=1',))
        result = self.prepare(self.archive())
        self.assertNotEqual(result.returncode, 0)

    def test_architecture_mismatch(self):
        result = self.prepare(self.archive(arch='aarch64'))
        self.assertNotEqual(result.returncode, 0)

    def test_real_database_lock(self):
        (self.db/'db.lck').write_text('fixture lock')
        result = self.prepare(self.archive())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('lock', result.stderr)
        self.assertTrue((self.db/'db.lck').exists())

    def test_default_signature_policy_refuses_unsigned_archive(self):
        result = self.prepare(self.archive(), signed=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('signature', result.stderr)

    def test_official_overlap_and_full_upgrade_gate(self):
        self.repo('demo')
        result = self.prepare(self.archive(), repos='core')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('official repository', result.stderr)
        self.repo('official')
        self.local('official')
        result = self.prepare(self.archive(), repos='core')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('official upgrade', result.stderr)

    def test_metadata_mutation_is_independently_rejected(self):
        path = self.archive()
        path.write_bytes(b'not a package')
        result = self.prepare(path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('metadata', result.stderr)


if __name__ == '__main__': unittest.main()
