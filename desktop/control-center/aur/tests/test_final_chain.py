"""Negative gates and extraction mechanics; live_signed_root proves crypto."""
from contextlib import ExitStack
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from worker import InstallationBroker, Worker, Rejected, digest, canonical
from sealed import Blob
from trust import TrustPolicy, approved_manifest
from provision import provision, package_database
from host_view import capture_host_view


class FinalChainTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.base=Path(self.temp.name);self.stack=ExitStack();self.addCleanup(self.stack.close)

    def blob(self,path):
        fd=os.open(path,os.O_RDONLY)
        try: return self.stack.enter_context(Blob(fd))
        finally: os.close(fd)

    def extraction_fixture(self,extras=(),metadata=None,schema=1,compressed=False):
        path=self.base/'package.tar'
        with tarfile.open(path,'w:gz' if compressed else 'w') as tar:
            data=metadata or b'pkgname = test-root\npkgver = 1-1\narch = any\n'
            info=tarfile.TarInfo('.PKGINFO');info.size=len(data);tar.addfile(info,io.BytesIO(data))
            for name,kind,data in extras:
                info=tarfile.TarInfo(name)
                if kind=='unreadable': info.mode=0o4110
                if kind=='link': info.type=tarfile.SYMTYPE;info.linkname=data;tar.addfile(info)
                elif kind=='hardlink': info.type=tarfile.LNKTYPE;info.linkname=data;tar.addfile(info)
                elif kind=='dir': info.type=tarfile.DIRTYPE;tar.addfile(info)
                elif kind=='device': info.type=tarfile.CHRTYPE;tar.addfile(info)
                else: info.size=len(data);tar.addfile(info,io.BytesIO(data))
        archive=self.blob(path)
        package=SimpleNamespace(archive=archive,scope='isolated-test',signer='fixture-no-crypto-proof')
        plan={'schema':schema,'purpose':'zethropol-aur-build-root','architecture':'x86_64','expires':int(time.time())+600,
            'packages':[{'name':'test-root','version':'1-1','sha256':archive.sha256}]}
        manifest=self.base/'plan';manifest.write_bytes(canonical(plan))
        signed=SimpleNamespace(archive=self.blob(manifest),scope='isolated-test')
        return signed,package

    def test_root_extraction_rejects_escape_link_device_and_metadata_mismatch(self):
        for index,extra in enumerate((('../escape','file',b'x'),('etc','link','/tmp'),('device','device',''))):
            manifest,package=self.extraction_fixture([extra,('etc/escape','file',b'x')] if extra[0]=='etc' else [extra])
            with self.subTest(extra=extra), self.assertRaises(Rejected):
                provision(self.base/f'root-{index}',manifest,[package])
        manifest,package=self.extraction_fixture(metadata=b'pkgname = other\npkgver = 1-1\narch = any\n')
        with self.assertRaisesRegex(Rejected,'identity|mismatch'):
            provision(self.base/'wrong',manifest,[package])
        self.assertFalse((self.base/'escape').exists())

    def test_extraction_quotas_and_missing_package(self):
        manifest,package=self.extraction_fixture([('large','file',b'a'*4096)])
        with self.assertRaisesRegex(Rejected,'quota'):
            provision(self.base/'over',manifest,[package],max_bytes=1024)
        with self.assertRaisesRegex(Rejected,'Incomplete'):
            provision(self.base/'missing',manifest,[])
        self.assertFalse((self.base/'over/.provenance.json').exists())

    def test_ignored_metadata_also_consumes_expansion_budget(self):
        manifest,package=self.extraction_fixture([('.MTREE','file',b'x'*40000)],compressed=True)
        with self.assertRaisesRegex(Rejected,'expansion quota'):
            provision(self.base/'ignored-bomb',manifest,[package],max_bytes=35000)

    def test_extraction_budget_is_versioned_and_cannot_be_overridden_upwards(self):
        manifest,package=self.extraction_fixture()
        with self.assertRaisesRegex(Rejected,'signed root byte budget'):
            provision(self.base/'legacy-over',manifest,[package],max_bytes=2*1024**3)
        manifest,package=self.extraction_fixture(schema=2)
        root=provision(self.base/'v2',manifest,[package],max_bytes=2*1024**3,max_entries=65536)
        self.assertEqual(root.receipt['scope'],'isolated-test')
        with self.assertRaisesRegex(Rejected,'entry budget'):
            provision(self.base/'over',manifest,[package],max_entries=65537)

    def test_root_receipt_preserves_isolated_scope(self):
        manifest,package=self.extraction_fixture([('usr/bin/data','file',b'fixture')])
        root=provision(self.base/'root',manifest,[package])
        self.assertEqual(root.path.stat().st_mode & 0o777,0o755)
        self.assertEqual(root.scope,'isolated-test')
        self.assertEqual(root.receipt['scope'],'isolated-test')
        self.assertEqual((root.path/'usr/bin/data').read_bytes(),b'fixture')

    def test_v2_hardlink_is_a_bounded_copy_of_prior_same_package_file(self):
        manifest,package=self.extraction_fixture([('usr/bin/tool','file',b'payload'),
                                                  ('usr/bin/alias','hardlink','usr/bin/tool')],schema=2)
        root=provision(self.base/'aliases',manifest,[package])
        a=root.path/'usr/bin/tool';b=root.path/'usr/bin/alias'
        self.assertEqual(a.read_bytes(),b.read_bytes());self.assertNotEqual(a.stat().st_ino,b.stat().st_ino)

    def test_hardlink_copies_consume_logical_expansion_budget(self):
        manifest,package=self.extraction_fixture([('tool','file',b'x'*30000),('alias','hardlink','tool')],schema=2,compressed=True)
        with self.assertRaisesRegex(Rejected,'Hardlink expansion quota'):
            provision(self.base/'alias-bomb',manifest,[package],max_bytes=50000)

    def test_v2_public_root_is_readable_without_setid_or_write_bits(self):
        manifest,package=self.extraction_fixture([('usr/lib/helper','unreadable',b'public')],schema=2)
        root=provision(self.base/'public-root',manifest,[package])
        path=root.path/'usr/lib/helper'
        self.assertEqual(path.read_bytes(),b'public')
        self.assertEqual(path.stat().st_mode&0o7777,0o554)

    def test_hardlink_cannot_escape_forward_reference_or_alias_symlink(self):
        for index,target in enumerate(('../escape','/etc/passwd','missing','usr/bin/link')):
            manifest,package=self.extraction_fixture([('usr/bin/link','link','/etc/passwd'),
                                                      ('usr/bin/alias','hardlink',target)],schema=2)
            with self.subTest(target=target),self.assertRaisesRegex(Rejected,'Hardlink'):
                provision(self.base/f'bad-hard-{index}',manifest,[package])
        manifest,package=self.extraction_fixture([('tool','file',b'x'),('alias','hardlink','tool')])
        with self.assertRaisesRegex(Rejected,'Unsupported hardlink'):
            provision(self.base/'legacy-link',manifest,[package])

    def test_signed_package_metadata_populates_native_dependency_database(self):
        import pyalpm
        manifest, package = self.extraction_fixture([('var/lib/pacman', 'dir', b'')], metadata=(
            b'pkgname = test-root\npkgver = 1-1\narch = any\n'
            b'depend = libc>=2\nprovides = virtual-tool=3\n'))
        root = provision(self.base/'root', manifest, [package])
        handle = pyalpm.Handle(str(root.path), str(root.path/'var/lib/pacman'))
        cache = handle.get_localdb().pkgcache
        self.assertEqual(len(cache), 1)
        self.assertEqual(cache[0].depends, ['libc>=2'])
        self.assertEqual(pyalpm.find_satisfier(cache, 'virtual-tool>=3').name, 'test-root')
        self.assertIsNone(pyalpm.find_satisfier(cache, 'virtual-tool>=4'))

    def test_package_cannot_inject_database_or_redirect_metadata_writes(self):
        for index, extra in enumerate((('var/lib/pacman/local/forged/desc', 'file', b'x'),
                                       ('var/lib', 'link', '/tmp'))):
            manifest, package = self.extraction_fixture([extra])
            with self.subTest(extra=extra), self.assertRaisesRegex(Rejected, 'database'):
                provision(self.base/f'rejected-{index}', manifest, [package])
            self.assertFalse((self.base/f'rejected-{index}/.provenance.json').exists())

    def test_generated_database_respects_quotas_and_rejects_section_injection(self):
        record = {'pkgname': ['test-root'], 'pkgver': ['1-1'], 'depend': ['libc>=2']}
        for byte_limit, entry_limit in ((1, 100), (10000, 1)):
            with self.subTest(limits=(byte_limit, entry_limit)), self.assertRaisesRegex(Rejected, 'quota'):
                package_database(self.base, [record], byte_limit, entry_limit)
        record['depend'] = ['%PROVIDES%']
        with self.assertRaisesRegex(Rejected, 'Ambiguous'):
            package_database(self.base, [record], 10000, 100)
        self.assertFalse((self.base/'var/lib/pacman').exists())

    def test_expired_manifest_and_untrusted_production_policy(self):
        path=self.base/'expired';path.write_bytes(canonical({'schema':1,'purpose':'zethropol-aur-build-root','expires':1,'architecture':'x86_64','packages':[]}))
        with self.assertRaisesRegex(Rejected,'expired'):
            approved_manifest(SimpleNamespace(archive=self.blob(path)))
        policy=self.base/'policy.json';policy.write_text('{}')
        with self.assertRaises(Rejected): TrustPolicy.load(policy)

    def test_worker_log_aggregate_quota(self):
        with self.assertRaisesRegex(Rejected,'log quota'):
            Worker(self.base).execute([sys.executable,'-B','-c','import os; b=b"x"*65536\nfor i in range(160): os.write(1,b)'],
                self.base,lambda:False,10)
        self.assertLessEqual((self.base/'worker.log').stat().st_size,8*1024**2)

    def test_readonly_host_observation_never_certifies_final_transaction(self):
        database=self.base/'db';(database/'local').mkdir(parents=True);(database/'sync').mkdir()
        (database/'local/ALPM_DB_VERSION').write_text('9\n')
        completed=subprocess.CompletedProcess([],0,b'[options]\nArchitecture = x86_64\n',b'')
        with patch('host_view.subprocess.run',return_value=completed):
            result=capture_host_view(self.base/'view',database)
        self.assertFalse(result['final_host_transaction_verified'])
        self.assertEqual((database/'local/ALPM_DB_VERSION').read_text(),'9\n')
        (database/'db.lck').write_text('existing')
        with self.assertRaisesRegex(Rejected,'lock'):
            capture_host_view(self.base/'blocked',database)

    def test_workspace_released_when_durable_result_write_fails(self):
        class CheckedWorker(Worker):
            released=False
            def release_workspace(self): self.released=True
        worker=CheckedWorker(self.base)
        with patch('worker.os.replace',side_effect=OSError('disk unavailable')), self.assertRaises(OSError):
            worker.run({},'not-approved',self.base,self.base)
        self.assertTrue(worker.released)

    def test_client_authorization_and_boot_snapshot_claims_cannot_enable_install(self):
        with self.assertRaises(Rejected):
            InstallationBroker().install(authorized=True,root_verified=True,host_alpm_verified=True,
                snapshot='fake',boot='verified',kernel='verified',recovery='verified',archive_hash='a'*64)

if __name__=='__main__': unittest.main()
