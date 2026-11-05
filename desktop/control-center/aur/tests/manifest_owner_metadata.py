"""Read-only pre/post-install inventory. Never opens keys or agent sockets.

Returns 2 while any ownership evidence is unavailable. Even an all-green
inventory is not signing permission or proof of private-key availability.
"""
import argparse
import json
import pwd
import grp
from pathlib import Path
import stat
import shutil
import subprocess
import os

OWNER='zethropol-aur-release'
GPG_HOME=Path('/var/lib/zethropol-aur-release/gnupg')


def inspect():
    result={'scope':'public identity and directory metadata only','private_key_accessed':False,
            'agent_contacted':False,'signing_authorized':False,'owner_key_binding_source':'explicit owner statement; no secret-key probe',
            'checks':[]}
    checks=result['checks']
    try:
        owner=pwd.getpwnam(OWNER)
        roles={user:pwd.getpwnam(user).pw_uid for user in ('zethropol-aur-archive','zethropol-aur-attestor')}
        checks.append({'name':'nonroot-role-separation','ok':owner.pw_uid==949 and owner.pw_uid not in roles.values(),
                       'owner_uid':owner.pw_uid,'other_uids':roles})
        checks.append({'name':'nologin','ok':owner.pw_shell in ('/usr/bin/nologin','/bin/nologin')})
        checks.append({'name':'account-home','ok':owner.pw_dir==str(GPG_HOME.parent)})
        groups=[g.gr_name for g in grp.getgrall() if OWNER in g.gr_mem or g.gr_gid==owner.pw_gid]
        checks.append({'name':'groups-inventory','ok':set(groups)=={OWNER},'groups':groups})
    except KeyError:
        checks.append({'name':'account','ok':False,'reason':'Required existing account missing'})
        result['metadata_complete']=False;return result
    for path in (GPG_HOME.parent,GPG_HOME):
        try:
            s=path.lstat()
            ok=stat.S_ISDIR(s.st_mode) and s.st_uid==owner.pw_uid and not stat.S_IMODE(s.st_mode)&0o077
            checks.append({'name':str(path),'ok':ok,'observed_uid':s.st_uid,'mode':oct(stat.S_IMODE(s.st_mode))})
        except OSError as error:
            checks.append({'name':str(path),'ok':False,'reason':type(error).__name__})
    acl=[]
    getfacl=shutil.which('getfacl',path='/usr/bin')
    for path in (GPG_HOME.parent,GPG_HOME):
        if not getfacl:
            acl.append({'path':str(path),'ok':False,'reason':'getfacl unavailable'});continue
        query=subprocess.run([getfacl,'--absolute-names','--omit-header','--',str(path)],
                             capture_output=True,text=True,timeout=5,env={'PATH':'/usr/bin','LC_ALL':'C'})
        # Read only directory ACLs, never recurse into private key files.
        lines=query.stdout.splitlines()
        extra=[line for line in lines if line.startswith('default:') or
               (line.startswith(('user:','group:')) and line.split(':',2)[1])]
        acl.append({'path':str(path),'ok':query.returncode==0 and not extra,
                    'query_returncode':query.returncode,'extended_or_default_acl':extra,
                    'reason':None if query.returncode==0 else 'Directory ACL unavailable'})
    result['directory_acl']=acl
    permissions=[]
    for path in map(Path,('/etc/sudoers','/etc/sudoers.d','/etc/polkit-1/rules.d')):
        try:
            if path.is_dir(): list(path.iterdir())
            else:
                with path.open('rb') as stream:stream.read(0)
            permissions.append({'path':str(path),'readable':True})
        except OSError as error:permissions.append({'path':str(path),'readable':False,'reason':type(error).__name__})
    result['authorization_policy_access']=permissions
    result['effective_sudo_polkit_review']='pending administrator review; readability is not an authorization audit'
    sockets=[]
    for path in (GPG_HOME/'S.gpg-agent',Path('/run/user/949/gnupg/S.gpg-agent')):
        try:
            s=path.lstat();sockets.append({'candidate_path':str(path),'socket':stat.S_ISSOCK(s.st_mode),
                'owner_uid':s.st_uid,'mode':oct(stat.S_IMODE(s.st_mode))})
        except OSError as error:sockets.append({'candidate_path':str(path),'reason':type(error).__name__})
    result['agent_socket_candidates_metadata']=sockets
    result['agent_access']='unverified; no socket connection, no agent start; nonstandard home may use a hashed runtime socket path'
    public_paths=[]
    for path in map(Path,('/etc/zethropol-update/aur-manifest-signer.json','/usr/lib/zethropol/aur/manifest_workflow.py',
                         '/var/lib/zethropol/aur-manifest-approvals','/var/lib/zethropol-aur-manifests/pending')):
        try:
            s=path.lstat();public_paths.append({'path':str(path),'exists':True,'uid':s.st_uid,'mode':oct(stat.S_IMODE(s.st_mode))})
        except OSError as error:public_paths.append({'path':str(path),'available':False,'reason':type(error).__name__})
    result['deployment_metadata']=public_paths
    result['metadata_complete']=all(c['ok'] for c in checks) and all(a['ok'] for a in acl)
    result['remaining_checks']=['host root ownership and ACL audit','private key/agent availability only after permission',
                               'deployed wrapper/config/ancestor verification','sudo/polkit administrative audit']
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();report=inspect()
    with args.output.open('x') as stream:json.dump(report,stream,indent=2);stream.write('\n')
    print(json.dumps(report,indent=2));raise SystemExit(0 if report['metadata_complete'] else 2)
