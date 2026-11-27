import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import inference_guard as guard


class LeaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='inference-guard-test-')
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def lease(self, owner='test'):
        return guard.ModelLease(owner, runtime_dir=self.root, min_available_bytes=1)

    def contender(self):
        source = ('import sys; sys.path.insert(0, ' + repr(str(HERE)) + '); '
                  'from inference_guard import ModelLease, BusyError; '
                  '\ntry:\n with ModelLease("contender",runtime_dir=' + repr(str(self.root)) + ',min_available_bytes=1): pass\n'
                  'except BusyError: sys.exit(17)\n')
        return subprocess.run([sys.executable, '-I', '-B', '-c', source], capture_output=True, text=True)

    def test_interprocess_exclusion_and_release(self):
        with self.lease():
            result = self.contender()
            self.assertEqual(result.returncode, 17, result.stderr)
        result = self.contender()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_refuses_low_ram_and_does_not_leave_a_lock(self):
        with patch.object(guard, 'available_ram', return_value=0):
            with self.assertRaises(guard.ResourceError):
                self.lease().acquire()
        with self.lease():
            pass

    def test_symlink_directory_refused_without_target_write(self):
        outside = self.root/'outside'; outside.mkdir()
        (self.root/'zethropol-inference').symlink_to(outside, target_is_directory=True)
        with self.assertRaises(guard.ResourceError):
            self.lease().acquire()
        self.assertEqual(list(outside.iterdir()), [])

    def test_symlink_lock_refused_without_target_write(self):
        directory=self.root/'zethropol-inference';directory.mkdir(mode=0o700)
        target=self.root/'sentinel';target.write_text('keep')
        (directory/'cpu.lock').symlink_to(target)
        with self.assertRaises(guard.ResourceError):
            self.lease().acquire()
        self.assertEqual(target.read_text(),'keep')

    def test_hardlink_and_public_mode_refused(self):
        directory=self.root/'zethropol-inference';directory.mkdir(mode=0o700)
        target=self.root/'sentinel';target.write_text('keep');target.chmod(0o600)
        os.link(target,directory/'cpu.lock')
        with self.assertRaises(guard.ResourceError):self.lease().acquire()
        self.assertEqual(target.read_text(),'keep')
        (directory/'cpu.lock').unlink()
        (directory/'cpu.lock').write_text('');(directory/'cpu.lock').chmod(0o644)
        with self.assertRaises(guard.ResourceError):self.lease().acquire()

    def test_invalid_record_fails_closed(self):
        directory=self.root/'zethropol-inference';directory.mkdir(mode=0o700)
        path=directory/'cpu.lock';path.write_text('{broken');path.chmod(0o600)
        with self.assertRaises(guard.ResourceError):self.lease().acquire()
        self.assertEqual(path.read_text(),'{broken')

    def test_orphan_record_blocks_until_child_exits(self):
        lease=self.lease().acquire()
        child=subprocess.Popen([sys.executable,'-I','-B','-c','import time; time.sleep(20)'])
        try:
            lease.attach_process(child.pid)
            self.assertFalse(os.get_inheritable(lease._fd))
            with self.assertRaises(guard.ResourceError):lease.release()
            self.assertEqual(self.contender().returncode,17)
        finally:
            child.terminate();child.wait(timeout=5)
            lease.release()
        with self.lease():pass

    def test_non_child_pid_cannot_be_attached(self):
        with self.lease() as lease:
            with self.assertRaises(guard.ResourceError):lease.attach_process(os.getpid())

    def test_double_acquire_rejected_release_idempotent(self):
        lease=self.lease().acquire()
        with self.assertRaises(RuntimeError):lease.acquire()
        lease.release();lease.release()
        with self.lease():pass

    def test_unsafe_runtime_root_and_invalid_options_refused(self):
        self.root.chmod(0o755)
        with self.assertRaises(guard.ResourceError):self.lease().acquire()
        self.root.chmod(0o700)
        with self.assertRaises(ValueError):self.lease('../unsafe')
        with self.assertRaises(ValueError):guard.ModelLease('test',min_available_bytes=0)

    def test_concurrent_model_startup(self):
        source = ('import sys, time; sys.path.insert(0, ' + repr(str(HERE)) + '); '
                  'from inference_guard import ModelLease, BusyError; '
                  '\ntry:\n with ModelLease("concurrent",runtime_dir=' + repr(str(self.root)) + ',min_available_bytes=1):\n'
                  '  time.sleep(0.4)\n'
                  'except BusyError: sys.exit(17)\n')
        procs = [subprocess.Popen([sys.executable, '-I', '-B', '-c', source]) for _ in range(5)]
        codes = [p.wait(timeout=10) for p in procs]
        self.assertEqual(codes.count(0), 1, f"Expected exactly 1 winner, got codes: {codes}")
        self.assertEqual(codes.count(17), 4, f"Expected 4 BusyError (17), got codes: {codes}")
        with self.lease():
            pass

    def test_unexpected_launcher_crash_with_dead_child_allows_reacquire(self):
        lease = self.lease().acquire()
        child = subprocess.Popen([sys.executable, '-I', '-B', '-c', 'import time; time.sleep(0.1)'])
        lease.attach_process(child.pid)
        child.wait(timeout=5)
        # Child is now dead. Simulate abrupt launcher death without clean release() by closing fd directly
        os.close(lease._fd)
        lease._fd = None
        # Record on disk still has child info, but child is dead: new lease must succeed
        with self.lease() as next_lease:
            self.assertIsNotNone(next_lease._fd)

    def test_stale_running_child_detected_as_orphan_blocks_new_lease(self):
        lease = self.lease().acquire()
        child = subprocess.Popen([sys.executable, '-I', '-B', '-c', 'import time; time.sleep(30)'])
        try:
            lease.attach_process(child.pid)
            # Simulate launcher crash without releasing child
            os.close(lease._fd)
            lease._fd = None
            # Child is still running: new lease must detect orphan and raise BusyError
            with self.assertRaises(guard.BusyError) as ctx:
                self.lease().acquire()
            self.assertIn('kalan model hâlâ çalışıyor', str(ctx.exception))
            # Verify child process was NOT terminated by foreign launcher attempt
            self.assertIsNone(child.poll(), "Foreign orphan child must not be killed")
        finally:
            child.terminate()
            child.wait(timeout=5)
        # Once orphan child exits, re-acquire must succeed cleanly
        with self.lease():
            pass

    def test_resource_cleanup_on_normal_and_aborted_exit(self):
        # 1. Normal exit cleanup
        with self.lease() as lease:
            fd = lease._fd
            self.assertIsNotNone(fd)
        self.assertIsNone(lease._fd)
        with self.assertRaises(OSError):
            os.fstat(fd)  # fd must be closed
        lock_content = (self.root / 'zethropol-inference' / 'cpu.lock').read_bytes()
        self.assertEqual(lock_content, b'', "Lock file must be truncated on release")

        # 2. Aborted/exception exit cleanup
        try:
            with self.lease() as lease:
                fd_err = lease._fd
                raise ValueError("Simulated engine failure")
        except ValueError:
            pass
        self.assertIsNone(lease._fd)
        with self.assertRaises(OSError):
            os.fstat(fd_err)
        # Immediate subsequent acquisition must succeed
        with self.lease():
            pass


if __name__=='__main__':unittest.main()
