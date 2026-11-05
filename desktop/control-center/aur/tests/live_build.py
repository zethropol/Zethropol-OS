"""Real benign makepkg build in sealed root; no host package install.

LAB ONLY: root files are copied from the currently installed host, hashed, and
sealed. This is NOT signed-repository provisioning or a production trust root.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import worker
from sealed import Blob
from journal import Supervisor
from quota import QuotaWorker


def copy_runtime(root):
    copied = set()
    def copy(path):
        requested = Path(path)
        source = requested.resolve(strict=True)
        alias = root / str(requested).lstrip('/')
        if requested != source and not alias.exists():
            alias.parent.mkdir(parents=True, exist_ok=True)
            alias.symlink_to(str(source))
        if source in copied: return
        copied.add(source)
        target = root / str(source).lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        target.chmod(source.stat().st_mode & 0o777)
        if source.read_bytes()[:4] == b'\x7fELF':
            result = subprocess.run(['/usr/bin/ldd', str(source)], capture_output=True, text=True)
            for dependency in re.findall(r'(?:=>\s*)?(/[^\s()]+)', result.stdout):
                copy(dependency)
    tools = 'getopt sh bash pacman makepkg fakeroot faked bsdtar gzip xz zstd bzip2 find grep sed awk readlink realpath install mkdir chmod cp rm mv cat head sort tee touch uname date stat du wc sha256sum file gettext tput locale tr cut tail ln env id cmp basename dirname od xargs printf'.split()
    for tool in tools:
        path = Path('/usr/bin')/tool
        if not path.exists(): raise RuntimeError('Missing installed lab tool: '+tool)
        copy(path)
        canonical = str(path.resolve()).lstrip('/')
        target = root/'usr/bin'/tool
        if not target.exists(): target.symlink_to('/'+canonical)
    for path in Path('/usr/lib/libfakeroot').glob('*.so*'): copy(path)
    for source, destination in [('/usr/share/makepkg', 'usr/share/makepkg'),
                                ('/usr/share/pacman', 'usr/share/pacman')]:
        if Path(source).exists(): shutil.copytree(source, root/destination, symlinks=False)
    magic = Path('/usr/share/file/misc/magic.mgc')
    if magic.exists():
        (root/'usr/share/file/misc').mkdir(parents=True, exist_ok=True)
        shutil.copyfile(magic, root/'usr/share/file/misc/magic.mgc')
    for name, target in [('bin','usr/bin'), ('sbin','usr/bin'), ('lib','usr/lib'), ('lib64','usr/lib'), ('usr/sbin','bin')]:
        destination = root/name
        if not destination.exists(): destination.symlink_to(target)
    for name in ('build','proc','dev','tmp','etc','var/lib/pacman/local'):
        (root/name).mkdir(parents=True, exist_ok=True)
    (root/'var/lib/pacman/local/ALPM_DB_VERSION').write_text('9\n')
    (root/'etc/passwd').write_text(f'builder:x:{os.getuid()}:{os.getgid()}::/build:/usr/bin/bash\n')
    (root/'etc/group').write_text(f'builder:x:{os.getgid()}:\n')
    (root/'etc/pacman.conf').write_text('[options]\nArchitecture = x86_64\nSigLevel = Required DatabaseRequired\n')
    (root/'etc/makepkg.conf').write_text('''CARCH="x86_64"
CHOST="x86_64-pc-linux-gnu"
BUILDENV=('!distcc' '!color' '!ccache' '!check' '!sign')
OPTIONS=('!strip' '!docs' '!libtool' '!staticlibs' 'emptydirs' '!zipman' '!purge' '!debug' '!lto' '!autodeps')
INTEGRITY_CHECK=(sha256)
PKGEXT='.pkg.tar'
SRCEXT='.src.tar'
COMPRESSGZ=(gzip -c -f -n)
COMPRESSXZ=(xz -c -z -)
COMPRESSZST=(zstd -c -z -q -)
''')


def main(root_setup=None, archive_check=None):
    if os.getuid()==0: raise RuntimeError('Desktop user only')
    with tempfile.TemporaryDirectory(prefix='zethropol-aur-real-build-') as temporary:
        base = Path(temporary)
        root = base/'root'
        if root_setup is None:
            root.mkdir(mode=0o700)
            copy_runtime(root)
            print('LAB host-copy root; no packages installed', flush=True)
        else:
            root_setup(root)
            print('LAB detached-signature verified package/manifest root; no packages installed', flush=True)
        recipe = base/'recipe'; recipe.mkdir()
        (recipe/'PKGBUILD').write_text('''pkgname=zethropol-aur-lab-fixture
pkgver=1
pkgrel=1
pkgdesc='Disposable isolation test only'
arch=('any')
license=('MIT')
source=('payload')
sha256sums=('''+"'"+worker.digest(b'lab payload\n')+"'"+''')
package() {
  test "$(awk '/^Uid:/{print $2}' /proc/self/status)" != 0 || return 1
  test ! -e /etc/shadow || return 1
  install -Dm644 "$srcdir/payload" "$pkgdir/usr/share/zethropol-aur-lab/payload"
}
''')
        (recipe/'.SRCINFO').write_text('pkgbase = zethropol-aur-lab-fixture\n\tpkgver = 1\n\tpkgrel = 1\n\tarch = any\npkgname = zethropol-aur-lab-fixture\n')
        (recipe/'payload').write_bytes(b'lab payload\n')
        store = base/'operations'; store.mkdir(mode=0o700)
        # This explicit LAB trust override permits user-owned copied fixtures.
        # It is absent from Worker and is never used for a real AUR recipe.
        with patch.object(worker, 'trusted_entry', return_value=None):
            plan, approved = worker.review(recipe, root, {'commit':'a'*40},
                [{'name':'payload','url':'','sha256':worker.digest(b'lab payload\n')}], [])
            helper=base/'quota-helper'
            subprocess.run(['/usr/bin/cc','-O2','-Wall','-Wextra','-Werror',str(Path(__file__).resolve().parents[1]/'quota_mount.c'),'-o',str(helper)],check=True)
            supervisor = Supervisor(store)
            try:
                result = supervisor.run(QuotaWorker(store, helper), plan, approved, recipe, root, timeout=60)
                print(result, flush=True)
                if result['state'] != 'built-untrusted':
                    print((store/result['operation']/'worker.log').read_text(), file=sys.stderr)
                    raise RuntimeError('Real makepkg sandbox build failed')
                assert supervisor.journal.status(result['operation'])['state'] == 'built-untrusted'
            finally:
                supervisor.journal.close()
        archive = Path(result['archives'][0]['path'])
        fd = os.open(archive, os.O_RDONLY | os.O_NOFOLLOW)
        try:
            with Blob(fd) as sealed:
                if archive_check is not None: archive_check(archive, sealed, base)
                # Swap the worker path after handoff. The verifier sees only
                # sealed original bytes, not this replacement or worker digest.
                archive.unlink()
                archive.write_bytes(b'changed after handoff')
                db = base/'alpm-root'; db.mkdir(mode=0o700)
                (db/'db/local').mkdir(parents=True, mode=0o700)
                (db/'db').chmod(0o700)
                (db/'db/local/ALPM_DB_VERSION').write_text('9\n')
                for signed in (False, True):
                    executable = base/('signed-preflight' if signed else 'unsigned-fixture-preflight')
                    extra = [] if signed else ['-DZETHROPOL_TEST_UNSIGNED=1']
                    subprocess.run(['/usr/bin/cc','-O2','-Wall','-Wextra','-Werror',*extra,
                        str(Path(__file__).resolve().parents[1]/'prepare_only.c'),'-lalpm','-o',str(executable)], check=True)
                    checked = subprocess.run([str(executable),str(db),str(db/'db'),'x86_64','-',str(sealed.fd)],
                        pass_fds=(sealed.fd,),capture_output=True,text=True,timeout=10)
                    print(('SIGNED DEFAULT' if signed else 'UNSIGNED LAB FIXTURE'), checked.stdout, checked.stderr, flush=True)
                    if signed:
                        assert checked.returncode != 0 and 'signature' in checked.stderr
                    else:
                        assert checked.returncode == 0, checked.stderr
        finally:
            os.close(fd)
        try: worker.InstallationBroker().install(result)
        except worker.Rejected: print('PASS: real build -> journal -> sealed FD -> ALPM; unsigned/signature and install gates CLOSED')
        else: raise AssertionError('Production installation unexpectedly enabled')

if __name__ == '__main__': main()
