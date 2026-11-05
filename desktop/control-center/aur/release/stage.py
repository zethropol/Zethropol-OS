"""Assemble a reviewable payload in a NEW workspace directory. Never install."""
import argparse
import hashlib
import json
import os
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
AUR = REPO / 'desktop/control-center/aur'
UPDATE = REPO / 'services/update'
METHODS = ('PrepareAurSelection', 'GetAurProvisioning', 'SubmitAurBuild', 'GetAurAttestation', 'ReleaseAurBuild')


def stage(output, broker, control_build, quota_helper, root_stage_helper):
    build = REPO / 'desktop/control-center/build'
    output = output.absolute()
    if output.parent.resolve() != build.resolve() or output.exists() or output.is_symlink():
        raise ValueError('A new direct child of desktop/control-center/build is required; existing outputs are preserved')
    cache = (control_build / 'CMakeCache.txt').read_text()
    if 'ZETHROPOL_AUR_PIPELINE_PATH:FILEPATH=/usr/lib/zethropol/aur/pipeline.py\n' not in cache:
        raise ValueError('Control Center candidate must use the packaged pipeline path')
    for file in (broker, control_build / 'control_center', quota_helper, root_stage_helper):
        if not file.is_file() or not os.access(file, os.X_OK):
            raise ValueError('Missing built executable: ' + str(file))
    output.mkdir(mode=0o700)
    image = output / 'rootfs'
    rows = []

    def put(target, data, mode=0o644):
        path = image / target.lstrip('/')
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('xb') as stream:
            stream.write(data)
        path.chmod(mode)
        deployed = Path(target)
        try:
            if deployed.is_symlink(): existing = {'type': 'symlink', 'requires_separate_review': True}
            elif deployed.is_file(): existing = {'type': 'file', 'sha256': hashlib.sha256(deployed.read_bytes()).hexdigest()}
            else: existing = {'type': 'absent-in-observed-filesystem'}
        except OSError as error: existing = {'type': 'unreadable', 'reason': str(error)}
        rows.append({'target': target, 'mode': oct(mode), 'owner_after_approved_deployment': 'root:root',
                     'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data), 'observed_existing': existing})

    put('/usr/lib/zethropol/zethropol-update', broker.read_bytes(), 0o755)
    put('/usr/bin/zethropol-control-center', (control_build / 'control_center').read_bytes(), 0o755)
    put('/usr/lib/zethropol/aur-quota-mount', quota_helper.read_bytes(), 0o755)
    put('/usr/lib/zethropol/aur-root-stage', root_stage_helper.read_bytes(), 0o755)
    for name in ('pipeline', 'worker', 'journal', 'scoped', 'quota', 'sealed', 'trust', 'provision', 'root_limits', 'staged_root', 'manifest_workflow', 'rebuild_acceptance'):
        put('/usr/lib/zethropol/aur/' + name + '.py', (AUR / (name + '.py')).read_bytes())
    put('/usr/lib/zethropol/aur/preflight.py', (AUR / 'release/preflight.py').read_bytes())
    put('/usr/lib/zethropol/aur/provider.py',(AUR/'provider.py').read_bytes())
    for name in ('zethropol-aur-provider@.service','zethropol-aur-sign@.service','zethropol-aur-attest@.service'):
        put('/usr/lib/systemd/system/'+name,(AUR/'release/providers'/name).read_bytes())
    put('/usr/lib/sysusers.d/zethropol-aur-providers.conf',(AUR/'release/providers/zethropol-aur-providers.sysusers').read_bytes())
    put('/usr/share/doc/zethropol-update/native-gpg.conf.example',(UPDATE/'deployment/aur/native-gpg.conf.example').read_bytes())
    put('/usr/share/doc/zethropol-update/aur-providers.json.example',(AUR/'release/providers/aur-providers.json.example').read_bytes())
    put('/usr/share/doc/zethropol-update/aur-manifest-signer.json.example',(AUR/'release/providers/aur-manifest-signer.json.example').read_bytes())
    for name in ('policy.json.template','distribution.json.template'):
        put('/usr/share/doc/zethropol-update/'+name,(AUR/'release/providers'/name).read_bytes())

    policy = (UPDATE / 'deployment/dbus/org.zethropol.Update.conf').read_text()
    anchor = '    <allow send_destination="org.zethropol.Update" send_interface="org.freedesktop.DBus.Introspectable"/>'
    if policy.count(anchor) != 1:
        raise ValueError('D-Bus policy seam changed; no broad allow rule will be generated')
    additions = ''.join('    <allow send_destination="org.zethropol.Update" send_interface="org.zethropol.Update.v1" send_member="' + method + '"/>\n' for method in METHODS)
    put('/etc/dbus-1/system.d/org.zethropol.Update.conf', policy.replace(anchor, additions + anchor).encode())
    for source, target in (
        ('dbus/org.zethropol.Update.service', '/usr/share/dbus-1/system-services/org.zethropol.Update.service'),
        ('polkit/org.zethropol.update.policy', '/usr/share/polkit-1/actions/org.zethropol.update.policy'),
        ('systemd/zethropol-update.service', '/usr/lib/systemd/system/zethropol-update.service'),
        ('systemd/zethropol-update-worker@.service', '/usr/lib/systemd/system/zethropol-update-worker@.service'),
    ):
        put(target, (UPDATE / 'deployment' / source).read_bytes())
    put('/etc/systemd/system/zethropol-update.service.d/20-aur-runtime.conf', b'[Service]\nReadWritePaths=/var/lib/zethropol/aur-transactions /var/lib/zethropol/aur-deliveries /run/zethropol-update/aur-artifacts\n')
    put('/usr/lib/tmpfiles.d/zethropol-aur.conf', b'''# Create only after approved deployment; no deletion/cleanup rules.
d /var/lib/zethropol 0755 root root -
d /var/lib/zethropol/aur-transactions 0700 root root -
d /var/lib/zethropol/aur-approved-reviews 0700 root root -
d /var/lib/zethropol/aur-deliveries 0700 root root -
d /var/lib/zethropol/aur-build-roots 0755 root root -
d /run/zethropol-update 0700 root root -
d /run/zethropol-update/aur-artifacts 0700 root root -
d /var/lib/zethropol/aur-provider-jobs 0750 root zethropol-aur-providers -
d /var/lib/zethropol/aur-provider-inputs 0755 root root -
d /var/lib/zethropol-aur-archive 0700 zethropol-aur-archive zethropol-aur-archive -
d /var/lib/zethropol-aur-archive/work 0700 zethropol-aur-archive zethropol-aur-archive -
d /var/lib/zethropol-aur-archive/gnupg 0700 zethropol-aur-archive zethropol-aur-archive -
d /var/lib/zethropol-aur-attestor 0700 zethropol-aur-attestor zethropol-aur-attestor -
d /var/lib/zethropol-aur-attestor/work 0700 zethropol-aur-attestor zethropol-aur-attestor -
d /var/lib/zethropol-aur-attestor/gnupg 0700 zethropol-aur-attestor zethropol-aur-attestor -
''')
    launcher = (REPO / 'desktop/control-center/zethropol-control-center.desktop').read_text()
    # The reviewed candidate has one canonical installed executable, no source-tree Exec path.
    launcher = '\n'.join('Exec=/usr/bin/zethropol-control-center' if line.startswith('Exec=') else line for line in launcher.splitlines()) + '\n'
    put('/usr/share/applications/zethropol-control-center.desktop', launcher.encode())
    manifest = {'schema': 1, 'installation_authority': 'conditional-not-activated', 'activated': False,
                'production_trust_included': False, 'provider_included': True,
                'configuration_required_not_in_payload': ['/etc/zethropol-update/aur-providers.json',
                    '/etc/zethropol-update/aur-trust/policy.json','/etc/zethropol-update/aur-trust/distribution.json',
                    '/etc/zethropol-update/aur-trust/manifest.gpg','/etc/zethropol-update/aur-trust/archive.gpg',
                    '/etc/zethropol-update/aur-trust/receipt.gpg','/etc/zethropol-update/aur-trust/distribution.gpg',
                    '/etc/zethropol-update/aur-native-trust'],

                'files': sorted(rows, key=lambda row: row['target']),
                'approval_required_actions': ['provide durable transaction-journal storage outside root snapshot rollback', 'create the two declared provider accounts and group with sysusers',
                    'prepare isolated software keys and agents under the two private role homes',
                    'approve real role public fingerprints, keyring hashes and native output-signature trust',
                    'validate PAM/user-manager runtime for the provider accounts', 'back up existing deployed files', 'review active unit fragment/overrides',
                    'install reviewed payload with declared root ownership/modes', 'create declared state directories',
                    'publish separately approved public trust and signed provider inputs', 'reload D-Bus policy and systemd units',
                    'restart Update service only when no transaction is active', 'start installed Control Center explicitly'],
                'blocking_conditions': ['no approved independent provider/public keys', 'real per-transaction authorization/recovery conditions not yet exercised',
                    'live authorization/worker/recovery acceptance missing']}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def package_recipe(output):
    """Offline makepkg input only. Does not run makepkg/pacman or sign."""
    payload = output / 'zethropol-update-aur-candidate-0.1.0-1-payload.tar'
    def metadata(info):
        info.uid=info.gid=0;info.uname=info.gname='root';info.mtime=0
        return info
    with tarfile.open(payload,'w') as archive:
        archive.add(output/'rootfs',arcname='rootfs',filter=metadata)
    checksum=hashlib.sha256(payload.read_bytes()).hexdigest()
    recipe="""# Unsigned, inactive candidate. No install script or activation hooks.
pkgname=zethropol-update-aur-candidate
pkgver=0.1.0
pkgrel=1
pkgdesc='Zethropol Update/AUR candidate; production inputs and transaction authorization required'
arch=('x86_64')
license=('MIT')
depends=('glibc' 'dbus' 'systemd' 'polkit' 'snapper' 'util-linux' 'pacman' 'python' 'bubblewrap' 'gnupg' 'qt6-base' 'qt6-declarative' 'pambase' 'coreutils')
provides=('zethropol-update=0.1.0' 'zethropol-control-center=0.1.0')
conflicts=('zethropol-update' 'zethropol-control-center')
source=('zethropol-update-aur-candidate-0.1.0-1-payload.tar')
sha256sums=('%s')
package() {
    cp -a "$srcdir/rootfs/." "$pkgdir/"
}
""" % checksum
    (output/'PKGBUILD').write_text(recipe)
    (output/'NO_ACTIVATION.txt').write_text('Candidate only. No production trust/private keys included. Providers are packaged but inactive. Do not install merely because compilation passed. No package install, key import, service restart or AUR transaction has been run. Review manifest.json and RELEASE_READINESS.md first.\n')
    return checksum


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--broker', type=Path, required=True)
    parser.add_argument('--control-center-build', type=Path, required=True)
    parser.add_argument('--quota-helper', type=Path, required=True)
    parser.add_argument('--root-stage-helper', type=Path, required=True)
    args = parser.parse_args()
    result = stage(args.output, args.broker, args.control_center_build, args.quota_helper, args.root_stage_helper)
    package_recipe(args.output)
    print(json.dumps({'files': len(result['files']), 'output': str(args.output), 'activated': False, 'installation_authority': 'conditional-not-activated'}))
