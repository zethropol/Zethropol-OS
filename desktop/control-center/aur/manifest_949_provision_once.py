"""One-shot, pinned public-file deployment. Run only via the reviewed sudo command.

No local imports, subprocesses, GPG, accounts, approvals or service operations.
The transaction directory is deliberately never reused or automatically removed.
"""
import sys

if __name__ == '__main__' and not (sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode):
    raise SystemExit('Reviewed launcher with Python -I -S -B required')

import contextlib
import ctypes
import errno
import hashlib
import json
import os
from pathlib import PurePosixPath
import signal
import stat

BASE = '/home/Zevor/Zethropol-OS/desktop/control-center/'
PLAN = BASE + 'build/aur-owner-949-final-distribution-20261101/distribution-plan.json'
PLAN_SHA = '2ef3c2471498a8c86e09c28ca6732f6bc231d65ef9f1a915768f1cff8ef53711'
TX = '/var/lib/zethropol/aur-manifest-provision-949-20261101'
CONFIG = '/etc/zethropol-update/aur-manifest-signer.json'
ACL = {'system.posix_acl_access', 'system.posix_acl_default'}
FLAGS = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK
DIRFLAGS = FLAGS | os.O_DIRECTORY
SIGNALS = {signal.SIGINT, signal.SIGTERM, signal.SIGHUP}


class Refused(RuntimeError):
    pass


def require(ok, message):
    if not ok:
        raise Refused(message)


def canonical(path):
    require(isinstance(path, str) and path.startswith('/') and
            str(PurePosixPath(path)) == path and '..' not in path.split('/') and
            '\x00' not in path and path != '/', 'Noncanonical path')
    return path


def acl(fd):
    require(not ACL.intersection(os.listxattr(fd)), 'Extended/default ACL')


def directory(fd, uid=0, gid=0, mode=None):
    s = os.fstat(fd)
    require(stat.S_ISDIR(s.st_mode) and (s.st_uid, s.st_gid) == (uid, gid) and
            not s.st_mode & 0o022 and (mode is None or stat.S_IMODE(s.st_mode) == mode),
            'Directory type/ownership/mode conflict')
    acl(fd)
    return s


def walk(path, secure=True):
    """Open each component relative to its pinned parent, never follow links."""
    require(path == '/' or canonical(path), 'Invalid directory')
    fd = os.open('/', DIRFLAGS)
    try:
        if secure:
            directory(fd)
        for part in PurePosixPath(path).parts[1:]:
            new = os.open(part, DIRFLAGS, dir_fd=fd)
            os.close(fd)
            fd = new
            if secure:
                directory(fd)
        return fd
    except BaseException:
        os.close(fd)
        raise


def parent(path, secure=True):
    canonical(path)
    p = PurePosixPath(path)
    return walk(str(p.parent), secure), p.name


def identity(s):
    return (s.st_dev, s.st_ino)


def revalidate_parent(path, held):
    fd, name = parent(path)
    try:
        require(identity(os.fstat(fd)) == identity(os.fstat(held)),
                'Parent moved/replaced: ' + path)
    finally:
        os.close(fd)


def fingerprint(s):
    return (s.st_dev, s.st_ino, s.st_mode, s.st_uid, s.st_gid, s.st_nlink,
            s.st_size, s.st_mtime_ns, s.st_ctime_ns)


def bounded_regular(s):
    require(stat.S_ISREG(s.st_mode) and s.st_nlink == 1 and
            0 <= s.st_size <= 1024 * 1024,
            'Expected bounded, single-link regular file')


def open_regular(pfd, name):
    # O_PATH never invokes a device driver's open or opens a FIFO for I/O.
    # Validate the pinned inode before acquiring any data-access descriptor.
    pin = os.open(name, os.O_PATH | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=pfd)
    try:
        before = os.fstat(pin)
        bounded_regular(before)
        # Deliberate kernel fd magiclink, NOT a path from the caller/workspace.
        # Opening this stable O_PATH reference cannot switch to a replaced name.
        procfd = walk('/proc/' + str(os.getpid()) + '/fd', secure=False)
        try:
            fd = os.open(str(pin), os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK,
                         dir_fd=procfd)
        finally:
            os.close(procfd)
        try:
            require(fingerprint(os.fstat(fd)) == fingerprint(before),
                    'Pinned source changed before data access')
            return fd
        except BaseException:
            os.close(fd)
            raise
    finally:
        os.close(pin)


def readfd(fd):
    s = os.fstat(fd)
    bounded_regular(s)
    os.lseek(fd, 0, os.SEEK_SET)
    data = bytearray()
    while True:
        block = os.read(fd, 65536)
        if not block:
            break
        data.extend(block)
        require(len(data) <= 1024 * 1024, 'File grew beyond limit')
    after = os.fstat(fd)
    require(fingerprint(s) == fingerprint(after), 'File metadata changed during read')
    return bytes(data)


def readpath(path):
    pfd, name = parent(path, secure=False)
    try:
        fd = open_regular(pfd, name)
        try:
            return readfd(fd)
        finally:
            os.close(fd)
    finally:
        os.close(pfd)


def console(message, *, error=False):
    # A closed terminal/pipe is not an audit failure and must not turn a
    # durably committed deployment into an unexpected rollback.
    try:
        print(message, file=sys.stderr if error else sys.stdout, flush=True)
    except (OSError, ValueError):
        pass


def sha(data):
    return hashlib.sha256(data).hexdigest()


def filecheck(pfd, name, expected):
    fd = open_regular(pfd, name)
    try:
        s = os.fstat(fd)
        require((s.st_uid, s.st_gid, stat.S_IMODE(s.st_mode)) == (0, 0, 0o644),
                'File ownership/mode conflict: ' + name)
        acl(fd)
        require(sha(readfd(fd)) == expected, 'File hash conflict: ' + name)
        after = os.fstat(fd)
        require(fingerprint(s) == fingerprint(after), 'File metadata changed during check')
        acl(fd)
        require(identity(os.stat(name, dir_fd=pfd, follow_symlinks=False)) == identity(after),
                'File name replaced during check')
        return after
    finally:
        os.close(fd)


def dspec(row):
    return row.get('uid', 0), row.get('gid', 0), int(row['mode'], 8)


def preflight(plan):
    """Missing ancestors are permitted only if explicitly planned directories."""
    dirs = {r['path']: r for r in plan['directories']}
    result = {}
    for path, row, isdir in ([(r['path'], r, True) for r in plan['directories']] +
                              [(r['target'], r, False) for r in plan['files']]):
        parts = PurePosixPath(canonical(path)).parts[1:]
        fd = os.open('/', DIRFLAGS)
        missing = False
        current = ''
        try:
            directory(fd)
            for part in parts[:-1]:
                current += '/' + part
                if missing:
                    require(current in dirs, 'Unplanned missing ancestor: ' + current)
                    continue
                try:
                    new = os.open(part, DIRFLAGS, dir_fd=fd)
                except FileNotFoundError:
                    require(current in dirs, 'Unplanned missing ancestor: ' + current)
                    missing = True
                    continue
                try:
                    directory(new)
                except BaseException:
                    os.close(new)
                    raise
                os.close(fd)
                fd = new
            name = parts[-1]
            if missing:
                result[path] = None
                continue
            try:
                s = os.stat(name, dir_fd=fd, follow_symlinks=False)
            except FileNotFoundError:
                result[path] = None
                continue
            if isdir:
                child = os.open(name, DIRFLAGS, dir_fd=fd)
                try:
                    checked = directory(child, *dspec(row))
                finally:
                    os.close(child)
            else:
                checked = filecheck(fd, name, row['sha256'])
            require(identity(s) == identity(checked), 'Target replaced during preflight')
            result[path] = {'dev': checked.st_dev, 'ino': checked.st_ino, 'uid': checked.st_uid,
                            'gid': checked.st_gid, 'mode': oct(stat.S_IMODE(checked.st_mode)),
                            'sha256': None if isdir else row['sha256'], 'acl': 'none'}
        finally:
            os.close(fd)
    return result


@contextlib.contextmanager
def critical():
    old = signal.pthread_sigmask(signal.SIG_BLOCK, SIGNALS)
    try:
        yield
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, old)


def stop(signum, frame):
    raise Refused('Interrupted by signal ' + str(signum))


class Transaction:
    def __init__(self):
        self.fd = None
        self.logfd = None
        self.created_dirs = []
        self.objects = []
        self.fds = []
        self.counter = 0
        libc = ctypes.CDLL(None, use_errno=True)
        self.rename = libc.renameat2
        self.rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                                ctypes.c_char_p, ctypes.c_uint]
        self.rename.restype = ctypes.c_int

    def move(self, srcfd, src, dstfd, dst):
        if self.rename(srcfd, os.fsencode(src), dstfd, os.fsencode(dst), 1) != 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))

    def event(self, event, **data):
        line = json.dumps({'event': event, **data}, sort_keys=True) + '\n'
        if self.logfd is not None:
            self.writeall(self.logfd, line.encode())
            os.fsync(self.logfd)
        console(line.rstrip('\n'))

    @staticmethod
    def writeall(fd, data):
        while data:
            n = os.write(fd, data)
            require(n > 0, 'Short write')
            data = data[n:]

    def open(self):
        pfd, name = parent(TX)
        try:
            # EEXIST always aborts. Never reuse or erase a previous transaction.
            os.mkdir(name, 0o700, dir_fd=pfd)
            self.fd = os.open(name, DIRFLAGS, dir_fd=pfd)
            directory(self.fd, mode=0o700)
            os.fsync(self.fd)
            os.fsync(pfd)
        finally:
            os.close(pfd)
        self.logfd = os.open('audit.jsonl', os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                             os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=self.fd)
        acl(self.logfd)
        self.fds.extend([self.fd, self.logfd])
        for name in ('staging', 'quarantine'):
            os.mkdir(name, 0o700, dir_fd=self.fd)
            fd = os.open(name, DIRFLAGS, dir_fd=self.fd)
            directory(fd, mode=0o700)
            os.fsync(fd)
            self.fds.append(fd)
            setattr(self, name, fd)
        os.fsync(self.fd)

    def save(self, name, data):
        fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW |
                     os.O_CLOEXEC, 0o600, dir_fd=self.fd)
        try:
            acl(fd)
            self.writeall(fd, data)
            os.fsync(fd)
        finally:
            os.close(fd)
        os.fsync(self.fd)

    def create_dir(self, row, before):
        path = row['path']
        pfd, name = parent(path)
        self.fds.append(pfd)
        if before is not None:
            fd = os.open(name, DIRFLAGS, dir_fd=pfd)
            try:
                s = directory(fd, *dspec(row))
                require(identity(s) == (before['dev'], before['ino']), 'Directory replaced: ' + path)
            finally:
                os.close(fd)
            return
        self.event('mkdir-intent', path=path, uid=dspec(row)[0], gid=dspec(row)[1], mode=row['mode'])
        with critical():
            os.mkdir(name, 0o700, dir_fd=pfd)
            # Record before chmod/chown, so even a setup error can be handled.
            fd = os.open(name, DIRFLAGS, dir_fd=pfd)
            self.fds.append(fd)
            record = [path, pfd, name, fd, (0, 0, 0o700)]
            self.created_dirs.append(record)
            os.fchown(fd, dspec(row)[0], dspec(row)[1])
            record[4] = (dspec(row)[0], dspec(row)[1], 0o700)
            os.fchmod(fd, dspec(row)[2])
            record[4] = dspec(row)
            s = directory(fd, *dspec(row))
            os.fsync(fd)
            os.fsync(pfd)
            self.event('mkdir-complete', path=path, dev=s.st_dev, ino=s.st_ino)

    def publish(self, row, before, data):
        path = row['target']
        pfd, name = parent(path)
        self.fds.append(pfd)
        self.counter += 1
        if before is not None:
            s = filecheck(pfd, name, row['sha256'])
            require(identity(s) == (before['dev'], before['ino']), 'Reused file replaced: ' + path)
            self.event('reuse', path=path)
            return
        temp = '.zethropol-manifest-provision-949-20261101-' + str(self.counter) + '.tmp'
        self.event('publish-intent', path=path, temporary=temp, sha256=row['sha256'])
        with critical():
            fd = os.open(temp, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW |
                         os.O_CLOEXEC, 0o600, dir_fd=pfd)
            self.fds.append(fd)
            record = {'path': path, 'pfd': pfd, 'name': temp, 'fd': fd,
                      'published': False, 'rename_attempted': False, 'sha256': row['sha256']}
            self.objects.append(record)
            acl(fd)
            self.writeall(fd, data)
            os.fchown(fd, 0, 0)
            os.fchmod(fd, 0o644)
            os.fsync(fd)
            require(sha(readfd(fd)) == row['sha256'], 'Temporary hash mismatch')
            filecheck(pfd, temp, row['sha256'])
            os.fsync(pfd)
            prepared = os.fstat(fd)
            self.event('publish-prepared', path=path, temporary=temp,
                       dev=prepared.st_dev, ino=prepared.st_ino, sha256=row['sha256'])
            # Reopen the ancestry and ensure the pinned directory is still reachable.
            check, leaf = parent(path)
            try:
                require(identity(os.fstat(check)) == identity(os.fstat(pfd)), 'Parent replaced')
            finally:
                os.close(check)
            record['rename_attempted'] = True
            self.move(pfd, temp, pfd, name)
            record.update(name=name, published=True)
            os.fsync(pfd)
            s = filecheck(pfd, name, row['sha256'])
            require(identity(s) == identity(os.fstat(fd)), 'Published inode mismatch')
            self.event('publish-complete', path=path, dev=s.st_dev, ino=s.st_ino)

    def rollback(self):
        """Never remove foreign/changed files. Preserve our exact bytes in quarantine."""
        issues = []
        for sig in SIGNALS:
            signal.signal(sig, signal.SIG_IGN)
        for i, record in enumerate(reversed(self.objects)):
            try:
                pfd, name, fd = record['pfd'], record['name'], record['fd']
                directory(pfd)
                revalidate_parent(record['path'], pfd)
                if record['rename_attempted'] and not record['published']:
                    # A rename can have an uncertain outcome after I/O failure.
                    # Identify our final inode before deciding what to withdraw.
                    final = PurePosixPath(record['path']).name
                    try:
                        actual = os.stat(final, dir_fd=pfd, follow_symlinks=False)
                    except FileNotFoundError:
                        pass
                    else:
                        if identity(actual) == identity(os.fstat(fd)):
                            name = final
                            record.update(name=final, published=True)
                s = os.stat(name, dir_fd=pfd, follow_symlinks=False)
                held = os.fstat(fd)
                require(identity(s) == identity(held) and s.st_nlink == 1 and
                        stat.S_ISREG(s.st_mode) and (s.st_uid, s.st_gid) == (0, 0),
                        'Rollback inode/owner conflict')
                acl(fd)
                if record['published']:
                    require(stat.S_IMODE(s.st_mode) == 0o644 and
                            sha(readfd(fd)) == record['sha256'], 'Rollback published file changed')
                dest = f'{i:02d}-' + PurePosixPath(record['path']).name
                try:
                    self.event('quarantine-intent', path=record['path'], name=dest,
                               dev=s.st_dev, ino=s.st_ino,
                               sha256=sha(readfd(fd)))
                except BaseException as audit_error:
                    issues.append('Quarantine audit persistence failed: ' + str(audit_error))
                try:
                    self.move(pfd, name, self.quarantine, dest)
                except OSError as move_error:
                    if move_error.errno != errno.EXDEV:
                        raise
                    # /usr and /var may be different filesystems. Preserve exact
                    # public bytes durably before unlinking our original inode.
                    evidence = readfd(fd)
                    qfd = os.open(dest, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                                  os.O_NOFOLLOW | os.O_CLOEXEC, 0o600,
                                  dir_fd=self.quarantine)
                    try:
                        acl(qfd)
                        self.writeall(qfd, evidence)
                        os.fsync(qfd)
                    finally:
                        os.close(qfd)
                    qread = os.open(dest, FLAGS, dir_fd=self.quarantine)
                    try:
                        require(readfd(qread) == evidence, 'Quarantine copy mismatch')
                    finally:
                        os.close(qread)
                    os.fsync(self.quarantine)
                    latest = os.stat(name, dir_fd=pfd, follow_symlinks=False)
                    require(identity(latest) == identity(s) and
                            (latest.st_uid, latest.st_gid, latest.st_mode) ==
                            (s.st_uid, s.st_gid, s.st_mode) and
                            readfd(fd) == evidence, 'Original changed during quarantine')
                    os.unlink(name, dir_fd=pfd)
                qfd = open_regular(self.quarantine, dest)
                try:
                    os.fchmod(qfd, 0o600)
                    acl(qfd)
                    os.fsync(qfd)
                finally:
                    os.close(qfd)
                os.fsync(self.quarantine)
                os.fsync(pfd)
                self.event('quarantined', path=record['path'], name=dest)
            except BaseException as error:
                issues.append(str(error))
                if record['path'] == CONFIG and record['rename_attempted']:
                    # Never dismantle dependencies while our config remains active.
                    return issues
        for path, pfd, name, fd, expected in reversed(self.created_dirs):
            try:
                directory(pfd)
                revalidate_parent(path, pfd)
                s = os.stat(name, dir_fd=pfd, follow_symlinks=False)
                held = os.fstat(fd)
                require(identity(s) == identity(held) and
                        (s.st_uid, s.st_gid, stat.S_IMODE(s.st_mode)) == expected, 'Rollback directory replaced')
                acl(fd)
                os.rmdir(name, dir_fd=pfd)  # Empty directories only; no recursion.
                os.fsync(pfd)
                self.event('rmdir', path=path)
            except BaseException as error:
                issues.append(path + ': ' + str(error))
        return issues

    def close(self):
        for fd in reversed(self.fds):
            try:
                os.close(fd)
            except OSError:
                pass


def config_valid(data):
    require(json.loads(data) == {
        'schema': 2, 'purpose': 'zethropol-manifest-signer',
        'identity_model': 'existing-key-owner', 'user': 'zethropol-aur-release',
        'uid': 949, 'gnupg_home': '/var/lib/zethropol-aur-release/gnupg',
        'signer': 'F3F36BFA6D0FC60DA1CB34F2927519292F6F497B'}, 'Signer config mismatch')


def run():
    require(sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode,
            'Reviewed launcher with Python -I -S -B required')
    require(os.getuid() == 0 and os.geteuid() == 0 and
            os.getgid() == 0 and os.getegid() == 0, 'Interactive sudo with root group required')
    require(readpath('/proc/' + str(os.getpid()) + '/uid_map').split() == [b'0', b'0', b'4294967295'],
            'Initial host UID namespace required')
    os.umask(0o077)
    for sig in SIGNALS:
        signal.signal(sig, stop)
    raw = readpath(PLAN)
    require(sha(raw) == PLAN_SHA, 'Pinned plan hash mismatch')
    plan = json.loads(raw)
    require(len(plan['files']) == 13 and len(plan['directories']) == 5 and
            plan['files'][-1]['target'] == CONFIG, 'Unexpected deployment shape')
    content = {}
    for row in plan['files']:
        data = readpath(row['source'])
        require(sha(data) == row['sha256'], 'Source hash mismatch: ' + row['source'])
        content[row['target']] = data
    config_valid(content[CONFIG])
    before = preflight(plan)
    if before[CONFIG] is not None:
        require(all(before[row['target']] is not None for row in plan['files']),
                'Existing config with incomplete module set; separate review required')
    # Refuse an existing transaction before creating anything.
    pfd, leaf = parent(TX)
    try:
        try:
            os.stat(leaf, dir_fd=pfd, follow_symlinks=False)
        except FileNotFoundError:
            pass
        else:
            raise Refused('Transaction already exists; inspect it, never auto-retry: ' + TX)
    finally:
        os.close(pfd)
    tx = Transaction()
    try:
        with critical():
            tx.open()
        tx.save('distribution-plan.json', raw)
        tx.save('preflight.json', json.dumps(before, indent=2).encode() + b'\n')
        tx.event('begin', plan_sha256=PLAN_SHA, transaction=TX)
        # Disk staging is root-only. Re-read exact bytes before any target changes.
        for row in plan['files']:
            name = PurePosixPath(row['target']).name
            fd = os.open(name, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW |
                         os.O_CLOEXEC, 0o600, dir_fd=tx.staging)
            try:
                acl(fd)
                tx.writeall(fd, content[row['target']])
                os.fsync(fd)
                require(sha(readfd(fd)) == row['sha256'], 'Staging hash mismatch')
            finally:
                os.close(fd)
        os.fsync(tx.staging)
        for row in plan['files']:
            require(sha(readpath(row['source'])) == row['sha256'],
                    'Source changed during staging: ' + row['source'])
        require(preflight(plan) == before, 'Targets changed after preflight')
        for row in plan['directories']:
            tx.create_dir(row, before[row['path']])
        for row in plan['files'][:-1]:
            tx.publish(row, before[row['target']], content[row['target']])
        # Validate every dependency before exposing schema 2 config.
        for row in plan['files'][:-1]:
            pfd, leaf = parent(row['target'])
            try:
                filecheck(pfd, leaf, row['sha256'])
            finally:
                os.close(pfd)
        tx.publish(plan['files'][-1], before[CONFIG], content[CONFIG])
        after = preflight(plan)
        require(all(v is not None for v in after.values()), 'Postflight missing target')
        for path, previous in before.items():
            if previous is not None:
                require((after[path]['dev'], after[path]['ino']) ==
                        (previous['dev'], previous['ino']), 'Reused inode replaced: ' + path)
        for record in tx.objects:
            state = after[record['path']]
            require((state['dev'], state['ino']) == identity(os.fstat(record['fd'])),
                    'Created file inode replaced: ' + record['path'])
        for path, _, _, fd, _ in tx.created_dirs:
            require((after[path]['dev'], after[path]['ino']) == identity(os.fstat(fd)),
                    'Created directory inode replaced: ' + path)
        config_valid(readpath(CONFIG))
        tx.save('postflight.json', json.dumps(after, indent=2).encode() + b'\n')
        # Publication temp names have all been renamed away; do not touch unrelated files.
        for record in tx.objects:
            require(record['published'], 'Unpublished temporary remains')
        tx.event('success', files_verified=13, directories_verified=5,
                 schema=2, signed=False, owner_approval_created=False, production_enabled=False)
    except BaseException as error:
        # Any audit failure must not prevent rollback from being attempted.
        try:
            tx.event('failure', error=str(error))
        except BaseException:
            pass
        issues = tx.rollback()
        try:
            tx.event('rollback', complete=not issues, issues=issues)
        except BaseException as audit_error:
            issues.append('Rollback audit persistence failed: ' + str(audit_error))
        console('FAILED; evidence retained at ' + TX + '; rollback issues: ' + repr(issues), error=True)
        return 2 if not issues else 3
    finally:
        tx.close()
    console('SUCCESS: 13 public files and 5 directory states verified. Audit: ' + TX)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(run())
    except (OSError, Refused, ValueError) as error:
        console('REFUSED before deployment: ' + str(error), error=True)
        sys.exit(1)
