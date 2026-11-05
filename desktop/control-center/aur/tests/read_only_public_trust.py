"""Inspect PUBLIC distribution artifacts only; never grants AUR authority.

No refresh, key import, transaction, host keyring write or private-key read.
Armored public rings are dearmored into a disposable private directory for gpgv.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def audit():
    rings = [Path('/usr/share/pacman/keyrings') / name
             for name in ('archlinux.gpg', 'cachyos.gpg')]
    report = {
        'scope': 'read-only distribution signature evidence; not project AUR authorization',
        'keyrings': [{'path': str(r), 'sha256': hashlib.sha256(r.read_bytes()).hexdigest()}
                     for r in rings],
    }
    candidates = [p for p in Path('/var/cache/pacman/pkg').glob('*-any.pkg.tar.zst')
                  if Path(str(p) + '.sig').is_file()]
    if not candidates:
        report['missing'] = 'No cached any-architecture package with detached signature'
        return report
    package = min(candidates, key=lambda p: p.stat().st_size)
    with tempfile.TemporaryDirectory(prefix='aur-public-gpgv-') as home:
        args = ['/usr/bin/gpgv', '--homedir', home, '--status-fd', '1']
        for index, ring in enumerate(rings):
            binary = Path(home) / f'ring-{index}.gpg'
            subprocess.run(['/usr/bin/gpg', '--homedir', home, '--batch', '--dearmor',
                            '--output', str(binary), str(ring)],
                           capture_output=True, check=True, timeout=15)
            args += ['--keyring', str(binary)]
        result = subprocess.run(args + ['--', str(package) + '.sig', str(package)],
                                capture_output=True, text=True, timeout=30)
    report['sample'] = {
        'path': str(package),
        'sha256': hashlib.sha256(package.read_bytes()).hexdigest(),
        'signature_sha256': hashlib.sha256(Path(str(package) + '.sig').read_bytes()).hexdigest(),
        'gpgv_returncode': result.returncode,
        'status': result.stdout,
        'diagnostic': result.stderr,
    }
    return report


if __name__ == '__main__':
    print(json.dumps(audit(), indent=2))
