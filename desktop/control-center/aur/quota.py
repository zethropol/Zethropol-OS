"""Per-operation kernel byte/inode quota. Requires user namespaces; no fallback."""
import array
import os
from pathlib import Path
import socket
import stat
import subprocess

from worker import Rejected


class QuotaDirectory:
    def __init__(self, helper, mountpoint, byte_limit=512*1024**2, inodes=16384):
        self.fd=-1
        self.namespaces=()
        Path(mountpoint).mkdir(mode=0o700,exist_ok=False)
        parent,child=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
        try:
            process=subprocess.Popen([str(helper),str(mountpoint),str(byte_limit),str(inodes),str(child.fileno())],
                pass_fds=(child.fileno(),),stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,
                env={'PATH':'/usr/bin','LC_ALL':'C'})
            child.close(); parent.settimeout(10)
            try:
                data,ancillary,flags,_=parent.recvmsg(1,socket.CMSG_SPACE(3*array.array('i').itemsize))
                fds=array.array('i')
                for level,kind,body in ancillary:
                    if level==socket.SOL_SOCKET and kind==socket.SCM_RIGHTS:
                        fds.frombytes(body[:len(body)-len(body)%fds.itemsize])
                if data!=b'Q' or flags & (socket.MSG_TRUNC|socket.MSG_CTRUNC) or len(fds)!=3:
                    for fd in fds: os.close(fd)
                    raise Rejected('Quota helper did not deliver directory/user/mount FDs')
                self.fd=fds[0]; self.namespaces=tuple(fds[1:])
                for handle in fds: os.set_inheritable(handle,False)
                _,error=process.communicate(timeout=10)
                if process.returncode or not stat.S_ISDIR(os.fstat(self.fd).st_mode):
                    raise Rejected('Quota mount failed: '+error.decode(errors='replace')[:1000])
                filesystem=os.fstatvfs(self.fd)
                if filesystem.f_blocks*filesystem.f_frsize>byte_limit or filesystem.f_files>inodes:
                    raise Rejected('Kernel quota exceeds requested limit')
                self.path=Path(f'/proc/self/fd/{self.fd}')
                self.limit=byte_limit
            except BaseException:
                if process.poll() is None: process.kill()
                process.communicate()
                self.close()
                raise
        finally:
            parent.close(); child.close()

    def close(self):
        if self.fd>=0: os.close(self.fd); self.fd=-1
        for handle in self.namespaces: os.close(handle)
        self.namespaces=()
    def __enter__(self): return self
    def __exit__(self,*args): self.close()


from scoped import ScopedWorker


class QuotaWorker(ScopedWorker):
    """All recipe/download/build bytes live on a kernel-bounded tmpfs.

    Host exports are bounded to 256 MiB/64 artifacts; worker.log to 8 MiB.
    Mounted root/tmp scratch remain separately bounded. No quota fallback.
    """
    def __init__(self, store, helper):
        super().__init__(store)
        self.helper=helper
        self.quota=None
        self.exported=0
        self.exports=0

    def create_workspace(self, logdir):
        self.quota=QuotaDirectory(self.helper, logdir/'quota')
        return self.quota.path

    def open_workspace(self, work):
        if self.quota is None: raise Rejected('Missing quota mount')
        return os.dup(self.quota.fd)

    def freeze_root(self, root, entries, limit, max_entries, cancel):
        # Only the authenticated manifest version (set by pipeline.run) selects
        # this backend. Neither a request field nor source-plan schema can do so.
        if getattr(self, 'root_schema', 1) == 2:
            from staged_root import StagedRoot
            return StagedRoot(root, entries, '/usr/lib/zethropol/aur-root-stage',
                              limit=limit, max_entries=max_entries, cancel=cancel)
        return super().freeze_root(root, entries, limit, max_entries, cancel)

    def execute(self, args, logdir, cancel, timeout):
        if self.quota is None: raise Rejected('Quota must exist before execution')
        old=getattr(self,'_sandbox_fds',())
        self._sandbox_fds=(*old,*self.quota.namespaces)
        try:
            super().execute([str(self.helper),'--enter',*(str(fd) for fd in self.quota.namespaces),'--',*args], logdir,cancel,timeout)
        finally:
            self._sandbox_fds=old

    def archive_result(self, archive, name, work, logdir):
        if self.exported+archive.size>256*1024**2 or self.exports>=64:
            raise Rejected('Aggregate artifact quota exceeded')
        destination=logdir/'artifacts'; destination.mkdir(mode=0o700,exist_ok=True)
        path=destination/name
        with open(path,'xb') as output:
            for offset in range(0,archive.size,1024**2):
                output.write(os.pread(archive.fd,min(1024**2,archive.size-offset),offset))
            output.flush(); os.fsync(output.fileno())
        path.chmod(0o400)
        self.exported+=archive.size; self.exports+=1
        return {'path':str(path),'sha256':archive.sha256}

    def release_workspace(self):
        if self.quota is not None:
            self.quota.close(); self.quota=None
