"""Passive VM prerequisites only. No daemon connection, VM/device open or install."""
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess


def inventory():
    names = ['qemu-system-x86_64', 'qemu-img', 'virsh', 'virt-install',
             'virt-host-validate', 'VBoxManage', 'cloud-localds', 'xorriso',
             'genisoimage', 'systemd-nspawn', 'bwrap', 'gpgv', 'timeout']
    report = {
        'recorded_at': datetime.now(timezone.utc).isoformat(),
        'scope': 'read-only inventory; no daemon connection, VM start, package operation, mount or device ioctl',
        'tools': {name: shutil.which(name) for name in names},
        'namespace': {},
    }
    for name in ['qemu-system-x86_64', 'qemu-img', 'virsh', 'virt-install']:
        path = report['tools'][name]
        if path:
            result = subprocess.run([path, '--version'], capture_output=True,
                                    text=True, timeout=10, stdin=subprocess.DEVNULL)
            report['tools'][name] = {'path': path, 'version': (result.stdout + result.stderr)[:2048],
                                     'exit_code': result.returncode}
    for key, path in [('self_mount', '/proc/self/ns/mnt'), ('pid1_mount', '/proc/1/ns/mnt')]:
        try:
            report['namespace'][key] = os.readlink(path)
        except OSError as error:
            report['namespace'][key] = str(error)
    visible = all(report['namespace'][key].startswith('mnt:[') for key in ('self_mount', 'pid1_mount'))
    report['namespace']['same_mount_as_pid1'] = (
        report['namespace']['self_mount'] == report['namespace']['pid1_mount'] if visible else None)
    device = Path('/dev/kvm')
    try:
        info = device.stat()
        report['kvm'] = {'exists': True, 'uid': info.st_uid, 'gid': info.st_gid,
                         'mode': oct(info.st_mode & 0o777), 'readable': os.access(device, os.R_OK),
                         'writable': os.access(device, os.W_OK),
                         'note': 'passive access checks only; acceleration NOT exercised'}
    except OSError as error:
        report['kvm'] = {'exists': False, 'reason': str(error), 'note': 'no device access attempted'}
    cpu = Path('/proc/cpuinfo').read_text()
    report['cpu_virtualization_flags'] = sorted(flag for flag in ('vmx', 'svm') if flag in cpu.split())
    report['kvm_module_visible'] = Path('/sys/module/kvm').exists()
    report['workspace_free_bytes'] = shutil.disk_usage('.').free
    report['images'] = {'provided': False, 'note': 'No image downloaded, created or searched; no authorized image/artifact set supplied'}
    return report


if __name__ == '__main__':
    print(json.dumps(inventory(), indent=2))
