"""Durable desktop-user build journal; never privileged authorization storage."""
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import stat
import time
import uuid

from worker import Rejected

TERMINAL = {'failed', 'cancelled', 'interrupted', 'built-untrusted'}


class Journal:
    def __init__(self, directory):
        self.path = Path(directory)
        self.fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        info = os.fstat(self.fd)
        if info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700:
            os.close(self.fd)
            raise Rejected('Journal requires private, caller-owned mode 0700 directory')
        # Anchor SQLite and its WAL files to the held directory, not its path.
        self.db = sqlite3.connect(f'/proc/self/fd/{self.fd}/operations.sqlite', timeout=5)
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.execute('PRAGMA synchronous=FULL')
        self.db.execute('''CREATE TABLE IF NOT EXISTS operations (
            id TEXT PRIMARY KEY, uid INTEGER, plan TEXT, state TEXT,
            cancel INTEGER, deadline REAL, boot TEXT, result TEXT)''')
        self.db.commit()
        os.fsync(self.fd)
        self.boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()

    def create(self, plan, timeout):
        operation = str(uuid.uuid4())
        with self.db:
            self.db.execute('INSERT INTO operations VALUES (?,?,?,?,?,?,?,?)',
                (operation, os.getuid(), plan, 'queued', 0,
                 time.clock_gettime(time.CLOCK_BOOTTIME)+timeout, self.boot, '{}'))
        return operation

    def status(self, operation):
        if str(uuid.UUID(operation)) != operation:
            raise Rejected('Invalid operation ID')
        row = self.db.execute('SELECT uid,plan,state,cancel,deadline,boot,result FROM operations WHERE id=?', (operation,)).fetchone()
        if row is None or row[0] != os.getuid():
            raise Rejected('Unknown or foreign operation')
        return dict(zip(('uid','plan','state','cancel','deadline','boot','result'), row))

    def cancel(self, operation):
        row = self.status(operation)
        if row['state'] not in TERMINAL:
            with self.db:
                self.db.execute("UPDATE operations SET cancel=1 WHERE id=? AND state IN ('queued','running')", (operation,))

    def check(self, operation):
        row = self.status(operation)
        if row['cancel']:
            raise Rejected('Cancelled')
        if row['boot'] != self.boot or time.clock_gettime(time.CLOCK_BOOTTIME) >= row['deadline']:
            raise Rejected('Worker timed out')
        if row['state'] != 'running':
            raise Rejected('Operation no longer running')

    def scope(self, operation, unit):
        row = self.status(operation)
        if row['state'] != 'running':
            raise Rejected('Scope requires running operation')
        data = json.loads(row['result'])
        data.setdefault('scopes', []).append(unit)
        with self.db:
            self.db.execute("UPDATE operations SET result=? WHERE id=? AND state='running'",
                (json.dumps(data), operation))

    def progress(self, operation, stage):
        row = self.status(operation)
        if row['state'] != 'running':
            raise Rejected('Progress requires running operation')
        data = json.loads(row['result']); data['stage'] = stage
        with self.db:
            self.db.execute("UPDATE operations SET result=? WHERE id=? AND state='running'",
                            (json.dumps(data), operation))

    def finish(self, operation, result):
        if result['state'] not in TERMINAL:
            raise Rejected('Invalid terminal build state')
        result=dict(result)
        with self.db:
            changed=self.db.execute("UPDATE operations SET state=?,result=? WHERE id=? AND state='running' AND (? != 'built-untrusted' OR cancel=0)",
                (result['state'],json.dumps(result,sort_keys=True),operation,result['state'])).rowcount
            if changed!=1 and result['state']=='built-untrusted' and self.status(operation)['cancel']:
                result.update(state='cancelled',reason='Cancellation won terminal publication; no artifact handoff',archives=[])
                changed=self.db.execute("UPDATE operations SET state=?,result=? WHERE id=? AND state='running'",
                    (result['state'],json.dumps(result,sort_keys=True),operation)).rowcount
            if changed!=1: raise Rejected('Terminal result cannot be overwritten')
        return result

    @contextmanager
    def lease(self, operation):
        self.status(operation)
        fd = os.open(operation+'.lease', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600, dir_fd=self.fd)
        try:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise Rejected('Worker already owns operation') from error
            with self.db:
                changed = self.db.execute("UPDATE operations SET state='running' WHERE id=? AND state='queued'", (operation,)).rowcount
                if changed != 1:
                    raise Rejected('Operation cannot be replayed')
            yield
        finally:
            os.close(fd)

    def recover(self, only=None):
        recovered = []
        for (operation,) in self.db.execute("SELECT id FROM operations WHERE state IN ('running','queued','interrupted')").fetchall():
            if only is not None and operation != only: continue
            prior=self.status(operation)
            if prior['state']=='interrupted' and json.loads(prior['result']).get('cleanup')!='unknown': continue
            fd = os.open(operation+'.lease', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600, dir_fd=self.fd)
            try:
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    continue
                active = json.loads(self.status(operation)['result']).get('scopes', [])
                for unit in active:
                    from scoped import stop_scope
                    stop_scope(operation, unit)
                with self.db:
                    self.db.execute("UPDATE operations SET state='interrupted',result=? WHERE id=? AND state IN ('queued','running','interrupted')",
                        (json.dumps({'state': 'interrupted', 'cleanup':'verified', 'reason': 'Supervisor lease lost; scopes stopped; no automatic retry'}), operation))
                recovered.append(operation)
            finally:
                os.close(fd)
        return recovered

    def close(self):
        self.db.close()
        os.close(self.fd)


class Supervisor:
    """Single-thread owner of a real worker. No install or authorization API."""
    def __init__(self, store):
        self.journal = Journal(store)

    def run(self, worker, plan, approved_hash, recipe, rootfs, timeout=1800, cancel=lambda: False, notify=lambda event: None):
        worker.journal = self.journal
        operation = self.journal.create(approved_hash, timeout)
        with self.journal.lease(operation):
            notify({'event':'started','operation':operation,'plan_sha256':approved_hash})
            def report(stage):
                self.journal.progress(operation, stage)
                notify({'event':'progress','operation':operation,'stage':stage})
            worker.report = report
            def cancelled():
                if cancel():
                    self.journal.cancel(operation)
                self.journal.check(operation)
                return False
            result = worker.run(plan, approved_hash, recipe, rootfs, cancelled, timeout, operation)
            # Cleanup is verified before publishing a terminal result. Never
            # convert an uncertain cgroup stop into successful cancellation.
            try:
                from scoped import stop_scope
                for unit in json.loads(self.journal.status(operation)['result']).get('scopes', []):
                    stop_scope(operation, unit)
                if cancel():
                    self.journal.cancel(operation)
                if self.journal.status(operation)['cancel']:
                    result.update(state='cancelled', reason='Cancelled; worker scopes stopped', archives=[])
                result['cleanup'] = 'verified'
            except (OSError, Rejected, subprocess.SubprocessError) as error:
                result.update(state='interrupted', cleanup='unknown', reason=str(error), archives=[],scopes=json.loads(self.journal.status(operation)['result']).get('scopes',[]))
            return self.journal.finish(operation, result)
