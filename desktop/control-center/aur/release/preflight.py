"""Read-only readiness inventory; never provisions, signs, enables or installs."""
import json
import os
from pathlib import Path
import shutil
import sys

_here = Path(__file__).resolve().parent
# Source layout keeps this read-only tool under release/; staged layout places
# it next to trust.py. Neither layout may import modules from the caller's cwd.
sys.path.insert(0, str(_here.parent if _here.name == 'release' else _here))
from trust import TrustPolicy


def inspect():
    checks = []
    role_signers = {}
    for role in ('manifest', 'receipt', 'archive', 'distribution'):
        path = '/etc/zethropol-update/aur-trust/' + ('distribution.json' if role == 'distribution' else 'policy.json')
        try:
            policy = TrustPolicy.load(path, role=None if role == 'distribution' else role)
            try:
                role_signers[role]=policy.signers
                checks.append({'check': role + '-public-trust', 'ok': True, 'signer_count': len(policy.signers)})
            finally:
                policy.keyring.close()
        except (OSError, ValueError, TypeError, KeyError, RuntimeError) as error:
            checks.append({'check': role + '-public-trust', 'ok': False, 'reason': str(error)})
    if all(role in role_signers for role in ('manifest','archive','receipt')):
        checks.append({'check':'independent-receipt-role','ok':not bool(role_signers['receipt'] & (role_signers['manifest']|role_signers['archive']))})
    try:
        from provider import configuration
        config,_=configuration()
        approved=all(config[role]['signer'] in role_signers.get(role,set()) for role in ('archive','receipt'))
        checks.append({'check':'provider-accounts-and-identities','ok':approved})
    except (OSError,ValueError,TypeError,KeyError,RuntimeError) as error:
        checks.append({'check':'provider-accounts-and-identities','ok':False,'reason':str(error)})
    for directory, mode in (
        ('/var/lib/zethropol/aur-transactions', 0o700),
        ('/var/lib/zethropol/aur-approved-reviews', 0o700),
        ('/var/lib/zethropol/aur-deliveries', 0o700),
        ('/var/lib/zethropol/aur-build-roots', 0o755),
        ('/run/zethropol-update/aur-artifacts', 0o700),
        ('/etc/zethropol-update/aur-native-trust', 0o755),
    ):
        try:
            info = Path(directory).lstat()
            import stat
            ok = stat.S_ISDIR(info.st_mode) and info.st_uid == 0 and stat.S_IMODE(info.st_mode) == mode
            checks.append({'check': directory, 'ok': ok})
        except OSError as error:
            checks.append({'check': directory, 'ok': False, 'reason': str(error)})
    for name in ('pubring.kbx','trustdb.gpg','gpg.conf'):
        path=Path('/etc/zethropol-update/aur-native-trust')/name
        try:
            import stat
            info=path.lstat()
            ok=stat.S_ISREG(info.st_mode) and info.st_uid==0 and not info.st_mode&0o022 and info.st_nlink==1
            checks.append({'check':str(path),'ok':ok})
        except OSError as error:
            checks.append({'check':str(path),'ok':False,'reason':str(error)})
    for command in ('gpg', 'gpgv', 'bwrap', 'systemd-run', 'systemctl', 'python3', 'timeout'):
        checks.append({'check': command, 'ok': shutil.which(command, path='/usr/bin') is not None})
    for helper in ('/usr/lib/zethropol/aur-root-stage','/usr/lib/zethropol/aur-quota-mount'):
        try:
            from provider import root_path
            path=root_path(helper)
            checks.append({'check':helper,'ok':path.is_file() and os.access(path,os.X_OK)})
        except (OSError,RuntimeError) as error:
            checks.append({'check':helper,'ok':False,'reason':str(error)})
    return {'installation': 'closed', 'ready_to_install': False,
            'public_prerequisites': 'blocked' if any(not item['ok'] for item in checks) else 'verified',
            'authority_gate': 'closed pending real production native-domain/PolicyKit/snapshot/boot/recovery acceptance; inventory is not a grant',
            'physical_key_required': False,
            'independent_provider': 'executable independent rebuild provider packaged; real identities/keys and runtime deployment still required',
            'signed_artifacts': 'not-certified-by-this-inventory',
            'checks': checks}


if __name__ == '__main__':
    if len(sys.argv) != 1:
        raise SystemExit('Read-only inventory takes no arguments')
    print(json.dumps(inspect(), indent=2, sort_keys=True))
    raise SystemExit(2)  # An inventory never grants transaction authorization.
