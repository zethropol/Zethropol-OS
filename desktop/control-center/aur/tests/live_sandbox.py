"""Explicit real namespace test; must fail, not skip, when isolation is unavailable."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from worker import tree, harden
from sealed import FrozenTree, sandbox_arguments


def main():
    if os.getuid() == 0:
        raise RuntimeError('Run sandbox test as desktop user')
    with tempfile.TemporaryDirectory(prefix='zethropol-aur-isolation-') as temporary:
        base = Path(temporary)
        root = base/'root'
        (root/'usr/bin').mkdir(parents=True)
        for name in ('build', 'proc', 'dev', 'tmp'):
            (root/name).mkdir()
        subprocess.run(['/usr/bin/cc', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(Path(__file__).with_name('sandbox_probe.c')), '-o', str(root/'usr/bin/probe')], check=True)
        work = base/'work'
        work.mkdir()
        sentinel = base/'host-secret'
        sentinel.write_text('must not be visible')
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        # Host process deliberately has a high PID unrelated to namespace PIDs.
        victim = subprocess.Popen(['/usr/bin/sleep', '30'])
        workfd = os.open(work, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            with FrozenTree(root, tree(root)) as frozen:
                # Replacing source bytes after sealing must not alter sandbox.
                (root/'usr/bin/probe').write_bytes(b'CORRUPTED AFTER SNAPSHOT')
                with sandbox_arguments(frozen.command(workfd, ['/usr/bin/probe', str(sentinel), str(victim.pid), str(listener.getsockname()[1])]), (*frozen.fds, workfd)) as (args, fds):
                    result = subprocess.run(args, pass_fds=fds, preexec_fn=harden,
                        env={'PATH':'/usr/bin'}, capture_output=True, text=True, timeout=20)
                print(result.stdout, end='')
                print(result.stderr, end='', file=sys.stderr)
                if result.returncode:
                    raise RuntimeError(f'Actual sandbox validation failed ({result.returncode})')
            assert victim.poll() is None
            assert (work/'allowed').is_file()
            assert sentinel.read_text() == 'must not be visible'
            print('PASS: real sealed-root namespace isolation; host files/process unchanged')
        finally:
            os.close(workfd)
            listener.close()
            victim.terminate()
            victim.wait()

if __name__ == '__main__':
    main()
