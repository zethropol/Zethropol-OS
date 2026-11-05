#!/usr/bin/python3
"""Explicitly-approved host test only. Never addresses a production start job."""
import argparse, hashlib, json, os, pathlib, re, shutil, subprocess, tempfile, time, uuid

HERE = pathlib.Path(__file__).resolve().parent
SOURCE_UNIT = 'zethropol-update-worker@00000000-0000-4000-8000-000000000000.service'
PROFILE = [
    'Type', 'User', 'Group', 'Restart', 'KillMode', 'TimeoutStartUSec', 'TimeoutStopUSec',
    'StandardInput', 'StandardOutput', 'StandardError', 'ProtectSystem', 'ReadWritePaths',
    'PrivateTmp', 'ProtectHome', 'NoNewPrivileges', 'ProtectKernelTunables',
    'ProtectControlGroups', 'LockPersonality', 'RestrictAddressFamilies',
    'PrivateDevices', 'PrivateUsers', 'PrivateNetwork', 'PrivateMounts',
    'ProtectKernelModules', 'ProtectKernelLogs', 'ProtectClock', 'ProtectHostname',
    'ProtectProc', 'ProcSubset', 'ReadOnlyPaths', 'InaccessiblePaths',
    'BindPaths', 'BindReadOnlyPaths', 'NoExecPaths', 'ExecPaths',
    'RestrictNamespaces', 'SystemCallFilter', 'CapabilityBoundingSet',
    'AmbientCapabilities', 'MemoryDenyWriteExecute', 'RestrictSUIDSGID', 'DynamicUser',
    'TemporaryFileSystem', 'RootDirectory', 'RootImage', 'MountAPIVFS',
    'ExtensionImages', 'ExtensionDirectories', 'PrivateIPC', 'PrivatePIDs',
]
CASES = [('probe', 'probe'), ('sigkill', 'hold'), ('cgroup', 'children'), ('restart', 'hold')]


def command(args, ok=True, timeout=45):
    p = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    if ok and p.returncode:
        raise RuntimeError(f'{args}: exit={p.returncode} stdout={p.stdout} stderr={p.stderr}')
    return p


def show(unit, properties):
    text = command(['systemctl', 'show', unit, '--property=' + ','.join(properties)]).stdout
    return dict(line.split('=', 1) for line in text.splitlines() if '=' in line)


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def preserved():
    paths = ['/usr/lib/zethropol/zethropol-update', '/etc/systemd/system/zethropol-update.service',
             '/usr/lib/systemd/system/zethropol-update-worker@.service', '/boot/limine.conf']
    paths += [str(p) for p in pathlib.Path('/boot').glob('*/limine_history/snapshots.json')]
    snapshots = {}
    for n in ['295', '296', '297']:
        p = pathlib.Path('/.snapshots') / n
        st = (p / 'snapshot').stat()
        snapshots[n] = {'metadata': digest(p / 'info.xml'), 'dev': st.st_dev, 'inode': st.st_ino}
    return {'hashes': {p: digest(p) for p in paths}, 'snapshots': snapshots,
            'broker': show('zethropol-update.service', ['ActiveState', 'SubState', 'MainPID', 'NRestarts', 'FragmentPath', 'DropInPaths'])}


def prepare():
    p = pathlib.Path(tempfile.mkdtemp(prefix='zethropol-host-worker-plan-'))
    binary = pathlib.Path('/tmp/zethropol-host-worker-probe')
    image = binary.read_bytes()
    assert image.startswith(b'\x7fELF')
    ident = uuid.uuid4().hex[:12]
    manifest = {'id': ident, 'uid': os.getuid(), 'gid': os.getgid(),
                'codex_sandbox_mnt_ns': os.readlink('/proc/self/ns/mnt'),
                'binary': str(binary), 'binary_sha256': hashlib.sha256(image).hexdigest(),
                'worker_unit_sha256': 'dca56a0415c1ffacf5ff09363cce575f11ea58206349229974a804d1e9dadd08',
                'scratch': '/run/zethropol-host-worker-test-' + ident,
                'units': {case: f'zethropol-host-worker-{ident}-{case}.service' for case, _ in CASES}}
    (p / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(p)
    print(json.dumps(manifest, indent=2))


def run(plan):
    assert os.geteuid() == 0
    plan = plan.resolve()
    assert plan.parent == pathlib.Path('/tmp') and plan.name.startswith('zethropol-host-worker-plan-')
    spec = json.loads((plan / 'manifest.json').read_text())
    assert re.fullmatch('[0-9a-f]{12}', spec['id'])
    scratch = pathlib.Path('/run/zethropol-host-worker-test-' + spec['id'])
    assert spec['scratch'] == str(scratch)
    expected_units = {case: f'zethropol-host-worker-{spec["id"]}-{case}.service' for case, _ in CASES}
    assert spec['units'] == expected_units
    image = pathlib.Path(spec['binary']).read_bytes()
    assert hashlib.sha256(image).hexdigest() == spec['binary_sha256']
    assert digest('/usr/lib/systemd/system/zethropol-update-worker@.service') == spec['worker_unit_sha256']
    assert pathlib.Path('/run/zethropol-update').is_dir(), 'Do not create/alter production runtime'
    baseline = preserved()
    assert baseline['broker']['ActiveState'] == 'active'
    assert command(['pacman', '-Q', 'zethropol-update']).stdout.strip() == 'zethropol-update 0.1.0-9'
    assert not command(['systemctl', 'list-units', '--state=active,activating,deactivating', '--plain', '--no-legend', 'zethropol-update-worker@*']).stdout.strip()
    profile = show(SOURCE_UNIT, PROFILE + ['DropInPaths', 'FragmentPath'])
    assert profile['DropInPaths'] == '' and profile['FragmentPath'] == '/usr/lib/systemd/system/zethropol-update-worker@.service'
    host_pid1_mnt_ns = os.readlink('/proc/1/ns/mnt')
    root_runner_mnt_ns = os.readlink('/proc/self/ns/mnt')
    assert root_runner_mnt_ns == host_pid1_mnt_ns, 'Privileged runner is not in the real host PID 1 mount namespace'
    assert spec['codex_sandbox_mnt_ns'] != host_pid1_mnt_ns, 'Codex sandbox unexpectedly shares the real host mount namespace'
    import dbus
    bus = dbus.SystemBus(private=True)
    manager = dbus.Interface(bus.get_object('org.freedesktop.systemd1', '/org/freedesktop/systemd1'), 'org.freedesktop.systemd1.Manager')
    created, referenced = [], []
    report = {'passed': False, 'spec': spec, 'baseline': baseline, 'production_profile': profile,
              'namespace_comparison': {'host_pid1': host_pid1_mnt_ns,
                                       'privileged_runner': root_runner_mnt_ns,
                                       'codex_sandbox': spec['codex_sandbox_mnt_ns']}, 'cases': {}}
    start_time = str(int(time.time()))
    scratch.mkdir(mode=0o700, exist_ok=False)
    results = scratch / 'results'; results.mkdir(mode=0o700)
    helper = scratch / 'probe'; helper.write_bytes(image); helper.chmod(0o755)
    settings = [
        'Type=exec', 'User=root', 'Group=root', 'Restart=no', 'KillMode=control-group',
        'TimeoutStartSec=infinity', 'TimeoutStopSec=30s', 'StandardInput=null',
        'NoNewPrivileges=yes', 'PrivateTmp=yes', 'ProtectSystem=full',
        'ReadWritePaths=' + profile['ReadWritePaths'], 'ProtectHome=yes',
        'ProtectKernelTunables=yes', 'ProtectControlGroups=yes', 'LockPersonality=yes',
        'RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK',
        'After=network-online.target', 'RuntimeMaxSec=90s',
    ]

    def wait_file(name):
        p = results / name; deadline = time.monotonic() + 8
        while not p.exists():
            assert time.monotonic() < deadline, f'missing fixture result {name}'
            time.sleep(.04)
        return p

    def state(unit):
        return show(unit, ['ActiveState', 'SubState', 'Result', 'MainPID', 'NRestarts', 'ExecMainStatus', 'ControlGroup', 'InvocationID'])

    def effective_rw_paths(unit):
        unit_path = manager.GetUnit(unit)
        obj = bus.get_object('org.freedesktop.systemd1', unit_path)
        props = dbus.Interface(obj, 'org.freedesktop.DBus.Properties')
        value = props.Get('org.freedesktop.systemd1.Service', 'ReadWritePaths')
        signature = 'a' + str(value.signature) if isinstance(value, dbus.Array) else str(value.signature)
        entries = [str(entry) for entry in value] if isinstance(value, dbus.Array) else str(value).split()
        return {'signature': signature, 'entries': entries}

    def mountinfo_for(pid):
        raw = pathlib.Path(f'/proc/{pid}/mountinfo').read_text()
        selected = []
        for line in raw.splitlines():
            fields = line.split()
            if len(fields) > 6 and fields[4] in ('/', '/usr', '/boot', '/efi'):
                selected.append(line)
        return raw, selected

    def mount_options_for(raw, path):
        candidates = []
        for line in raw.splitlines():
            fields = line.split()
            if len(fields) > 6 and (path == fields[4] or path.startswith(fields[4].rstrip('/') + '/')):
                candidates.append((len(fields[4]), fields[4], fields[5]))
        return max(candidates) if candidates else None

    def settle(unit):
        deadline = time.monotonic() + 8
        while True:
            value = state(unit)
            if value['ActiveState'] in ('inactive', 'failed'):
                return value
            assert time.monotonic() < deadline, value
            time.sleep(.04)

    def start(case, mode):
        unit = expected_units[case]
        created.append(unit)
        args = ['systemd-run', '--quiet', '--unit=' + unit, '--description=Harmless host worker sandbox test']
        args += ['--property=' + value for value in settings]
        args += ['--property=ExecStopPost=' + str(helper) + ' post ' + str(results) + ' ' + case,
                 '--', str(helper), mode, str(results), case]
        launched = command(args, ok=False)
        if launched.returncode:
            report['cases'][case] = {'start_exit': launched.returncode, 'stderr': launched.stderr,
                                     'properties': state(unit)}
            raise RuntimeError('Unexpected host startup/namespace/permission failure; stop all tests')
        manager.RefUnit(unit); referenced.append(unit)
        effective = show(unit, PROFILE)
        mismatch = {k: {'production': profile.get(k), 'test': effective.get(k)} for k in PROFILE if profile.get(k) != effective.get(k)}
        assert not mismatch, f'Profile mismatch; stop tests: {mismatch}'
        ready = wait_file('ready-' + case + '.txt').read_text().splitlines()
        pid = int(ready[0])
        assert pid == int(state(unit)['MainPID'])
        trace = (results / ('start-' + case + '-' + ready[1] + '.txt')).read_text()
        unit_ns = os.readlink(f'/proc/{pid}/ns/mnt')
        raw_mountinfo, selected_mountinfo = mountinfo_for(pid)
        actual_rw_paths = effective_rw_paths(unit)
        expected_rw_paths = profile['ReadWritePaths'].split()
        actual_mounts = {path: mount_options_for(raw_mountinfo, path) for path in ('/usr', '/boot')}
        report['cases'][case] = {
            'profile_matches': True, 'trace': trace, 'invocation': ready[1], 'pid': pid,
            'mount_namespace': unit_ns,
            'mount_namespace_isolated_from_pid1': unit_ns != host_pid1_mnt_ns,
            'mount_namespace_isolated_from_codex_sandbox': unit_ns != spec['codex_sandbox_mnt_ns'],
            'dbus_ReadWritePaths': actual_rw_paths,
            'expected_ReadWritePaths': expected_rw_paths,
            'mountinfo_paths': selected_mountinfo,
            'mountinfo_path_flags': actual_mounts,
        }
        assert 'NoNewPrivs=1' in trace and 'PACKAGE_ACCESS_DENIED=1' in trace and 'EXEC_DENIED=1' in trace
        assert unit_ns != host_pid1_mnt_ns, 'Test unit did not receive its own mount namespace'
        assert unit_ns != spec['codex_sandbox_mnt_ns'], 'Test process unexpectedly shares the Codex sandbox mount namespace'
        assert actual_rw_paths['signature'] == 'as', f"ReadWritePaths D-Bus signature was {actual_rw_paths['signature']!r}"
        assert actual_rw_paths['entries'] == expected_rw_paths, f"ReadWritePaths D-Bus entries differ: {actual_rw_paths['entries']!r}"
        assert actual_mounts['/usr'] is not None and 'ro' not in actual_mounts['/usr'][2].split(','), f"/usr mount is not writable: {actual_mounts['/usr']!r}"
        assert actual_mounts['/boot'] is not None and 'ro' not in actual_mounts['/boot'][2].split(','), f"/boot mount is not writable: {actual_mounts['/boot']!r}"
        assert 'VFS /usr rw' in trace, 'Expected /usr writable in test namespace; observed mount flags differ'
        assert 'VFS /boot rw' in trace, 'Expected /boot writable in test namespace; observed mount flags differ'
        if pathlib.Path('/efi').exists():
            assert 'VFS /efi rw' in trace, 'Expected /efi writable in test namespace; observed mount flags differ'
        else:
            assert 'VFS /efi absent' in trace, 'Unexpected /efi mount state'
        return unit, ready

    try:
        unit, ready = start('probe', 'probe')
        (results / 'finish-probe').write_text('finish harmless probe')
        finished = settle(unit)
        post = wait_file('post-probe-' + ready[1] + '.txt').read_text()
        assert finished['Result'] == 'success' and 'SERVICE_RESULT=success' in post
        report['cases']['probe'].update(properties=finished, post=post)
        unit, ready = start('sigkill', 'hold')
        command(['systemctl', 'kill', '--kill-whom=main', '--signal=SIGKILL', unit])
        finished = settle(unit)
        post = wait_file('post-sigkill-' + ready[1] + '.txt').read_text()
        assert finished['NRestarts'] == '0' and finished['Result'] == 'signal'
        assert 'EXIT_CODE=killed' in post and 'EXIT_STATUS=KILL' in post
        time.sleep(.3); assert state(unit)['NRestarts'] == '0'
        report['cases']['sigkill'].update(properties=finished, post=post)
        unit, ready = start('cgroup', 'children')
        child = int(wait_file('child-cgroup.txt').read_text())
        cg = state(unit)['ControlGroup']
        assert unit in cg and cg in pathlib.Path(f'/proc/{child}/cgroup').read_text()
        stopped_at = time.monotonic()
        command(['systemctl', 'stop', unit], timeout=42)
        elapsed = time.monotonic() - stopped_at
        finished = settle(unit)
        child_status = pathlib.Path(f'/proc/{child}/stat')
        assert not child_status.exists() or child_status.read_text().split()[2] == 'Z'
        procs = pathlib.Path('/sys/fs/cgroup' + cg + '/cgroup.procs')
        assert not procs.exists() or not procs.read_text().strip()
        assert elapsed >= 29 and elapsed < 42 and finished['Result'] == 'timeout'
        post = wait_file('post-cgroup-' + ready[1] + '.txt').read_text()
        report['cases']['cgroup'].update(properties=finished, post=post, stop_seconds=elapsed, child_not_running=True, cgroup_empty=True)
        unit, ready = start('restart', 'hold')
        command(['systemctl', 'restart', unit])
        deadline = time.monotonic() + 8
        while True:
            newer = wait_file('ready-restart.txt').read_text().splitlines()
            if newer[1] != ready[1]: break
            assert time.monotonic() < deadline
            time.sleep(.04)
        assert int(newer[0]) == int(state(unit)['MainPID'])
        assert len(list(results.glob('start-restart-*.txt'))) == 2
        command(['systemctl', 'stop', unit])
        finished = settle(unit)
        assert len(list(results.glob('post-restart-*.txt'))) == 2 and finished['NRestarts'] == '0'
        report['cases']['restart'].update(properties=finished, explicit_restart_new_invocation=True, starts=2, post_count=2)
        report['passed'] = True
    except BaseException as exc:
        report['error'] = repr(exc)
    finally:
        cleanup_errors = []
        for unit in reversed(created):
            try:
                command(['systemctl', 'stop', unit], ok=False, timeout=42)
                command(['systemctl', 'reset-failed', unit], ok=False)
                if unit in referenced: manager.UnrefUnit(unit)
            except BaseException as exc:
                cleanup_errors.append({'unit': unit, 'error': repr(exc)})
        bus.close()
        for unit in created:
            deadline = time.monotonic() + 6
            p = pathlib.Path('/run/systemd/transient') / unit
            while p.exists() and time.monotonic() < deadline: time.sleep(.05)
            if p.exists(): cleanup_errors.append({'unit': unit, 'error': 'transient declaration remains'})
        report['cleanup_errors'] = cleanup_errors
        report['after'] = preserved()
        report['production_preserved'] = report['after'] == baseline
        report['journal'] = command(['journalctl', '--no-pager', '--since=@' + start_time, *sum((['-u', u] for u in created), [])], ok=False).stdout
        destination = plan / 'results'
        shutil.copytree(results, destination, dirs_exist_ok=False)
        (plan / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        (plan / 'journal.txt').write_text(report['journal'])
        for directory, _, files in os.walk(plan):
            os.chown(directory, spec['uid'], spec['gid'])
            for name in files: os.chown(pathlib.Path(directory) / name, spec['uid'], spec['gid'])
        if not cleanup_errors:
            shutil.rmtree(scratch)  # Only this newly-created approved scratch tree.
        report['passed'] = report['passed'] and not cleanup_errors and report['production_preserved']
        (plan / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print('HOST_TEST_RESULT', json.dumps(report), flush=True)
    return 0 if report['passed'] else 1

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--run', type=pathlib.Path)
    args = parser.parse_args()
    if args.run:
        raise SystemExit(run(args.run))
    prepare()
