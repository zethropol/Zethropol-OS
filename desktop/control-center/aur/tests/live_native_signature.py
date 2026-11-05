"""Non-installing native ALPM probe with a public-only disposable trust copy.

No transaction, key import, key signing, host DB refresh or host GPG home use.
An absent Archive public key is a blocker, never synthesized into trust.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    out=args.output.resolve();out.mkdir(mode=0o700,parents=True,exist_ok=False)
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    native=Path(subprocess.run(['/usr/bin/pacman-conf','GPGDir'],capture_output=True,text=True,check=True).stdout.strip().removeprefix('GPGDir = '))
    policy=subprocess.run(['/usr/bin/pacman-conf','SigLevel'],capture_output=True,text=True,check=True).stdout.splitlines()
    assert 'PackageRequired' in policy and 'PackageTrustedOnly' in policy,policy
    original={name:sha(native/name) for name in ('pubring.kbx','trustdb.gpg')}
    home=out/'public-native';home.mkdir(mode=0o700)
    for name in original:shutil.copyfile(native/name,home/name)
    (home/'gpg.conf').write_text('no-auto-key-retrieve\nno-auto-key-import\nauto-key-locate clear\nno-auto-check-trustdb\n')
    root=out/'root';root.mkdir();db=out/'db';db.mkdir()
    helper=out/'native-probe'
    subprocess.run(['/usr/bin/cc','-O2','-Wall','-Wextra','-Werror',str(Path(__file__).with_name('native_signature_probe.c')),'-lalpm','-o',str(helper)],check=True)
    candidates=[p for p in Path('/var/cache/pacman/pkg').glob('*.pkg.tar.*') if p.is_file() and Path(str(p)+'.sig').is_file()]
    package=min(candidates,key=lambda p:p.stat().st_size)
    target=out/package.name;shutil.copyfile(package,target);shutil.copyfile(str(package)+'.sig',str(target)+'.sig')
    def probe():
        return subprocess.run([str(helper),str(root),str(db),str(home),str(target)],capture_output=True,text=True,timeout=30)
    good=probe();assert good.returncode==0,(good.stdout,good.stderr)
    Path(str(target)+'.sig').write_bytes(b'invalid signature')
    bad=probe();assert bad.returncode!=0
    Path(str(target)+'.sig').unlink()
    missing=probe();assert missing.returncode!=0
    project=json.loads(Path('/etc/zethropol-update/aur-trust/policy.json').read_bytes())
    archive_keys=project['archive']['signers'];present={}
    for fingerprint in archive_keys:
        result=subprocess.run(['/usr/bin/gpg','--no-options','--homedir',str(home),'--batch','--no-autostart',
            '--no-auto-check-trustdb','--no-auto-key-retrieve','--with-colons','--list-keys',fingerprint],capture_output=True,text=True,timeout=15)
        present[fingerprint]=any(line.startswith('fpr:') and line.split(':')[9]==fingerprint for line in result.stdout.splitlines())
        assert (result.returncode==0 if present[fingerprint] else result.returncode==2 and 'No public key' in result.stderr),result.stderr
    assert original=={name:sha(native/name) for name in original},'Host public keyring changed'
    report={'production_accepted':False,'installation_performed':False,'native_transaction_started':False,
            'scope':'isolated native package loader only; no official worker GPG domain change',
            'native_siglevel':policy,'effective_probe_policy':'PackageRequired/TrustedOnly; no optional/marginal/unknown flags',
            'public_snapshot_sha256':original,'host_public_keyring_unchanged':True,
            'sample':{'name':package.name,'sha256':sha(package),'valid_returncode':good.returncode,'native_result':good.stdout.strip()},
            'tampered_signature_returncode':bad.returncode,'missing_signature_returncode':missing.returncode,
            'archive_primary_present_in_native_snapshot':present,'archive_native_install_trust':'blocked; role pin is not native trust'}
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))


if __name__=='__main__':main()
