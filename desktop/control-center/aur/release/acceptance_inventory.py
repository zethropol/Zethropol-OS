"""Passive acceptance infrastructure inventory; never probes a device or guest.

No subprocess, daemon connection, image creation, key import or signing. This
source-only operator tool never grants acceptance, even with complete metadata.
"""
import itertools
import json
import os
from pathlib import Path
import shutil
import stat
import sys

TOOLS = ('qemu-system-x86_64', 'qemu-system-aarch64', 'qemu-img', 'virsh',
         'virt-install', 'VBoxManage', 'cloud-localds', 'xorriso',
         'systemd-nspawn', 'bwrap')
PUBLIC_INPUTS = '/var/lib/zethropol/aur-provider-inputs'
NATIVE_DOMAIN = '/etc/zethropol-update/aur-native-trust'


def directory_inventory(path):
    """Bounded directory metadata, not verification or approval of its contents."""
    try:
        info = Path(path).lstat()
        if not stat.S_ISDIR(info.st_mode):
            return {'state': 'rejected', 'reason': 'real directory required; symlinks refused'}
        with os.scandir(path) as entries:
            count = sum(1 for _ in itertools.islice(entries, 257))
        return {'state': 'observed', 'uid': info.st_uid,
                'mode': oct(stat.S_IMODE(info.st_mode)),
                'entry_count': min(count, 256), 'truncated': count > 256,
                'contents_verified': False}
    except FileNotFoundError:
        return {'state': 'missing'}
    except PermissionError:
        return {'state': 'inaccessible', 'contents_verified': False}
    except OSError as error:
        return {'state': 'unknown', 'errno': error.errno}


def inspect():
    try:
        info = Path('/dev/kvm').lstat()
        kvm = {'state': 'observed', 'character_device': stat.S_ISCHR(info.st_mode),
               'readable': os.access('/dev/kvm', os.R_OK),
               'writable': os.access('/dev/kvm', os.W_OK)}
    except FileNotFoundError:
        kvm = {'state': 'missing-in-this-namespace'}
    except OSError as error:
        kvm = {'state': 'unknown', 'errno': error.errno}
    kvm.update({'opened': False, 'acceleration_tested': False})
    return {
        'schema': 1,
        'scope': 'passive metadata in the current execution namespace only',
        'installation': 'closed', 'production_acceptance': 'blocked',
        'guest_verified': False, 'public_artifacts_verified': False,
        'tools': {name: shutil.which(name, path='/usr/bin') for name in TOOLS},
        'kvm': kvm,
        'directories': {path: directory_inventory(path) for path in (
            PUBLIC_INPUTS, NATIVE_DOMAIN, '/var/lib/libvirt/images', '/var/lib/machines')},
        'required_external_inputs': [
            'owner-approved current public manifest/signatures/exact artifacts and role pins',
            'separately approved public Archive native keybox/trustdb/config',
            'approved VM runner, disposable image SHA256/identity/layout and guest-only authority scope',
        ],
        'operations_performed': {'device_open': False, 'daemon_connection': False,
                                 'guest_boot': False, 'trust_change': False,
                                 'package_install': False},
    }


if __name__ == '__main__':
    if len(sys.argv) != 1:
        raise SystemExit('Passive acceptance inventory takes no arguments')
    print(json.dumps(inspect(), indent=2, sort_keys=True))
    raise SystemExit(2)  # Observation never establishes acceptance.
