"""Read-only source inventory must not promote public prerequisites to consent."""
import importlib.util
import os
from pathlib import Path
import stat
import subprocess
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

SOURCE = Path(__file__).resolve().parents[1]/'release/preflight.py'
spec = importlib.util.spec_from_file_location('source_release_preflight', SOURCE)
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)


class ReleasePreflight(unittest.TestCase):
    def test_source_entrypoint_rejects_arguments_without_import_failure(self):
        result = subprocess.run([sys.executable, '-I', '-B', str(SOURCE), '--unexpected'],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Read-only inventory takes no arguments', result.stderr)
        self.assertNotIn('ModuleNotFoundError', result.stderr)

    def test_complete_public_prerequisites_still_do_not_enable_installation(self):
        keys = {'manifest':'A'*40, 'archive':'B'*40, 'receipt':'C'*40, 'distribution':'D'*40}
        def policy(path, role=None):
            return SimpleNamespace(signers=frozenset([keys[role or 'distribution']]), keyring=SimpleNamespace(close=Mock()))
        def metadata(path):
            mode = 0o755 if str(path).endswith('aur-build-roots') else 0o700
            return SimpleNamespace(st_mode=stat.S_IFDIR | mode, st_uid=0)
        config = {role:{'signer':keys[role]} for role in ('archive','receipt')}
        with patch.object(preflight.TrustPolicy, 'load', side_effect=policy), \
             patch('provider.configuration', return_value=(config, {})), \
             patch('provider.root_path', side_effect=lambda p:Path(p)), \
             patch.object(Path, 'lstat', metadata), patch.object(Path, 'is_file', return_value=True), \
             patch.object(os, 'access', return_value=True), patch.object(preflight.shutil, 'which', return_value='/usr/bin/tool'):
            result = preflight.inspect()
        self.assertTrue(all(check['ok'] for check in result['checks']))
        self.assertEqual(result['public_prerequisites'], 'verified')
        self.assertEqual(result['installation'], 'closed')
        self.assertFalse(result['ready_to_install'])
