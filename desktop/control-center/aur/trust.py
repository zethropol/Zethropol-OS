"""Detached signatures over independently sealed bytes. No install authority."""
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import time

from root_limits import root_limits
from sealed import Blob
from worker import Rejected, canonical, digest


def approved_signers(values):
    # Check the original JSON array before deduplication: repeated entries must
    # not bypass the same 32-entry bound enforced by the native verifier.
    if (not isinstance(values, list) or not 1 <= len(values) <= 32 or
            any(not isinstance(f, str) or not re.fullmatch(r'(?:[A-F0-9]{40}|[A-F0-9]{64})', f)
                for f in values) or len(set(values)) != len(values)):
        raise Rejected('Explicit unique signer allowlist of at most 32 required')
    return frozenset(values)


@dataclass(frozen=True)
class TrustPolicy:
    keyring: Blob
    signers: frozenset
    deployment: str

    @classmethod
    def load(cls, policy_path, role=None):
        """Only a root-owned local policy may define production trust anchors.

        No client-supplied key, fingerprint or self-signed worker key is trusted.
        This is not an installed policy or an activation switch.
        """
        parent = Path(policy_path).absolute().parent
        for path in [parent, *parent.parents, Path(policy_path)]:
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
                raise Rejected('Production trust policy/ancestors must be root-owned and immutable')
        raw = Path(policy_path).read_bytes()
        if len(raw) > 65536:
            raise Rejected('Production trust policy too large')
        def unique(pairs):
            result = {}
            for key, value in pairs:
                if key in result: raise Rejected('Duplicate trust policy field')
                result[key] = value
            return result
        config = json.loads(raw, object_pairs_hook=unique)
        if role is not None:
            if role not in ('manifest', 'receipt', 'archive') or set(config) != {'schema','purpose','manifest','receipt','archive'} or config['schema'] != 1 or config['purpose'] != 'zethropol-aur-trust':
                raise Rejected('Explicit versioned project trust role required')
            config = config[role]
        if not isinstance(config, dict) or set(config) != {'keyring','keyring_sha256','signers'}:
            raise Rejected('Explicit keyring, digest and signers required')
        keypath = parent / config['keyring']
        if keypath.parent != parent or keypath.name != config['keyring']:
            raise Rejected('Keyring must be a policy-local basename')
        info = keypath.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise Rejected('Untrusted production keyring')
        fd = os.open(keypath, os.O_RDONLY | os.O_NOFOLLOW)
        try:
            blob = Blob(fd, 16 * 1024**2)
        finally:
            os.close(fd)
        try:
            if blob.sha256 != config['keyring_sha256']:
                raise Rejected('Keyring digest changed')
            signers = approved_signers(config['signers'])
            return cls(blob, signers, 'production-policy-only')
        except BaseException:
            blob.close()
            raise

    def verify(self, archive, signature):
        if self.deployment not in ('production-policy-only', 'isolated-test'):
            raise Rejected('Unknown trust policy scope')
        if not self.signers:
            raise Rejected('No approved signing keys')
        with tempfile.TemporaryDirectory(prefix='zethropol-gpgv-') as home:
            result = subprocess.run(['/usr/bin/gpgv', '--homedir', home, '--status-fd', '1',
                '--keyring', f'/proc/self/fd/{self.keyring.fd}', '--',
                f'/proc/self/fd/{signature.fd}', f'/proc/self/fd/{archive.fd}'],
                pass_fds=(self.keyring.fd, signature.fd, archive.fd),
                env={'PATH':'/usr/bin','LC_ALL':'C'}, capture_output=True, timeout=30)
        if result.returncode:
            raise Rejected('Detached signature verification failed')
        valid = []
        for line in result.stdout.decode('ascii',errors='strict').splitlines():
            fields = line.split()
            if fields[:2] == ['[GNUPG:]', 'VALIDSIG']:
                # Permit signing subkeys only when their primary key is pinned.
                valid.append(fields[11] if len(fields)>11 else fields[2])
            if len(fields)>1 and fields[1] in ('BADSIG','ERRSIG','EXPSIG','EXPKEYSIG','REVKEYSIG'):
                raise Rejected('Invalid/expired/revoked signing key')
        if len(valid)!=1 or valid[0] not in self.signers:
            raise Rejected('Signature signer is not explicitly approved')
        return valid[0]


class SignedArchive:
    """Both untrusted input FDs are copied before signature verification."""
    def __init__(self, archive_fd, signature_fd, policy, archive_limit=256*1024**2):
        self.archive = Blob(archive_fd, min(archive_limit, 256*1024**2))
        try:
            self.signature = Blob(signature_fd, 1024**2)
            try:
                self.signer = policy.verify(self.archive, self.signature)
            except BaseException:
                self.signature.close()
                raise
        except BaseException:
            self.archive.close()
            raise
        self.scope = policy.deployment

    def close(self):
        self.archive.close()
        self.signature.close()

    def __enter__(self): return self
    def __exit__(self, *args): self.close()


def approved_manifest(signed):
    data = os.pread(signed.archive.fd, signed.archive.size, 0)
    if len(data)>1024**2: raise Rejected('Build-root manifest too large')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise Rejected('Duplicate build-root manifest field')
            result[key] = value
        return result
    plan = json.loads(data, object_pairs_hook=unique)
    if not isinstance(plan, dict) or set(plan) != {'schema','purpose','architecture','expires','packages'}:
        raise Rejected('Exact signed build-root fields required')
    if plan.get('purpose')!='zethropol-aur-build-root':
        raise Rejected('Wrong signed manifest purpose/version')
    try:
        package_limit, _, _, _ = root_limits(plan.get('schema'))
    except ValueError as error:
        raise Rejected(str(error)) from error
    if type(plan.get('expires')) is not int or not time.time()<plan['expires']<=time.time()+86400:
        raise Rejected('Build-root manifest expired or lifetime exceeds one day')
    if plan.get('architecture') not in ('x86_64','aarch64') or not isinstance(plan.get('packages'), list) or not plan['packages'] or len(plan['packages'])>package_limit:
        raise Rejected('Missing build-root identity')
    seen = set()
    for package in plan['packages']:
        if set(package) != {'name','version','sha256'} or not re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9@._+-]{0,199}', package['name']):
            raise Rejected('Invalid build-root package identity')
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.+_:~\-]{0,199}', package['version']):
            raise Rejected('Invalid pinned package version')
        if package['name'] in seen or not re.fullmatch(r'[0-9a-f]{64}', package['sha256']):
            raise Rejected('Duplicate package or missing pinned SHA256')
        seen.add(package['name'])
    return plan, digest(canonical(plan))
