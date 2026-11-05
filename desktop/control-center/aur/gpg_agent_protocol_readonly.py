"""Bounded public Assuan/peer diagnostics; no signing, key, cache or mutation."""
import os
from pathlib import Path
import re
import selectors
import socket
import stat
import struct
import subprocess
import time
from types import SimpleNamespace
from urllib.parse import unquote_to_bytes

import gpg_backend_readonly as base


def collect(argv,timeout=8):
    """Drain both client streams; retain bounded bytes, never expose raw text."""
    child=subprocess.Popen(argv,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                           env=dict(base.ENV),start_new_session=True)
    data={'stdout':bytearray(),'stderr':bytearray()};counts={'stdout':0,'stderr':0}
    limits={'stdout':4096,'stderr':2048};deadline=time.monotonic()+timeout;expired=False
    try:
        with selectors.DefaultSelector() as selector:
            for name,stream in (('stdout',child.stdout),('stderr',child.stderr)):
                os.set_blocking(stream.fileno(),False);selector.register(stream,selectors.EVENT_READ,name)
            while selector.get_map():
                left=deadline-time.monotonic()
                if left<=0:expired=True;child.kill();break
                for key,_ in selector.select(min(left,0.2)):
                    try:chunk=os.read(key.fd,4096)
                    except BlockingIOError:continue
                    if not chunk:selector.unregister(key.fileobj);continue
                    name=key.data;counts[name]+=len(chunk);room=limits[name]-len(data[name]);data[name].extend(chunk[:room])
            if not expired:
                try:child.wait(timeout=max(0.001,deadline-time.monotonic()))
                except subprocess.TimeoutExpired:expired=True;child.kill()
        child.wait()
    finally:
        child.stdout.close();child.stderr.close()
        if child.poll() is None:child.kill();child.wait()
    return SimpleNamespace(returncode=child.returncode,stdout=bytes(data['stdout']),stderr=bytes(data['stderr']),
                           stdout_bytes=counts['stdout'],stderr_bytes=counts['stderr'],
                           stdout_truncated=counts['stdout']>4096,stderr_truncated=counts['stderr']>2048,timed_out=expired)


def parse_public_response(raw):
    result={'assuan_status':'MALFORMED','data_records':0,'ok_records':0,'err_records':0,
            'status_records':0,'unknown_records':0,'error_code':None,'pid':None}
    if type(raw) is not bytes or len(raw)>4096:return result
    terminal=None;data=bytearray();malformed=False
    for line in raw.split(b'\n'):
        line=line.removesuffix(b'\r')
        if not line or line.startswith(b'#'):continue
        if line==b'S' or line.startswith(b'S '):result['status_records']+=1;continue
        if line==b'D' or line.startswith(b'D '):
            result['data_records']+=1
            if terminal is not None:malformed=True;continue
            payload=line[2:] if len(line)>1 else b''
            if re.search(rb'%(?![0-9A-Fa-f]{2})',payload):malformed=True;continue
            data.extend(unquote_to_bytes(payload))
            if len(data)>64:malformed=True
        elif line==b'OK' or line.startswith(b'OK '):
            result['ok_records']+=1
            if terminal is not None:malformed=True
            terminal='OK'
        elif line==b'ERR' or line.startswith(b'ERR '):
            result['err_records']+=1
            if terminal is not None:malformed=True
            terminal='ERR';match=re.fullmatch(rb'ERR ([0-9]{1,10})(?: .*)?',line)
            if not match or int(match[1])>2**32-1:malformed=True
            else:result['error_code']=int(match[1])
        else:result['unknown_records']+=1;malformed=True
    if not malformed and terminal is not None:
        result['assuan_status']=terminal
        if terminal=='OK' and re.fullmatch(rb'[1-9][0-9]{0,9}(?:\r?\n)?',data):
            pid=int(data)
            if pid<=2**31-1:result['pid']=pid
    elif not raw.strip():result['assuan_status']='EMPTY'
    return result


def peer_metadata(path):
    """Same fixed socket: SO_PEERCRED + greeting only, no command sent."""
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as client:
        client.settimeout(3);client.connect(path)
        pid,uid,gid=struct.unpack('3i',client.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,struct.calcsize('3i')))
        report={'pid':pid,'uid':uid,'gid':gid,'greeting':'NOT_READ','process':'unknown','process_uid_verified':False}
        if uid!=949:return report
        try:
            lines=(Path('/proc')/str(pid)/'status').read_text().splitlines()
            uids=[line.split()[1:] for line in lines if line.startswith('Uid:')]
            report['process_uid_verified']=len(uids)==1 and uids[0]==['949']*4
            report['process']='gpg-agent' if (Path('/proc')/str(pid)/'comm').read_text().strip()=='gpg-agent' else 'other'
        except OSError:return report
        greeting=bytearray()
        while b'\n' not in greeting and len(greeting)<=1024:
            chunk=client.recv(min(128,1025-len(greeting)))
            if not chunk:break
            greeting.extend(chunk)
        first=bytes(greeting).split(b'\n',1)[0].removesuffix(b'\r')
        report['greeting']='OK' if first==b'OK' or first.startswith(b'OK ') else 'ERR' if first.startswith(b'ERR ') else 'INVALID'
        return report


def inspect(run=collect,peer=peer_metadata,socket_check=base.safe_socket):
    if (os.getuid(),os.geteuid(),os.getgid(),os.getegid(),os.getgroups())!=(949,949,948,948,[948]):
        raise ValueError('Exact UID949 context required')
    report={'uid':949,'gid':948,'agent_access':'BLOCKED','transport':'NOT_TESTED','agent_autostart':False,
            'signing_attempted':False,'secret_key_presence':'NOT_PROBED','passphrase_cache':'NOT_PROBED'}
    if base.home_metadata()!='PASS':report['result']='HOME_METADATA_BLOCKED';return report
    located=run(['/usr/bin/gpgconf','--homedir',base.HOME,'--list-dirs','agent-socket'])
    if located.returncode or getattr(located,'stdout_truncated',False):report['result']='SOCKET_RESOLUTION_BLOCKED';return report
    try:path=located.stdout.decode('ascii').strip()
    except UnicodeError:report['result']='SOCKET_RESOLUTION_BLOCKED';return report
    report['socket_state']=socket_check(path)
    if report['socket_state']!='safe-existing-socket':report['result']='SOCKET_METADATA_BLOCKED';return report
    before=os.lstat(path)
    try:info=peer(path)
    except OSError:report.update(transport='BLOCKED',result='PEER_CONNECTION_BLOCKED');return report
    report['transport']='CONNECTED'
    report.update(peer_uid=info['uid'],peer_gid=info['gid'],peer_process=info['process'],
                  peer_process_uid_verified=info['process_uid_verified'],greeting=info['greeting'])
    if info['uid']!=949 or not info['process_uid_verified']:
        report['result']='PEER_OWNER_BLOCKED';return report
    if info['process']!='gpg-agent':report['result']='PEER_NOT_LOCAL_GPG_AGENT';return report
    if info['greeting']!='OK':report['result']='ASSUAN_GREETING_BLOCKED';return report
    report['transport']='PASS'
    answer=run(['/usr/bin/gpg-connect-agent','--no-autostart','--no-history','--no-ext-connect',
                '--homedir',base.HOME,'--raw-socket',path,'GETINFO pid','/bye'])
    report.update(agent_query_returncode=answer.returncode,stdout_bytes=getattr(answer,'stdout_bytes',len(answer.stdout)),
                  stderr_bytes=getattr(answer,'stderr_bytes',len(answer.stderr)),
                  stderr_present=bool(answer.stderr),stdout_truncated=getattr(answer,'stdout_truncated',False),
                  stderr_truncated=getattr(answer,'stderr_truncated',False))
    after=os.lstat(path)
    if (before.st_dev,before.st_ino,before.st_uid,before.st_mode)!=(after.st_dev,after.st_ino,after.st_uid,after.st_mode):
        report['result']='SOCKET_REPLACED_DURING_QUERY';return report
    if answer.returncode or report['stdout_truncated'] or getattr(answer,'timed_out',False):
        report['result']='CLIENT_QUERY_BLOCKED';return report
    parsed=parse_public_response(answer.stdout)
    pid=parsed.pop('pid');report.update(parsed)
    if parsed['assuan_status']=='OK' and pid is not None and pid!=info['pid']:
        report.update(pid_matches_peer=False,result='PID_PEER_MISMATCH');return report
    report['agent_access']='PASS'  # Verified same-user local gpg-agent transport, not key use.
    if parsed['assuan_status']=='ERR':report['result']='AGENT_ACCESS_PASS_GETINFO_ERR'
    elif parsed['assuan_status']=='OK' and pid==info['pid']:
        report.update(pid_matches_peer=True,result='AGENT_ACCESS_PASS_KEY_CACHE_UNKNOWN')
    else:report['result']='AGENT_ACCESS_PASS_RESPONSE_UNCLASSIFIED'
    return report


def main():
    import json
    try:print(json.dumps(inspect(),sort_keys=True,separators=(',',':')))
    except Exception:print(json.dumps({'agent_access':'BLOCKED','result':'READONLY_CONTEXT_OR_IO_BLOCKED','signing_attempted':False}))


if __name__=='__main__':main()
