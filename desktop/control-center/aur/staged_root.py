"""Single immutable root mount, constructed without privileged host operations.

Authorization stays with the signed-root pipeline. This module only proves that
the staged bytes equal its already authenticated tree, with setid/write bits
removed. No mutable-directory fallback exists.
"""
import array
import hashlib
import os
from pathlib import PurePosixPath
import re
import socket
import stat
import subprocess
import time

from worker import Rejected, canonical, digest
from sealed import beneath, FrozenTree


def checked_entries(entries, max_entries):
    if not isinstance(entries, dict) or not 1 <= len(entries) <= max_entries:
        raise Rejected('Staged root entry quota exceeded')
    result = {}
    for name, entry in entries.items():
        if (not isinstance(name, str) or '\0' in name or len(os.fsencode(name)) > 4096 or
                len(PurePosixPath(name).parts) > 128 or
                (name != '.' and (str(PurePosixPath(name)) != name or name.startswith('/') or
                                  '..' in PurePosixPath(name).parts))):
            raise Rejected('Unsafe staged root path')
        if not isinstance(entry, list) or not entry:
            raise Rejected('Invalid staged root entry')
        if entry[0] in ('file', 'dir'):
            if len(entry) != (3 if entry[0] == 'file' else 2) or type(entry[1]) is not int or not 0 <= entry[1] <= 0o7777:
                raise Rejected('Invalid staged root mode')
            mode = entry[1] & 0o555
            if entry[0] == 'file':
                if not isinstance(entry[2], str) or not re.fullmatch('[0-9a-f]{64}', entry[2]):
                    raise Rejected('Invalid staged root hash')
                result[name] = ['file', mode, entry[2]]
            else:
                # Directories must remain traversable by the mapped builder.
                result[name] = ['dir', mode | 0o500]
        elif entry[0] == 'link' and len(entry) == 2 and isinstance(entry[1], str) and entry[1] and '\0' not in entry[1] and len(os.fsencode(entry[1])) <= 4096:
            result[name] = list(entry)
        else:
            raise Rejected('Unsupported staged root entry')
    if result.get('.', [None])[0] != 'dir':
        raise Rejected('Staged root directory required')
    for name in result:
        if name != '.' and result.get(str(PurePosixPath(name).parent), [None])[0] != 'dir':
            raise Rejected('Staged root parent must be a reviewed directory')
    for name in ('build', 'tmp', 'proc', 'dev'):
        if name not in result:
            result[name] = ['dir', 0o555]
        elif result[name][0] != 'dir':
            raise Rejected('Sandbox mountpoint must be a directory')
    if len(result) > max_entries:
        raise Rejected('Staged root mountpoint entry quota exceeded')
    return result


def copy_tree(source, targetfd, entries, limit, max_entries, checkpoint=lambda: None):
    expected = checked_entries(entries, max_entries)
    sourcefd = os.open(source, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    used = 0
    try:
        for name, entry in sorted(expected.items(), key=lambda pair: (len(PurePosixPath(pair[0]).parts), pair[0])):
            checkpoint()
            if name == '.': continue
            parent = beneath(targetfd, str(PurePosixPath(name).parent), os.O_RDONLY | os.O_DIRECTORY) if '/' in name else os.dup(targetfd)
            leaf = PurePosixPath(name).name
            try:
                if entry[0] == 'dir':
                    os.mkdir(leaf, 0o700, dir_fd=parent)
                elif entry[0] == 'link':
                    # No further operation ever traverses a manifest symlink.
                    os.symlink(entry[1], leaf, dir_fd=parent)
                else:
                    fd = beneath(sourcefd, name)
                    with os.fdopen(fd, 'rb') as stream:
                        before = os.fstat(stream.fileno())
                        if not stat.S_ISREG(before.st_mode) or before.st_size > min(256*1024**2, limit-used):
                            raise Rejected('Staged root byte quota/type violation')
                        used += before.st_size
                        out = os.open(leaf, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=parent)
                        h = hashlib.sha256()
                        with os.fdopen(out, 'wb') as output:
                            remaining = before.st_size
                            while remaining:
                                checkpoint()
                                block = stream.read(min(remaining, 1024**2))
                                if not block: raise Rejected('Staged root input truncated')
                                h.update(block); output.write(block); remaining -= len(block)
                            output.flush(); os.fchmod(output.fileno(), entry[1])
                        after = os.fstat(stream.fileno())
                        if ((before.st_size, before.st_mtime_ns, before.st_ctime_ns) !=
                                (after.st_size, after.st_mtime_ns, after.st_ctime_ns) or h.hexdigest() != entry[2]):
                            raise Rejected('Staged root input changed')
            finally:
                os.close(parent)
        # Keep directories writable only until every child has been created.
        for name, entry in sorted(expected.items(), reverse=True):
            if entry[0] == 'dir':
                fd = os.dup(targetfd) if name == '.' else beneath(targetfd, name, os.O_RDONLY | os.O_DIRECTORY)
                try: os.fchmod(fd, entry[1])
                finally: os.close(fd)
        return expected, used
    finally:
        os.close(sourcefd)


def measure_fd(rootfd, limit, max_entries, checkpoint=lambda: None):
    result = {}; used = 0
    rootinfo = os.fstat(rootfd)
    if rootinfo.st_uid != os.getuid(): raise Rejected('Staged root owner mismatch')
    for directory, dirs, files, fd in os.fwalk('.', dir_fd=rootfd, follow_symlinks=False):
        checkpoint()
        name = str(PurePosixPath(directory))
        result[name] = ['dir', stat.S_IMODE(os.fstat(fd).st_mode)]
        for leaf in dirs + files:
            checkpoint()
            info = os.stat(leaf, dir_fd=fd, follow_symlinks=False)
            name = str(PurePosixPath(directory) / leaf)
            if info.st_dev != rootinfo.st_dev or info.st_uid != os.getuid():
                raise Rejected('Staged root ownership/device mismatch')
            if stat.S_ISLNK(info.st_mode): result[name] = ['link', os.readlink(leaf, dir_fd=fd)]
            elif stat.S_ISREG(info.st_mode):
                used += info.st_size
                if info.st_size > 256*1024**2 or used > limit:
                    raise Rejected('Staged root measurement byte quota exceeded')
                h = hashlib.sha256()
                source = os.open(leaf, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
                with os.fdopen(source, 'rb') as stream:
                    while block := stream.read(1024**2): checkpoint(); h.update(block)
                result[name] = ['file', stat.S_IMODE(info.st_mode), h.hexdigest()]
            elif stat.S_ISDIR(info.st_mode): result[name] = ['dir', stat.S_IMODE(info.st_mode)]
            else: raise Rejected('Special staged root file')
            if len(result) > max_entries: raise Rejected('Staged root measurement entry quota exceeded')
    if len(result) > max_entries: raise Rejected('Staged root measurement entry quota exceeded')
    return result


class StagedRoot:
    def __init__(self, source, entries, helper, limit=1024**3, max_entries=16384, cancel=lambda: False, timeout=240):
        self.fd = -1
        self.mountfd = -1
        self.helper = str(helper)
        self.blobs = []  # FrozenTree.command shares the isolation flags only.
        start = time.monotonic()
        def checkpoint():
            if cancel(): raise Rejected('Cancelled')
            if time.monotonic()-start > timeout: raise Rejected('Root staging timed out')
        if (type(limit) is not int or not 4096 <= limit <= 2*1024**3 or
                type(max_entries) is not int or not 16 <= max_entries <= 65536):
            raise Rejected('Fixed staged root capacity required')
        # Reject malformed trees before starting a namespace helper.
        checked_entries(entries, max_entries)
        checkpoint()
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        process = None
        try:
            process = subprocess.Popen([str(helper), str(limit), str(max_entries), str(child.fileno())],
                pass_fds=(child.fileno(),), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE, env={'PATH':'/usr/bin','LC_ALL':'C'})
            child.close(); parent.settimeout(10)
            data, ancillary, flags, _ = parent.recvmsg(1, socket.CMSG_SPACE(array.array('i').itemsize), socket.MSG_CMSG_CLOEXEC)
            handles = array.array('i')
            for level, kind, body in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    handles.frombytes(body[:len(body)-len(body)%handles.itemsize])
            if data != b'S' or flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC) or len(handles) != 1:
                for fd in handles: os.close(fd)
                raise Rejected('Detached root helper did not deliver one mount FD')
            # Keep the original fsmount open-file description alive. Closing
            # it dissolves its anonymous mount namespace even if a separately
            # reopened directory FD still retains the filesystem's bytes.
            self.mountfd = handles[0]
            self.fd = os.open(f'/proc/self/fd/{self.mountfd}', os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
            fs = os.fstatvfs(self.fd)
            if fs.f_blocks*fs.f_frsize > limit or fs.f_files > max_entries:
                raise Rejected('Detached root kernel quota mismatch')
            expected, self.logical_bytes = copy_tree(source, self.fd, entries, limit, max_entries, checkpoint)
            checkpoint(); parent.sendall(b'F')
            if parent.recv(2) != b'R': raise Rejected('Detached root freeze failed')
            _, error = process.communicate(timeout=10)
            if process.returncode: raise Rejected('Detached root helper failed: '+error.decode(errors='replace')[:500])
            # All namespace/context handles have died before publication.
            fs = os.fstatvfs(self.fd)
            if not fs.f_flag & os.ST_RDONLY or not fs.f_flag & os.ST_NOSUID or not fs.f_flag & os.ST_NODEV:
                raise Rejected('Detached root is not readonly/nosuid/nodev')
            if measure_fd(self.fd, limit, max_entries, checkpoint) != expected:
                raise Rejected('Frozen staged root differs from reviewed bytes')
            self.tree_sha256 = digest(canonical(expected))
            self.entry_count = len(expected)
            self.allocated_bytes = (fs.f_blocks-fs.f_bfree)*fs.f_frsize
            self.mounts = ['--ro-bind-fd', str(self.mountfd), '/']
        except BaseException:
            self.close()
            raise
        finally:
            if process is not None:
                if process.poll() is None: process.kill()
                process.communicate()
            parent.close(); child.close()

    @property
    def fds(self): return (self.mountfd,)
    def command(self, workfd, command, source_date_epoch=None):
        return [self.helper, '--run', str(self.mountfd), '--',
                *FrozenTree.command(self, workfd, command, source_date_epoch)]
    def close(self):
        if self.fd >= 0: os.close(self.fd); self.fd = -1
        if self.mountfd >= 0: os.close(self.mountfd); self.mountfd = -1
    def __enter__(self): return self
    def __exit__(self, *args): self.close()
