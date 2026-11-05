"""Read-only host DB observation. A snapshot is NOT a final locked host plan."""
import os
from pathlib import Path
import stat
import subprocess

from sealed import beneath
from worker import Rejected, canonical, digest


def capture_host_view(destination, database='/var/lib/pacman', max_bytes=512*1024**2):
    source=Path(database)
    def unlocked():
        try: (source/'db.lck').lstat()
        except FileNotFoundError: return
        raise Rejected('Host pacman lock exists; observation refused')
    def configuration():
        result=subprocess.run(['/usr/bin/pacman-conf'],env={'PATH':'/usr/bin','LC_ALL':'C'},
            stdin=subprocess.DEVNULL,capture_output=True,timeout=10)
        if result.returncode or len(result.stdout)>1024**2:
            raise Rejected('Cannot observe effective host pacman configuration')
        return result.stdout
    def scan(copy_to=None):
        files={};used=0
        rootfd=os.open(source,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
        try:
            paths=sorted([*source.joinpath('local').rglob('*'),*source.joinpath('sync').rglob('*')])
            for path in paths:
                if path.is_dir() and not path.is_symlink(): continue
                name=str(path.relative_to(source))
                fd=beneath(rootfd,name)
                try:
                    info=os.fstat(fd)
                    if not stat.S_ISREG(info.st_mode) or info.st_size>256*1024**2:
                        raise Rejected('Unsupported host database entry')
                    used+=info.st_size
                    if used>max_bytes: raise Rejected('Host DB observation quota exceeded')
                    chunks=[];offset=0
                    while offset<info.st_size:
                        data=os.pread(fd,min(1024**2,info.st_size-offset),offset)
                        if not data: raise Rejected('Host DB changed while reading')
                        chunks.append(data);offset+=len(data)
                    after=os.fstat(fd)
                    if (info.st_size,info.st_mtime_ns,info.st_ctime_ns)!=(after.st_size,after.st_mtime_ns,after.st_ctime_ns):
                        raise Rejected('Host DB changed while reading')
                    data=b''.join(chunks);files[name]=digest(data)
                    if copy_to is not None:
                        target=copy_to/name;target.parent.mkdir(parents=True,mode=0o700,exist_ok=True)
                        with open(target,'xb') as output: output.write(data)
                finally: os.close(fd)
        finally: os.close(rootfd)
        return files
    unlocked(); config=configuration()
    root=Path(destination);root.mkdir(mode=0o700,exist_ok=False)
    db=root/'db';db.mkdir(mode=0o700)
    first=scan(db);second=scan()
    unlocked()
    if first!=second or config!=configuration(): raise Rejected('Host changed during DB snapshot')
    (root/'effective-pacman.conf').write_bytes(config)
    report={'database_sha256':digest(canonical(first)), 'configuration_sha256':digest(config),
        'files':len(first),'final_host_transaction_verified':False,
        'reason':'Read-only stable snapshot only; real broker must re-resolve under actual host ALPM lock with complete signature/hook/recovery policy'}
    (root/'observation.json').write_bytes(canonical(report))
    return report
