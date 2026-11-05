"""Prepare public distribution candidates; never modifies native trust or installs.

Repository resolution is an observation, not a locked libalpm transaction. Only
cached exact repository bytes with valid signatures and existing native full
trust contribute signers. Missing artifacts remain explicit blockers.
"""
import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def sha(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(args):
    return subprocess.run(args, capture_output=True, timeout=120,
                          env={'PATH': '/usr/bin', 'LC_ALL': 'C'})


def primary(status, require_trust=False):
    """Fail closed on expired/revoked/multiple signatures and marginal trust."""
    rows = [line.split()[1:] for line in status.splitlines()
            if line.startswith('[GNUPG:] ')]
    bad = {'BADSIG', 'ERRSIG', 'EXPSIG', 'EXPKEYSIG', 'REVKEYSIG',
           'KEYREVOKED', 'KEYEXPIRED', 'SIGEXPIRED', 'NO_PUBKEY',
           'TRUST_NEVER', 'TRUST_UNDEFINED', 'TRUST_MARGINAL'}
    if any(row and row[0] in bad for row in rows):
        raise ValueError('Invalid signature or insufficient native trust')
    valid = [row[10] if len(row) > 10 else row[1]
             for row in rows if row and row[0] == 'VALIDSIG']
    if len(valid) != 1 or not re.fullmatch(r'(?:[A-F0-9]{40}|[A-F0-9]{64})', valid[0]):
        raise ValueError('Exactly one primary signature required')
    if require_trust and not any(row and row[0] in ('TRUST_FULLY', 'TRUST_ULTIMATE') for row in rows):
        raise ValueError('Existing native full trust required')
    return valid[0]


def allowlist(signers):
    values = sorted(set(signers))
    if not values or len(values) > 32:
        raise ValueError('Distribution policy requires 1..32 actual primary signers')
    if any(not re.fullmatch(r'(?:[A-F0-9]{40}|[A-F0-9]{64})', f) for f in values):
        raise ValueError('Full uppercase primary fingerprints required')
    return values


def resolve(seeds, repositories):
    import pyalpm
    handle = pyalpm.Handle('/', '/var/lib/pacman')
    pool = []
    for name in repositories:
        pool.extend(handle.register_syncdb(name, 0).pkgcache)
    queue = deque(seeds)
    selected, unresolved = {}, []
    while queue:
        dependency = queue.popleft()
        # Prefer an already selected satisfier, avoiding inconsistent providers.
        package = pyalpm.find_satisfier(list(selected.values()), dependency)
        if package:
            continue
        package = pyalpm.find_satisfier(pool, dependency)
        if not package:
            unresolved.append(dependency)
            continue
        if package.name in selected:
            unresolved.append(dependency)
            continue
        selected[package.name] = package
        queue.extend(package.depends)
    return handle, sorted(selected.values(), key=lambda p: p.name), unresolved


def prepare(output, seeds, extra_cache=None, repositories=None):
    output = Path(output)
    config = run(['pacman-conf'])
    if config.returncode:
        raise ValueError('Cannot read effective pacman policy')
    native_policy = run(['pacman-conf', 'SigLevel']).stdout.decode().splitlines()
    if 'PackageRequired' not in native_policy or 'PackageTrustedOnly' not in native_policy:
        raise ValueError('Required/TrustedOnly native policy required')
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    configured = run(['pacman-conf', '--repo-list']).stdout.decode().split()
    repositories = configured if repositories is None else repositories
    if not repositories or len(set(repositories)) != len(repositories) or any(r not in configured for r in repositories):
        raise ValueError('Only explicitly configured distribution repositories may be selected')
    handle, packages, unresolved = resolve(seeds, repositories)
    rings = [Path('/usr/share/pacman/keyrings') / (name + '.gpg') for name in ('archlinux', 'cachyos')]
    native = Path(run(['pacman-conf', 'GPGDir']).stdout.decode().strip().removeprefix('GPGDir = '))
    inputs = rings + [native / 'pubring.kbx', native / 'trustdb.gpg']
    inputs += [Path('/usr/share/pacman/keyrings') / (name + suffix)
               for name in ('archlinux', 'cachyos') for suffix in ('-trusted', '-revoked')]
    inputs += [Path('/var/lib/pacman/sync') / (name + '.db') for name in repositories]
    before = {str(p): sha(p) for p in inputs}
    report = {'schema': 1, 'scope': 'uninstalled distribution review candidate',
              'seeds': seeds, 'repositories': repositories, 'native_siglevel': native_policy,
              'effective_config_sha256': hashlib.sha256(config.stdout).hexdigest(),
              'inputs_sha256': before, 'unresolved': unresolved, 'packages': [],
              'production_authorized': False, 'locked_native_resolution': False}
    revoked = set()
    for name in ('archlinux', 'cachyos'):
        revoked.update(line.split(':')[0].strip().upper() for line in
                       Path('/usr/share/pacman/keyrings', name + '-revoked').read_text().splitlines()
                       if line.strip() and not line.startswith('#'))
    with tempfile.TemporaryDirectory(prefix='aur-distribution-public-') as temporary:
        home = Path(temporary)
        public = home / 'public'; public.mkdir(mode=0o700)
        trusted = home / 'native'; trusted.mkdir(mode=0o700)
        # Copy only named PUBLIC material. Never use host GPG homes or agents.
        for name in ('pubring.kbx', 'trustdb.gpg'):
            shutil.copyfile(native / name, trusted / name)
        gpg = ['gpg', '--no-options', '--homedir', str(public), '--batch', '--no-autostart']
        for ring in rings:
            result = run(gpg + ['--import', str(ring)])
            if result.returncode:
                raise ValueError('Cannot import packaged PUBLIC keys into disposable verifier')
        full = home / 'all.gpg'
        full.write_bytes(run(gpg + ['--export']).stdout)
        signers = set()
        for package in packages:
            row = {'name': package.name, 'version': package.version, 'repository': package.db.name,
                   'filename': package.filename, 'dependencies': package.depends}
            report['packages'].append(row)
            path = Path('/var/cache/pacman/pkg') / package.filename
            if not path.is_file() and extra_cache is not None:
                path = Path(extra_cache) / package.filename
            signature = Path(str(path) + '.sig')
            if not path.is_file() or not signature.is_file():
                row['status'] = 'missing-exact-cached-archive-or-signature'
                continue
            row['sha256'] = sha(path); row['signature_sha256'] = sha(signature)
            if package.sha256sum and row['sha256'] != package.sha256sum:
                row['status'] = 'repository-hash-mismatch'; continue
            verified = run(['gpgv', '--homedir', str(public), '--status-fd', '1',
                            '--keyring', str(full), '--', str(signature), str(path)])
            trusted_result = run(['gpg', '--no-options', '--homedir', str(trusted), '--batch',
                                  '--no-autostart', '--no-auto-check-trustdb', '--no-auto-key-retrieve',
                                  '--trust-model', 'pgp', '--status-fd', '1', '--verify', str(signature), str(path)])
            try:
                if verified.returncode or trusted_result.returncode:
                    raise ValueError('Public/native signature verification failed')
                signer = primary(verified.stdout.decode())
                if signer != primary(trusted_result.stdout.decode(), require_trust=True) or signer in revoked:
                    raise ValueError('Native signer mismatch or distribution revocation')
            except ValueError as error:
                row['status'] = str(error)
                row['public_status'] = verified.stdout.decode()
                row['native_status'] = trusted_result.stdout.decode()
                continue
            row.update(status='verified-public-and-native-full-trust', signer=signer,
                       public_status=verified.stdout.decode(), native_status=trusted_result.stdout.decode())
            signers.add(signer)
        approved = allowlist(signers)
        exported = run(gpg + ['--export', *approved])
        if exported.returncode or not exported.stdout:
            raise ValueError('Public signer export failed')
        (output / 'distribution.gpg').write_bytes(exported.stdout)
        policy = {'keyring': 'distribution.gpg', 'keyring_sha256': sha(output / 'distribution.gpg'), 'signers': approved}
        (output / 'distribution.json').write_text(json.dumps(policy, indent=2) + '\n')
    if before != {str(p): sha(p) for p in inputs} or config.stdout != run(['pacman-conf']).stdout:
        raise ValueError('Host public inputs changed during audit; discard candidate')
    report['signers'] = approved
    report['package_count'] = len(packages)
    report['within_legacy_root_package_limit'] = len(packages) <= 64
    report['within_v2_root_package_limit'] = len(packages) <= 256
    report['complete_verified_closure'] = not unresolved and all(p['status'] == 'verified-public-and-native-full-trust' for p in report['packages'])
    report['policy_sha256'] = sha(output / 'distribution.json')
    (output / 'audit.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True)
    parser.add_argument('--extra-cache', help='Additional downloaded public artifacts; host cache is never written')
    parser.add_argument('--repositories', nargs='+', help='Explicit subset of configured repositories; separate seeds with --')
    parser.add_argument('seeds', nargs='+')
    args = parser.parse_args()
    result = prepare(args.output, args.seeds, args.extra_cache, args.repositories)
    print(json.dumps({k: result[k] for k in ('package_count', 'signers', 'complete_verified_closure', 'within_v2_root_package_limit')}, indent=2))
