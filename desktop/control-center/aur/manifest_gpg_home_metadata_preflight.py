"""Read only fixed GPG directory metadata; never enumerate or open key files.

Prepare for privileged execution as literal reviewed code, not a workspace
import. No approval, signing, agent contact, or filesystem mutation occurs.
"""
import json
import os
import stat
import time

EXPIRES = 1793565357
DIGEST = '4597d7e9243d516cf55718caa8aedaf9efc4901c351d94364508cc0ff3a2dc21'
ACL = {'system.posix_acl_access', 'system.posix_acl_default'}


def inspect():
    if os.getuid() != 0 or os.geteuid() != 0:
        raise ValueError('Read-only metadata inspection requires real/effective root')
    if time.time() >= EXPIRES:
        raise ValueError('Exact Manifest expired; this preflight cannot authorize reuse')
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    fd = os.open('/', flags)
    records = []
    try:
        for part, path in ((None, '/'), ('var', '/var'), ('lib', '/var/lib'),
                           ('zethropol-aur-release', '/var/lib/zethropol-aur-release'),
                           ('gnupg', '/var/lib/zethropol-aur-release/gnupg')):
            if part is not None:
                child = os.open(part, flags, dir_fd=fd)
                os.close(fd)
                fd = child
            info = os.fstat(fd)
            xattrs = os.listxattr(fd)
            expected_uid = 949 if path.endswith(('zethropol-aur-release', '/gnupg')) else 0
            if (not stat.S_ISDIR(info.st_mode) or info.st_uid != expected_uid or
                    info.st_mode & 0o022 or ACL.intersection(xattrs)):
                raise ValueError('Unsafe owner/mode/ACL at ' + path)
            if expected_uid == 949 and (info.st_gid != 948 or info.st_mode & 0o077):
                raise ValueError('Expected private UID949:GID948 directory at ' + path)
            records.append({'path': path, 'uid': info.st_uid, 'gid': info.st_gid,
                            'mode': oct(stat.S_IMODE(info.st_mode)),
                            'device': info.st_dev, 'inode': info.st_ino,
                            'extended_or_default_acl': False, 'symlink': False})
        if time.time() >= EXPIRES:
            raise ValueError('Manifest expired during preflight')
        return {'metadata_preflight': 'PASS', 'manifest_sha256': DIGEST,
                'checked_unix': int(time.time()), 'directories': records,
                'scope': 'metadata snapshot only; recheck immediately before authorized signing',
                'manifest_integrity_checked': False, 'approval_created': False,
                'signing_performed': False, 'private_key_or_agent_access': False}
    finally:
        os.close(fd)


if __name__ == '__main__':
    try:
        print(json.dumps(inspect(), indent=2))
    except (OSError, ValueError) as error:
        print(json.dumps({'metadata_preflight': 'BLOCKED', 'reason': str(error),
                          'approval_created': False, 'signing_performed': False}))
        raise SystemExit(1)
