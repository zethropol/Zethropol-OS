"""Only pre-namespace argument/path rejects; no host mount or root operation."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest


@unittest.skipIf(os.getuid()==0 or os.geteuid()==0,'Guard tests require a normal user')
class QuotaHelperGuards(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary=tempfile.TemporaryDirectory(prefix='quota-helper-guards-')
        cls.base=Path(cls.temporary.name)
        cls.helper=cls.base/'quota-helper'
        subprocess.run(['/usr/bin/cc','-O2','-Wall','-Wextra','-Werror',str(Path(__file__).resolve().parents[1]/'quota_mount.c'),'-o',str(cls.helper)],check=True)

    @classmethod
    def tearDownClass(cls):cls.temporary.cleanup()

    def reject(self,args,fds=()):
        result=subprocess.run([str(self.helper),*args],pass_fds=fds,capture_output=True,timeout=5)
        self.assertEqual(result.returncode,2,result.stderr.decode())

    def test_overflow_signs_whitespace_and_fd_truncation(self):
        for value in ('','+4096',' 4096','-1','18446744073709551616','1073741825'):
            with self.subTest(value=value):self.reject(['/unused',value,'128','3'])
        for value in ('4294967299','1048577','18446744073709551616','+3',' 3','2'):
            with self.subTest(value=value):self.reject(['--enter',value,'4','--','/usr/bin/bwrap','--version'])

    def test_arbitrary_executable_is_rejected(self):
        self.reject(['--enter','3','4','--','/bin/sh','-c','exit 0'])

    def test_channel_must_be_open_seqpacket(self):
        self.reject(['/unused','4096','16','1048576'])
        left,right=socket.socketpair(socket.AF_UNIX,socket.SOCK_STREAM)
        try:self.reject(['/unused','4096','16',str(right.fileno())],(right.fileno(),))
        finally:left.close();right.close()

    def test_leaf_and_ancestor_symlinks_traversal_rejected(self):
        with tempfile.TemporaryDirectory(dir=self.base) as directory:
            base=Path(directory);real=base/'real';real.mkdir(mode=0o700);(real/'quota').mkdir(mode=0o700)
            link=base/'link';link.symlink_to(real)
            left,right=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
            try:
                for path in (str(link),str(link/'quota'),str(base)+'/real/../real/quota'):
                    with self.subTest(path=path):self.reject([path,'4096','16',str(right.fileno())],(right.fileno(),))
            finally:left.close();right.close()

    def test_shared_mountpoint_is_rejected(self):
        with tempfile.TemporaryDirectory(dir=self.base) as directory:
            path=Path(directory);path.chmod(0o755)
            left,right=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
            try:self.reject([str(path),'4096','16',str(right.fileno())],(right.fileno(),))
            finally:left.close();right.close()


if __name__=='__main__':unittest.main()
