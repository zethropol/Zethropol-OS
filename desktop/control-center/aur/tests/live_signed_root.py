"""Real detached signatures and signed root provisioning using an ephemeral LAB key.

This proves mechanism, NOT trust in this test key for production.
"""
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
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from worker import Rejected, digest, canonical
from sealed import Blob
from trust import TrustPolicy, SignedArchive
from provision import provision
from live_build import copy_runtime, main as build


def main():
    with tempfile.TemporaryDirectory(prefix='zethropol-signed-root-') as temporary, ExitStack() as stack:
        base=Path(temporary); home=base/'gpg'; home.mkdir(mode=0o700)
        def gpg(*args):
            return subprocess.run(['/usr/bin/gpg','--homedir',str(home),'--batch','--no-tty',*args],
                env={'PATH':'/usr/bin','LC_ALL':'C'},capture_output=True,check=True,timeout=30)
        try:
            gpg('--pinentry-mode','loopback','--passphrase','','--quick-generate-key','Zethropol ephemeral isolated test <test@invalid>','ed25519','sign','1d')
            keys=gpg('--with-colons','--list-keys').stdout.decode()
            fingerprint=next(line.split(':')[9] for line in keys.splitlines() if line.startswith('fpr:'))
            ring=base/'keys.gpg'; ring.write_bytes(gpg('--export',fingerprint).stdout)
            def blob(path):
                fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW)
                try: return stack.enter_context(Blob(fd))
                finally: os.close(fd)
            keyring=blob(ring)
            policy=TrustPolicy(keyring,frozenset([fingerprint]),'isolated-test')
            def sign(path):
                target=Path(str(path)+'.sig')
                gpg('--pinentry-mode','loopback','--passphrase','','--local-user',fingerprint,'--detach-sign','--output',str(target),str(path))
                return target
            def receive(path,signature,trust=policy):
                fds=[os.open(p,os.O_RDONLY|os.O_NOFOLLOW) for p in (path,signature)]
                try: return stack.enter_context(SignedArchive(*fds,trust))
                finally:
                    for fd in fds: os.close(fd)
            source=base/'source'; source.mkdir(mode=0o700); copy_runtime(source)
            package=base/'fixture-root.pkg.tar'
            def root_payload(member):
                # The provisioner owns the local database and derives it from
                # signed .PKGINFO. Never ship copy_runtime's empty LAB database
                # as package payload or relax the production extraction guard.
                name = member.name.removeprefix('./').rstrip('/')
                if name == 'var/lib/pacman' or name.startswith('var/lib/pacman/'):
                    return None
                return member
            with tarfile.open(package,'w') as tar:
                info=b'pkgname = lab-build-root\npkgver = 1-1\narch = x86_64\n'
                entry=tarfile.TarInfo('.PKGINFO'); entry.size=len(info);tar.addfile(entry,io.BytesIO(info))
                tar.add(source,arcname='.',recursive=True,filter=root_payload)
            package_sig=sign(package)
            signed_package=receive(package,package_sig)
            manifest=base/'manifest.json'
            manifest.write_bytes(canonical({'schema':1,'purpose':'zethropol-aur-build-root','architecture':'x86_64',
                'expires':int(time.time())+1800,'packages':[{'name':'lab-build-root','version':'1-1','sha256':digest(package.read_bytes())}]}))
            manifest_sig=sign(manifest); signed_manifest=receive(manifest,manifest_sig)
            # Inputs can now be replaced: only independently sealed original
            # archive/signature bytes are used below.
            bad=base/'bad'; bad.write_bytes(b'modified')
            for archive,signature,trust in ((bad,package_sig,policy),(package,bad,policy),
                    (package,package_sig,TrustPolicy(keyring,frozenset(['A'*40]),'isolated-test'))):
                try: receive(archive,signature,trust)
                except Rejected: pass
                else: raise AssertionError('Modified/unapproved signature accepted')
            print('PASS real detached signature: archive/signature tampering and unapproved signer rejected',flush=True)
            # The root is sourced only from signed, manifest-pinned package
            # bytes here. The ephemeral signer is explicitly LAB-only.
            def setup(root):
                built=provision(root,signed_manifest,[signed_package])
                assert built.scope=='isolated-test'
                assert built.receipt['packages'][0]['signer']==fingerprint
                assert (root/'usr/bin/makepkg').is_file()
                print('PASS signed manifest + pinned signed root package + bounded extraction',flush=True)
            def check_archive(archive, sealed, staging):
                signed=receive(archive,sign(archive))
                root=staging/'signed-alpm';root.mkdir(mode=0o700)
                (root/'db/local').mkdir(parents=True,mode=0o700);(root/'db').chmod(0o700)
                (root/'db/local/ALPM_DB_VERSION').write_text('9\n')
                gpgdir=root/'gnupg';gpgdir.mkdir(mode=0o700)
                try:
                    def public_gpg(*args, **kwargs):
                        return subprocess.run(['/usr/bin/gpg','--homedir',str(gpgdir),'--batch','--no-tty',*args],
                            env={'PATH':'/usr/bin','LC_ALL':'C'},capture_output=True,check=True,timeout=30,**kwargs)
                    public_gpg('--import',str(ring))
                    public_gpg('--import-ownertrust',input=(fingerprint+':6:\n').encode())
                    executable=staging/'signed-native-prepare'
                    subprocess.run(['/usr/bin/cc','-O2','-Wall','-Wextra','-Werror',str(Path(__file__).resolve().parents[1]/'prepare_only.c'),'-lalpm','-o',str(executable)],check=True)
                    def prepare(sig):
                        return subprocess.run([str(executable),str(root),str(root/'db'),'x86_64','-',f'{signed.archive.fd}:{sig.fd}'],
                            pass_fds=(signed.archive.fd,sig.fd),capture_output=True,text=True,timeout=15)
                    valid=prepare(signed.signature)
                    assert valid.returncode==0,valid.stderr
                    invalid=prepare(blob(bad))
                    assert invalid.returncode!=0 and 'signature' in invalid.stderr,invalid.stderr
                    print('PASS native ALPM REQUIRED signatures + dependencies; bad detached signature rejected; no commit',flush=True)
                finally:
                    subprocess.run(['/usr/bin/gpgconf','--homedir',str(gpgdir),'--kill','all'],capture_output=True,timeout=10)
            build(root_setup=setup,archive_check=check_archive)
        finally:
            subprocess.run(['/usr/bin/gpgconf','--homedir',str(home),'--kill','all'],
                env={'PATH':'/usr/bin','LC_ALL':'C'},capture_output=True,timeout=10)

if __name__=='__main__': main()
