"""Stage/errno and listener metadata only. No agent command or lifecycle action.

Root inventory reads public kernel socket/process metadata; the actor is fixed
UID949/GID948. Secret files, command lines and process environments are unread.
"""
import errno
import json
import os
from pathlib import Path
import re
import socket
import stat
import struct
import subprocess
import sys

import gpg_backend_readonly as base
ROOT_PHASE='initial'


def config_binding():
    p=Path('/etc/zethropol-update/aur-manifest-signer.json');s=p.lstat()
    if (not stat.S_ISREG(s.st_mode) or s.st_nlink!=1 or (s.st_uid,s.st_gid)!=(0,0) or
            stat.S_IMODE(s.st_mode)!=0o644 or s.st_size>65536):raise ValueError('Public config metadata')
    c=json.loads(p.read_bytes())
    expected={'schema':2,'purpose':'zethropol-manifest-signer','identity_model':'existing-key-owner',
              'user':'zethropol-aur-release','uid':949,'gnupg_home':base.HOME,
              'signer':'F3F36BFA6D0FC60DA1CB34F2927519292F6F497B'}
    if c!=expected or type(c.get('schema')) is not int or type(c.get('uid')) is not int:
        raise ValueError('Explicit homedir/identity mismatch')
    return True


def failure(stage,error):
    number=getattr(error,'errno',None)
    if type(number) is not int or not 1<=number<=4095:number=None
    return {'failed_stage':stage,'errno_number':number,'errno_name':errno.errorcode.get(number,'UNAVAILABLE'),
            'error_kind':'timeout' if isinstance(error,TimeoutError) else 'os-error'}


def metadata(path):
    p=Path(path);s=p.lstat();attrs=set(os.listxattr(p,follow_symlinks=False))
    return {'device':str(s.st_dev),'inode':str(s.st_ino),'uid':s.st_uid,'gid':s.st_gid,
            'mode':oct(stat.S_IMODE(s.st_mode)),'is_socket':stat.S_ISSOCK(s.st_mode),
            'is_directory':stat.S_ISDIR(s.st_mode),
            'is_symlink':stat.S_ISLNK(s.st_mode),'access_acl':'system.posix_acl_access' in attrs,
            'default_acl':'system.posix_acl_default' in attrs}


def traversal(path):
    rows=[]
    for index,parent in enumerate(reversed(Path(path).parents)):
        try:row=metadata(parent)
        except OSError as error:return {'rows':rows,'blocked_component':index,'failure':failure('parent-traversal-metadata',error)}
        row['component']=index
        row['actor_can_traverse']=os.access(parent,os.X_OK,effective_ids=True)
        rows.append(row)
    return {'rows':rows,'failure':None}


def policy_context():
    """Only called AFTER an actual EACCES/EPERM; facts are not a denial cause."""
    report={'denial_source_proven':False}
    try:
        lines=Path('/proc/self/status').read_text().splitlines()
        for name in ('Seccomp','NoNewPrivs'):
            values=[line.split()[1] for line in lines if line.startswith(name+':')]
            if values and values[0].isdigit():report[name.lower()]=int(values[0])
    except OSError:pass
    for path,label in (('/proc/self/attr/apparmor/current','apparmor'),('/proc/self/attr/current','lsm_current')):
        try:
            value=Path(path).read_text().strip()
            report[label]='unconfined' if value=='unconfined' else 'present-confined-or-labelled'
        except OSError:report[label]='unavailable'
    return report


def connect_stages(path,factory=socket.socket,timeout=3,allow_connect=True):
    report={'transport':'BLOCKED','socket_create':'NOT_RUN','connect':'NOT_RUN',
            'peer_credentials':'NOT_RUN','greeting':'NOT_RUN','command_sent':False}
    client=None;stage='socket-create'
    try:
        client=factory(socket.AF_UNIX,socket.SOCK_STREAM);report['socket_create']='PASS'
        stage='socket-timeout-config';client.settimeout(timeout)
        if not allow_connect:
            report.update(connect='SKIPPED_LIFECYCLE_GUARD',result='SOCKET_CREATED_CONNECT_GUARDED');return report
        stage='unix-connect';client.connect(path);report.update(connect='PASS',transport='CONNECTED')
        stage='so-peercred'
        pid,uid,gid=struct.unpack('3i',client.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,struct.calcsize('3i')))
        report.update(peer_credentials='PASS',peer_pid=str(pid),peer_uid=uid,peer_gid=gid)
        if uid!=949:report['result']='FOREIGN_PEER_UID';return report
        stage='greeting-read';data=bytearray()
        while b'\n' not in data and len(data)<=1024:
            chunk=client.recv(min(128,1025-len(data)))
            if not chunk:break
            data.extend(chunk)
        line=bytes(data).split(b'\n',1)[0].removesuffix(b'\r')
        report['greeting']='OK' if line==b'OK' or line.startswith(b'OK ') else 'ERR' if line.startswith(b'ERR ') else 'INVALID_OR_EOF'
        report['result']='CONNECTED_PUBLIC_GREETING' if report['greeting']=='OK' else 'CONNECTED_GREETING_NOT_OK'
    except OSError as error:
        key={'socket-create':'socket_create','unix-connect':'connect','so-peercred':'peer_credentials','greeting-read':'greeting'}.get(stage)
        if key:report[key]='BLOCKED'
        report.update(failure(stage,error));report['result']='SOCKET_STAGE_BLOCKED'
        if error.errno in (errno.EACCES,errno.EPERM):report['related_policy_facts']=policy_context()
    finally:
        if client is not None:client.close()
    return report


def unix_rows(raw,path):
    rows=[]
    for line in raw.splitlines()[1:]:
        fields=line.split(None,7)
        if len(fields)!=8 or fields[7]!=path:continue
        try:
            rows.append({'kernel_inode':str(int(fields[6])),'listening':bool(int(fields[3],16)&0x10000),
                         'type':fields[4],'state':fields[5]})
        except ValueError:continue
    return rows


def listener_inventory(path):
    """Never compare filesystem st_ino to /proc's different kernel socket inode."""
    processes=[];complete=True;rows={}
    try:
        for row in unix_rows(Path('/proc/net/unix').read_text(),path):rows[row['kernel_inode']]=row
    except OSError:complete=False
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():continue
        try:
            name=(entry/'comm').read_text().strip()
            if name not in ('gpg-agent','systemd'):continue
            status=(entry/'status').read_text().splitlines()
            uid_lines=[line.split()[1:] for line in status if line.startswith('Uid:')]
            if len(uid_lines)!=1:continue
            uid=int(uid_lines[0][1])
            # All gpg-agents can establish wrong-owner evidence. systemd only
            # PID1 or this actor's manager, to detect activation without starting it.
            if name=='systemd' and entry.name!='1' and uid!=949:continue
            process={'pid':entry.name,'uid':uid,'kind':name,'socket_inodes':[]}
            try:
                for row in unix_rows((entry/'net/unix').read_text(),path):rows[row['kernel_inode']]=row
                for fd in (entry/'fd').iterdir():
                    try:target=os.readlink(fd)
                    except FileNotFoundError:continue
                    match=re.fullmatch(r'socket:\[([0-9]+)\]',target)
                    if match:process['socket_inodes'].append(match[1])
            except FileNotFoundError:continue
            except PermissionError:complete=False
            processes.append(process)
        except FileNotFoundError:continue
        except (PermissionError,OSError,ValueError):complete=False
    inodes=set(rows)
    holders=[]
    for p in processes:
        matched=sorted(inodes.intersection(p['socket_inodes']))
        if matched:holders.append({'pid':p['pid'],'uid':p['uid'],'kind':p['kind'],'kernel_inodes':matched})
    return {'visibility_complete':complete,'matched_socket_entries':list(rows.values()),'holders':holders,
            'uid949_gpg_agent_processes':sum(p['uid']==949 and p['kind']=='gpg-agent' for p in processes)}


def decision(inventory):
    listeners={row['kernel_inode'] for row in inventory['matched_socket_entries'] if row['listening']}
    agents=[p for p in inventory['holders'] if p['kind']=='gpg-agent' and p['uid']==949 and listeners.intersection(p['kernel_inodes'])]
    systemd=[p for p in inventory['holders'] if p['kind']=='systemd' and listeners.intersection(p['kernel_inodes'])]
    if systemd:return False,'SOCKET_ACTIVATION_POSSIBLE_NO_AGENT_START_ALLOWED'
    if not inventory['visibility_complete']:return False,'LISTENER_VISIBILITY_INCOMPLETE'
    if agents:return True,'EXISTING_UID949_AGENT_LISTENER'
    if listeners:return False,'LISTENER_OWNER_UNPROVEN_OR_FOREIGN'
    if not inventory['visibility_complete']:return False,'LISTENER_VISIBILITY_INCOMPLETE'
    return True,'NO_LISTENER_OBSERVED_STALE_PROBE_ONLY'


def actor(path,allow,expected_socket=None):
    if (os.getuid(),os.geteuid(),os.getgid(),os.getegid(),os.getgroups())!=(949,949,948,948,[948]):
        raise ValueError('Exact actor required')
    report={'uid':949,'gid':948,'signing_attempted':False,'command_sent':False,
            'secret_key_presence':'NOT_PROBED','passphrase_cache':'NOT_PROBED'}
    report['path_guard']=base.safe_socket(path)
    report['traversal']=traversal(path)
    if report['traversal']['failure'] is not None:
        report.update(report['traversal']['failure']);report['result']='ACTOR_TRAVERSAL_BLOCKED';return report
    try:report['socket']=metadata(path)
    except OSError as error:
        report.update(failure('socket-metadata',error));report['result']='ACTOR_SOCKET_METADATA_BLOCKED';return report
    report['actor_socket_write_access']=os.access(path,os.W_OK,effective_ids=True)
    if expected_socket is not None:
        report['root_actor_socket_identity_match']=all(report['socket'][k]==expected_socket[k] for k in ('device','inode','uid','gid','mode'))
        if not report['root_actor_socket_identity_match']:allow=False
    if report['path_guard']!='safe-existing-socket' or not all(row['actor_can_traverse'] for row in report['traversal']['rows']):
        report['result']='ACTOR_FILESYSTEM_GUARD_BLOCKED';return report
    before=report['socket'];report.update(connect_stages(path,allow_connect=allow))
    try:after=metadata(path)
    except OSError as error:
        report['socket_identity_stable']=False;report['post_observation_error']=failure('socket-post-metadata',error);return report
    report['socket_identity_stable']=all(before[k]==after[k] for k in ('device','inode','uid','gid','mode'))
    return report


def root_main(actor_body):
    global ROOT_PHASE
    import pwd
    ROOT_PHASE='root-dispatch-identity'
    if os.getuid()!=0 or os.geteuid()!=0 or not (sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode):
        raise ValueError('Local isolated root dispatch required')
    if Path('/proc/self/uid_map').read_text().split()!=['0','0','4294967295']:
        raise ValueError('Initial host dispatch required')
    p=pwd.getpwnam('zethropol-aur-release')
    if (p.pw_uid,p.pw_gid,p.pw_dir,p.pw_shell)!=(949,948,'/var/lib/zethropol-aur-release','/usr/bin/nologin'):
        raise ValueError('Identity changed')
    ROOT_PHASE='public-signer-homedir-binding';config_binding()
    def drop():os.setgroups([948]);os.setgid(948);os.setuid(949)
    ROOT_PHASE='uid949-socket-resolution'
    located=subprocess.run(['/usr/bin/gpgconf','--homedir',base.HOME,'--list-dirs','agent-socket'],preexec_fn=drop,
        stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=8,env=dict(base.ENV))
    if located.returncode or len(located.stdout)>4096:raise ValueError('Socket resolution failed')
    path=located.stdout.decode('ascii').strip()
    ROOT_PHASE='root-socket-metadata';guard=base.safe_socket(path)
    if guard=='unexpected-socket-location':raise ValueError('Unexpected computed socket')
    root_meta=metadata(path)
    ROOT_PHASE='listener-metadata-inventory'
    inventory=listener_inventory(path);allow,reason=decision(inventory)
    if guard!='safe-existing-socket':allow=False
    suffix='\nprint(json.dumps(actor('+repr(path)+','+repr(allow)+','+repr(root_meta)+'),sort_keys=True,separators=(",",":")))\n'
    ROOT_PHASE='uid949-connect-stages'
    result=subprocess.run(['/usr/bin/python3','-I','-S','-B','-c',actor_body+suffix],preexec_fn=drop,
        stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=12,env=dict(base.ENV))
    if result.returncode or len(result.stdout)>32768:raise ValueError('Actor metadata unavailable')
    observation=json.loads(result.stdout)
    if observation.get('signing_attempted') is not False:raise ValueError('Unexpected actor response')
    print(json.dumps({'socket_location':'fixed-homedir' if path==base.HOME+'/S.gpg-agent' else 'fixed-uid949-runtime',
                      'signer_homedir_binding':True,
                      'root_socket':root_meta,'root_path_guard':guard,'listener_inventory':inventory,
                      'connect_safety_gate':reason,'actor':observation,
                      'agent_start_restart_socket_delete_config_change':False},sort_keys=True,separators=(',',':')))
