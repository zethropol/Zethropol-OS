"""Unprivileged deterministic mountpoint-race regression. /tmp fixtures only."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from quota import QuotaDirectory


def main():
    assert os.getuid()!=0 and os.geteuid()!=0
    with tempfile.TemporaryDirectory(prefix='aur-quota-swap-') as temporary:
        base=Path(temporary); aur=Path(__file__).resolve().parents[1]
        helper=base/'quota'; hook=base/'swap.so'
        subprocess.run(['/usr/bin/cc','-O2','-Wall','-Wextra','-Werror',str(aur/'quota_mount.c'),'-o',str(helper)],check=True)
        subprocess.run(['/usr/bin/cc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',str(Path(__file__).with_name('quota_path_swap_probe.c')),'-ldl','-o',str(hook)],check=True)
        parent=base/'parent';parent.mkdir(mode=0o700)
        victim=base/'victim';(victim/'quota').mkdir(parents=True,mode=0o700)
        sentinel=victim/'quota/sentinel';sentinel.write_bytes(b'preserve')
        original=subprocess.Popen
        def injected(*args,**kwargs):
            kwargs['env']={**kwargs['env'],'LD_PRELOAD':str(hook),'QUOTA_TEST_FROM':str(parent),
                           'QUOTA_TEST_MOVED':str(base/'moved'),'QUOTA_TEST_VICTIM':str(victim)}
            return original(*args,**kwargs)
        with patch('quota.subprocess.Popen',side_effect=injected):
            with QuotaDirectory(helper,parent/'quota',byte_limit=1024**2,inodes=128) as quota:
                assert parent.is_symlink(),'Race hook did not run'
                assert (base/'moved/quota').is_dir()
                assert not (quota.path/'sentinel').exists(),'Caller-selected host directory escaped into quota FD'
                (quota.path/'only-private').write_bytes(b'bounded')
                assert not (victim/'quota/only-private').exists()
                assert sentinel.read_bytes()==b'preserve'
        assert list((base/'moved/quota').iterdir())==[],'Mount propagated to host'
        print('PASS ancestor swapped after pin: quota FD remains private tmpfs; victim untouched; host mount unchanged')


if __name__=='__main__':main()
