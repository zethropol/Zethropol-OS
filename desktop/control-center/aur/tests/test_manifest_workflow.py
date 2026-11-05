"""No real signing, key creation, publication, ownership bypass or installation."""
import os
from pathlib import Path
import sys
import tempfile
import time
import stat
from contextlib import ExitStack
from types import SimpleNamespace
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import manifest_workflow as flow
from worker import canonical, digest, Rejected


class ManifestWorkflow(unittest.TestCase):
    def owner_config(self):
        return {'schema':2,'purpose':'zethropol-manifest-signer','identity_model':'existing-key-owner',
                'user':'zethropol-aur-release','uid':949,'gnupg_home':flow.OWNER_GPG_HOME,'signer':'A'*40}

    def test_owner_model_is_exact_and_legacy_is_not_silently_reinterpreted(self):
        config=self.owner_config()
        self.assertEqual(flow.signer_model(config),('zethropol-aur-release',949,flow.OWNER_HOME,flow.OWNER_GPG_HOME))
        bad=[dict(config,uid=v) for v in (0,950,951,1000,True,'949')]
        bad += [dict(config,user='zethropol-aur-archive'),dict(config,gnupg_home='/home/owner/.gnupg'),
                dict(config,identity_model='auto'),dict(config,schema=True),dict(config,schema=1),dict(config,command='gpg')]
        for item in bad:
            with self.assertRaises(Rejected):flow.signer_model(item)
        legacy={'schema':1,'purpose':'zethropol-manifest-signer','user':flow.SIGNER_USER,
                'gnupg_home':flow.SIGNER_GPG_HOME,'signer':'A'*40}
        self.assertEqual(flow.signer_model(legacy)[0],flow.SIGNER_USER)
        with self.assertRaises(Rejected):flow.signer_model(dict(legacy,user=flow.OWNER_USER,gnupg_home=flow.OWNER_GPG_HOME))

    def test_owner_nss_identity_and_role_separation_are_pinned(self):
        account=SimpleNamespace(pw_uid=949,pw_dir=flow.OWNER_HOME,pw_shell='/usr/bin/nologin')
        flow.owner_identity(self.owner_config(),account,{950,951})
        for fields in ({'pw_uid':0},{'pw_uid':950},{'pw_uid':1000},{'pw_dir':'/tmp'},{'pw_shell':'/bin/bash'}):
            with self.assertRaises(Rejected):flow.owner_identity(self.owner_config(),SimpleNamespace(**dict(vars(account),**fields)),{950,951})
        with self.assertRaises(Rejected):flow.owner_identity(self.owner_config(),account,{949,951})

    def context_fixture(self,actor):
        sha='a'*64;config=self.owner_config()
        approval={'schema':1,'purpose':'zethropol-approved-root-manifest','manifest_sha256':sha,'expires':int(time.time())+600}
        account=SimpleNamespace(pw_uid=949,pw_dir=flow.OWNER_HOME,pw_shell='/usr/bin/nologin')
        def document(path):return config if path==flow.CONFIG else approval
        def lookup(user):return account if user==flow.OWNER_USER else SimpleNamespace(pw_uid=950 if user.endswith('attestor') else 951)
        metadata=SimpleNamespace(st_uid=949,st_mode=stat.S_IFDIR|0o700)
        with patch('manifest_workflow.document',side_effect=document),patch('manifest_workflow.pwd.getpwnam',side_effect=lookup),\
             patch('pathlib.Path.lstat',return_value=metadata),patch('manifest_workflow.root_path'):
            return flow.context(sha,actor=actor)

    def test_only_uid_949_with_matching_effective_uid_may_sign(self):
        for uid,euid in ((0,0),(950,950),(951,951),(1000,1000),(949,0),(949,1000)):
            with patch('manifest_workflow.os.getuid',return_value=uid),patch('manifest_workflow.os.geteuid',return_value=euid),\
                 patch('manifest_workflow.private_home') as home,patch('manifest_workflow.subprocess.run') as gpg:
                with self.assertRaises(Rejected):self.context_fixture(True)
                home.assert_not_called();gpg.assert_not_called()
        with patch('manifest_workflow.os.getuid',return_value=949),patch('manifest_workflow.os.geteuid',return_value=949),\
             patch('manifest_workflow.private_home') as home:
            self.context_fixture(True)
            home.assert_called_once_with(Path(flow.OWNER_GPG_HOME),949)

    def test_publisher_context_does_not_inspect_or_contact_private_home(self):
        with patch('manifest_workflow.private_home',side_effect=AssertionError('private home touched')),\
             patch('manifest_workflow.sign',side_effect=AssertionError('sign invoked')),\
             patch('manifest_workflow.subprocess.run') as gpg:
            config,_,_=self.context_fixture(False)
            self.assertEqual(config['uid'],949)
            gpg.assert_not_called()

    def test_gpg_home_and_ancestors_reject_symlinks_shared_permissions_and_wrong_owner(self):
        home=Path(flow.OWNER_GPG_HOME)
        def metadata(path):
            return SimpleNamespace(st_uid=949 if path==home else 0,
                                   st_mode=stat.S_IFDIR|(0o700 if path==home else 0o755))
        with patch('pathlib.Path.lstat',autospec=True,side_effect=metadata):flow.private_home(home,949)
        for target,uid,mode in ((home,950,stat.S_IFDIR|0o700),(home,949,stat.S_IFDIR|0o750),
                                (home,949,stat.S_IFLNK|0o777),(home.parent,0,stat.S_IFDIR|0o777)):
            def changed(path):return SimpleNamespace(st_uid=uid,st_mode=mode) if path==target else metadata(path)
            with patch('pathlib.Path.lstat',autospec=True,side_effect=changed),self.assertRaises(Rejected):flow.private_home(home,949)

    def test_dedicated_identity_cannot_alias_builder_or_use_login_shell(self):
        config={'user':flow.SIGNER_USER,'gnupg_home':flow.SIGNER_GPG_HOME}
        account=SimpleNamespace(pw_uid=952,pw_dir=flow.SIGNER_HOME,pw_shell='/usr/bin/nologin')
        flow.dedicated_identity(config,account,{950,951}) # Fixture UID only; never provisioned.
        for fields in ({'pw_uid':0},{'pw_uid':950},{'pw_uid':951},
                       {'pw_dir':'/home/owner'},{'pw_shell':'/bin/bash'}):
            bad=SimpleNamespace(**dict(vars(account),**fields))
            with self.assertRaises(Rejected):flow.dedicated_identity(config,bad,{950,951})
        for fields in ({'user':'zethropol-aur-archive'},{'gnupg_home':'/home/owner/.gnupg'}):
            with self.assertRaises(Rejected):flow.dedicated_identity(dict(config,**fields),account,{950,951})

    def test_manifest_and_approval_expire_during_operation(self):
        with tempfile.TemporaryDirectory() as d,ExitStack() as stack:
            path=Path(d)/'manifest.json'
            plan={'schema':2,'purpose':'zethropol-aur-build-root','architecture':'x86_64','expires':1200,
                  'packages':[{'name':'fixture','version':'1-1','sha256':'a'*64}]}
            path.write_bytes(canonical(plan));blob=flow.manifest_blob(path,stack)
            approval={'schema':1,'purpose':'zethropol-approved-root-manifest',
                      'manifest_sha256':blob.sha256,'expires':1150}
            with patch('trust.time.time',return_value=1100):flow.validate_bytes(blob,blob.sha256)
            flow.approval_valid(approval,blob.sha256,1100)
            with patch('manifest_workflow.time.time',return_value=1100):
                flow.validate_authorization(blob,blob.sha256,approval)
                with self.assertRaisesRegex(Rejected,'outlive'):
                    flow.validate_authorization(blob,blob.sha256,dict(approval,expires=1201))
            with self.assertRaises(Rejected):flow.approval_valid(approval,blob.sha256,1150)
            with patch('trust.time.time',return_value=1200),self.assertRaises(Rejected):flow.validate_bytes(blob,blob.sha256)

    def test_approval_is_exact_fresh_and_hash_bound(self):
        now=1000;sha='a'*64
        approval={'schema':1,'purpose':'zethropol-approved-root-manifest','manifest_sha256':sha,'expires':1100}
        flow.approval_valid(approval,sha,now)
        for bad in (dict(approval,schema=True),dict(approval,expires=1000),dict(approval,expires=90000),
                    dict(approval,manifest_sha256='b'*64),dict(approval,purpose='source-review'),
                    dict(approval,approved=True)):
            with self.assertRaises(Rejected):flow.approval_valid(bad,sha,now)

    def test_changed_or_noncanonical_manifest_never_reaches_signer(self):
        with tempfile.TemporaryDirectory() as d,ExitStack() as stack:
            path=Path(d)/'manifest.json'
            plan={'schema':2,'purpose':'zethropol-aur-build-root','architecture':'x86_64',
                  'expires':int(time.time())+600,'packages':[{'name':'fixture','version':'1-1','sha256':'a'*64}]}
            path.write_bytes(canonical(plan))
            blob=flow.manifest_blob(path,stack)
            self.assertEqual(flow.validate_bytes(blob,blob.sha256),plan)
            with self.assertRaises(Rejected):flow.validate_bytes(blob,'0'*64)
            path.write_bytes(b' '+canonical(plan))
            blob=flow.manifest_blob(path,stack)
            with self.assertRaisesRegex(Rejected,'Canonical'):flow.validate_bytes(blob,blob.sha256)

    def test_unapproved_primary_cannot_call_gpg(self):
        with patch('manifest_workflow.context',return_value=({'signer':'B'*40},Path('/unused'),{})),\
             patch('manifest_workflow.TrustPolicy.load',return_value=SimpleNamespace(signers={'A'*40},keyring=SimpleNamespace(close=lambda:None))),\
             patch('manifest_workflow.subprocess.run') as gpg:
            with self.assertRaises(flow.SigningPhaseRejected) as caught:flow.sign('a'*64)
            self.assertEqual(caught.exception.phase,'policy-load')
            gpg.assert_not_called()

    def test_missing_root_owned_approval_never_calls_gpg(self):
        with patch('manifest_workflow.document',side_effect=Rejected('Missing owner approval')),patch('manifest_workflow.subprocess.run') as gpg:
            with self.assertRaises(Rejected):flow.sign('a'*64)
            gpg.assert_not_called()

    def test_unprivileged_release_publisher_never_reads_private_inputs(self):
        with patch('manifest_workflow.os.getuid',return_value=1000),patch('manifest_workflow.context') as context:
            with self.assertRaises(Rejected):flow.publish('a'*64)
            context.assert_not_called()


if __name__=='__main__':unittest.main()
