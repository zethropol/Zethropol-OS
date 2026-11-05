"""Metadata + procfs only. Never connect, borrow FDs, enter namespaces or authorize maintenance."""
import json
import os
from pathlib import Path
import re
import stat
import sys
import stale_socket_binding_readonly as binding

GNUPG=binding.GNUPG
NAMES=('S.gpg-agent','S.gpg-agent.ssh','S.gpg-agent.extra','S.gpg-agent.browser',
       'S.scdaemon','S.keyboxd','S.dirmngr','S.tpm2daemon')
ACL=binding.ACL

def kind(mode):
    for check,label in ((stat.S_ISSOCK,'socket'),(stat.S_ISLNK,'symlink'),(stat.S_ISREG,'regular'),
                        (stat.S_ISDIR,'directory'),(stat.S_ISFIFO,'fifo'),(stat.S_ISCHR,'character'),(stat.S_ISBLK,'block')):
        if check(mode):return label
    return 'unknown'

def snapshot(parent,name):
    try:s=os.stat(name,dir_fd=parent,follow_symlinks=False)
    except FileNotFoundError:return {'path':GNUPG+'/'+name,'state':'ABSENT'}
    fd=os.open(name,os.O_PATH|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=parent)
    try:
        if binding.fingerprint(os.fstat(fd))!=binding.fingerprint(s):raise ValueError('inode pin changed')
        anchored='/proc/self/fd/'+str(parent)+'/'+name
        attrs={n:os.getxattr(anchored,n,follow_symlinks=False).hex() for n in sorted(ACL.intersection(os.listxattr(anchored,follow_symlinks=False)))}
        if binding.fingerprint(os.stat(name,dir_fd=parent,follow_symlinks=False))!=binding.fingerprint(s):raise ValueError('entry changed')
        return dict(binding.metadata(s),path=GNUPG+'/'+name,type=kind(s.st_mode),nlink=s.st_nlink,
                    acl=attrs,state='EXISTS_REQUIRES_COLLISION_REVIEW')
    finally:os.close(fd)

def parse_table(raw,namespace,paths):
    rows=[]
    lines=raw.splitlines()
    if not lines or not lines[0].startswith('Num'):raise ValueError('missing unix table header')
    for line in lines[1:]:
        f=line.split(None,7)
        if len(f)<7:raise ValueError('malformed unix table')
        inode=str(int(f[6]));flags=int(f[3],16)
        if len(f)==8 and f[7] in paths:
            rows.append({'net_namespace':str(namespace),'kernel_inode':inode,'path':f[7],
                         'listening':bool(flags&0x10000),'socket_type':f[4],'state':f[5]})
    return rows

def starttime(task):return (task/'stat').read_text().rsplit(')',1)[1].split()[19]

def proc_scan(paths):
    tables=set();rows=[];socket_fds=[];net_fds=[];errors=[];task_count=0
    def error(stage,e):errors.append({'stage':stage,'errno':e.errno if isinstance(e,OSError) else None})
    try:pids=sorted(p for p in Path('/proc').iterdir() if p.name.isdigit())
    except OSError as e:return {'visibility':'INCOMPLETE','errors':[{'stage':'proc-list','errno':e.errno}]}
    for proc in pids:
        try:tasks=list((proc/'task').iterdir())
        except OSError as e:error('task-list',e);continue
        for task in tasks:
            task_count+=1
            try:
                before=starttime(task)
                ns=str(os.stat(task/'ns/net').st_ino);mnt=str(os.stat(task/'ns/mnt').st_ino)
                if ns not in tables:
                    parsed=parse_table((task/'net/unix').read_text(),ns,paths)
                    if str(os.stat(task/'ns/net').st_ino)!=ns:raise ValueError('net namespace changed')
                    rows.extend(parsed);tables.add(ns)
                for fd in (task/'fd').iterdir():
                    target=os.readlink(fd)
                    match=re.fullmatch(r'socket:\[(\d+)\]',target)
                    if match:socket_fds.append({'pid':proc.name,'tid':task.name,'fd':fd.name,'starttime':before,
                                               'kernel_inode':match[1],'current_net_namespace':ns,'mount_namespace':mnt})
                    match=re.fullmatch(r'net:\[(\d+)\]',target)
                    if match:net_fds.append({'pid':proc.name,'tid':task.name,'fd':fd.name,'net_namespace':match[1]})
                if before!=starttime(task) or ns!=str(os.stat(task/'ns/net').st_ino) or mnt!=str(os.stat(task/'ns/mnt').st_ino):
                    raise ValueError('task identity or namespace changed')
            except (OSError,ValueError,IndexError) as e:error('task-observation',e)
    matching={r['kernel_inode'] for r in rows}
    holders=[dict(f,correlation='kernel inode candidate; pathname/mount identity not proven') for f in socket_fds if f['kernel_inode'] in matching]
    return {'visibility':'INCOMPLETE' if errors or not tables else 'COMPLETE_FOR_OBSERVED_TASKS_ONLY',
            'table_count':len(tables),'task_count':task_count,'entries':rows,'candidate_fd_holders':holders,
            'socket_fd_count':len(socket_fds),'namespace_fd_references':net_fds,
            'unrepresented_net_namespace_fds':[f for f in net_fds if f['net_namespace'] not in tables],
            'error_count':len(errors),'errors':errors[:16],
            'scope':'observed task tables and readlink FD references across all visible UIDs; no FD borrowing or setns',
            'global_holder_absence_proven':False,
            'limitations':['unobserved tasks/namespaces, kernel-held or in-flight SCM_RIGHTS FDs not excluded',
                           'FDs may outlive or differ from holder current network namespace',
                           'pathname aliases/mount namespaces and procfs sampling races remain',
                           'filesystem socket inode is never compared with kernel socket inode']}

def review():
    if not (sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode):raise ValueError('isolated no-bytecode reader required')
    context=binding.base_report();binding.root_context(context)
    parent=os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
    try:
        for part in GNUPG.strip('/').split('/'):
            child=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=parent)
            os.close(parent);parent=child
        initial=os.fstat(parent)
        if (initial.st_uid,initial.st_gid,stat.S_IMODE(initial.st_mode))!=(949,948,0o700) or ACL.intersection(os.listxattr(parent)):raise ValueError('parent security')
        names=sorted(set(NAMES)|{n for n in os.listdir(parent) if n.startswith('S.')})
        before={n:snapshot(parent,n) for n in names}
        observation=proc_scan({GNUPG+'/'+n for n in names})
        after={n:snapshot(parent,n) for n in names}
        stable=(before==after and names==sorted(set(NAMES)|{n for n in os.listdir(parent) if n.startswith('S.')}) and
                binding.fingerprint(initial)==binding.fingerprint(os.fstat(parent))==binding.fingerprint(os.lstat(GNUPG)))
        return {'status':'READONLY_SOCKET_SAFETY_CAPTURED' if stable else 'BLOCKED_SOCKET_METADATA_RACE',
                'objects':before,'after':after if not stable else None,'observations':observation,
                'maintenance_gate':'BLOCKED_GLOBAL_FD_AND_NAMESPACE_ABSENCE_NOT_PROVEN',
                'activation_approved':False,'quarantine_authorized':False,'host_changed':False,
                'agent_started':False,'signing_attempted':False,'credential_readiness':'NOT_PROBED'}
    finally:os.close(parent)

def main():
    try:r=review()
    except Exception as e:
        r={'status':'BLOCKED_READONLY_SOCKET_SAFETY','errno':e.errno if isinstance(e,OSError) else None,
           'activation_approved':False,'quarantine_authorized':False,'host_changed':False}
    print(json.dumps(r,sort_keys=True))

if __name__=='__main__':main()
