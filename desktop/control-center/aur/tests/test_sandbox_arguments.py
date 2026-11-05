"""Bounded transport tests; no package, key, service or mount operation."""
import fcntl
import os
from pathlib import Path
import subprocess
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sealed import sandbox_arguments, SEALS
from worker import Rejected


class SandboxArguments(unittest.TestCase):
    def test_large_arguments_are_sealed_and_exact_and_closed(self):
        tail = ['--dir', '/'+('a'*2048)] * 2000 + ['--', '/usr/bin/true']
        self.assertGreater(sum(len(arg)+1 for arg in tail), os.sysconf('SC_ARG_MAX'))
        with sandbox_arguments(['/usr/bin/bwrap', *tail], (0,)) as (args, fds):
            self.assertEqual(args[:2], ['/usr/bin/bwrap', '--args'])
            fd = int(args[2]); self.assertEqual(fds, (0, fd))
            self.assertEqual(fcntl.fcntl(fd, fcntl.F_GET_SEALS) & SEALS, SEALS)
            self.assertEqual(args[3:], ['--','/usr/bin/true'])
            self.assertEqual(os.pread(fd, os.fstat(fd).st_size, 0), b'\0'.join(os.fsencode(a) for a in tail[:-2])+b'\0')
            with self.assertRaises(OSError): os.write(fd, b'tamper')
        with self.assertRaises(OSError): os.fstat(fd)

    def test_scope_prefix_is_preserved(self):
        prefix=['/usr/bin/systemd-run','--user','--scope','--property=MemoryMax=2G','--']
        with sandbox_arguments([*prefix,'/usr/bin/bwrap','--unshare-all','--','/usr/bin/true']) as (args, fds):
            self.assertEqual(args[:len(prefix)+1], [*prefix,'/usr/bin/bwrap'])
            self.assertEqual(args[-2:], ['--','/usr/bin/true'])
            self.assertEqual(len(fds),1)

    def test_quotas_nul_and_failure_cleanup(self):
        for tail in (['a']*9001, ['a'*(16*1024**2)], ['abc\0def']):
            with self.assertRaises(Rejected):
                with sandbox_arguments(['/usr/bin/bwrap',*tail]): self.fail('Accepted unsafe input')
        with self.assertRaisesRegex(RuntimeError,'spawn failure'):
            with sandbox_arguments(['/usr/bin/bwrap','--version']) as (_,fds):
                fd=fds[-1]
                raise RuntimeError('spawn failure')
        with self.assertRaises(OSError): os.fstat(fd)

    def test_real_bwrap_parser_without_namespace_or_install(self):
        # --version exits during parsing, before any mounts or namespaces.
        # The preceding options still exercise a real payload above ARG_MAX.
        tail=['--dir','/'+('a'*2048)]*2000+['--version']
        with sandbox_arguments(['/usr/bin/bwrap',*tail]) as (args,fds):
            result=subprocess.run(args,pass_fds=fds,capture_output=True,timeout=5,check=True)
            self.assertIn(b'bubblewrap',result.stdout)

    def test_other_commands_are_unchanged(self):
        args=['/usr/bin/true']
        with sandbox_arguments(args,(0,)) as result: self.assertEqual(result,(args,(0,)))


if __name__ == '__main__': unittest.main()
