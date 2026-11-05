"""Fresh readonly metadata binding only. No GPG/socket connection/service call."""
import hashlib,json,os,stat
from pathlib import Path
import agent_lifecycle_949 as life
import stale_socket_binding_readonly as diagnostic

PHASE='initial'

def phase(name):
    global PHASE
    PHASE=name

def public_binary_hash(path):
    for parent in Path(path).parents:
        s=parent.lstat()
        life.require(stat.S_ISDIR(s.st_mode) and (s.st_uid,s.st_gid)==(0,0) and not s.st_mode&0o022 and not life.ACL.intersection(os.listxattr(parent,follow_symlinks=False)))
    s=life.node(path,0,0,0o755,False)
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NOATIME|os.O_CLOEXEC)
    try:
        life.require(life.stable_stat(os.fstat(fd))==life.stable_stat(s))
        with os.fdopen(os.dup(fd),'rb') as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
        life.require(life.stable_stat(os.fstat(fd))==life.stable_stat(s) and life.stable_stat(os.lstat(path))==life.stable_stat(s))
        return digest
    finally:os.close(fd)

def config_binding():
    s=os.lstat(life.CONFIG)
    life.require(stat.S_ISREG(s.st_mode) and (s.st_uid,s.st_gid)==(949,948) and s.st_nlink==1 and
                 stat.S_IMODE(s.st_mode) in (0o600,0o644) and 0<s.st_size<=16384)
    fd=os.open(life.CONFIG,os.O_RDONLY|os.O_NOFOLLOW|os.O_NOATIME|os.O_CLOEXEC)
    try:
        life.require(life.config_metadata(os.fstat(fd))==life.config_metadata(s) and not life.ACL.intersection(os.listxattr(fd)))
        raw=os.pread(fd,16385,0)
        life.require(life.config_metadata(os.fstat(fd))==life.config_metadata(s) and life.config_metadata(os.lstat(life.CONFIG))==life.config_metadata(s))
        life.require(len(raw)==s.st_size)
        digest=hashlib.sha256(raw).hexdigest()
        return {'metadata':life.config_metadata(s),'sha256':digest,'reviewed_hash_match':digest==life.CONFIG_HASH}
    finally:os.close(fd)

def target_state(path):
    try:s=os.lstat(path)
    except FileNotFoundError:return {'state':'ABSENT'}
    result={'path':path,'state':'EXISTS_REQUIRES_COLLISION_REVIEW','device':str(s.st_dev),'inode':str(s.st_ino),
            'uid':s.st_uid,'gid':s.st_gid,'mode':oct(stat.S_IMODE(s.st_mode)),'ctime_ns':str(s.st_ctime_ns),
            'type':'socket' if stat.S_ISSOCK(s.st_mode) else 'symlink' if stat.S_ISLNK(s.st_mode) else 'directory' if stat.S_ISDIR(s.st_mode) else 'file' if stat.S_ISREG(s.st_mode) else 'other'}
    if path==life.UNIT_PATH and stat.S_ISREG(s.st_mode) and (s.st_uid,s.st_gid)==(0,0):
        raw=life.root_input(path)
        life.require(life.stable_stat(os.lstat(path))==life.stable_stat(s))
        result['sha256']=hashlib.sha256(raw).hexdigest()
    return result


def review():
    phase('root-host-identity')
    life.require(os.getresuid()==(0,0,0))
    life.require(Path('/proc/self/uid_map').read_text().split()==['0','0','4294967295'])
    for kind in ('user','net','pid'):
        life.require(os.stat('/proc/self/ns/'+kind).st_ino==os.stat('/proc/1/ns/'+kind).st_ino)
    phase('private-home-metadata')
    for path in ('/','/var','/var/lib'):
        s=os.lstat(path)
        life.require(stat.S_ISDIR(s.st_mode) and (s.st_uid,s.st_gid)==(0,0) and not s.st_mode&0o022 and not life.ACL.intersection(os.listxattr(path,follow_symlinks=False)))
    life.node(life.HOME,949,948,0o700);life.node(life.GNUPG,949,948,0o700)
    phase('stale-socket-metadata-only')
    socket_context=diagnostic.base_report();socket_context['root_context_verified']=True
    socket_report=diagnostic.inspect_socket(socket_context)
    if socket_report['blockers']:raise diagnostic.BindingBlocked(socket_report)
    before={key:socket_report['observed'][key] for key in ('device','inode','uid','gid','mode','ctime_ns','acl')}
    no_listener=socket_report['listener_state']=='NO_ENDPOINT_IN_OBSERVED_TABLES'
    phase('uid949-agent-process-metadata')
    no_agent=not life.any_uid949_agent()
    phase('auxiliary-socket-metadata')
    auxiliary=target_state(life.AUX_SOCKET)
    auxiliary_no_listener=life.no_listener(life.AUX_SOCKET)
    phase('preserved-config-binding')
    config=config_binding()
    phase('public-executable-hashes')
    binaries={path:public_binary_hash(path) for path in ('/usr/bin/gpg-agent','/usr/bin/gpgconf')}
    phase('deployment-target-collisions')
    targets={path:target_state(path) for path in (life.RELEASE,life.UNIT_PATH,life.STATE,life.AUDIT)}
    phase('stale-socket-post-observation')
    final_context=diagnostic.base_report();final_context['root_context_verified']=True
    final_report=diagnostic.inspect_socket(final_context)
    if final_report['blockers']:raise diagnostic.BindingBlocked(final_report)
    after={key:final_report['observed'][key] for key in before}
    if after!=before:
        raise diagnostic.BindingBlocked({'status':'BLOCKED_STALE_SOCKET_BINDING','phase':'full-review-socket-recheck',
              'observed':before,'after':after,'blockers':[diagnostic.reason('full_review_inode_ctime_stability',before,after)],
              'host_changed':False,'agent_started':False,'signing_attempted':False})
    life.require((final_report['listener_state']=='NO_ENDPOINT_IN_OBSERVED_TABLES')==no_listener and (not life.any_uid949_agent())==no_agent)
    return {'status':'READONLY_BINDINGS_CAPTURED','accepted_socket_path':life.SOCKET,
            'route_evidence':'USER_CONFIRMED_NOT_REQUERIED','old_socket':before,
            'listener_absent':no_listener,'listener_absence_scope':'observed process network namespace tables only',
            'listener_visibility':socket_report['listener_visibility'],
            'listener_observation':socket_report['listener_observation'],
            'unobserved_namespace_and_fd_holders':'NOT_EXCLUDED_REQUIRES_SEPARATE_MAINTENANCE_PROOF',
            'quarantine_authorized':False,'uid949_agent_absent':no_agent,
            'auxiliary_socket':auxiliary,'auxiliary_listener_absent':auxiliary_no_listener,
            'agent_config':config,'binary_sha256':binaries,'deployment_targets':targets,
            'host_changed':False,'agent_started':False,'signing_attempted':False,
            'credential_readiness':'NOT_PROBED','activation_approved':False}

def main():
    try:result=review()
    except diagnostic.BindingBlocked as error:
        result=dict(error.report,phase=error.report.get('phase',diagnostic.PHASE))
    except Exception as error:
        result={'status':'BLOCKED_READONLY_LIFECYCLE_BINDING','phase':PHASE,
                'errno':error.errno if isinstance(error,OSError) else None,
                'host_changed':False,'agent_started':False,'signing_attempted':False}
    print(json.dumps(result,sort_keys=True))

if __name__=='__main__':main()
