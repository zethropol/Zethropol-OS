#!/usr/bin/python3
"""Prepare a private OS tree; privileged mode starts only its unique lab unit.
No host bind mounts and no package/snapshot executables enter the container.
"""
import argparse, hashlib, json, os, pathlib, re, shutil, subprocess, sys, tempfile, uuid

HERE = pathlib.Path(__file__).resolve().parent

def copy_with_libraries(source, root):
    source = pathlib.Path(source)
    destination = root / str(source).lstrip('/')
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination, follow_symlinks=True)
    process = subprocess.run(['/usr/bin/ldd', str(source)], capture_output=True, text=True)
    for library in re.findall(r'(/[^\s()]+)', process.stdout):
        p = pathlib.Path(library)
        if p.is_file():
            target = root / library.lstrip('/')
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                shutil.copy2(p, target, follow_symlinks=True)

def prepare():
    lab = pathlib.Path(tempfile.mkdtemp(prefix='zethropol-systemd-lab-'))
    root = lab / 'root'
    for name in ['usr/bin', 'usr/lib/systemd', 'etc/systemd/system', 'proc', 'sys', 'dev', 'run/zethropol-update', 'var/lib/pacman', 'var/cache/pacman', 'boot', 'efi', 'tmp', 'var/tmp', 'root', 'home', 'fixture', 'results']:
        (root / name).mkdir(parents=True, exist_ok=True)
    for name, target in [('bin', 'usr/bin'), ('sbin', 'usr/bin'), ('lib', 'usr/lib'), ('lib64', 'usr/lib')]:
        (root / name).symlink_to(target)
    for name in ['/usr/lib/systemd/systemd', '/usr/lib/systemd/systemd-executor', '/usr/lib/systemd/systemd-shutdown', '/usr/lib/systemd/systemd-journald', '/usr/bin/journalctl', '/usr/lib/libseccomp.so.2', '/usr/bin/systemctl', '/usr/bin/python3']:
        copy_with_libraries(name, root)
    (root / 'usr/bin/init').symlink_to('../lib/systemd/systemd')
    version = f'python{sys.version_info.major}.{sys.version_info.minor}'
    shutil.copytree('/usr/lib/' + version, root / 'usr/lib' / version,
        ignore=shutil.ignore_patterns('site-packages', '__pycache__', 'test', 'tests', 'idlelib', 'turtledemo'))
    for module in (root / 'usr/lib' / version / 'lib-dynload').glob('*.so'):
        copy_with_libraries('/usr/lib/' + version + '/lib-dynload/' + module.name, root)
    uid = uuid.uuid4().hex
    (root / 'etc/machine-id').write_text(uid + '\n')
    (root / 'etc/zethropol-systemd-lab').write_text(uid + '\n')
    (root / 'etc/os-release').write_text('ID=zethropol-worker-lab\nNAME="Isolated Worker Fixture"\n')
    (root / 'etc/passwd').write_text('root:x:0:0:root:/root:/usr/bin/python3\n')
    (root / 'etc/group').write_text('root:x:0:\nsystemd-journal:x:190:\n')
    (root / 'etc/nsswitch.conf').write_text('passwd: files\ngroup: files\nhosts: files\n')
    (root / 'tmp').chmod(0o1777)
    (root / 'var/tmp').chmod(0o1777)
    for name in ['worker.py', 'suite.py']:
        shutil.copy2(HERE / name, root / 'fixture' / name)
    ns = {n: os.readlink('/proc/self/ns/' + n) for n in ['user', 'pid', 'mnt', 'net', 'ipc', 'uts', 'cgroup']}
    (root / 'fixture/host-namespaces.json').write_text(json.dumps(ns))
    units = root / 'etc/systemd/system'
    for name in ['sysinit.target', 'basic.target', 'shutdown.target', 'umount.target', 'final.target']:
        (units / name).write_text('[Unit]\nDescription=Lab-only target\nDefaultDependencies=no\n')
    (units / 'system.slice').write_text('[Unit]\nDefaultDependencies=no\n[Slice]\n')
    (units / 'default.target').write_text('[Unit]\nDescription=Isolated fixture test\nDefaultDependencies=no\nWants=lab-driver.service\n')
    shutil.copy2('/usr/lib/systemd/system/systemd-journald.socket', units / 'systemd-journald.socket')
    (units / 'systemd-journald.service').write_text('[Unit]\nDefaultDependencies=no\n[Service]\nType=notify\nNotifyAccess=all\nExecStart=/usr/lib/systemd/systemd-journald\nSockets=systemd-journald.socket\nStandardOutput=null\nStandardError=append:/results/journald-console.log\n')
    (root / 'etc/systemd/journald.conf').write_text('[Journal]\nStorage=volatile\nRuntimeMaxUse=8M\n')
    (units / 'lab-driver.service').write_text('''[Unit]
DefaultDependencies=no
[Service]
Type=oneshot
ExecStart=/usr/bin/python3 /fixture/suite.py
Environment=PYTHONUNBUFFERED=1
TimeoutStartSec=110
StandardOutput=console
StandardError=console
''')
    production = HERE.parents[1] / 'deployment/systemd/zethropol-update-worker@.service'
    template = production.read_text().replace('ExecStart=/usr/lib/zethropol/zethropol-update --worker %i', 'ExecStart=/usr/bin/python3 /fixture/worker.py start %i').replace('ExecStopPost=/usr/lib/zethropol/zethropol-update --worker-stopped %i', 'ExecStopPost=/usr/bin/python3 /fixture/worker.py stopped %i')
    template += '\nStandardOutput=append:/results/worker-console.log\nStandardError=append:/results/worker-console.log\n'
    (units / 'lab-worker@.service').write_text(template)
    dropin = units / 'lab-worker@fail-exec.service.d'
    dropin.mkdir()
    (dropin / '10-fixture.conf').write_text('[Service]\nExecStart=\nExecStart=/fixture/does-not-exist\n')
    (lab / 'manifest.json').write_text(json.dumps({'uid': os.getuid(), 'gid': os.getgid(), 'id': uid,
        'production_unit_sha256': hashlib.sha256(production.read_bytes()).hexdigest()}, indent=2))
    print(lab, flush=True)

def run(lab):
    assert os.geteuid() == 0, 'privileged isolated-container setup required'
    lab = lab.resolve()
    assert lab.parent == pathlib.Path('/tmp') and lab.name.startswith('zethropol-systemd-lab-')
    manifest = json.loads((lab / 'manifest.json').read_text())
    root = lab / 'root'
    assert root.is_dir() and not root.is_symlink()
    assert uuid.UUID(hex=manifest['id']).hex == manifest['id']
    for entry in root.rglob('*'):
        if entry.is_symlink():
            assert entry.resolve().is_relative_to(root), f'Host-escaping lab symlink: {entry}'
    assert (root / 'etc/zethropol-systemd-lab').read_text().strip() == manifest['id']
    for name in ['usr/bin/pacman', 'usr/bin/snapper', 'usr/lib/zethropol/zethropol-update']:
        assert not (root / name).exists(), name
    namespaces = {n: os.readlink('/proc/self/ns/' + n) for n in ['user', 'pid', 'mnt', 'net', 'ipc', 'uts', 'cgroup']}
    (root / 'fixture/host-namespaces.json').write_text(json.dumps(namespaces))
    watched = ['/usr/lib/zethropol/zethropol-update', '/etc/systemd/system/zethropol-update.service', '/usr/lib/systemd/system/zethropol-update-worker@.service', '/boot/limine.conf']
    before = {p: hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest() for p in watched}
    pid = subprocess.check_output(['systemctl', 'show', 'zethropol-update.service', '-p', 'MainPID', '--value'], text=True).strip()
    # Private tree only. Never follow a symlink into the host when changing ownership.
    for directory, names, files in os.walk(root, followlinks=False):
        os.chown(directory, 0, 0, follow_symlinks=False)
        for name in names + files:
            os.chown(pathlib.Path(directory) / name, 0, 0, follow_symlinks=False)
    unit = 'zethropol-worker-lab-' + manifest['id'][:12]
    command = ['/usr/bin/systemd-run', '--unit=' + unit, '--description=Isolated synthetic worker validation',
        '--property=Delegate=yes', '--property=RuntimeMaxSec=150', '--property=TimeoutStopSec=15',
        '--collect', '--wait', '--pipe', '/usr/bin/systemd-nspawn', '--directory=' + str(root),
        '--machine=' + unit, '--register=no', '--keep-unit', '--settings=no', '--private-network',
        '--private-users=pick', '--private-users-ownership=chown', '--link-journal=no',
        '--resolv-conf=off', '--timezone=off', '--console=pipe',
        '--setenv=SYSTEMD_UNIT_PATH=/etc/systemd/system', '--setenv=PYTHONUNBUFFERED=1', '--boot']
    print('ISOLATED_LAB_COMMAND', json.dumps(command), flush=True)
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=185)
    (lab / 'console.log').write_text(result.stdout)
    os.chown(lab / 'console.log', manifest['uid'], manifest['gid'])
    print(result.stdout, flush=True)
    checks = {'command_exit': result.returncode, 'host_files_unchanged': all(hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest() == v for p, v in before.items()),
        'broker_pid_unchanged': subprocess.check_output(['systemctl', 'show', 'zethropol-update.service', '-p', 'MainPID', '--value'], text=True).strip() == pid}
    guest = root / 'results/result.json'
    if guest.exists():
        checks['guest_result'] = json.loads(guest.read_text())
        shutil.copy2(guest, lab / 'guest-result.json')
        os.chown(lab / 'guest-result.json', manifest['uid'], manifest['gid'])
    else:
        checks['guest_result'] = None
    guest_log = root / 'results/worker-console.log'
    if guest_log.exists():
        checks['worker_console'] = guest_log.read_text()[-18000:]
    (lab / 'host-result.json').write_text(json.dumps(checks, indent=2))
    os.chown(lab / 'host-result.json', manifest['uid'], manifest['gid'])
    print('LAB_HOST_RESULT', json.dumps(checks), flush=True)
    assert checks['host_files_unchanged'] and checks['broker_pid_unchanged'], 'host preservation check failed'
    return 0 if result.returncode == 0 and checks['guest_result'] and checks['guest_result'].get('passed') else 1

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--run', type=pathlib.Path)
    args = parser.parse_args()
    if args.run:
        raise SystemExit(run(args.run))
    prepare()
