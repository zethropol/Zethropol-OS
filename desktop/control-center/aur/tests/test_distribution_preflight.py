import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from distribution_preflight import file_hash, inspect, target


class PreflightTests(unittest.TestCase):
    def test_source_hash_and_mismatch(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'source'; path.write_bytes(b'public')
            plan = {'files': [{'source': str(path), 'sha256': file_hash(path)}]}
            self.assertTrue(inspect(plan)['ok'])
            path.write_bytes(b'changed')
            self.assertFalse(inspect(plan)['ok'])

    def test_symlink_dangling_and_hardlink_sources_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'source'; path.write_bytes(b'public')
            link = Path(temporary) / 'link'; link.symlink_to(path)
            with self.assertRaises(OSError): file_hash(link)
            path.unlink()
            with self.assertRaises(OSError): file_hash(link)
            path.write_bytes(b'public'); link.unlink(); os.link(path, link)
            with self.assertRaises(ValueError): file_hash(path)

    def test_target_conflicts_and_reuse(self):
        with tempfile.TemporaryDirectory() as temporary, patch('distribution_preflight.ancestors'):
            path = Path(temporary) / 'target'
            row = {'target': str(path), 'uid': os.getuid(), 'gid': os.getgid(), 'mode': '0644'}
            self.assertEqual(target(row), 'create')
            path.write_bytes(b'public'); path.chmod(0o644); row['sha256'] = file_hash(path)
            self.assertEqual(target(row), 'reuse')
            path.write_bytes(b'conflict')
            with self.assertRaises(ValueError): target(row)
            path.unlink(); path.symlink_to('/nonexistent')
            with self.assertRaises(ValueError): target(row)

    def test_symlink_ancestor_rejected(self):
        from distribution_preflight import ancestors
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'link'; path.symlink_to('/usr')
            with self.assertRaises(ValueError): ancestors(path / 'target')

    def test_acl_conflict(self):
        with tempfile.TemporaryDirectory() as temporary, patch('distribution_preflight.ancestors'), patch('distribution_preflight.os.listxattr', return_value=['system.posix_acl_access']):
            path = Path(temporary) / 'directory'; path.mkdir(mode=0o700)
            with self.assertRaises(ValueError): target({'path': str(path), 'uid': os.getuid(), 'gid': os.getgid(), 'mode': '0700'}, True)
