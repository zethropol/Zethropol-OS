"""Linux-only immutable content handoff. No pathname is installation authority."""
import fcntl
import hashlib
import os
import resource
from pathlib import PurePosixPath
import stat
from contextlib import contextmanager

from worker import Rejected

SEALS = fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL
BWRAP_MAX_ARGS = 9000  # Upstream parse_args_recurse hardening, including --args.


@contextmanager
def sandbox_arguments(args, fds=()):
    """Avoid ARG_MAX with a finite sealed bwrap --args FD.

    Only the direct sandbox or the fixed systemd scope wrapper is recognized.
    This does not expand the root FD budget or the managed JSON protocol.
    bwrap consumes/closes its args FD before executing the recipe.
    """
    index = 0
    if args and args[0] == '/usr/bin/systemd-run':
        try:
            index = args.index('--') + 1
        except ValueError:
            raise Rejected('Missing scope command delimiter')
    if index >= len(args) or args[index] != '/usr/bin/bwrap':
        yield args, fds
        return
    tail = args[index+1:]
    # --args is an option source, not a source for the payload command. Keep
    # the command on argv: bwrap deliberately does not take it from the FD.
    try:
        boundary = tail.index('--')
    except ValueError:
        boundary = len(tail)
    command = tail[boundary:]
    tail = tail[:boundary]
    if not tail or len(tail) + len(command) + 8 > BWRAP_MAX_ARGS:
        raise Rejected('Sandbox argument count quota exceeded')
    data = bytearray()
    for arg in tail:
        if not isinstance(arg, str) or '\0' in arg:
            raise Rejected('Invalid sandbox argument')
        encoded = os.fsencode(arg)
        if len(data) + len(encoded) + 1 > 16 * 1024**2:
            raise Rejected('Sandbox argument byte quota exceeded')
        data.extend(encoded); data.append(0)
    fd = os.memfd_create('zethropol-bwrap-args', os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
    try:
        view = memoryview(data)
        while view:
            written = os.write(fd, view)
            if not written:
                raise Rejected('Cannot write sandbox arguments')
            view = view[written:]
        os.lseek(fd, 0, os.SEEK_SET)
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS, SEALS)
        yield [*args[:index+1], '--args', str(fd), *command], (*fds, fd)
    finally:
        os.close(fd)


def beneath(rootfd, name, flags=os.O_RDONLY):
    parts = PurePosixPath(name).parts
    if not parts or name.startswith('/') or any(p in ('.', '..') for p in parts):
        raise Rejected('Unsafe relative file name')
    parent = os.dup(rootfd)
    try:
        for part in parts[:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent)
            os.close(parent)
            parent = child
        return os.open(parts[-1], flags | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
    finally:
        os.close(parent)


class Blob:
    """Broker independently snapshots caller FD, then hashes sealed bytes.

    Hash is an observation, never evidence of authorization or authenticity.
    Only the sealed FD may be handed to a metadata/signature verifier.
    """
    def __init__(self, source_fd, limit=256 * 1024 * 1024, allow_empty=False):
        self.fd = -1
        info = os.fstat(source_fd)
        if not stat.S_ISREG(info.st_mode) or (info.st_size == 0 and not allow_empty) or info.st_size > limit:
            raise Rejected('Invalid archive/blob size or type')
        target = os.memfd_create('zethropol-immutable', os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
        try:
            offset = 0
            while offset < info.st_size:
                block = os.pread(source_fd, min(1024 * 1024, info.st_size - offset), offset)
                if not block:
                    raise Rejected('Input truncated during FD handoff')
                view = memoryview(block)
                while view:
                    count = os.write(target, view)
                    view = view[count:]
                offset += len(block)
            after = os.fstat(source_fd)
            if (info.st_size, info.st_mtime_ns, info.st_ctime_ns) != (after.st_size, after.st_mtime_ns, after.st_ctime_ns):
                raise Rejected('Input mutated during FD handoff')
            fcntl.fcntl(target, fcntl.F_ADD_SEALS, SEALS)
            h = hashlib.sha256()
            for offset in range(0, info.st_size, 1024 * 1024):
                h.update(os.pread(target, min(1024 * 1024, info.st_size-offset), offset))
            os.lseek(target, 0, os.SEEK_SET)
            self.sha256 = h.hexdigest()
            self.size = info.st_size
            self.fd = target
        except BaseException:
            os.close(target)
            raise

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class FrozenTree:
    """Materialize reviewed root solely from sealed FDs, never path bind mounts.

    Caller must authenticate the manifest; this class proves byte equality,
    not repository signatures. The caller selects the authenticated manifest
    version's finite byte/entry budget; legacy roots default to 1 GiB.
    """
    def __init__(self, root, manifest, limit=1024**3, max_entries=16384):
        if len(manifest) > max_entries:
            raise Rejected('Frozen root entry quota exceeded')
        needed = sum(entry[0] == 'file' for entry in manifest.values())
        mount_args = sum({'dir': 2, 'link': 3, 'file': 5}.get(entry[0], 0) for entry in manifest.values())
        if mount_args + 256 > BWRAP_MAX_ARGS:
            raise Rejected('Frozen root exceeds bubblewrap argument quota; args FD does not bypass it')
        if needed + 64 > resource.getrlimit(resource.RLIMIT_NOFILE)[0]:
            raise Rejected('Frozen root requires more file descriptors than the worker budget')
        self.blobs = []
        self.mounts = []
        rootfd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        used = 0
        try:
            for name, entry in sorted(manifest.items(), key=lambda pair: (len(PurePosixPath(pair[0]).parts), pair[0])):
                if name == '.':
                    if entry[0] != 'dir':
                        raise Rejected('Invalid root entry')
                    continue
                if str(PurePosixPath(name)) != name or name.startswith('/') or '..' in PurePosixPath(name).parts:
                    raise Rejected('Unsafe root manifest path')
                destination = '/' + name
                if entry[0] == 'dir':
                    self.mounts += ['--dir', destination]
                elif entry[0] == 'link':
                    # Manifest links resolve inside the new root only.
                    self.mounts += ['--symlink', entry[1], destination]
                elif entry[0] == 'file':
                    source = beneath(rootfd, name)
                    try:
                        blob = Blob(source, min(256 * 1024**2, limit-used), allow_empty=True)
                    finally:
                        os.close(source)
                    self.blobs.append(blob)
                    used += blob.size
                    if blob.sha256 != entry[2]:
                        raise Rejected('Build root bytes changed')
                    # Drop setuid/setgid and write bits from all build-root files.
                    mode = entry[1] & 0o555
                    self.mounts += ['--perms', format(mode, 'o'), '--ro-bind-data', str(blob.fd), destination]
                else:
                    raise Rejected('Unsupported root entry')
        except BaseException:
            self.close()
            raise
        finally:
            os.close(rootfd)

    @property
    def fds(self):
        return tuple(blob.fd for blob in self.blobs)

    def command(self, workfd, command, source_date_epoch=None):
        for blob in self.blobs:
            os.lseek(blob.fd, 0, os.SEEK_SET)
        extra=[] if source_date_epoch is None else ['--setenv','SOURCE_DATE_EPOCH',str(source_date_epoch)]
        return ['/usr/bin/bwrap', '--unshare-all', '--unshare-user', '--disable-userns',
                '--assert-userns-disabled', '--die-with-parent', '--new-session',
                '--cap-drop', 'ALL', '--uid', str(os.getuid()), '--gid', str(os.getgid()),
                *self.mounts, '--proc', '/proc', '--dev', '/dev', '--size', '268435456', '--tmpfs', '/tmp',
                '--bind-fd', str(workfd), '/build', '--remount-ro', '/',
                '--chdir', '/build', '--clearenv', '--setenv', 'PATH', '/usr/bin',
                '--setenv', 'HOME', '/build', '--setenv', 'LC_ALL', 'C',
                '--setenv', 'PKGDEST', '/build/out', '--setenv', 'SRCDEST', '/build',
                *extra, '--', *command]

    def close(self):
        for blob in self.blobs:
            blob.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
