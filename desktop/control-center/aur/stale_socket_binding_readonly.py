"""Fixed socket metadata diagnostics only; no GPG, connection or mutation.

Accept only exact 0600/0700 Linux socket modes. Report failed predicates, partial
metadata and unknown listener visibility instead of collapsing policy errors.
"""
import errno
import json
import os
from pathlib import Path
import stat

HOME='/var/lib/zethropol-aur-release'
GNUPG=HOME+'/gnupg'
SOCKET=GNUPG+'/S.gpg-agent'
ACL={'system.posix_acl_access','system.posix_acl_default'}
EXPECTED={'uid':949,'gid':948,'mode':('0o600','0o700'),'acl':{}}
PHASE='initial'

class BindingBlocked(RuntimeError):
    def __init__(self,report):self.report=report

def phase(value):
    global PHASE
    PHASE=value

def fingerprint(s):
    # atime is not identity. Preserve nanosecond ctime, not tuple float precision.
    return (s.st_dev,s.st_ino,s.st_mode,s.st_uid,s.st_gid,s.st_nlink,s.st_size,s.st_mtime_ns,s.st_ctime_ns)

def metadata(s):
    return {'device':str(s.st_dev),'inode':str(s.st_ino),'uid':s.st_uid,'gid':s.st_gid,
            'mode':oct(stat.S_IMODE(s.st_mode)),'ctime_ns':str(s.st_ctime_ns),
            'type':'socket' if stat.S_ISSOCK(s.st_mode) else 'symlink' if stat.S_ISLNK(s.st_mode) else 'directory' if stat.S_ISDIR(s.st_mode) else 'other'}

def reason(field,expected,observed):return {'field':field,'expected':expected,'observed':observed}

def base_report():
    return {'status':'BLOCKED_STALE_SOCKET_BINDING','socket_path':SOCKET,'observed':{},'blockers':[],
            'listener_state':'NOT_PROBED','listener_visibility':'NOT_PROBED',
            'host_changed':False,'agent_started':False,'signing_attempted':False,'activation_approved':False,
            'quarantine_authorized':False,'route_requeried':False}

def fail(report,field,expected,observed):
    report['blockers'].append(reason(field,expected,observed));raise BindingBlocked(report)

def listener_observation():
    """Read all process-represented network namespaces; never enter one.

    No filesystem inode/kernel socket inode comparison; no connect or FD borrow.
    Absence is limited to observed tables, not hidden/orphan namespace FD holders.
    """
    tables={};errors=[];matched=[]
    entries=[Path('/proc/self')]+[p for p in Path('/proc').iterdir() if p.name.isdigit()]
    for entry in entries:
        try:
            ns=os.stat(entry/'ns/net').st_ino
            if ns in tables:continue
            raw=(entry/'net/unix').read_text()
            tables[ns]=True
            for line in raw.splitlines()[1:]:
                fields=line.split(None,7)
                if len(fields)!=8 or fields[7]!=SOCKET:continue
                matched.append({'network_namespace':str(ns),'kernel_inode':str(int(fields[6])),
                                'listening':bool(int(fields[3],16)&0x10000),'type':fields[4],'state':fields[5]})
        except FileNotFoundError:
            # Do not turn process churn into negative evidence.
            errors.append({'kind':'process-disappeared','errno':errno.ENOENT})
        except (OSError,ValueError) as error:
            errors.append({'kind':'table-unreadable-or-malformed','errno':error.errno if isinstance(error,OSError) else None})
    return {'visibility':'COMPLETE_FOR_OBSERVED_PROCESS_NAMESPACES' if tables and not errors else 'INCOMPLETE',
            'scope':'process-represented network namespace tables; not hidden/orphan namespace FD-holder proof',
            'table_count':len(tables),'entries':matched,'errors':errors[:8],
            'error_count':len(errors)}

def historical_comparison(observed):
    # Retained inventory summaries provide no numerical preimage to compare.
    # Actor GID948 is not substituted for the socket's unretained historical GID.
    return {'device':'OLD_VALUE_NOT_RETAINED','inode':'OLD_VALUE_NOT_RETAINED',
            'ctime_ns':'OLD_VALUE_NOT_RETAINED','gid':'OLD_SOCKET_VALUE_NOT_RETAINED_ACTOR_GID_WAS_948',
            'mode':'OLD_RULE_OWNER_ONLY_NO_GROUP_OTHER_BITS_EXACT_VALUE_NOT_RETAINED',
            'uid':'MATCHES_OLD_SAFE_SOCKET_UID949' if observed.get('uid')==949 else 'DIFFERS_FROM_OLD_SAFE_SOCKET_UID949',
            'acl':'MATCHES_OLD_NO_POSIX_ACL' if observed.get('acl')=={} else 'UNKNOWN_OR_DIFFERS_FROM_OLD_NO_POSIX_ACL',
            'listener':'HISTORICAL_ECONNREFUSED_AND_NO_LISTENER_NOT_CURRENT_PROOF'}

def inspect_socket(report=None):
    report=report if report is not None else base_report()
    parent=None;leaf=None
    try:
        phase('socket-parent-nofollow-open')
        parent=os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
        for part in GNUPG.strip('/').split('/'):
            child=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=parent)
            os.close(parent);parent=child
        parent_before=os.fstat(parent)
        if report.get('root_context_verified'):
            phase('pinned-parent-security')
            observed=(parent_before.st_uid,parent_before.st_gid,stat.S_IMODE(parent_before.st_mode))
            if observed!=(949,948,0o700):fail(report,'pinned_parent_uid_gid_mode',[949,948,'0o700'],[observed[0],observed[1],oct(observed[2])])
            if ACL.intersection(os.listxattr(parent)):fail(report,'pinned_parent_acl',{},'POSIX ACL present')
        phase('socket-lstat')
        s=os.stat('S.gpg-agent',dir_fd=parent,follow_symlinks=False)
        report['observed']=metadata(s)
        phase('socket-type')
        if not stat.S_ISSOCK(s.st_mode):fail(report,'type','socket',report['observed']['type'])
        if s.st_nlink!=1:fail(report,'nlink',1,s.st_nlink)
        phase('socket-inode-pin')
        leaf=os.open('S.gpg-agent',os.O_PATH|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=parent)
        if fingerprint(os.fstat(leaf))!=fingerprint(s):fail(report,'inode_pin','same linked inode','changed during open')
        phase('socket-posix-acl-read')
        anchored='/proc/self/fd/'+str(parent)+'/S.gpg-agent'
        attrs={name:os.getxattr(anchored,name,follow_symlinks=False).hex() for name in sorted(ACL.intersection(os.listxattr(anchored,follow_symlinks=False)))}
        report['observed']['acl']=attrs
        phase('socket-field-guards')
        for field,expected in EXPECTED.items():
            value=report['observed'][field]
            if not (value in expected if field=='mode' else value==expected):report['blockers'].append(reason(field,expected,value))
        report['observed']['owner_only_no_group_other_access']=not bool(stat.S_IMODE(s.st_mode)&0o077)
        phase('listener-metadata-read')
        try:
            inventory=listener_observation();report['listener_observation']=inventory
            report['listener_visibility']=inventory['visibility']
            if inventory['entries']:
                report['listener_state']='LISTENING' if any(row['listening'] for row in inventory['entries']) else 'BOUND_ENDPOINT_PRESENT'
                report['blockers'].append(reason('listener','no bound endpoint',report['listener_state']))
            elif inventory['visibility']=='INCOMPLETE':report['listener_state']='UNKNOWN'
            else:report['listener_state']='NO_ENDPOINT_IN_OBSERVED_TABLES'
            if inventory['visibility']=='INCOMPLETE':report['blockers'].append(reason('listener_visibility','all observed tables readable','INCOMPLETE'))
        except OSError as error:
            report['listener_state']='UNKNOWN';report['listener_visibility']='INCOMPLETE'
            report['blockers'].append({'field':'listener_read','errno':error.errno,'expected':'readable kernel tables','observed':'unavailable'})
        phase('socket-post-observation-stability')
        after=os.stat('S.gpg-agent',dir_fd=parent,follow_symlinks=False)
        if fingerprint(after)!=fingerprint(s) or fingerprint(os.fstat(leaf))!=fingerprint(s):
            report['after']=metadata(after);report['blockers'].append(reason('socket_identity_ctime_stability',metadata(s),metadata(after)))
        attrs_after={name:os.getxattr(anchored,name,follow_symlinks=False).hex() for name in sorted(ACL.intersection(os.listxattr(anchored,follow_symlinks=False)))}
        if attrs_after!=attrs:report['blockers'].append(reason('acl_stability',attrs,attrs_after))
        if fingerprint(os.fstat(parent))!=fingerprint(parent_before) or fingerprint(os.lstat(GNUPG))!=fingerprint(parent_before):
            report['blockers'].append(reason('parent_identity_stability','same pinned directory','changed'))
        if fingerprint(os.stat('S.gpg-agent',dir_fd=parent,follow_symlinks=False))!=fingerprint(s):
            report['blockers'].append(reason('final_inode_ctime_recheck','same pinned socket','changed'))
        report['historical_comparison']=historical_comparison(report['observed'])
        report['status']='FRESH_METADATA_CAPTURED_NOT_MAINTENANCE_AUTHORITY' if not report['blockers'] else 'BLOCKED_STALE_SOCKET_BINDING'
        report['phase']=PHASE;return report
    except BindingBlocked:
        raise
    except OSError as error:
        report['blockers'].append({'field':PHASE,'errno':error.errno,'errno_name':errno.errorcode.get(error.errno,'UNKNOWN')})
        report['phase']=PHASE;return report
    finally:
        if leaf is not None:os.close(leaf)
        if parent is not None:os.close(parent)

def root_context(report):
    phase('root-host-context')
    if os.getresuid()!=(0,0,0):fail(report,'real_effective_saved_uid',[0,0,0],list(os.getresuid()))
    rows=Path('/proc/self/uid_map').read_text().split()
    if rows!=['0','0','4294967295']:fail(report,'initial_uid_namespace','identity host map','remapped or restricted')
    for kind in ('user','net','pid'):
        if os.stat('/proc/self/ns/'+kind).st_ino!=os.stat('/proc/1/ns/'+kind).st_ino:
            fail(report,kind+'_namespace','same as PID1','different')
    # Keep the same private-home and root ancestor requirements as v2.
    phase('socket-parent-security')
    for path in ('/','/var','/var/lib',HOME,GNUPG):
        s=os.lstat(path);private=path in (HOME,GNUPG)
        expected_uid,expected_gid=(949,948) if private else (0,0)
        for key,value,expected in (('directory',stat.S_ISDIR(s.st_mode),True),('uid',s.st_uid,expected_uid),('gid',s.st_gid,expected_gid)):
            if value!=expected:fail(report,'parent:'+path+':'+key,expected,value)
        if private and stat.S_IMODE(s.st_mode)!=0o700:fail(report,'parent:'+path+':mode','0o700',oct(stat.S_IMODE(s.st_mode)))
        if not private and s.st_mode&0o022:fail(report,'parent:'+path+':writable','no group/other write',oct(stat.S_IMODE(s.st_mode)))
        attrs=sorted(ACL.intersection(os.listxattr(path,follow_symlinks=False)))
        if attrs:fail(report,'parent:'+path+':acl',[],attrs)
    report['root_context_verified']=True


def main():
    report=base_report()
    try:root_context(report);report=inspect_socket(report)
    except BindingBlocked as error:report=error.report;report['phase']=PHASE
    except OSError as error:report['phase']=PHASE;report['blockers'].append({'field':PHASE,'errno':error.errno})
    except Exception:report['phase']=PHASE;report['blockers'].append({'field':PHASE,'observed':'DIAGNOSTIC_PROTOCOL_FAILURE'})
    print(json.dumps(report,sort_keys=True))

if __name__=='__main__':main()
