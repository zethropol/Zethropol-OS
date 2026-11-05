"""Real kernel aggregate byte/inode exhaustion. No root/host mounts."""
import errno
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from quota import QuotaDirectory


def main():
    with tempfile.TemporaryDirectory(prefix='zethropol-quota-test-') as temporary:
        base=Path(temporary); helper=base/'quota-helper'
        subprocess.run(['/usr/bin/cc','-O2','-Wall','-Wextra','-Werror',str(Path(__file__).resolve().parents[1]/'quota_mount.c'),'-o',str(helper)],check=True)
        with QuotaDirectory(helper,base/'bytes',byte_limit=1024*1024,inodes=128) as quota:
            written=0
            try:
                # Multiple files: a per-file limit cannot satisfy this check.
                for i in range(128):
                    with open(quota.path/str(i),'xb') as output:
                        output.write(b'x'*65536)
                    written+=65536
            except OSError as error:
                assert error.errno==errno.ENOSPC, error
            else: raise AssertionError('Aggregate byte quota was not enforced')
            assert written<=1024*1024
            assert not list((base/'bytes').iterdir()),'Private mount escaped into host mount namespace'
            print('PASS aggregate multi-file ENOSPC; host mountpoint unchanged',flush=True)
        with QuotaDirectory(helper,base/'nodes',byte_limit=1024*1024,inodes=32) as quota:
            try:
                for i in range(64): (quota.path/str(i)).touch()
            except OSError as error:
                assert error.errno==errno.ENOSPC,error
            else: raise AssertionError('Inode quota was not enforced')
            print('PASS inode ENOSPC',flush=True)

if __name__=='__main__': main()
