"""Version-locked diagnostics bundle entry point. --protocol-check never signs.

Actual signing additionally requires a fresh exact approval/pending operation
and explicit user consent. This module never creates approval or pending.
"""
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
import stat
import subprocess
import sys
import time

PROTOCOL='zethropol-manifest-diagnostics/1'
ROOT_UID=0
ROOT_GID=0
DENIED_HASHES={'4597d7e9243d516cf55718caa8aedaf9efc4901c351d94364508cc0ff3a2dc21'}
ACL={'system.posix_acl_access','system.posix_acl_default'}


def canonical(value):return json.dumps(value,sort_keys=True,separators=(',',':')).encode()


def read_fixed(path,limit=1024*1024):
    path=Path(path)
    for item in (path,*path.parents):
        s=item.lstat()
        if s.st_uid!=ROOT_UID or s.st_gid!=ROOT_GID or s.st_mode&0o022 or stat.S_ISLNK(s.st_mode):
            raise RuntimeError('Unsafe bundle ownership/path')
        if ACL.intersection(os.listxattr(item,follow_symlinks=False)):
            raise RuntimeError('Unexpected bundle ancestor ACL')
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK|os.O_CLOEXEC)
    try:
        s=os.fstat(fd)
        if not stat.S_ISREG(s.st_mode) or s.st_nlink!=1 or not 0<s.st_size<=limit or ACL.intersection(os.listxattr(fd)):
            raise RuntimeError('Unsafe bounded bundle file')
        raw=os.pread(fd,s.st_size,0)
        after=os.fstat(fd)
        if (s.st_dev,s.st_ino,s.st_mtime_ns,s.st_ctime_ns,s.st_size)!=(after.st_dev,after.st_ino,after.st_mtime_ns,after.st_ctime_ns,after.st_size):
            raise RuntimeError('Bundle changed during read')
        return raw
    finally:os.close(fd)


def verify_bundle(directory):
    directory=Path(directory)
    raw=read_fixed(directory/'bundle.json')
    bundle=json.loads(raw)
    release=hashlib.sha256(raw).hexdigest()
    if (raw!=canonical(bundle) or set(bundle)!={'schema','protocol','files'} or
            type(bundle['schema']) is not int or bundle['schema']!=1 or bundle['protocol']!=PROTOCOL or
            directory.name!='aur-manifest-diagnostics-'+release):
        raise RuntimeError('Exact versioned bundle required')
    expected={'bundle.json'}
    for row in bundle['files']:
        if (not isinstance(row,dict) or set(row)!={'name','sha256','mode'} or
                not isinstance(row['name'],str) or not re.fullmatch(r'[a-z_]+\.py',row['name']) or
                row['name'] in expected or row['mode']!='0644' or
                not isinstance(row['sha256'],str) or not re.fullmatch('[0-9a-f]{64}',row['sha256'])):
            raise RuntimeError('Exact bundle member contract required')
        expected.add(row['name'])
        file=directory/row['name']
        if hashlib.sha256(read_fixed(file)).hexdigest()!=row['sha256'] or stat.S_IMODE(file.lstat().st_mode)!=0o644:
            raise RuntimeError('Mixed or changed bundle member')
    if set(os.listdir(directory))!=expected or not {'manifest_workflow.py','manifest_failure_capture.py','manifest_bundle_launcher.py'}<=expected:
        raise RuntimeError('Incomplete/extra bundle members')
    return bundle


def protocol_check(producer,consumer):
    if producer.DIAGNOSTIC_PROTOCOL!=PROTOCOL or consumer.PROTOCOL_ID!=PROTOCOL:
        raise RuntimeError('Mixed diagnostic protocol')
    digest='0'*64
    marker=producer.failure_record(producer.SigningPhaseRejected('gpg-signing',2,kind='backend-nonzero'),digest).encode()
    result=consumer.process_outcome(1,marker,digest)
    if (result['diagnostic_status']!='validated' or result['diagnostic']['backend_returncode']!=2 or
            result['retry_permitted'] is not False or result['signature_verified'] is not False):
        raise RuntimeError('Producer/consumer handshake failed')


def drop_identity():
    os.setgroups([948]);os.setgid(948);os.setuid(949)


def observe_process(command,consumer,digest,audit,*,popen=subprocess.Popen,clock=time.monotonic,timeout=90):
    """Bounded stderr drain; never audit argv, stderr or exception messages."""
    capture=consumer.BoundedFailureCapture()
    start=clock()
    try:
        child=popen(command,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,
                    preexec_fn=drop_identity,start_new_session=True,
                    env={'PATH':'/usr/bin','LC_ALL':'C','HOME':'/var/lib/zethropol-aur-release'})
    except (OSError,subprocess.SubprocessError):
        return consumer.audit_process_result(capture,None,digest,audit,launch_failed=True)
    timed_out=False
    try:
        os.set_blocking(child.stderr.fileno(),False)
        with selectors.DefaultSelector() as selector:
            selector.register(child.stderr,selectors.EVENT_READ)
            while selector.get_map():
                remaining=timeout-(clock()-start)
                if remaining<=0:
                    timed_out=True;os.killpg(child.pid,signal.SIGKILL);break
                for key,_ in selector.select(min(remaining,0.25)):
                    data=os.read(key.fd,4096)
                    if data:capture.feed(data)
                    else:selector.unregister(key.fileobj)
            if not timed_out:
                try:child.wait(timeout=max(0.001,timeout-(clock()-start)))
                except subprocess.TimeoutExpired:
                    timed_out=True;os.killpg(child.pid,signal.SIGKILL)
        child.wait()
    finally:child.stderr.close()
    return consumer.audit_process_result(capture,None if timed_out else child.returncode,digest,audit,timed_out=timed_out)


def main():
    here=Path(__file__).resolve().parent
    verify_bundle(here)
    sys.path.insert(0,str(here))
    producer=importlib.import_module('manifest_workflow')
    consumer=importlib.import_module('manifest_failure_capture')
    if Path(producer.__file__).parent!=here or Path(consumer.__file__).parent!=here:
        raise RuntimeError('Cross-bundle module selection')
    protocol_check(producer,consumer)
    if sys.argv[1:]==['--protocol-check']:
        print('PASS protocol='+PROTOCOL+' signing=false');return
    # Deployment readiness intentionally exposes no sign CLI. A future new
    # exact transaction must bind approval, pending, durable audit and public
    # post-verification before using observe_process. No old attempt is resumed.
    raise RuntimeError('Deployment entry point is protocol-check only; fresh signing transaction required')


if __name__=='__main__':
    try:main()
    except Exception:raise SystemExit('BLOCKED diagnostics bundle; no signing or retry')
