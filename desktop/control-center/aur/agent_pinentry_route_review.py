"""No pinentry execution, agent start or key access. Exact-config-bound review."""
import hashlib,json,os,re,stat,subprocess
import agent_owner_config_review as base
EXPECTED='250266c8436ebf3f94595d564f042fc5691b7d7104ca4f71c4d4f4ac1f804bae'
CANDIDATE='/run/user/949/gnupg/d.rjcy1hk6m59tkb59mjte8uhm/S.gpg-agent'
ENV={'PATH':'/usr/bin','LC_ALL':'C'}


def target(raw):
    if hashlib.sha256(raw).hexdigest()!=EXPECTED:raise ValueError('configuration changed')
    rows=[x.strip() for x in raw.decode('ascii').splitlines() if x.strip() and not x.strip().startswith('#')]
    if len(rows)!=1:raise ValueError('unexpected options')
    fields=rows[0].split()
    if len(fields)!=2 or fields[0]!='pinentry-program':raise ValueError('unexpected syntax')
    path=fields[1]
    if not path.startswith('/') or any(x in ('','.','..') for x in path.split('/')[1:]):raise ValueError('unsafe path')
    return path


def trusted(path,executable=True):
    """Reject all symlinks, writable ancestors, special bits and ACLs."""
    fd=os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
    try:
        parts=path.strip('/').split('/')
        for i,part in enumerate(parts):
            flags=os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC|os.O_NONBLOCK
            if i<len(parts)-1:flags|=os.O_DIRECTORY
            child=os.open(part,flags,dir_fd=fd);os.close(fd);fd=child
            s=os.fstat(fd)
            if s.st_uid!=0 or s.st_gid!=0 or s.st_mode&0o7022 or base.ACL.intersection(os.listxattr(fd)):raise ValueError('untrusted metadata')
            if i<len(parts)-1:
                if not stat.S_ISDIR(s.st_mode) or not s.st_mode&0o001:raise ValueError('untraversable')
            elif not stat.S_ISREG(s.st_mode) or s.st_nlink!=1 or (executable and not s.st_mode&0o001):raise ValueError('not executable')
        return {'device':str(s.st_dev),'inode':str(s.st_ino),'mode':oct(stat.S_IMODE(s.st_mode)),'uid':s.st_uid,'gid':s.st_gid,'acl_present':False,'uid949_dac_access':'PASS'}
    finally:os.close(fd)


def query(argv):
    return subprocess.run(argv,env=ENV,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=8)


def inspect(raw):
    path=target(raw)
    result={'classification':'PINENTRY_REVIEW','headless_pinentry_required':False,'pinentry_executed':False,'service_interactive_context':'ABSENT_BY_DESIGN','credential_readiness':'NOT_PROBED'}
    try:
        result['target_metadata']=trusted(path)
        trusted('/usr/bin/pacman')
        owner=query(['/usr/bin/pacman','-Qqo','--',path])
        # Never emit the executable path or arbitrary package-query output.
        package=owner.stdout.strip()
        if owner.returncode==0 and package and all(c in b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@._+-' for c in package):
            version=query(['/usr/bin/pacman','-Q','--',package.decode('ascii')])
            result['package_ownership']='PASS' if version.returncode==0 else 'BLOCKED'
            fields=version.stdout.decode('ascii',errors='ignore').split()
            if version.returncode==0 and len(fields)==2 and fields[0]==package.decode('ascii') and re.fullmatch(r'[0-9][0-9A-Za-z.+:~_-]{0,79}',fields[1]):
                result['target_package_version']=fields[1]
            gnupg=query(['/usr/bin/pacman','-Q','--','gnupg'])
            fields=gnupg.stdout.decode('ascii',errors='ignore').split()
            if gnupg.returncode==0 and len(fields)==2 and fields[0]=='gnupg' and re.fullmatch(r'[0-9][0-9A-Za-z.+:~_-]{0,79}',fields[1]):result['gnupg_package_version']=fields[1]
        else:result['package_ownership']='BLOCKED'
        result['version_compatibility']='NOT_PROVEN_NOT_EXECUTED'
    except Exception:
        result['target_security']='BLOCKED_NOFOLLOW_OR_METADATA_OR_PACKAGE_REVIEW'
    return result


def route():
    from native_socket_route_readonly import probe
    return probe()


def main():
    original=base.classify
    try:
        base.classify=inspect
        result=base.review()
        if result.get('content_read'):result.update(route())
        result.update(agent_started=False,signing_attempted=False)
        print(json.dumps(result,sort_keys=True));return 0
    except Exception:
        print(json.dumps({'status':'BLOCKED_PINENTRY_ROUTE_REVIEW','agent_started':False,'signing_attempted':False}));return 1
    finally:base.classify=original

if __name__=='__main__':raise SystemExit(main())
