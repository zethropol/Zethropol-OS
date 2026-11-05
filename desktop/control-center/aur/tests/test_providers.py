"""Key-free provider rule tests. No real signer, unit, host transaction or key."""
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import fcntl
from contextlib import ExitStack, contextmanager
from types import SimpleNamespace
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import provider
from worker import Rejected


class ProviderRules(unittest.TestCase):
    def budget(self):
        from rebuild_acceptance import runtime_clock,NS
        wall,boot,identity=runtime_clock();start=boot-boot%NS
        return {'schema':1,'purpose':'zethropol-provider-reservation-budget','boot_id':identity,
                'issued_boottime_ns':start,'deadline_boottime_ns':start+300*NS,
                'captured_boottime_ns':boot,'captured_wall_ns':wall}

    def test_legacy_job_cannot_silently_gain_a_new_deadline(self):
        operation='11111111-1111-4111-8111-111111111111'
        value={'schema':1,'operation_id':operation,'purpose':'zethropol-untrusted-attestation-intake'}
        with patch('provider.document',return_value=value),patch('provider.sign') as signer:
            with self.assertRaisesRegex(Rejected,'broker job'):provider.job(operation)
            signer.assert_not_called()

    def test_expired_server_budget_stops_before_private_signing(self):
        from rebuild_acceptance import runtime_clock,NS,CaptureRejected
        wall,boot,identity=runtime_clock();deadline=boot-NS
        frame={'schema':1,'purpose':'zethropol-provider-reservation-budget','boot_id':identity,
               'issued_boottime_ns':deadline-300*NS,'deadline_boottime_ns':deadline,
               'captured_boottime_ns':deadline-NS,'captured_wall_ns':wall-2*NS}
        with patch('provider.actor'),patch('provider.job',return_value={'budget':frame}),patch('provider.sign') as signer:
            with self.assertRaises(CaptureRejected):provider.sign_build('11111111-1111-4111-8111-111111111111')
            signer.assert_not_called()
    def test_signing_input_is_sealed_exact_and_noninteractive(self):
        from sealed import SEALS
        from worker import digest
        fingerprint='A'*40;data=b'EXPLICIT NON-CRYPTO TEST DATA'
        def fake_gpg(args,**kwargs):
            self.assertIn('--no-options',args);self.assertIn('--no-autostart',args)
            self.assertEqual(args[args.index('--pinentry-mode')+1],'error')
            input_fd,output_fd=kwargs['pass_fds']
            self.assertEqual(fcntl.fcntl(input_fd,fcntl.F_GET_SEALS)&SEALS,SEALS)
            self.assertEqual(os.pread(input_fd,len(data),0),data)
            with self.assertRaises(OSError):os.write(input_fd,b'tamper')
            os.write(output_fd,b'NOT A REAL SIGNATURE')
            return SimpleNamespace(returncode=0)
        @contextmanager
        def noncrypto_fixture(input_fd,output_fd,policy):
            yield SimpleNamespace(archive=SimpleNamespace(sha256=digest(data)),signer=fingerprint,
                                  signature=SimpleNamespace(fd=output_fd,size=20))
        with ExitStack() as stack,patch('provider.actor',return_value=(fingerprint,Path('/fixture'))),\
                patch('provider.subprocess.run',side_effect=fake_gpg),patch('provider.SignedArchive',side_effect=noncrypto_fixture):
            signature=provider.sign(data,'archive',stack,{'archive':SimpleNamespace(signers={fingerprint})})
            self.assertTrue(signature.startswith(b'NOT A REAL'))

    def test_developer_signature_cannot_replace_independent_rebuild(self):
        with tempfile.TemporaryDirectory() as temporary:
            home=Path(temporary);(home/'work').mkdir(mode=0o700)
            value={'budget':self.budget(),'request':{'plan':{'identity':{'source_date_epoch':int(time.time())-30},'recipe':{'.':['dir',0o700]}}},
                   'archives':[{'filename':'demo.pkg.tar','sha256':'a'*64}]}
            result={'state':'built-untrusted','cleanup':'verified','archives':[{'path':'/fixture/demo.pkg.tar','sha256':'b'*64}]}
            signed=SimpleNamespace(archive=SimpleNamespace(sha256='a'*64),signature=SimpleNamespace(sha256='c'*64))
            for signum in (signal.SIGTERM,signal.SIGINT): self.addCleanup(signal.signal,signum,signal.getsignal(signum))
            with patch('provider.actor',return_value=('approved-public-id',home)),patch('provider.job',return_value=value),patch('provider.public_roles',return_value={'archive':object()}),patch('provider.signed',return_value=signed),patch('pipeline.run',return_value=result),patch('provider.sign') as signer:
                with self.assertRaisesRegex(Rejected,'Independent build differs'): provider.attest('11111111-1111-4111-8111-111111111111')
                signer.assert_not_called()
            self.assertFalse((home/'work/11111111-1111-4111-8111-111111111111/receipt.json').exists())

    def test_uncertain_cleanup_rejects_even_matching_hash(self):
        value={'archives':[{'filename':'demo.pkg.tar','sha256':'a'*64}]}
        result={'state':'built-untrusted','cleanup':'unknown','archives':[{'path':'/demo.pkg.tar','sha256':'a'*64}]}
        with self.assertRaisesRegex(Rejected,'cleanup failed'): provider.compare_rebuild(value,result)

    def test_unconfigured_template_cannot_select_a_signer(self):
        import json
        config=json.loads((Path(__file__).resolve().parents[1]/'release/providers/aur-providers.json.example').read_text())
        with patch('provider.document',return_value=config),patch('provider.pwd.getpwnam') as account:
            with self.assertRaisesRegex(Rejected,'Real public signing fingerprint'): provider.configuration()
            account.assert_not_called()

    def test_same_operating_account_is_not_independent(self):
        config={'schema':1,'purpose':'zethropol-aur-providers','archive':{'user':'zethropol-aur-archive','signer':'A'*40},'receipt':{'user':'zethropol-aur-attestor','signer':'B'*40}}
        with patch('provider.document',return_value=config),patch('provider.pwd.getpwnam',return_value=SimpleNamespace(pw_uid=1000)):
            with self.assertRaisesRegex(Rejected,'separate operating account'): provider.configuration()

    def test_provider_accounts_match_the_accepted_uid_boundary(self):
        config={'schema':1,'purpose':'zethropol-aur-providers','archive':{'user':'zethropol-aur-archive','signer':'A'*40},'receipt':{'user':'zethropol-aur-attestor','signer':'B'*40}}
        for archive,receipt in ((951,950),(949,950),(950,951),(1000,1001)):
            with self.subTest(archive=archive,receipt=receipt), patch('provider.document',return_value=config), patch('provider.pwd.getpwnam',side_effect=[SimpleNamespace(pw_uid=archive),SimpleNamespace(pw_uid=receipt)]):
                if (archive,receipt)==(951,950):
                    self.assertEqual(provider.configuration()[1],{'archive':951,'receipt':950})
                else:
                    with self.assertRaisesRegex(Rejected,'UID 951.*UID 950'): provider.configuration()

    def test_sysusers_declares_the_same_provider_uids(self):
        source=(Path(__file__).resolve().parents[1]/'release/providers/zethropol-aur-providers.sysusers').read_text()
        users={fields[1]:int(fields[2]) for line in source.splitlines() if (fields:=line.split()) and fields[0]=='u'}
        self.assertEqual(users,{provider.USERS[role]:uid for role,uid in provider.ROLE_UIDS.items()})

    def test_provision_never_runs_as_desktop_user(self):
        with patch('provider.os.getuid',return_value=1000),patch('provider.document') as load:
            with self.assertRaisesRegex(Rejected,'Root provisioning'): provider.provision('a'*64)
            load.assert_not_called()

    def test_epoch_is_bound_to_sandbox_environment(self):
        from sealed import FrozenTree
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);(root/'data').write_bytes(b'fixture')
            from worker import tree
            fd=os.open(root,os.O_RDONLY|os.O_DIRECTORY)
            try:
                with FrozenTree(root,tree(root)) as frozen:
                    command=frozen.command(fd,['/usr/bin/true'],1234567890)
                    index=command.index('SOURCE_DATE_EPOCH');self.assertEqual(command[index+1],'1234567890')
                    self.assertIn('--unshare-all',command);self.assertIn('--clearenv',command)
            finally: os.close(fd)

    def test_developer_key_is_not_a_signing_oracle_for_untrusted_intake(self):
        with tempfile.TemporaryDirectory() as temporary:
            home=Path(temporary);(home/'work').mkdir(mode=0o700)
            value={'budget':self.budget(),'request':{'plan':{'identity':{'source_date_epoch':int(time.time())-30},'recipe':{'.':['dir',0o700]}}},
                   'archives':[{'filename':'demo.pkg.tar','sha256':'a'*64}]}
            result={'state':'built-untrusted','cleanup':'verified','archives':[{'path':'/fixture/demo.pkg.tar','sha256':'b'*64}]}
            for signum in (signal.SIGTERM,signal.SIGINT): self.addCleanup(signal.signal,signum,signal.getsignal(signum))
            with patch('provider.actor',return_value=('approved-public-id',home)),patch('provider.job',return_value=value),patch('provider.public_roles',return_value={'archive':object()}),patch('pipeline.run',return_value=result),patch('provider.sign') as signer:
                with self.assertRaisesRegex(Rejected,'Independent build differs'): provider.sign_build('11111111-1111-4111-8111-111111111111')
                signer.assert_not_called()

    def test_cleanup_does_not_depend_on_revoked_key_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            with patch.dict(provider.HOMES,{'receipt':Path(temporary)}),patch('provider.pwd.getpwnam',return_value=SimpleNamespace(pw_uid=os.getuid())),patch('provider.configuration',side_effect=Rejected('revoked')) as config:
                provider.recover('11111111-1111-4111-8111-111111111111')
                config.assert_not_called()
