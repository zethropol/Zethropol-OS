"""Metadata and existing-agent GETINFO only. Never open/list secret key files.

No GPG sign/decrypt/list-secret-keys, keygrips, cache/passphrase query, agent
launch/reload/kill, pinentry, configuration edits or signing retries.
"""
import json
import os
from pathlib import Path
import pwd
import stat
import subprocess

HOME='/var/lib/zethropol-aur-release/gnupg'
ENV={'PATH':'/usr/bin','LC_ALL':'C'}  # Exactly the installed GPG subprocess env.
REQUIRED={'--no-options','--homedir','--batch','--yes','--no-tty','--no-autostart',
          '--no-auto-key-retrieve','--no-auto-key-import','--pinentry-mode','--local-user','--output','--detach-sign'}


def home_metadata():
    try:
        for p in (Path(HOME),*Path(HOME).parents):
            s=p.lstat();private=str(p) in (HOME,'/var/lib/zethropol-aur-release')
            if (not stat.S_ISDIR(s.st_mode) or s.st_uid!=(949 if private else 0) or
                    s.st_mode&0o022 or (private and (s.st_gid!=948 or s.st_mode&0o077)) or
                    {'system.posix_acl_access','system.posix_acl_default'}&set(os.listxattr(p,follow_symlinks=False))):
                return 'BLOCKED'
        return 'PASS'
    except OSError:return 'BLOCKED'


def safe_socket(path):
    """Inspect only the computed socket/ancestors, never directory contents."""
    p=Path(path)
    if (not isinstance(path,str) or not p.is_absolute() or str(p)!=path or '..' in p.parts or
            not (path==HOME+'/S.gpg-agent' or path.startswith('/run/user/949/gnupg/'))):
        return 'unexpected-socket-location'
    try:
        for item in p.parents:
            s=item.lstat()
            if not stat.S_ISDIR(s.st_mode) or s.st_uid not in (0,949) or s.st_mode&0o022:
                return 'unsafe-socket-ancestor'
            if {'system.posix_acl_access','system.posix_acl_default'}&set(os.listxattr(item,follow_symlinks=False)):
                return 'unsafe-socket-acl'
        s=p.lstat()
        if not stat.S_ISSOCK(s.st_mode) or s.st_uid!=949 or s.st_mode&0o077:
            return 'unsafe-socket-metadata'
        if {'system.posix_acl_access','system.posix_acl_default'}&set(os.listxattr(p,follow_symlinks=False)):
            return 'unsafe-socket-acl'
        return 'safe-existing-socket'
    except FileNotFoundError:return 'socket-or-ancestor-absent'
    except PermissionError:return 'socket-permission-denied'
    except OSError:return 'socket-io-error'


def run_public(argv,run=subprocess.run):
    # Programs/argv are fixed by this module; stdin closed, no raw stderr logged.
    return run(argv,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,
               timeout=8,env=dict(ENV))


def inspect(run=run_public,socket_check=safe_socket):
    context=(os.getuid(),os.geteuid(),os.getgid(),os.getegid(),os.getgroups())
    if context!=(949,949,948,948,[948]):raise ValueError('Exact read-only UID949 context required')
    p=pwd.getpwuid(949)
    if (p.pw_name,p.pw_dir,p.pw_shell)!=('zethropol-aur-release','/var/lib/zethropol-aur-release','/usr/bin/nologin'):
        raise ValueError('Identity metadata mismatch')
    report={'uid':949,'gid':948,'environment':'exact reduced PATH/LC_ALL; homedir explicit',
            'gpg_config':'--no-options ignores gpg.conf; agent configuration not read',
            'pinentry_mode':'error','batch':True,'loopback':False,'agent_autostart':False,
            'secret_key_presence':'NOT_PROBED','passphrase_cache':'NOT_PROBED','signing_attempted':False}
    report['home_ancestor_metadata']=home_metadata()
    if report['home_ancestor_metadata']!='PASS':
        report['result']='HOME_METADATA_BLOCKED';return report
    options=run(['/usr/bin/gpg','--no-options','--homedir',HOME,'--dump-options'])
    supported=set(options.stdout.decode('ascii').splitlines())
    report['signing_options']='PASS' if options.returncode==0 and REQUIRED<=supported else 'BLOCKED'
    if report['signing_options']!='PASS':
        report['result']='SIGNING_OPTIONS_UNSUPPORTED';return report
    connect_options=run(['/usr/bin/gpg-connect-agent','--dump-options'])
    if connect_options.returncode or not {'--no-autostart','--no-history','--homedir'}<=set(connect_options.stdout.decode('ascii').splitlines()):
        report['result']='AGENT_QUERY_OPTIONS_UNSUPPORTED';return report
    located=run(['/usr/bin/gpgconf','--homedir',HOME,'--list-dirs','agent-socket'])
    if located.returncode or len(located.stdout)>4096:
        report['result']='SOCKET_RESOLUTION_FAILED';return report
    try:path=located.stdout.decode('ascii').strip()
    except UnicodeError:report['result']='SOCKET_RESOLUTION_FAILED';return report
    state=socket_check(path);report['socket_state']=state
    if state!='safe-existing-socket':
        report['agent_reachable']=False
        report['result']='NO_EXISTING_AGENT_SOCKET' if state=='socket-or-ancestor-absent' else 'SOCKET_ACCESS_OR_METADATA_BLOCKED'
        return report
    # Only GETINFO pid; no SIGN/PKSIGN/KEYINFO/HAVEKEY/PASSPHRASE or startup.
    answer=run(['/usr/bin/gpg-connect-agent','--no-autostart','--no-history','--homedir',HOME,'GETINFO pid','/bye'])
    report['agent_query_returncode']=answer.returncode
    if answer.returncode or len(answer.stdout)>4096:
        report.update(agent_reachable=False,result='EXISTING_AGENT_CONNECTION_FAILED');return report
    lines=answer.stdout.decode('ascii',errors='replace').splitlines()
    ids=[line[2:] for line in lines if line.startswith('D ')]
    if len(ids)!=1 or not ids[0].isdigit():
        report.update(agent_reachable=False,result='AGENT_PUBLIC_RESPONSE_INVALID');return report
    pid=int(ids[0])
    try:
        status=Path('/proc')/str(pid)/'status'
        uids=[line.split()[1:] for line in status.read_text().splitlines() if line.startswith('Uid:')]
        owner_ok=len(uids)==1 and uids[0]==['949']*4
    except (OSError,ValueError):owner_ok=False
    report.update(agent_reachable=True,agent_process_uid_verified=owner_ok,
                  result='AGENT_REACHABLE_KEY_AND_CACHE_UNKNOWN' if owner_ok else 'AGENT_OWNER_UNVERIFIED')
    return report


def main():
    try:print(json.dumps(inspect(),sort_keys=True))
    except Exception:print(json.dumps({'result':'READONLY_CONTEXT_OR_IO_BLOCKED','signing_attempted':False}))


if __name__=='__main__':main()
