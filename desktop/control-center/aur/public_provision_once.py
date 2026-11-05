"""One-shot four-public-file deployment. Default is READ-ONLY; --apply needs separate owner permission.

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
import struct
import re

BASE = '/home/Zevor/Zethropol-OS/desktop/control-center/'
PLAN = BASE + 'build/aur-chain-continuation-post949-20261101/next-public-deployment-plan.json'
PLAN_SHA = '107cbe5d87912b54da756ee10b96db36ec8a34f916ba5b1731ffe2d2294ab99e'
TX = '/var/lib/zethropol/aur-public-provision-20261101'
CONFIG = '/etc/zethropol-update/aur-trust/distribution.json'
PROJECT = '/etc/zethropol-update/aur-trust/policy.json'
RING = '/etc/zethropol-update/aur-trust/distribution.gpg'
ACCEPTED_PLAN = BASE + 'build/aur-owner-949-final-distribution-20261101/distribution-plan.json'
ACCEPTED_SHA = '2ef3c2471498a8c86e09c28ca6732f6bc231d65ef9f1a915768f1cff8ef53711'
CANDIDATES = BASE + 'build/aur-chain-continuation-post949-20261101/'
DISTRIBUTION = BASE + 'build/aur-yay-offline-20261101/generic-distribution/'
SPECS = {
    '/usr/lib/zethropol/aur-root-stage': (CANDIDATES+'aur-root-stage.CANDIDATE', '0755', '984be618ae00b201c5161f38f270ab8070fdd717001be8898140da24983cd21e'),
    '/usr/lib/zethropol/aur-quota-mount': (CANDIDATES+'aur-quota-mount.CANDIDATE.r2', '0755', 'c06bbf9c7979161ae909904c19f51414ecd6e137fc926aec365faa60fcaa720b'),
    '/etc/zethropol-update/aur-trust/distribution.gpg': (DISTRIBUTION+'distribution.gpg', '0644', 'f442bfff1aa040feeeb5613e7285d54ec54b57c4a823b31da1a6c5bd9f21df8b'),
    CONFIG: (DISTRIBUTION+'distribution.json', '0644', '4475e9d9c592dfab329c8a0fc3008b1dddfb63e73d3cc9c0f733fbe6cbd9c76b'),
}
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
            '\x00' not in path and not path.startswith('//') and path != '/', 'Noncanonical path')
    return path


def acl(fd):
    require(not (ACL | {'security.capability'}).intersection(os.listxattr(fd)), 'Extended/default ACL')


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


def filecheck(pfd, name, expected, mode=0o644):
    fd = open_regular(pfd, name)
    try:
        s = os.fstat(fd)
        require((s.st_uid, s.st_gid, stat.S_IMODE(s.st_mode)) == (0, 0, mode),
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


def unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON field')
        result[key] = value
    return result


def parse(data):
    return json.loads(data, object_pairs_hook=unique)


def state(s, digest=None):
    return {'dev': s.st_dev, 'ino': s.st_ino, 'uid': s.st_uid, 'gid': s.st_gid,
            'mode': oct(stat.S_IMODE(s.st_mode)), 'sha256': digest, 'nlink': s.st_nlink,
            'mtime_ns': s.st_mtime_ns, 'ctime_ns': s.st_ctime_ns}


def inspect_targets(plan, installed=False):
    result = {}
    for row in plan['files']:
        path = row['target']
        pfd, name = parent(path)
        try:
            directory(pfd, mode=0o755)
            revalidate_parent(path, pfd)
            if not installed:
                try:
                    os.stat(name, dir_fd=pfd, follow_symlinks=False)
                except FileNotFoundError:
                    result[path] = None
                else:
                    raise Refused('Any existing target conflicts: ' + path)
            else:
                s = filecheck(pfd, name, row['sha256'], int(row['mode'], 8))
                result[path] = state(s, row['sha256'])
            temporary = '.zethropol-aur-public-20261101-' + str(list(SPECS).index(path)+1) + '.tmp'
            try:
                os.stat(temporary, dir_fd=pfd, follow_symlinks=False)
            except FileNotFoundError:
                pass
            else:
                raise Refused('Reserved publication temporary exists: ' + temporary)
        finally:
            os.close(pfd)
    return result


def protected_payload():
    raw = readpath(ACCEPTED_PLAN)
    require(sha(raw) == ACCEPTED_SHA, 'Accepted signer plan changed')
    accepted = parse(raw)
    require(len(accepted['files']) == 13, 'Accepted signer payload must have 13 files')
    states = {}
    for row in accepted['files']:
        pfd, leaf = parent(row['target'])
        try:
            s = filecheck(pfd, leaf, row['sha256'])
            states[row['target']] = state(s, row['sha256'])
        finally:
            os.close(pfd)
    return states


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

    def publish(self, row, data):
        path = row['target']
        pfd, name = parent(path)
        self.fds.append(pfd)
        self.counter += 1
        mode = int(row['mode'], 8)
        temp = '.zethropol-aur-public-20261101-' + str(self.counter) + '.tmp'
        self.event('publish-intent', path=path, temporary=temp, sha256=row['sha256'])
        with critical():
            fd = os.open(temp, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW |
                         os.O_CLOEXEC, 0o600, dir_fd=pfd)
            self.fds.append(fd)
            record = {'path': path, 'pfd': pfd, 'name': temp, 'fd': fd,
                      'published': False, 'rename_attempted': False, 'sha256': row['sha256'], 'mode': mode}
            self.objects.append(record)
            acl(fd)
            self.writeall(fd, data)
            os.fchown(fd, 0, 0)
            os.fchmod(fd, mode)
            os.fsync(fd)
            require(sha(readfd(fd)) == row['sha256'], 'Temporary hash mismatch')
            filecheck(pfd, temp, row['sha256'], mode)
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
            s = filecheck(pfd, name, row['sha256'], mode)
            require(identity(s) == identity(os.fstat(fd)), 'Published inode mismatch')
            self.event('publish-complete', path=path, dev=s.st_dev, ino=s.st_ino)

    def rollback(self):
        """Never remove foreign/changed files. Preserve our exact bytes in quarantine."""
        issues = []
        for sig in SIGNALS:
            signal.signal(sig, signal.SIG_IGN)
        # A competing/changed policy may depend on the ring/helpers, including
        # when our config rename failed with EEXIST or was never attempted.
        # Only an absent policy or our exact held, verified inode permits any
        # withdrawal. A foreign/unknown policy is never an ownership claim.
        try:
            pfd, leaf = parent(CONFIG)
            try:
                directory(pfd)
                revalidate_parent(CONFIG, pfd)
                try:
                    actual = os.stat(leaf, dir_fd=pfd, follow_symlinks=False)
                except FileNotFoundError:
                    actual = None
                if actual is not None:
                    owned = next((r for r in self.objects if r['path'] == CONFIG and r['rename_attempted']), None)
                    require(owned is not None and identity(actual) == identity(os.fstat(owned['fd'])),
                            'Foreign policy active; preserve all public dependencies')
                    require(identity(os.fstat(pfd)) == identity(os.fstat(owned['pfd'])),
                            'Policy parent changed; preserve all public dependencies')
                    filecheck(pfd, leaf, owned['sha256'], owned['mode'])
            finally:
                os.close(pfd)
        except BaseException as error:
            return ['Policy withdrawal not proven safe; dependencies preserved: ' + str(error)]
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
                    require(stat.S_IMODE(s.st_mode) == record['mode'] and
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
        return issues

    def close(self):
        for fd in reversed(self.fds):
            try:
                os.close(fd)
            except OSError:
                pass


def elf_valid(data):
    """Static ELF inspection only. Never execute helpers or run a loader/tool."""
    require(len(data) >= 64 and data[:7] == b'\x7fELF\x02\x01\x01', 'ELF64 little-endian helper required')
    fields = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    require(fields[1] == 3 and fields[2] == 62, 'x86_64 PIE helper required')
    off, size, count = fields[5], fields[9], fields[10]
    require(size == 56 and 1 <= count <= 64 and off+size*count <= len(data), 'Bounded ELF program headers required')
    headers = [struct.unpack_from('<IIQQQQQQ', data, off+i*size) for i in range(count)]
    for h in headers:
        require(h[2] <= len(data) and h[5] <= len(data)-h[2], 'ELF segment outside file')
    stacks = [h for h in headers if h[0] == 0x6474e551]
    require(len(stacks) == 1 and not stacks[0][1] & 1, 'Executable/missing ELF stack prohibited')
    interps = [data[h[2]:h[2]+h[5]] for h in headers if h[0] == 3]
    require(interps == [b'/lib64/ld-linux-x86-64.so.2\x00'], 'Expected system loader required')
    dynamic = [h for h in headers if h[0] == 2]
    require(len(dynamic) == 1 and dynamic[0][5] % 16 == 0, 'Unique ELF dynamic table required')
    h = dynamic[0]
    entries = [struct.unpack_from('<qQ', data, offset) for offset in range(h[2], h[2]+h[5], 16)]
    require(any(k == 0 for k,v in entries), 'Terminated ELF dynamic table required')
    entries = entries[:next(i for i,(k,v) in enumerate(entries) if k == 0)]
    require(not any(k in (15,29) for k,v in entries), 'ELF RPATH/RUNPATH prohibited')
    strtabs = [v for k,v in entries if k == 5]
    strsz = [v for k,v in entries if k == 10]
    require(len(strtabs) == len(strsz) == 1, 'Bounded ELF string table required')
    mappings = [h[2]+strtabs[0]-h[3] for h in headers if h[0] == 1 and h[3] <= strtabs[0] < h[3]+h[5]]
    require(len(mappings) == 1 and mappings[0]+strsz[0] <= len(data), 'ELF strings outside file')
    strings = data[mappings[0]:mappings[0]+strsz[0]]
    needed = []
    for k,v in entries:
        if k == 1:
            require(v < len(strings) and b'\x00' in strings[v:], 'Invalid ELF dependency')
            needed.append(strings[v:strings.index(b'\x00',v)])
    require(needed == [b'libc.so.6'], 'Only system libc dependency allowed')


def validate_plan(plan):
    require(type(plan.get('schema')) is int and plan['schema'] == 2 and
            plan.get('purpose') == 'zethropol-four-public-files-review', 'Explicit public plan schema required')
    require(plan.get('approved') is False and plan.get('applied') is False and
            plan.get('directories_to_create') == [], 'Pinned review does not imply runtime authority')
    rows = plan['files']
    require(len(rows) == 4 and [row['target'] for row in rows] == list(SPECS) and
            plan['publish_order'] == list(SPECS), 'Exact four targets and config-last order required')
    require(plan['transaction_layout']['root'] == TX, 'Fixed transaction root required')
    for row in rows:
        require((row['source'],row['mode'],row['sha256']) == SPECS[row['target']] and
                row['owner'] == 'root:root', 'Reviewed public payload drift')


def validate_policy(plan, content):
    config = parse(content[CONFIG])
    require(set(config) == {'keyring','keyring_sha256','signers'} and config['keyring'] == 'distribution.gpg',
            'Fixed policy-local keyring required')
    ring = content[RING]
    require(config['keyring_sha256'] == sha(ring), 'Policy keyring digest mismatch')
    values = config['signers']
    require(isinstance(values,list) and 1 <= len(values) <= 32 and
            all(isinstance(v,str) and re.fullmatch(r'(?:[A-F0-9]{40}|[A-F0-9]{64})',v) for v in values) and
            len(values) == len(set(values)) and sorted(values) == plan['distribution_primary_signers'],
            'Exact unique distribution primary allowlist required')
    pfd, leaf = parent(PROJECT)
    try:
        filecheck(pfd, leaf, plan['project_policy_sha256'])
    finally:
        os.close(pfd)
    project = parse(readpath(PROJECT))
    observed = {role:project[role]['signers'] for role in ('manifest','archive','receipt')}
    require(observed == plan['project_role_signers'] and all(observed.values()), 'Project role trust drift')
    require(not set(values).intersection(v for keys in observed.values() for v in keys),
            'Distribution policy must not authorize project role keys')


def identity_guard(apply):
    require(sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode,
            'Reviewed launcher with Python -I -S -B required')
    if apply:
        require(os.getuid() == 0 and os.geteuid() == 0 and os.getgid() == 0 and os.getegid() == 0,
                'Separately authorized root deployment required')
    require(readpath('/proc/' + str(os.getpid()) + '/uid_map').split() == [b'0',b'0',b'4294967295'],
            'Initial host UID namespace required')


def absent_transaction():
    pfd, leaf = parent(TX)
    try:
        directory(pfd, mode=0o755)
        try:
            os.stat(leaf, dir_fd=pfd, follow_symlinks=False)
        except FileNotFoundError:
            return
        raise Refused('Transaction exists; preserve evidence, do not retry: ' + TX)
    finally:
        os.close(pfd)


def run(apply=False):
    identity_guard(apply)
    raw = readpath(PLAN)
    require(sha(raw) == PLAN_SHA, 'Pinned public plan changed')
    plan = parse(raw)
    validate_plan(plan)
    content = {}
    for row in plan['files']:
        data = readpath(row['source'])
        require(sha(data) == row['sha256'], 'Source hash mismatch: ' + row['source'])
        if row['mode'] == '0755': elf_valid(data)
        content[row['target']] = data
    validate_policy(plan, content)
    protected = protected_payload()
    before = inspect_targets(plan)
    absent_transaction()
    if not apply:
        console(json.dumps({'status':'PASS','scope':'read-only preflight only; not installed',
                            'plan_sha256':PLAN_SHA,'files_checked':4,'protected_signer_files_checked':13,
                            'deployment_performed':False,'signing_performed':False},sort_keys=True))
        return 0
    os.umask(0o077)
    for sig in SIGNALS: signal.signal(sig, stop)
    tx = Transaction()
    try:
        with critical(): tx.open()
        tx.save('distribution-plan.json', raw)
        tx.save('preflight.json', json.dumps({'targets':before,'protected_signer_files':protected},indent=2).encode()+b'\n')
        tx.event('begin', plan_sha256=PLAN_SHA, transaction=TX)
        for row in plan['files']:
            name = PurePosixPath(row['target']).name
            fd = os.open(name,os.O_RDWR|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o600,dir_fd=tx.staging)
            try:
                acl(fd); tx.writeall(fd,content[row['target']]); os.fsync(fd)
                require(sha(readfd(fd)) == row['sha256'], 'Staging hash mismatch')
            finally: os.close(fd)
        os.fsync(tx.staging)
        for row in plan['files']:
            require(sha(readpath(row['source'])) == row['sha256'], 'Source changed during staging')
        require(inspect_targets(plan) == before and protected_payload() == protected,
                'Target/protected payload changed during staging')
        for row in plan['files'][:-1]: tx.publish(row,content[row['target']])
        for row in plan['files'][:-1]:
            pfd,leaf = parent(row['target'])
            try: filecheck(pfd,leaf,row['sha256'],int(row['mode'],8))
            finally: os.close(pfd)
        validate_policy(plan,content)
        require(protected_payload() == protected,'Protected signer changed before config publication')
        tx.publish(plan['files'][-1],content[CONFIG])
        after = inspect_targets(plan,installed=True)
        for record in tx.objects:
            require(record['published'] and (after[record['path']]['dev'],after[record['path']]['ino']) ==
                    identity(os.fstat(record['fd'])), 'Created inode changed after publication')
        require(protected_payload() == protected,'Protected signer changed after publication')
        validate_policy(plan,content)
        for row in plan['files']:
            if row['mode'] == '0755': elf_valid(readpath(row['target']))
        tx.save('postflight.json',json.dumps({'targets':after,'protected_signer_files':protected},indent=2).encode()+b'\n')
        tx.event('success',files_verified=4,protected_signer_files_verified=13,
                 manifest_signed=False,owner_approval_created=False,production_enabled=False)
    except BaseException as error:
        try: tx.event('failure',error=str(error))
        except BaseException: pass
        issues = tx.rollback()
        try: tx.event('rollback',complete=not issues,issues=issues)
        except BaseException as audit_error: issues.append('Rollback audit persistence failed: '+str(audit_error))
        console('FAILED; evidence retained at '+TX+'; rollback issues: '+repr(issues),error=True)
        return 3 if issues else 2
    finally: tx.close()
    console('SUCCESS: four public targets verified; accepted signer payload unchanged. Audit: '+TX)
    return 0


if __name__ == '__main__':
    if sys.argv[1:] not in ([],['--check'],['--apply']):
        raise SystemExit('Use --check (read-only default) or separately approved --apply')
    try:
        sys.exit(run(apply=sys.argv[1:] == ['--apply']))
    except (OSError,Refused,ValueError,KeyError,TypeError,struct.error) as error:
        console('REFUSED before deployment: '+str(error),error=True)
        sys.exit(1)
