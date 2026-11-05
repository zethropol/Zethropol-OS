"""Capacity mechanics only; no signing, installation or production authority."""
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch, Mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from root_limits import root_limits, package_count_valid
from sealed import Blob, FrozenTree
from trust import approved_manifest, SignedArchive
from worker import Rejected


class CapacityRules(unittest.TestCase):
    def test_legacy_is_not_implicitly_expanded(self):
        self.assertEqual(root_limits(1), (64, 1024**3, 1024**3, 16384))
        self.assertEqual(root_limits(2), (256, 1024**3, 2*1024**3, 65536))
        for schema, n, expected in [(1,64,True),(1,65,False),(2,256,True),(2,257,False),(2,0,False)]:
            self.assertEqual(package_count_valid(schema,n),expected)
        for schema in (True, 0, 3, '2', None):
            with self.assertRaises(ValueError): root_limits(schema)

    def test_manifest_capacity_is_bound_to_version(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'unsigned-unit-data'
            for schema,count,valid in [(1,64,True),(1,65,False),(2,256,True),(2,257,False)]:
                data={'schema':schema,'purpose':'zethropol-aur-build-root','architecture':'x86_64',
                      'expires':int(time.time())+600,'packages':[{'name':f'p{i}','version':'1-1','sha256':'a'*64} for i in range(count)]}
                path.write_text(json.dumps(data));fd=os.open(path,os.O_RDONLY)
                try:
                    with Blob(fd) as blob:
                        if valid: self.assertEqual(len(approved_manifest(SimpleNamespace(archive=blob))[0]['packages']),count)
                        else:
                            with self.assertRaises(Rejected): approved_manifest(SimpleNamespace(archive=blob))
                finally: os.close(fd)

    def test_remaining_compressed_budget_rejects_before_signature_or_copy(self):
        with tempfile.TemporaryFile() as data, tempfile.TemporaryFile() as signature:
            data.write(b'12345');data.flush();signature.write(b'sig');signature.flush()
            policy=Mock()
            with patch('sealed.os.memfd_create') as allocate, self.assertRaises(Rejected):
                SignedArchive(data.fileno(),signature.fileno(),policy,archive_limit=4)
            allocate.assert_not_called();policy.verify.assert_not_called()

    def test_duplicate_or_caller_supplied_capacity_field_is_rejected(self):
        values = ['{"schema":1,"schema":2}', '{"schema":2,"limits":{"expanded":999999999999}}']
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'invalid'
            for raw in values:
                path.write_text(raw);fd=os.open(path,os.O_RDONLY)
                try:
                    with Blob(fd) as blob, self.assertRaises(Rejected):
                        approved_manifest(SimpleNamespace(archive=blob))
                finally:os.close(fd)

    def test_large_root_rejects_before_descriptor_exhaustion(self):
        manifest={f'f{i}':['file',0o444,'a'*64] for i in range(70)}
        with patch('sealed.resource.getrlimit',return_value=(100,100)), self.assertRaisesRegex(Rejected,'file descriptors'):
            FrozenTree('/nonexistent',manifest)
        with self.assertRaisesRegex(Rejected,'entry quota'):
            FrozenTree('/nonexistent',manifest,max_entries=10)

    def test_bwrap_parser_limit_rejects_before_opening_or_copying_root(self):
        # FD budget alone would permit this tree; upstream MAX_ARGS would not.
        manifest={f'f{i}':['file',0o444,'a'*64] for i in range(2000)}
        with patch('sealed.os.open') as opened, self.assertRaisesRegex(Rejected,'bubblewrap argument quota'):
            FrozenTree('/nonexistent',manifest)
        opened.assert_not_called()


if __name__ == '__main__': unittest.main()
