"""Unprivileged user-manager cgroup wrapper. No direct-execution fallback."""
import os
import subprocess
import uuid

from worker import Worker, Rejected


def manager_env():
    runtime = f'/run/user/{os.getuid()}'
    return {'PATH':'/usr/bin', 'LC_ALL':'C', 'XDG_RUNTIME_DIR':runtime,
            'DBUS_SESSION_BUS_ADDRESS':f'unix:path={runtime}/bus'}


def stop_scope(operation, unit):
    # Journal contents are not allowed to name arbitrary user units.
    if str(uuid.UUID(operation)) != operation or not unit.startswith('zethropol-aur-'+operation+'-') or not unit.endswith('.scope'):
        raise Rejected('Invalid worker scope identity')
    suffix = unit[len('zethropol-aur-'+operation+'-'):-len('.scope')]
    if not suffix.isdecimal(): raise Rejected('Invalid phase identity')
    queried = subprocess.run(['/usr/bin/systemctl','--user','show','--property=LoadState','--value',unit],
        env=manager_env(), capture_output=True, text=True, timeout=10)
    if queried.returncode or queried.stdout.strip() not in ('not-found', 'loaded'):
        raise Rejected('Cannot inspect worker cgroup; outcome unknown')
    if queried.stdout.strip() == 'not-found': return
    stopped = subprocess.run(['/usr/bin/systemctl','--user','stop',unit], env=manager_env(), capture_output=True, timeout=10)
    if stopped.returncode:
        # --collect may unload the scope between show and stop. Accept only an
        # independently observed absent/inactive unit, never a failed bus call.
        again = subprocess.run(['/usr/bin/systemctl','--user','show','--property=LoadState','--value',unit],
            env=manager_env(), capture_output=True, text=True, timeout=10)
        if again.returncode == 0 and again.stdout.strip() == 'not-found': return
        raise Rejected('Cannot stop worker cgroup; outcome unknown: '+stopped.stderr.decode(errors='replace')[:1000])


class ScopedWorker(Worker):
    """Fixed resource limits and KillMode supplied by systemd scope semantics.

    Supervisor injects a durable journal before any recipe code starts. The
    ordinary Worker remains a laboratory primitive, not a production executor.
    """
    def execute(self, args, logdir, cancel, timeout):
        if not hasattr(self, 'journal'):
            raise Rejected('Scoped worker requires durable Supervisor')
        operation = logdir.name
        phase = getattr(self, '_phase', 0) + 1
        self._phase = phase
        unit = f'zethropol-aur-{operation}-{phase}.scope'
        self.journal.scope(operation, unit)
        with open(logdir / 'worker.log', 'a') as log:
            log.write('\nSupervisor scope: '+unit+' MemoryMax=2G TasksMax=128 CPUQuota=200%\n')
        wrapped = ['/usr/bin/systemd-run','--user','--scope','--collect','--quiet',
            '--unit='+unit, '--property=MemoryMax=2G', '--property=MemorySwapMax=0',
            '--property=TasksMax=128','--property=CPUQuota=200%',
            '--property=RuntimeMaxSec='+str(max(1,int(timeout))), '--', *args]
        self._process_env = manager_env()
        try:
            super().execute(wrapped, logdir, cancel, timeout)
        finally:
            stop_scope(operation, unit)
