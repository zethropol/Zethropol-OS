import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import worker as w


class FakeWorker(w.Worker):
    """Never executes makepkg, pacman, bwrap or a broker."""
    failure = None
    calls = 0

    def execute(self, args, logdir, cancel, timeout):
        self.calls += 1
        (logdir / 'worker.log').write_text('fake phase ' + str(self.calls))
        if self.failure:
            raise w.Rejected(self.failure)
        if '--cleanbuild' in args:
            out = logdir / 'work/out'
            out.mkdir()
            (out / 'demo-1-1-any.pkg.tar.zst').write_bytes(b'FAKE ARCHIVE')


class FakeBroker:
    """Contract fixture, NOT an authorization or libalpm implementation."""
    def __init__(self, result):
        self.artifacts = {a['path']: a['sha256'] for a in result['archives']}
        self.used = False

    def install(self, authorized=True, locked=False, recovery=True, metadata=True,
                dependencies=True, conflicts=False, post=True):
        if not authorized:
            raise w.Rejected('Unauthorized')
        if locked or self.used:
            raise w.Rejected('Pacman locked or operation consumed')
        if not recovery or not metadata or not dependencies or conflicts:
            raise w.Rejected('Precommit verification failed')
        for path, expected in self.artifacts.items():
            if w.digest(w.regular(path)) != expected:
                raise w.Rejected('Archive changed')
        self.used = True
        return 'completed' if post else 'unknown'


class WorkerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.recipe = self.base / 'recipe'
        self.recipe.mkdir()
        (self.recipe / 'PKGBUILD').write_text('build() { false; }')
        (self.recipe / '.SRCINFO').write_text('pkgbase = demo\n')
        (self.recipe / 'source').write_bytes(b'source')
        self.root = self.base / 'root'
        self.root.mkdir()
        self.store = self.base / 'operations'
        self.store.mkdir()
        # Trusted-root checks need root-owned files. Fixtures only replace this
        # check, never enable the real executor or production installer.
        original = w.tree
        self.addCleanup(patch.stopall)
        patch.object(w, 'tree', side_effect=lambda p, trusted=False: original(p, False)).start()
        patch.object(w.os, 'getuid', return_value=1000).start()
        patch.object(w.os, 'geteuid', return_value=1000).start()
        self.plan, self.hash = w.review(self.recipe, self.root, {'commit': 'a'*40},
            [{'name': 'source', 'url': '', 'sha256': w.digest(b'source')}], ['glibc'])

    def run_worker(self, cls=FakeWorker, **kwargs):
        worker = cls(self.store)
        result = worker.run(self.plan, self.hash, self.recipe, self.root, **kwargs)
        self.assertEqual(json.loads((self.store / result['operation'] / 'result.json').read_text()), result)
        return worker, result

    def test_build_only_and_production_broker_always_disabled(self):
        worker, result = self.run_worker()
        self.assertEqual(worker.calls, 3)
        self.assertEqual(result['state'], 'built-untrusted')
        with self.assertRaises(w.Rejected):
            w.InstallationBroker().install(result, authorized=True)

    def test_compact_plan_revalidates_root_before_build(self):
        previous=self.plan
        self.plan,self.hash=w.review(self.recipe,self.root,previous['identity'],
            previous['sources'],previous['dependencies'],schema=2)
        self.assertEqual(set(self.plan['rootfs']),{'sha256','entries'})
        worker,result=self.run_worker()
        self.assertEqual(worker.calls,3)
        self.assertEqual(result['state'],'built-untrusted')
        (self.root/'injected').write_bytes(b'changed')
        worker,result=self.run_worker()
        self.assertEqual(worker.calls,0)
        self.assertNotEqual(result['state'],'built-untrusted')

    def test_attestor_metadata_is_read_inside_existing_sandbox(self):
        class MetadataWorker(FakeWorker):
            capture_metadata=True
            def execute(self,args,logdir,cancel,timeout):
                if '/usr/bin/bsdtar' in args:
                    self.calls+=1
                    self.asserted_sandbox='--unshare-all' in args and '--remount-ro' in args
                    with open(logdir/'worker.log','a') as stream: stream.write('\npkgname = demo\npkgbase = demo\npkgver = 1-1\narch = any\n')
                else: super().execute(args,logdir,cancel,timeout)
        worker,result=self.run_worker(MetadataWorker)
        self.assertTrue(worker.asserted_sandbox);self.assertEqual(result['state'],'built-untrusted')
        self.assertEqual(result['archives'][0]['metadata']['arch'],'any')

    def test_attestor_duplicate_package_metadata_is_rejected(self):
        class MetadataWorker(FakeWorker):
            capture_metadata=True
            def execute(self,args,logdir,cancel,timeout):
                if '/usr/bin/bsdtar' in args:
                    with open(logdir/'worker.log','a') as stream: stream.write('\npkgname = demo\npkgname = other\npkgbase = demo\npkgver = 1-1\narch = any\n')
                else: super().execute(args,logdir,cancel,timeout)
        _,result=self.run_worker(MetadataWorker)
        self.assertEqual(result['state'],'failed');self.assertIn('Duplicate package metadata',result['reason'])

    def test_pkgbuild_change(self):
        (self.recipe / 'PKGBUILD').write_text('changed')
        worker, result = self.run_worker()
        self.assertEqual(worker.calls, 0)
        self.assertEqual(result['state'], 'failed')

    def test_plan_change(self):
        self.plan['dependencies'].append('unexpected')
        worker, result = self.run_worker()
        self.assertIn('plan changed', result['reason'])
        self.assertEqual(worker.calls, 0)

    def test_checksum_mismatch(self):
        self.plan['sources'][0]['sha256'] = 'b'*64
        self.hash = w.digest(w.canonical(self.plan))
        worker, result = self.run_worker()
        self.assertIn('checksum mismatch', result['reason'])
        self.assertEqual(worker.calls, 0)

    def test_failures_and_worker_crash(self):
        for failure in ('Build error', 'Missing dependency', 'Worker crashed (-9)'):
            with self.subTest(failure=failure):
                class Broken(FakeWorker):
                    pass
                Broken.failure = failure
                _, result = self.run_worker(Broken)
                self.assertEqual(result['state'], 'failed')
                self.assertIn(failure, result['reason'])

    def test_cancel(self):
        worker, result = self.run_worker(cancel=lambda: True)
        self.assertEqual(result['state'], 'cancelled')
        self.assertEqual(worker.calls, 0)

    def test_root_rejected(self):
        with patch.object(w.os, 'geteuid', return_value=0):
            worker, result = self.run_worker()
        self.assertEqual(worker.calls, 0)
        self.assertIn('root', result['reason'])

    def test_archive_tamper(self):
        _, result = self.run_worker()
        broker = FakeBroker(result)
        Path(result['archives'][0]['path']).write_bytes(b'tampered')
        with self.assertRaisesRegex(w.Rejected, 'Archive changed'):
            broker.install()

    def test_broker_gates(self):
        _, result = self.run_worker()
        for args in ({'authorized': False}, {'locked': True}, {'recovery': False},
                     {'metadata': False}, {'dependencies': False}, {'conflicts': True}):
            with self.subTest(args=args), self.assertRaises(w.Rejected):
                FakeBroker(result).install(**args)
        broker = FakeBroker(result)
        self.assertEqual(broker.install(post=False), 'unknown')
        with self.assertRaises(w.Rejected):
            broker.install()

    def test_isolation_command(self):
        from sealed import FrozenTree
        fd = os.open(self.recipe, os.O_RDONLY | os.O_DIRECTORY)
        try:
            with FrozenTree(self.root, self.plan['rootfs']) as frozen:
                args = frozen.command(fd, ['/usr/bin/makepkg'])
        finally:
            os.close(fd)
        for flag in ('--unshare-all', '--cap-drop', '--clearenv', '--remount-ro', '--disable-userns', '--new-session'):
            self.assertIn(flag, args)
        for forbidden in ('--share-net', '--syncdeps', '--install', 'sudo', '/run/dbus', '/home'):
            self.assertNotIn(forbidden, args)

    def test_download_checksum_and_redirect(self):
        class Response:
            def __enter__(self): return self
            def __exit__(self, *args): pass
            first=True
            def read1(self, limit):
                if self.first: self.first=False; return b'bad'
                return b''
        with patch.object(w.urllib.request.OpenerDirector, 'open', return_value=Response()):
            with self.assertRaisesRegex(w.Rejected, 'checksum'):
                w.download({'url': 'https://example.org/source', 'sha256': 'a'*64}, self.base/'download')
        with self.assertRaises(w.Rejected):
            w.NoRedirect().redirect_request(None)

    def test_recipe_symlink(self):
        (self.recipe / 'link').symlink_to('/etc/passwd')
        with self.assertRaises(w.Rejected):
            w.review(self.recipe, self.root, {'commit': 'a'*40}, [], [])

    def test_real_supervisor_failure_and_timeout(self):
        # Benign Python subprocesses only. No package executor or broker.
        supervisor = w.Worker(self.store)
        with self.assertRaisesRegex(w.Rejected, 'failed/crashed'):
            supervisor.execute([sys.executable, '-c', 'raise SystemExit(9)'],
                               self.base, lambda: False, 5)
        with self.assertRaisesRegex(w.Rejected, 'timed out'):
            supervisor.execute([sys.executable, '-c', 'import time; time.sleep(10)'],
                               self.base, lambda: False, 0.05)

    def test_real_supervisor_cancel(self):
        with self.assertRaisesRegex(w.Rejected, 'Cancelled'):
            w.Worker(self.store).execute(
                [sys.executable, '-c', 'import time; time.sleep(10)'],
                self.base, lambda: True, 5)


if __name__ == '__main__':
    unittest.main()
