"""Read-only deployment checks. No installation, key access or agent contact."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat


def file_hash(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise ValueError('Regular single-link file required')
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def metadata(path, uid, gid, mode, directory):
    info = path.lstat()
    kind = stat.S_ISDIR if directory else stat.S_ISREG
    if not kind(info.st_mode) or (info.st_uid, info.st_gid) != (uid, gid) or stat.S_IMODE(info.st_mode) != mode:
        raise ValueError('Type/owner/mode conflict: ' + str(path))
    # Linux exposes extended access/default ACLs as xattrs. Unreadable metadata
    # fails closed; no getfacl subprocess or private-directory traversal.
    if any(name in ('system.posix_acl_access', 'system.posix_acl_default')
           for name in os.listxattr(path, follow_symlinks=False)):
        raise ValueError('Extended/default ACL: ' + str(path))


def ancestors(path):
    for parent in reversed(path.parents):
        try:
            info = parent.lstat()
        except FileNotFoundError:
            continue
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_gid != 0 or info.st_mode & 0o022:
            raise ValueError('Unsafe ancestor: ' + str(parent))
        if any(n in ('system.posix_acl_access', 'system.posix_acl_default')
               for n in os.listxattr(parent, follow_symlinks=False)):
            raise ValueError('Ancestor ACL: ' + str(parent))


def target(row, directory=False):
    path = Path(row['path' if directory else 'target'])
    if not path.is_absolute() or '..' in path.parts:
        raise ValueError('Absolute canonical target required')
    ancestors(path)
    try:
        metadata(path, row.get('uid', 0), row.get('gid', 0), int(row['mode'], 8), directory)
    except FileNotFoundError:
        return 'create'
    if not directory and file_hash(path) != row['sha256']:
        raise ValueError('Installed hash conflict: ' + str(path))
    return 'reuse'


def inspect(plan, check_targets=False):
    report = {'applied': False, 'signing_authorized': False, 'sources': [], 'targets': []}
    for row in plan['files']:
        try:
            actual = file_hash(row['source'])
            report['sources'].append({'source': row['source'], 'sha256': actual, 'ok': actual == row['sha256']})
        except (OSError, ValueError) as error:
            report['sources'].append({'source': row['source'], 'ok': False, 'error': str(error)})
    if check_targets:
        for directory, rows in ((True, plan['directories']), (False, plan['files'])):
            for row in rows:
                name = row['path' if directory else 'target']
                try:
                    report['targets'].append({'path': name, 'ok': True, 'action': target(row, directory)})
                except (OSError, ValueError) as error:
                    report['targets'].append({'path': name, 'ok': False, 'error': str(error)})
    report['ok'] = all(r['ok'] for r in report['sources'] + report['targets'])
    report['targets_checked'] = check_targets
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plan', type=Path)
    parser.add_argument('--targets', action='store_true', help='Check public target metadata in real host namespace')
    args = parser.parse_args()
    report = inspect(json.loads(args.plan.read_text()), args.targets)
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report['ok'] else 2)
