#!/usr/bin/python3
"""Driver inside a private systemd/PID/user/cgroup namespace only."""
import json, os, pathlib, subprocess, time, traceback, ctypes, shutil

RESULT = pathlib.Path('/results/result.json')
checks = {}
isolated = False

def ctl(*args, ok=True):
    p = subprocess.run(['/usr/bin/systemctl', *args], capture_output=True, text=True, timeout=45)
    if ok and p.returncode:
        diagnostic = subprocess.run(['/usr/bin/systemctl', 'show', 'lab-worker@normal.service', '-p', 'Result', '-p', 'ExecMainStatus', '-p', 'ExecStart', '-p', 'ExecStopPost'], capture_output=True, text=True).stdout
        journal = subprocess.run(['/usr/bin/journalctl', '--no-pager', '-p', 'warning'], capture_output=True, text=True).stdout
        raise AssertionError(f'systemctl {args}: {p.returncode} {p.stdout} {p.stderr} {diagnostic} {journal}')
    return p

def wait_file(name):
    path = pathlib.Path('/results') / name
    deadline = time.monotonic() + 8
    while not path.exists():
        assert time.monotonic() < deadline, f'not created: {path}'
        time.sleep(.05)
    return path

def state(name):
    return json.loads((pathlib.Path('/run/zethropol-update') / (name + '.json')).read_text())

def properties(name):
    text = ctl('show', f'lab-worker@{name}.service', '-p', 'ActiveState', '-p', 'SubState', '-p', 'Result', '-p', 'NRestarts', '-p', 'ExecMainStatus', '-p', 'ControlGroup').stdout
    return dict(line.split('=', 1) for line in text.splitlines() if '=' in line)

def stopped(name):
    wait_file(name + '.stopped')
    deadline = time.monotonic() + 8
    while properties(name)['SubState'] not in ('dead', 'failed'):
        assert time.monotonic() < deadline
        time.sleep(.05)
    return state(name)

try:
    host = json.loads(pathlib.Path('/fixture/host-namespaces.json').read_text())
    current = {n: os.readlink('/proc/self/ns/' + n) for n in host}
    assert all(current[n] != host[n] for n in host), (host, current)
    assert pathlib.Path('/proc/1/comm').read_text().strip() == 'systemd'
    assert not pathlib.Path('/usr/bin/pacman').exists()
    assert not pathlib.Path('/usr/bin/snapper').exists()
    assert not pathlib.Path('/usr/lib/zethropol/zethropol-update').exists()
    assert pathlib.Path('/etc/zethropol-systemd-lab').read_text().strip()
    checks['isolation'] = {'host': host, 'guest': current, 'pid1': 'systemd'}
    isolated = True
    # The backing root is a host tmpfs owned by the initial user namespace.
    # Use child-userns-owned disposable mounts for the protected/RW-exception
    # paths, without exposing a single writable host bind mount.
    libc = ctypes.CDLL(None, use_errno=True)
    for target in ['/usr', '/etc', '/boot', '/efi']:
        old = os.open(target, os.O_RDONLY | os.O_DIRECTORY)
        try:
            if libc.mount(b'tmpfs', target.encode(), b'tmpfs', 0, b'mode=0755,size=256M') != 0:
                raise OSError(ctypes.get_errno(), 'private userns tmpfs mount failed', target)
            shutil.copytree('/proc/self/fd/' + str(old), target, dirs_exist_ok=True, symlinks=True)
        finally:
            os.close(old)
    checks['private_userns_owned_mounts'] = ['/usr', '/etc', '/boot', '/efi']
    pathlib.Path('/run/zethropol-update').mkdir(parents=True, exist_ok=True, mode=0o700)
    pathlib.Path('/run/user').mkdir(parents=True, exist_ok=True)
    ctl('start', 'systemd-journald.socket', 'systemd-journald.service')
    ctl('log-target', 'journal')
    ctl('log-level', 'debug')
    for name in ['normal', 'sandbox', 'sigkill', 'cgstop', 'restart', 'locked', 'changed', 'fail-exec']:
        (pathlib.Path('/run/zethropol-update') / (name + '.json')).write_text(json.dumps({
            'status': 'queued', 'executions': 0, 'plan': [{'name': 'fixture', 'version': '2'}]}))
    ctl('start', 'lab-worker@normal.service')
    s = stopped('normal')
    assert s['status'] == 'succeeded' and s['executions'] == 1 and s['stop_post_count'] == 1
    assert s['service_result'] == 'success'
    checks['exec_start_stop_post_success'] = s
    p = ctl('start', 'lab-worker@fail-exec.service', ok=False)
    s = stopped('fail-exec')
    assert p.returncode != 0 and s['status'] == 'failed' and s['executions'] == 0
    assert properties('fail-exec')['ExecMainStatus'] == '203'
    checks['exec_start_missing_executable'] = {'state': s, 'properties': properties('fail-exec')}
    ctl('start', 'lab-worker@sandbox.service')
    s = stopped('sandbox')
    assert s['status'] == 'succeeded'
    probes = s['sandbox']
    for directory in ['/usr', '/etc', '/var/lib/pacman', '/var/cache/pacman', '/boot', '/run/zethropol-update', '/tmp']:
        assert probes[directory] is True, (directory, probes)
    assert isinstance(probes['/root'], dict)
    assert isinstance(probes['/proc/sys/kernel/domainname'], dict)
    assert isinstance(probes['/sys/fs/cgroup/cgroup.subtree_control'], dict)
    assert probes['no_new_privs'] and probes['af_packet_blocked']
    assert not pathlib.Path('/tmp/lab-write-sandbox').exists()
    checks['sandbox_write_and_restriction_probes'] = probes
    lock = pathlib.Path('/var/lib/pacman/db.lck')
    lock.write_text('synthetic fixture lock only')
    ctl('start', 'lab-worker@locked.service', ok=False)
    s = stopped('locked')
    assert s['error'] == 'pacman_lock_collision' and s['executions'] == 0
    assert lock.read_text() == 'synthetic fixture lock only'
    lock.unlink()
    checks['synthetic_pacman_lock_collision'] = s
    ctl('start', 'lab-worker@changed.service', ok=False)
    s = stopped('changed')
    assert s['error'] == 'plan_changed' and s['executions'] == 0
    checks['synthetic_plan_change'] = s
    for name in ['sigkill', 'restart']:
        ctl('start', f'lab-worker@{name}.service')
        wait_file(name + '.ready')
        ctl('kill', '--kill-whom=main', '--signal=SIGKILL', f'lab-worker@{name}.service')
        s = stopped(name)
        assert s['status'] == 'failed' and s['executions'] == 1
        assert s['exit_code'] == 'killed' and s['exit_status'] == 'KILL'
        time.sleep(.3)
        assert properties(name)['NRestarts'] == '0'
        checks[name + '_no_automatic_restart'] = {'state': s, 'properties': properties(name)}
    # Explicit test-only restart of the same failed fixture record must not
    # execute the synthetic package stage again.
    ctl('restart', 'lab-worker@restart.service', ok=False)
    deadline = time.monotonic() + 8
    while state('restart')['stop_post_count'] < 2:
        assert time.monotonic() < deadline
        time.sleep(.05)
    assert state('restart')['executions'] == 1
    checks['explicit_restart_rejected_terminal_record'] = state('restart')
    ctl('start', 'lab-worker@cgstop.service')
    wait_file('cgstop.ready')
    child = int(wait_file('cgstop.child').read_text())
    cg = properties('cgstop')['ControlGroup']
    assert cg in pathlib.Path(f'/proc/{child}/cgroup').read_text()
    started = time.monotonic()
    ctl('stop', 'lab-worker@cgstop.service')
    duration = time.monotonic() - started
    s = stopped('cgstop')
    assert duration >= 29, duration  # Production TimeoutStopSec=30s is retained.
    child_stat = pathlib.Path(f'/proc/{child}/stat')
    assert not child_stat.exists() or child_stat.read_text().split()[2] == 'Z'
    procs = pathlib.Path('/sys/fs/cgroup' + cg + '/cgroup.procs')
    assert not procs.exists() or not procs.read_text().strip()
    assert s['status'] == 'failed'
    checks['cgroup_stop_sigterm_resistant_child'] = {'duration_seconds': duration, 'state': s, 'child_pid': child, 'control_group': cg}
    result = {'passed': True, 'checks': checks}
except BaseException:
    result = {'passed': False, 'checks': checks, 'error': traceback.format_exc()}
finally:
    RESULT.write_text(json.dumps(result, indent=2))
    print('SYSTEMD_LAB_RESULT', json.dumps(result), flush=True)
    if isolated:
        # Private PID1 container only; never address the host manager.
        subprocess.run(['/usr/bin/systemctl', '--force', 'exit', '0' if result['passed'] else '1'], timeout=10)
