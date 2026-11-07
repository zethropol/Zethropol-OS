"""Managed non-root makepkg composition using the existing quota worker and journal.

Artifacts are sealed and previewed by the Update broker before separate PolicyKit
installation consent. Project signing and activation are not part of this path.
"""
from contextlib import ExitStack
import json
import os
from pathlib import Path
import stat
import sys
import signal
import uuid

from root_limits import root_limits
from worker import Rejected, canonical, digest
from quota import QuotaWorker
from journal import Supervisor, Journal
HELPER = '/usr/lib/zethropol/aur-quota-mount'


def run(request, cancel=lambda: False, notify=lambda event: None, require_metadata=False, timeout=1800):
    if os.getuid() == 0 or os.geteuid() == 0: raise Rejected('Build must never run as root')
    if set(request) != {'store','recipe','plan','plan_sha256'} or request['plan'].get('kind') != 'normal-makepkg':
        raise Rejected('Normal reviewed makepkg request required; signed-root provisioning retired')
    raw=json.dumps(request['plan'],sort_keys=True,separators=(',',':'),ensure_ascii=False).encode()
    if digest(raw) != request['plan_sha256']: raise Rejected('Reviewed plan changed')
    store=Path(request['store']);info=store.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid!=os.getuid() or stat.S_IMODE(info.st_mode)!=0o700: raise Rejected('Private desktop build store required')
    supervisor=Supervisor(store)
    try:
        supervisor.journal.recover()
        return supervisor.run(QuotaWorker(store,Path(HELPER)),request['plan'],request['plan_sha256'],request['recipe'],None,timeout=timeout,cancel=cancel,notify=notify)
    finally: supervisor.journal.close()


class ManagedChannel:
    """Private parent pipe; only the active execution token may cancel it."""
    def __init__(self, fd=0, output=None):
        self.fd, self.output = fd, output or sys.stdout
        self.buffer=b''; self.cancel_requested=False; self.execution=None; self.operation=None

    def initial(self):
        while b'\n' not in self.buffer:
            block=os.read(self.fd, 65536)
            if not block: raise Rejected('Missing managed request')
            self.buffer+=block
            if len(self.buffer)>1024**2+65536: raise Rejected('Build request too large')
        first,self.buffer=self.buffer.split(b'\n',1)
        if len(first)>1024**2: raise Rejected('Build request too large')
        envelope=json.loads(first)
        if set(envelope)!={'execution','request'} or str(uuid.UUID(envelope['execution']))!=envelope['execution']:
            raise Rejected('Exact managed execution envelope required')
        self.execution=envelope['execution']
        os.set_blocking(self.fd,False)
        return envelope['request']

    def cancelled(self):
        while True:
            try: block=os.read(self.fd,4096)
            except BlockingIOError: break
            if not block: self.cancel_requested=True; break
            self.buffer+=block
            if len(self.buffer)>4096: self.cancel_requested=True; self.buffer=b''; break
        while b'\n' in self.buffer:
            line,self.buffer=self.buffer.split(b'\n',1)
            try:
                if json.loads(line)=={'cancel':self.execution}: self.cancel_requested=True
            except (ValueError,TypeError): self.cancel_requested=True
        return self.cancel_requested

    def emit(self,event):
        if event.get("event")=="started": self.operation=event["operation"]
        try:
            self.output.write(json.dumps(dict(event,execution=self.execution),sort_keys=True)+'\n')
            self.output.flush()
        except (BrokenPipeError,OSError): self.cancel_requested=True


def main():
    channel=None
    try:
        if len(sys.argv) in (3,4) and sys.argv[1]=='--recover':
            if os.getuid()==0 or os.geteuid()==0: raise Rejected('Never root')
            journal=Journal(sys.argv[2])
            try:
                journal.recover(sys.argv[3] if len(sys.argv)==4 else None)
            finally: journal.close()
            return 0
        if sys.argv[1:]!=['--managed']:
            raise Rejected('Manual build requests disabled; reviewed makepkg request required')
        channel=ManagedChannel()
        request=channel.initial()
        for signum in (signal.SIGTERM,signal.SIGINT):
            signal.signal(signum,lambda *_: setattr(channel,'cancel_requested',True))
        channel.emit({'event':'progress','stage':'validating-reviewed-makepkg'})
        if channel.cancelled(): raise Rejected('Cancelled')
        result=run(request,channel.cancelled,channel.emit)
        channel.emit({'event':'result','result':result})
        return 0 if result['state']=='built-untrusted' else 1
    except (OSError,ValueError,KeyError,TypeError,Rejected) as error:
        result={'source':'aur','state':'cancelled' if str(error)=='Cancelled' else 'blocked','reason':str(error),'cleanup':'verified'}
        if channel:
            if channel.operation: result.update(operation=channel.operation,state='interrupted',cleanup='unknown')
            channel.emit({'event':'result','result':result})
        else: print(json.dumps(result))
        return 1


if __name__=='__main__':
    raise SystemExit(main())
