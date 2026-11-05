"""Unprivileged composition of existing signed-root, quota worker and journal.

Emits built-untrusted/awaiting-attestation, NEVER an installation grant or a
self-signed receipt. The broker independently authenticates the later FD handoff.
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
from trust import SignedArchive, TrustPolicy, approved_manifest
from worker import tree, matches_root
from quota import QuotaWorker
from journal import Supervisor, Journal

POLICY = '/etc/zethropol-update/aur-trust/policy.json'
DISTRIBUTION_POLICY = '/etc/zethropol-update/aur-trust/distribution.json'
HELPER = '/usr/lib/zethropol/aur-quota-mount'


def run(request, cancel=lambda: False, notify=lambda event: None, require_metadata=False, timeout=1800):
    if os.getuid() == 0 or os.geteuid() == 0:
        raise Rejected('Build pipeline must run as the desktop user, never root')
    if cancel(): raise Rejected('Cancelled')
    if set(request) != {'store','recipe','plan','plan_sha256','manifest','manifest_signature','packages','build_root'}:
        raise Rejected('Exact bounded build request required')
    if not isinstance(request['packages'], list) or not 1 <= len(request['packages']) <= 256:
        raise Rejected('Bounded signed root dependency closure required')
    if digest(canonical(request['plan'])) != request['plan_sha256']:
        raise Rejected('Reviewed source plan changed')
    with ExitStack() as stack:
        manifest_policy = TrustPolicy.load(POLICY, role='manifest')
        stack.callback(manifest_policy.keyring.close)
        distribution_policy = TrustPolicy.load(DISTRIBUTION_POLICY)
        stack.callback(distribution_policy.keyring.close)
        def signed(path, signature, policy, archive_limit=256*1024**2):
            archive_fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
            try:
                signature_fd = os.open(signature, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
                try:
                    return stack.enter_context(SignedArchive(archive_fd, signature_fd, policy, archive_limit=archive_limit))
                finally:
                    os.close(signature_fd)
            finally:
                os.close(archive_fd)
        manifest = signed(request['manifest'], request['manifest_signature'], manifest_policy)
        root_plan, root_id = approved_manifest(manifest)
        package_limit, archive_limit, _, _ = root_limits(root_plan['schema'])
        if len(request['packages']) != len(root_plan['packages']) or len(request['packages']) > package_limit:
            raise Rejected('Signed root package count mismatch')
        packages = []
        total_bytes = 0
        for package in request['packages']:
            if cancel(): raise Rejected('Cancelled')
            if set(package) != {'archive','signature'}:
                raise Rejected('Signed root archive/signature pairs required')
            item = signed(package['archive'], package['signature'], distribution_policy,
                          archive_limit=archive_limit-total_bytes)
            total_bytes += item.archive.size
            if total_bytes > archive_limit:
                raise Rejected('Aggregate signed input quota exceeded')
            packages.append(item)
        store = Path(request['store'])
        info = store.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700:
            raise Rejected('Private caller-owned build store required')
        supervisor = Supervisor(store)
        stack.callback(supervisor.journal.close)
        supervisor.journal.recover()
        # Provisioning is the existing root-owned, bounded, script-free stage.
        # Never replace Worker.tree(trusted=True) with a caller-owned extraction.
        root_plan, root_id = approved_manifest(manifest)
        root = Path(request['build_root'])
        expected_root = Path('/var/lib/zethropol/aur-build-roots') / manifest.archive.sha256
        if root != expected_root or root.resolve() != root:
            raise Rejected('Server-provisioned immutable build root required')
        root_tree = tree(root, trusted=True)
        if not matches_root(request['plan'], root_tree):
            raise Rejected('Provisioned build root differs from reviewed source plan')
        provenance = json.loads((root/'.provenance.json').read_bytes())
        expected_packages = [{'name':expected['name'], 'sha256':signed.archive.sha256,
                              'signer':signed.signer}
                             for expected,signed in zip(root_plan['packages'],packages)]
        if len(packages) != len(root_plan['packages']) or any(
                signed.archive.sha256 != expected['sha256']
                for expected,signed in zip(root_plan['packages'],packages)):
            raise Rejected('Signed distribution root closure mismatch')
        if provenance != {'manifest_sha256':root_id, 'scope':'production-policy-only',
                          'packages':expected_packages}:
            raise Rejected('Root-owned provisioning provenance does not match signed inputs')
        worker = QuotaWorker(store, Path(HELPER))
        worker.root_schema=root_plan['schema']
        worker.capture_metadata=require_metadata
        result = supervisor.run(worker, request['plan'], request['plan_sha256'],
                                request['recipe'], root, cancel=cancel, notify=notify, timeout=timeout)
        if result['state'] == 'built-untrusted':
            result['handoff'] = 'awaiting-independent-attestation'
            result['build_root_sha256'] = manifest.archive.sha256
            result['reason'] = 'Build output is untrusted until an approved independent receipt and broker native preview pass; installation is not authorized'
        return result


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
            raise Rejected('Manual build requests disabled; authenticated provisioning delivery required')
        channel=ManagedChannel()
        request=channel.initial()
        for signum in (signal.SIGTERM,signal.SIGINT):
            signal.signal(signum,lambda *_: setattr(channel,'cancel_requested',True))
        channel.emit({'event':'progress','stage':'validating-provisioning'})
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
