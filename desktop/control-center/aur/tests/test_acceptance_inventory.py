"""Inventory metadata cannot imply trust, usable KVM or guest acceptance."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1]/'release/acceptance_inventory.py'
spec = importlib.util.spec_from_file_location('acceptance_inventory', SOURCE)
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


class AcceptanceInventory(unittest.TestCase):
    def test_empty_directory_is_not_verified_and_symlink_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = inventory.directory_inventory(root)
            self.assertEqual(result['entry_count'], 0)
            self.assertFalse(result['contents_verified'])
            link = root/'link'
            link.symlink_to(root, target_is_directory=True)
            self.assertEqual(inventory.directory_inventory(link)['state'], 'rejected')
            self.assertEqual(inventory.directory_inventory(root/'missing')['state'], 'missing')

    def test_inaccessible_is_not_reported_as_absent(self):
        with patch.object(Path, 'lstat', side_effect=PermissionError):
            self.assertEqual(inventory.directory_inventory('/var/lib/machines')['state'], 'inaccessible')

    def test_large_directory_scan_is_bounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            for index in range(258):
                (Path(tmp)/str(index)).touch()
            result = inventory.directory_inventory(tmp)
            self.assertEqual(result['entry_count'], 256)
            self.assertTrue(result['truncated'])
            self.assertFalse(result['contents_verified'])

    def test_all_tools_and_metadata_never_open_acceptance_or_devices(self):
        with patch.object(inventory.shutil, 'which', return_value='/usr/bin/tool'), \
             patch.object(inventory, 'directory_inventory', return_value={'state':'observed'}), \
             patch.object(inventory.os, 'access', return_value=True), \
             patch('builtins.open', side_effect=AssertionError('must not read devices/files')), \
             patch('subprocess.run', side_effect=AssertionError('must not execute helpers')):
            result = inventory.inspect()
        self.assertEqual(result['installation'], 'closed')
        self.assertEqual(result['production_acceptance'], 'blocked')
        self.assertFalse(result['guest_verified'])
        self.assertFalse(result['public_artifacts_verified'])
        self.assertFalse(result['kvm']['opened'])
        self.assertFalse(result['kvm']['acceleration_tested'])
        self.assertTrue(all(value is False for value in result['operations_performed'].values()))

    def test_cli_cannot_select_a_guest_or_acceptance_scope(self):
        result = subprocess.run([sys.executable, '-I', '-B', str(SOURCE), '--guest'],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('takes no arguments', result.stderr)
