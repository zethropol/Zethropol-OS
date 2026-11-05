"""Staging path/hash/stream bounds. These filesystem fixtures are not authority."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from staged_root import copy_tree, checked_entries, measure_fd, StagedRoot
from worker import digest, Rejected


class StagingRules(unittest.TestCase):
    def test_paths_parents_special_files_and_reserved_mountpoints(self):
        for name in ('../out','/out','usr/../out','usr//out','usr/./out','bad\0name','/'.join(['deep']*129)):
            with self.assertRaises(Rejected): checked_entries({'.':['dir',0o755],name:['file',0o444,'a'*64]},128)
        for value in ({'.':['dir',0o755],'usr/file':['file',0o444,'a'*64]},
                      {'.':['dir',0o755],'usr':['link','/tmp'],'usr/file':['file',0o444,'a'*64]},
                      {'.':['dir',0o755],'proc':['link','/host']},
                      {'.':['dir',0o755],'device':['char',0o666]},
                      {'.':['dir',True]}):
            with self.assertRaises(Rejected): checked_entries(value,128)

    def test_copy_strips_privilege_materializes_aliases_and_preserves_links(self):
        with tempfile.TemporaryDirectory() as d:
            base=Path(d);source=base/'source';source.mkdir();target=base/'target';target.mkdir()
            data=b'bytes';(source/'file').write_bytes(data);os.link(source/'file',source/'alias')
            entries={'.':['dir',0o755],'file':['file',0o6755,digest(data)],
                     'alias':['file',0o444,digest(data)],'link':['link','../../outside']}
            fd=os.open(target,os.O_RDONLY|os.O_DIRECTORY)
            try:
                expected,used=copy_tree(source,fd,entries,4096,128)
                self.assertEqual(used,10)
                self.assertNotEqual((target/'file').stat().st_ino,(target/'alias').stat().st_ino)
                self.assertEqual((target/'file').stat().st_mode&0o7777,0o555)
                self.assertEqual(measure_fd(fd,4096,128),expected)
            finally:
                os.close(fd)
                # Fixtures are ordinary directories, not frozen production FS.
                target.chmod(0o700)

    def test_source_symlink_hash_mismatch_quota_and_cancellation(self):
        for failure in ('link','hash','quota','cancel'):
            with self.subTest(failure=failure),tempfile.TemporaryDirectory() as d:
                base=Path(d);source=base/'source';source.mkdir();target=base/'target';target.mkdir()
                (source/'data').write_bytes(b'payload')
                if failure=='link': (source/'file').symlink_to('data')
                else: (source/'file').write_bytes(b'payload')
                entries={'.':['dir',0o755],'file':['file',0o444,digest(b'wrong' if failure=='hash' else b'payload')]}
                fd=os.open(target,os.O_RDONLY|os.O_DIRECTORY)
                def checkpoint():
                    if failure=='cancel': raise Rejected('Cancelled')
                try:
                    with self.assertRaises((Rejected,OSError)):
                        copy_tree(source,fd,entries,1 if failure=='quota' else 4096,128,checkpoint)
                finally: os.close(fd)

    def test_full_scale_metadata_and_no_fd_preallocation(self):
        entries={'.':['dir',0o755],**{f'f{i}':['file',0o444,'a'*64] for i in range(65000)}}
        normalized=checked_entries(entries,65536)
        self.assertEqual(len(normalized),65005)
        with patch('staged_root.socket.socketpair') as socket:
            with self.assertRaises(Rejected): StagedRoot('/unused',entries,'/unused',max_entries=16384)
            socket.assert_not_called()

    def test_authenticated_capacity_selects_backend_not_source_plan(self):
        from quota import QuotaWorker
        worker=QuotaWorker('/unused','/unused')
        with patch('sealed.FrozenTree') as legacy,patch('staged_root.StagedRoot') as staged:
            worker.freeze_root('/unused',{},1024**3,16384,lambda:False)
            legacy.assert_called_once();staged.assert_not_called()
            worker.root_schema=2
            worker.freeze_root('/unused',{},2*1024**3,65536,lambda:False)
            self.assertEqual(staged.call_args.args[2],'/usr/lib/zethropol/aur-root-stage')

    def test_cancelled_staging_starts_no_helper(self):
        with patch('staged_root.socket.socketpair') as socket:
            with self.assertRaisesRegex(Rejected,'Cancelled'):
                StagedRoot('/unused',{'.':['dir',0o755]},'/unused',cancel=lambda:True)
            socket.assert_not_called()


if __name__=='__main__':unittest.main()
