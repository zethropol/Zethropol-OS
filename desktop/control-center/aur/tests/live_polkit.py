"""Read-only, noninteractive actual caller-bound PolicyKit denial probe."""
from pathlib import Path
import subprocess
import tempfile


def main():
    with tempfile.TemporaryDirectory(prefix='zethropol-polkit-denial-') as temporary:
        executable=Path(temporary)/'probe'
        subprocess.run(['/usr/bin/cc','-O2','-Wall','-Wextra','-Werror',str(Path(__file__).with_name('polkit_probe.c')),
            '-lsystemd','-o',str(executable)],check=True)
        subprocess.run([str(executable)],check=True,timeout=15)

if __name__=='__main__': main()
