#!/usr/bin/python3
"""Synthetic worker: only runs in the lab root, never invokes package tools."""
import fcntl, json, os, pathlib, signal, socket, subprocess, sys, time

assert pathlib.Path('/etc/zethropol-systemd-lab').is_file()
assert not pathlib.Path('/usr/bin/pacman').exists()
assert not pathlib.Path('/usr/lib/zethropol/zethropol-update').exists()
BASE = pathlib.Path('/run/zethropol-update')
RESULTS = pathlib.Path('/results')

def read(name):
    return json.loads((BASE / (name + '.json')).read_text())

def write(name, value):
    p = BASE / (name + '.json')
    temporary = p.with_suffix('.new')
    temporary.write_text(json.dumps(value))
    temporary.replace(p)

mode, name = sys.argv[1:3]
assert name in {'normal', 'sandbox', 'sigkill', 'cgstop', 'restart', 'locked', 'changed', 'fail-exec'}
if mode == 'child':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    (RESULTS / (name + '.child')).write_text(str(os.getpid()))
    while True:
        time.sleep(.05)
if mode == 'stopped':
    state = read(name)
    if state['status'] in ('queued', 'running'):
        state['status'] = 'failed'
    state['stop_post_count'] = state.get('stop_post_count', 0) + 1
    state['service_result'] = os.getenv('SERVICE_RESULT')
    state['exit_code'] = os.getenv('EXIT_CODE')
    state['exit_status'] = os.getenv('EXIT_STATUS')
    write(name, state)
    (RESULTS / (name + '.stopped')).write_text(json.dumps(state))
    raise SystemExit(0)
assert mode == 'start'
lease = open(BASE / (name + '.lease'), 'a')
fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
state = read(name)
if state['status'] != 'queued':
    raise SystemExit(70)
lock_path = pathlib.Path('/var/lib/pacman/db.lck')
try:
    lock = os.open(lock_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
except FileExistsError:
    state.update(status='failed', error='pacman_lock_collision')
    write(name, state)
    raise SystemExit(72)
try:
    current = [{'name': 'fixture', 'version': '3' if name == 'changed' else '2'}]
    if current != state['plan']:
        state.update(status='failed', error='plan_changed')
        write(name, state)
        raise SystemExit(73)
    state.update(status='running', executions=state['executions'] + 1, pid=os.getpid())
    write(name, state)
finally:
    os.close(lock)
    lock_path.unlink()
if name in ('sigkill', 'cgstop', 'restart'):
    if name == 'cgstop':
        subprocess.Popen(['/usr/bin/python3', '/fixture/worker.py', 'child', name])
    (RESULTS / (name + '.ready')).write_text('running')
    while True:
        time.sleep(.05)
if name == 'sandbox':
    probes = {}
    for directory in ['/usr', '/etc', '/var/lib/pacman', '/var/cache/pacman', '/boot', '/run/zethropol-update', '/tmp', '/root']:
        p = pathlib.Path(directory) / ('lab-write-' + name)
        try:
            with p.open('x') as f:
                f.write('synthetic lab only')
            probes[directory] = True
        except OSError as e:
            probes[directory] = {'errno': e.errno}
    for p in ['/proc/sys/kernel/domainname', '/sys/fs/cgroup/cgroup.subtree_control']:
        try:
            fd = os.open(p, os.O_WRONLY)  # Do not write any bytes.
            os.close(fd)
            probes[p] = True
        except OSError as e:
            probes[p] = {'errno': e.errno}
    probes['no_new_privs'] = 'NoNewPrivs:\t1' in pathlib.Path('/proc/self/status').read_text()
    try:
        s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
        s.close()
        probes['af_packet_blocked'] = False
    except OSError:
        probes['af_packet_blocked'] = True
    state['sandbox'] = probes
state['status'] = 'succeeded'
write(name, state)
