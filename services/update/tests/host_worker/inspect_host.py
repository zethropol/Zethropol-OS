#!/usr/bin/python3
"""Read-only privileged host preflight. Never starts or changes a unit."""
import hashlib, json, os, pathlib, subprocess
assert os.geteuid() == 0
for command in [
    ['systemctl', 'show', 'zethropol-update.service', '-p', 'MainPID', '-p', 'ActiveState', '-p', 'SubState', '-p', 'NRestarts', '-p', 'FragmentPath', '-p', 'DropInPaths'],
    ['systemctl', 'cat', 'zethropol-update-worker@.service'],
    ['systemctl', 'show', 'zethropol-update-worker@00000000-0000-4000-8000-000000000000.service', '-p', 'LoadState', '-p', 'FragmentPath', '-p', 'DropInPaths', '-p', 'User', '-p', 'Group', '-p', 'ProtectSystem', '-p', 'ReadWritePaths', '-p', 'PrivateTmp', '-p', 'ProtectHome', '-p', 'NoNewPrivileges', '-p', 'ProtectKernelTunables', '-p', 'ProtectControlGroups', '-p', 'LockPersonality', '-p', 'RestrictAddressFamilies', '-p', 'KillMode', '-p', 'Restart', '-p', 'TimeoutStopUSec'],
    ['systemctl', 'list-units', '--all', '--plain', '--no-legend', 'zethropol-update-worker@*'],
    ['pacman', '-Q', 'zethropol-update'],
]:
    result = subprocess.run(command, capture_output=True, text=True)
    print(json.dumps({'read_command': command, 'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}), flush=True)
for name in ['/etc/systemd/system/zethropol-update.service', '/usr/lib/systemd/system/zethropol-update-worker@.service', '/usr/lib/zethropol/zethropol-update', '/boot/limine.conf']:
    print('SHA256', name, hashlib.sha256(pathlib.Path(name).read_bytes()).hexdigest(), flush=True)
for name in ['295', '296', '297']:
    base = pathlib.Path('/.snapshots') / name
    print('SNAPSHOT', name, 'metadata_exists', (base / 'info.xml').is_file(), 'subvolume_exists', (base / 'snapshot').is_dir(), flush=True)
p = pathlib.Path('/run/zethropol-update')
print('PRODUCTION_RUNTIME_EXISTS', p.is_dir(), flush=True)
print('EFI_EXISTS', pathlib.Path('/efi').exists(), flush=True)
