"""Actual LAB native transactions; never production trust or host acceptance.

Every GPGME/libalpm domain runs in a fresh process. Only ephemeral PUBLIC keys
are imported into disposable native homes; private signing homes stay separate.
"""
import fcntl
import hashlib
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


class NativeDomainTransactions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.getuid() == 0:
            raise RuntimeError('Native domain LAB transactions require an unprivileged test user')
        cls.temporary = tempfile.TemporaryDirectory(prefix='zethropol-domain-lab-', dir='/tmp')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.base = Path(cls.temporary.name)
        cls.probe = cls.base/'native-domain-probe'
        subprocess.run(['/usr/bin/cc', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DZETHROPOL_LAB_ONLY=1',
                        str(Path(__file__).with_name('native_domain_transaction_probe.c')),
                        '-lalpm', '-o', str(cls.probe)], check=True, capture_output=True)
        cls.homes = {}
        cls.packages = {}
        for role in ('official', 'archive'):
            private = cls.base/('lab-signing-' + role)
            public = cls.base/('lab-native-' + role)
            private.mkdir(mode=0o700); public.mkdir(mode=0o700)
            cls.addClassCleanup(cls.kill_agent, private)
            cls.addClassCleanup(cls.kill_agent, public)
            cls.gpg(private, '--quick-generate-key', 'Ephemeral ' + role + ' LAB <lab@invalid>',
                    'ed25519', 'sign', '1d')
            listing = cls.gpg(private, '--with-colons', '--list-keys').stdout.decode()
            primary = next(line.split(':')[9] for line in listing.splitlines() if line.startswith('fpr:'))
            ring = cls.gpg(private, '--export', primary).stdout
            cls.gpg(public, '--import', input=ring)
            cls.gpg(public, '--import-ownertrust', input=(primary + ':6:\n').encode())
            cls.gpg(public, '--check-trustdb')
            cls.homes[role] = (private, public)
            cls.packages[role] = cls.package(role, 'lab-' + role)
        cls.packages['missing-dependency'] = cls.package('archive', 'lab-missing-dependency',
                                                        'depend = absent-lab-runtime>=1\n')
        cls.sequence = 0

    @staticmethod
    def kill_agent(home):
        subprocess.run(['/usr/bin/gpgconf', '--homedir', str(home), '--kill', 'all'],
                       capture_output=True, timeout=10, env={'PATH':'/usr/bin','LC_ALL':'C'})

    @staticmethod
    def gpg(home, *args, input=None):
        return subprocess.run(['/usr/bin/gpg', '--no-options', '--homedir', str(home),
                               '--batch', '--no-tty', '--pinentry-mode', 'loopback',
                               '--passphrase', '', *args], input=input, capture_output=True,
                              timeout=30, check=True, env={'PATH':'/usr/bin','LC_ALL':'C'})

    @classmethod
    def package(cls, role, name, extra=''):
        path = cls.base/(name + '.pkg.tar')
        payload = ('inert LAB payload for ' + name + '\n').encode()
        with tarfile.open(path, 'w') as archive:
            for filename, data in (('.PKGINFO', ('pkgname = ' + name + '\npkgver = 2-1\narch = any\n'
                                                'pkgdesc = Disposable LAB transaction only\nsize = 32\n' + extra).encode()),
                                   ('usr/share/zethropol-domain-lab/' + name, payload)):
                info = tarfile.TarInfo(filename); info.size = len(data); info.mode = 0o644
                archive.addfile(info, io.BytesIO(data))
        cls.gpg(cls.homes[role][0], '--detach-sign', str(path))
        return path, Path(str(path) + '.sig'), name, payload

    @staticmethod
    def sealed(data):
        fd = os.memfd_create('lab-native-artifact', os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
        try:
            with os.fdopen(os.dup(fd), 'wb') as output: output.write(data)
            fcntl.fcntl(fd, fcntl.F_ADD_SEALS,
                        fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
            return fd
        except BaseException:
            os.close(fd); raise

    @staticmethod
    def database_digest(db):
        return {str(p.relative_to(db)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in db.rglob('*') if p.is_file()}

    def transaction(self, domain, artifact, *, expected_name=None, version='2-1', arch='any',
                    mutate_archive=False, mutate_signature=False, success=False):
        cls = type(self); cls.sequence += 1
        case = cls.base/('case-' + str(cls.sequence)); case.mkdir(mode=0o700)
        root = case/'target-root'; root.mkdir(mode=0o700)
        db = case/'target-db'; db.mkdir(mode=0o700)
        (db/'local').mkdir(mode=0o700); (db/'local/ALPM_DB_VERSION').write_text('9\n')
        before = self.database_digest(db)
        path, signature, name, payload = cls.packages[artifact]
        raw = path.read_bytes(); sig = signature.read_bytes()
        package_hash = hashlib.sha256(raw).hexdigest()
        if mutate_archive: raw += b'changed after signing'
        if mutate_signature: sig = b'not a signature'
        fds = [self.sealed(raw), self.sealed(sig)]
        public = cls.homes[domain][1]
        trust_before = {p.name:hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in (public/'pubring.kbx', public/'trustdb.gpg')}
        try:
            # Hostile inherited settings must not change the child trust domain.
            result = subprocess.run([str(cls.probe), str(root), str(db), str(public),
                                     *(str(fd) for fd in fds), expected_name or name, version, arch],
                                    pass_fds=fds, capture_output=True, text=True, timeout=20,
                                    env={'PATH':'/usr/bin', 'GNUPGHOME':'/nonexistent-client-domain',
                                         'GPGME_DEBUG':'/nonexistent-client-log'})
        finally:
            for fd in fds: os.close(fd)
        self.assertEqual(package_hash, hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual(trust_before, {p.name:hashlib.sha256(p.read_bytes()).hexdigest()
                                       for p in (public/'pubring.kbx', public/'trustdb.gpg')})
        self.assertFalse((db/'db.lck').exists())
        deployed = root/'usr/share/zethropol-domain-lab'/name
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('LAB-COMMITTED ' + name + ' 2-1', result.stdout)
            self.assertEqual(deployed.read_bytes(), payload)
            self.assertIn('%VERSION%\n2-1', (db/'local'/(name + '-2-1')/'desc').read_text())
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(deployed.exists())
            self.assertEqual(before, self.database_digest(db))

    def test_each_domain_commits_its_own_artifact_and_rejects_cross_authority(self):
        for domain in ('official', 'archive'):
            for artifact in ('official', 'archive'):
                with self.subTest(domain=domain, artifact=artifact):
                    self.transaction(domain, artifact, success=domain == artifact)

    def test_changed_bytes_signature_or_reviewed_identity_cannot_commit(self):
        for changes in ({'mutate_archive':True}, {'mutate_signature':True},
                        {'expected_name':'unreviewed-name'}, {'version':'3-1'}, {'arch':'x86_64'}):
            with self.subTest(changes=changes): self.transaction('archive', 'archive', **changes)

    def test_missing_runtime_dependency_fails_native_prepare_without_mutation(self):
        self.transaction('archive', 'missing-dependency')
