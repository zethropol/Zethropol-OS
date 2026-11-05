"""Root metadata planning only. No GPG/gpgconf/agent/service command is run.

GnuPG socket-directory discovery can create runtime directories, so this reader
uses the pinned 2.4.9 path algorithm and leaves route confirmation to the later
explicitly authorized startup. No socket is connected, renamed or unlinked.
"""
import hashlib
import json
import os
from pathlib import Path
import pwd
import stat
import sys

import agent_lifecycle_949 as life
import gpg_socket_connect_readonly as socket_meta
PHASE='initial'


def digest(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()


def main(helper_sha):
    global PHASE
    PHASE='root-identity'
    if os.getuid()!=0 or os.geteuid()!=0 or not (sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode):
        raise ValueError('Isolated local root required')
    if Path('/proc/self/uid_map').read_text().split()!=['0','0','4294967295']:raise ValueError('Initial host required')
    p=pwd.getpwnam('zethropol-aur-release')
    life.require((p.pw_uid,p.pw_gid,p.pw_dir,p.pw_shell)==(949,948,life.HOME,'/usr/bin/nologin'))
    life.require(set(os.getgrouplist(p.pw_name,948))=={948})
    PHASE='fixed-home-and-signer-binding'
    socket_meta.config_binding()
    life.node(life.HOME,949,948,0o700);life.node(life.GNUPG,949,948,0o700)
    PHASE='existing-keystore-directory-metadata'
    life.node(life.GNUPG+'/private-keys-v1.d',949,948,0o700)
    old=life.GNUPG+'/S.gpg-agent';initial=socket_meta.metadata(old);old_ctime=str(Path(old).lstat().st_ctime_ns)
    PHASE='old-socket-no-listener'
    life.require(initial['uid']==949 and initial['gid']==948 and initial['is_socket'] and not initial['is_symlink'] and
                 int(initial['mode'],8)&0o077==0 and not initial['access_acl'] and not initial['default_acl'])
    inventory=socket_meta.listener_inventory(old)
    life.require(inventory['visibility_complete'] and not inventory['holders'] and inventory['uid949_gpg_agent_processes']==0)
    life.require(not any(row['listening'] for row in inventory['matched_socket_entries']))
    PHASE='owner-configuration-and-route-review'
    life.owner_configs_absent()
    PHASE='new-runtime-scope'
    runtime_fn = getattr(life, 'runtime_directory', None)
    if runtime_fn is None:
        import base64
        def runtime_fn():
            data = hashlib.sha1(life.GNUPG.encode()).digest()[:15]
            suffix = base64.b32encode(data).decode().translate(str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ234567', 'ybndrfg8ejkmcpqxot1uwisza345h769'))
            return getattr(life, 'RUNTIME_BASE', '/run/user/949/gnupg') + '/d.' + suffix
    runtime_base = getattr(life, 'RUNTIME_BASE', '/run/user/949/gnupg')
    runtime = runtime_fn(); runtime_nodes = {}
    for directory in ('/run/user/949', runtime_base, runtime):
        try:
            info=life.node(directory,949,948,0o700)
            runtime_nodes[directory]={'exists':True,'device':str(info.st_dev),'inode':str(info.st_ino)}
            if directory!= '/run/user/949':
                entries=os.listdir(directory)
                life.require(not entries or directory==runtime_base and set(entries)=={Path(runtime).name})
        except FileNotFoundError:runtime_nodes[directory]={'exists':False}
    targets=['/usr/lib/zethropol/aur-agent-lifecycle-949.py',life.POLICY,
             '/etc/systemd/system/'+life.UNIT,'/etc/systemd/system/multi-user.target.wants/'+life.UNIT,life.AUDIT]
    for prefix in ('/etc/systemd/system/','/run/systemd/system/','/usr/lib/systemd/system/','/run/systemd/transient/'):
        targets.append(prefix+life.UNIT);targets.append(prefix+life.UNIT+'.d')
    PHASE='new-host-targets-and-unit-collisions'
    for path in targets:
        try:Path(path).lstat()
        except FileNotFoundError:continue
        raise ValueError('Existing lifecycle target requires review')
    life.require(Path('/usr/lib/systemd/system/user-runtime-dir@.service').is_file())
    life.require(Path('/proc/1/comm').read_text().strip()=='systemd')
    PHASE='original-socket-stability'
    final=socket_meta.metadata(old);life.require(final==initial and str(Path(old).lstat().st_ctime_ns)==old_ctime)
    policy={'schema':1,'purpose':'zethropol-agent-lifecycle-949','uid':949,'gid':948,'gnupg_home':life.GNUPG,
            'old_socket':{key:initial[key] for key in ('device','inode','uid','gid','mode')},
            'runtime_directory':runtime,'helper_sha256':helper_sha,
            'binary_sha256':{path:digest(path) for path in ('/usr/bin/gpg-agent','/usr/bin/gpgconf')},'activation_approved':False}
    policy['old_socket']['ctime_ns']=old_ctime
    print(json.dumps({'status':'PASS_LIFECYCLE_PLANNING_ONLY','old_socket_untouched':True,'no_listener_verified':True,
                      'runtime_nodes':runtime_nodes,'new_targets_absent':True,'policy_draft_not_authorization':policy,
                      'runtime_route_native_confirmation':'required after separately approved runtime creation and before agent exec',
                      'agent_started':False,'signing_attempted':False},sort_keys=True,separators=(',',':')))


if __name__=='__main__':raise SystemExit('Use reviewed literal-code local planning command')
