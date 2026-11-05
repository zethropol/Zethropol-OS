"""Module hash/transport rejection tests, independent of any Go execution."""
import io
import stat
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from offline_go import bundle, hash1, ziphash


class OfflineGoRules(unittest.TestCase):
    def setup_proxy(self,root):
        cache=root/'cache';version=cache/'example.org/demo/@v';version.mkdir(parents=True)
        mod=b'module example.org/demo\n';code=b'package demo\n'
        data=io.BytesIO()
        with zipfile.ZipFile(data,'w') as z:z.writestr('example.org/demo@v1.0.0/demo.go',code)
        (version/'v1.0.0.zip').write_bytes(data.getvalue());(version/'v1.0.0.mod').write_bytes(mod)
        (version/'v1.0.0.info').write_text('{"Version":"v1.0.0"}')
        sums=root/'go.sum';sums.write_text('example.org/demo v1.0.0 '+hash1([('example.org/demo@v1.0.0/demo.go',code)])+'\nexample.org/demo v1.0.0/go.mod '+hash1([('go.mod',mod)])+'\n')
        return cache,version,sums

    def test_deterministic_bundle_and_reject_tampering(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);cache,version,sums=self.setup_proxy(root)
            a=bundle(cache,sums,root/'a.tar');b=bundle(cache,sums,root/'b.tar')
            self.assertEqual(a['bundle_sha256'],b['bundle_sha256'])
            (version/'v1.0.0.mod').write_bytes(b'module attacker\n')
            with self.assertRaisesRegex(ValueError,'h1 mismatch'):bundle(cache,sums,root/'bad.tar')
            self.assertFalse((root/'bad.tar').exists())

    def test_unpinned_module_and_symlink_are_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);cache,version,sums=self.setup_proxy(root)
            (version/'v2.0.0.mod').write_bytes(b'module other\n')
            with self.assertRaisesRegex(ValueError,'absent'):bundle(cache,sums,root/'bad.tar')
            (version/'v2.0.0.mod').unlink();(version/'v1.0.0.mod').unlink()
            (version/'v1.0.0.mod').symlink_to(sums)
            with self.assertRaisesRegex(ValueError,'regular'):bundle(cache,sums,root/'bad2.tar')

    def test_zip_path_escape_is_rejected(self):
        data=io.BytesIO()
        with zipfile.ZipFile(data,'w') as z:z.writestr('example.org/demo@v1.0.0/../escape',b'x')
        with self.assertRaisesRegex(ValueError,'Unsafe'):ziphash(data.getvalue(),'example.org/demo@v1.0.0')

    def test_zip_symlink_and_noncanonical_paths_are_rejected(self):
        for name, mode in [('link', stat.S_IFLNK | 0o777),
                           ('./file', stat.S_IFREG | 0o644),
                           ('dir//file', stat.S_IFREG | 0o644),
                           ('fifo', stat.S_IFIFO | 0o600)]:
            with self.subTest(name=name):
                data=io.BytesIO()
                with zipfile.ZipFile(data,'w') as archive:
                    item=zipfile.ZipInfo('example.org/demo@v1.0.0/'+name)
                    item.create_system=3; item.external_attr=mode << 16
                    archive.writestr(item,b'x')
                with self.assertRaisesRegex(ValueError,'Unsafe'):
                    ziphash(data.getvalue(),'example.org/demo@v1.0.0')

    def test_directory_entries_cannot_bypass_zip_quota(self):
        data=io.BytesIO()
        with zipfile.ZipFile(data,'w') as archive:
            for i in range(16385): archive.writestr(f'example.org/demo@v1.0.0/d{i}/',b'')
        with self.assertRaisesRegex(ValueError,'entry quota'):
            ziphash(data.getvalue(),'example.org/demo@v1.0.0')

    def test_proxy_directory_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);cache,version,sums=self.setup_proxy(root)
            (cache/'alias').symlink_to(version, target_is_directory=True)
            with self.assertRaisesRegex(ValueError,'directory.*symlink'):
                bundle(cache,sums,root/'bad.tar')
            self.assertFalse((root/'bad.tar').exists())


if __name__=='__main__':unittest.main()
