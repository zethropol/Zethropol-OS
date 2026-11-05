"""Bounded owner configuration review. No GPG, socket or service operations.

Only this fixed public configuration file may be read. Unknown lines, comments,
paths and values are never returned; no key storage is opened.
"""
import hashlib
import json
import os
import stat

HOME='/var/lib/zethropol-aur-release/gnupg'
CONFIG=HOME+'/gpg-agent.conf'
ACL={'system.posix_acl_access','system.posix_acl_default'}
FLAGS={'no-allow-loopback-pinentry','no-allow-external-cache'}
NUMBERS={'default-cache-ttl','max-cache-ttl'}
LIMIT=16384


def classify(raw):
    if len(raw)>LIMIT:return {'classification':'REQUIRES_REVIEW','reason':'size-limit'}
    try:text=raw.decode('ascii')
    except UnicodeDecodeError:return {'classification':'REQUIRES_REVIEW','reason':'non-ascii'}
    safe={};unknown=0;duplicates=0
    for line in text.splitlines():
        line=line.strip()
        if not line or line.startswith('#'):continue
        fields=line.split();key=fields[0]
        if key in FLAGS and len(fields)==1:value=True
        elif key in NUMBERS and len(fields)==2 and fields[1].isascii() and fields[1].isdigit() and len(fields[1])<=7 and int(fields[1])<=86400:value=int(fields[1])
        else:unknown+=1;continue
        if key in safe:duplicates+=1
        safe[key]=value
    return {'classification':'SAFE_SUBSET_CANDIDATE' if not unknown and not duplicates else 'REQUIRES_REVIEW',
            'safe_options':safe,'unreviewed_line_count':unknown,'duplicate_count':duplicates}


def identity(s):
    return {'device':str(s.st_dev),'inode':str(s.st_ino),'uid':s.st_uid,'gid':s.st_gid,
            'mode':oct(stat.S_IMODE(s.st_mode)),'ctime_ns':str(s.st_ctime_ns),'size':s.st_size,
            'type':'regular' if stat.S_ISREG(s.st_mode) else 'symlink' if stat.S_ISLNK(s.st_mode) else 'other'}


def metadata_rejections(s, acls):
    # Review-only acceptance for a non-secret options file inside verified 0700
    # home. This NEVER changes the activation/key/socket permission policy.
    reasons=[]
    if not stat.S_ISREG(s.st_mode):reasons.append('not-regular')
    if (s.st_uid,s.st_gid)!=(949,948):reasons.append('owner-group')
    if stat.S_IMODE(s.st_mode) not in (0o600,0o644):reasons.append('mode')
    if s.st_nlink!=1:reasons.append('link-count')
    if acls:reasons.append('acl')
    if not 0<=s.st_size<=LIMIT:reasons.append('size')
    return reasons


def review():
    if os.getuid()!=0 or os.geteuid()!=0:raise PermissionError(1,'root required')
    if open('/proc/self/uid_map').read().split()!=['0','0','4294967295']:raise PermissionError(1,'initial host required')
    fd=os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
    try:
        for part in HOME.strip('/').split('/'):
            child=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=fd)
            os.close(fd);fd=child
            s=os.fstat(fd)
            if s.st_uid not in (0,949) or s.st_mode&0o022 or ACL.intersection(os.listxattr(fd)):raise PermissionError(1,'unsafe ancestor')
        parent=os.fstat(fd)
        if (parent.st_uid,parent.st_gid,stat.S_IMODE(parent.st_mode))!=(949,948,0o700):raise PermissionError(1,'unsafe home')
        s=os.stat('gpg-agent.conf',dir_fd=fd,follow_symlinks=False)
        result={'path':CONFIG,'metadata':identity(s),'content_read':False,'acl_present':'NOT_PROBED_UNSAFE_TYPE'}
        if not stat.S_ISREG(s.st_mode):return result
        f=os.open('gpg-agent.conf',os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC|os.O_NOATIME,dir_fd=fd)
        try:
            before=os.fstat(f);acls=sorted(ACL.intersection(os.listxattr(f)));result['acl_present']=bool(acls)
            if identity(before)!=identity(s):raise PermissionError(1,'identity changed')
            rejections=metadata_rejections(s,acls)
            result['metadata_acceptance']='BLOCKED' if rejections else 'PASS_REVIEW_ONLY'
            result['metadata_rejections']=rejections
            if rejections:
                result['classification']='BLOCKED_METADATA';return result
            raw=os.pread(f,LIMIT+1,0)
            after=os.fstat(f);linked=os.stat('gpg-agent.conf',dir_fd=fd,follow_symlinks=False)
            if identity(before)!=identity(after) or identity(after)!=identity(linked) or len(raw)!=s.st_size:raise PermissionError(1,'identity changed')
            result.update(classify(raw));result['content_read']=True
            result['sha256']=hashlib.sha256(raw).hexdigest()
            return result
        finally:os.close(f)
    finally:os.close(fd)


def main():
    try:
        result=review();result.update(agent_started=False,signing_attempted=False,route_binding='NOT_VERIFIED')
        print(json.dumps(result,sort_keys=True))
    except Exception as error:
        print(json.dumps({'status':'BLOCKED_CONFIG_REVIEW','errno':error.errno if isinstance(error,OSError) else None,'agent_started':False,'signing_attempted':False},sort_keys=True))
        return 1
    return 0

if __name__=='__main__':raise SystemExit(main())
