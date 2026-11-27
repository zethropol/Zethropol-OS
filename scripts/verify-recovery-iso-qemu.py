#!/usr/bin/env python3
"""Isolated rootless KVM/QEMU UEFI OVMF boot & smoke verifier for Zethropol Recovery ArchISO.

Boots the built zethropol-recovery-*.iso read-only under UEFI OVMF with -net none
and verifies over serial console (ttyS0):
  1. Real UEFI boot (/sys/firmware/efi) & hostname (zethropol-recovery)
  2. Installed zethropol-recovery 0.1.0-3 + limine + limine-snapper-sync + snapper + btrfs-progs
  3. On-demand D-Bus activation of org.zethropol.Recovery.v1.GetState & active service
  4. systemd-analyze security checks (CAP_SYS_MODULE & @module)
  5. Safety lock preserved (PreflightRollback reports rollbackExecutionEnabled: false)
  6. /usr/local/bin/zethropol-recovery CLI helper exits 0
"""
import argparse
import glob
import json
import os
from pathlib import Path
import pty
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time

OVMF_CODE = Path('/usr/share/edk2/x64/OVMF_CODE.4m.fd')
OVMF_VARS_SRC = Path('/usr/share/edk2/x64/OVMF_VARS.4m.fd')
ANSI_RE = re.compile(
    r'\x1b\[[0-9;?]*[A-Za-z]'
    r'|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)'
    r'|\x1b\([A-Za-z0-9]'
    r'|\x1b[=>]'
    r'|\r'
)


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub('', text)


def find_latest_iso() -> Path:
    candidates = sorted(
        Path('/tmp/zethropol-recovery-iso/out').glob('zethropol-recovery-*.iso'),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    if not candidates:
        raise FileNotFoundError('No zethropol-recovery-*.iso found under /tmp/zethropol-recovery-iso/out/')
    return candidates[0]


def read_until(master_fd: int, pattern: str, timeout: float, log_chunks: list) -> str:
    deadline = time.monotonic() + timeout
    raw_buf = ''
    while time.monotonic() < deadline:
        remaining = max(0.05, min(0.5, deadline - time.monotonic()))
        r, _, _ = select.select([master_fd], [], [], remaining)
        if not r:
            continue
        try:
            data = os.read(master_fd, 4096)
        except OSError:
            break
        if not data:
            break
        chunk = data.decode('utf-8', errors='replace')
        log_chunks.append(chunk)
        raw_buf += chunk
        clean = strip_ansi(raw_buf)
        if re.search(pattern, clean):
            return clean
    raise TimeoutError(f'Timed out after {timeout}s waiting for pattern {pattern!r}. Tail:\n{strip_ansi(raw_buf)[-1500:]}')


def run_serial_cmd(master_fd: int, cmd: str, tag: str, timeout: float, log_chunks: list) -> str:
    start_tok = f'__ZETH_BEGIN_{tag}__'
    end_tok = f'__ZETH_END_{tag}__'
    payload = f"printf '\\n{start_tok}\\n'; {cmd}; printf '\\n{end_tok}:%d\\n' $?\n"
    os.write(master_fd, payload.encode('utf-8'))
    out = read_until(master_fd, rf'{re.escape(end_tok)}:\d+', timeout, log_chunks)
    m = re.search(rf'{re.escape(start_tok)}\n(.*?)\n{re.escape(end_tok)}:(\d+)', out, re.S)
    if not m:
        raise RuntimeError(f'Failed to parse command block {tag}. Output tail:\n{out[-1000:]}')
    return m.group(1).strip(), int(m.group(2))


def main() -> int:
    parser = argparse.ArgumentParser(description='Verify Zethropol Recovery ISO in isolated KVM/QEMU UEFI OVMF.')
    parser.add_argument('--iso', type=Path, default=None, help='Path to zethropol-recovery-*.iso')
    parser.add_argument('--out-json', type=Path, default=None, help='Optional JSON output path')
    parser.add_argument('--boot-timeout', type=float, default=180.0, help='Seconds to wait for live root shell')
    args = parser.parse_args()

    iso_path = args.iso if args.iso else find_latest_iso()
    if not iso_path.is_file():
        print(f'ERROR: ISO not found: {iso_path}', file=sys.stderr)
        return 2
    if not OVMF_CODE.is_file() or not OVMF_VARS_SRC.is_file():
        print('ERROR: OVMF firmware files missing under /usr/share/edk2/x64/', file=sys.stderr)
        return 2

    tmp_dir = Path(tempfile.mkdtemp(prefix='zethropol-iso-qemu.'))
    ovmf_vars = tmp_dir / 'OVMF_VARS.4m.fd'
    shutil.copy2(OVMF_VARS_SRC, ovmf_vars)
    log_chunks: list[str] = []
    proc = None
    master_fd = -1

    try:
        master_fd, slave_fd = pty.openpty()
        cmd = [
            '/usr/bin/qemu-system-x86_64',
            '-enable-kvm',
            '-machine', 'q35,accel=kvm',
            '-cpu', 'host',
            '-smp', '4',
            '-m', '3072',
            '-nographic',
            '-no-reboot',
            '-net', 'none',
            '-drive', f'if=pflash,format=raw,readonly=on,file={OVMF_CODE}',
            '-drive', f'if=pflash,format=raw,file={ovmf_vars}',
            '-drive', f'file={iso_path},media=cdrom,readonly=on,format=raw',
        ]
        print(f'==> Launching isolated QEMU/OVMF for {iso_path}', flush=True)
        proc = subprocess.Popen(cmd, stdin=slave_fd, stdout=slave_fd, stderr=slave_fd, close_fds=True)
        os.close(slave_fd)

        # Wait for autologin zsh prompt on ttyS0
        read_until(master_fd, r'root@zethropol-recovery[^\n]*[#%]\s*', args.boot_timeout, log_chunks)
        print('==> Live UEFI ISO shell reached on ttyS0!', flush=True)

        # Disable TTY echo & ZLE so command payloads are not matched as output delimiters
        os.write(master_fd, b"stty -echo; unsetopt zle 2>/dev/null || true; printf '\\n__ZETH_READY__\\n'\n")
        read_until(master_fd, r'\n__ZETH_READY__\n', 20, log_chunks)

        efi_out, efi_rc = run_serial_cmd(master_fd, 'test -d /sys/firmware/efi && echo EFI_OK', 'EFI', 20, log_chunks)
        host_out, host_rc = run_serial_cmd(master_fd, 'cat /etc/hostname', 'HOST', 20, log_chunks)
        pkg_out, pkg_rc = run_serial_cmd(
            master_fd,
            'pacman -Q zethropol-recovery limine limine-snapper-sync snapper btrfs-progs',
            'PKGS', 30, log_chunks,
        )
        dbus_out, dbus_rc = run_serial_cmd(
            master_fd,
            'busctl --json=short call org.zethropol.Recovery /org/zethropol/Recovery org.zethropol.Recovery.v1 GetState',
            'DBUS', 30, log_chunks,
        )
        svc_out, svc_rc = run_serial_cmd(
            master_fd,
            'systemctl is-active zethropol-recovery.service',
            'SVC', 20, log_chunks,
        )
        sec_out, sec_rc = run_serial_cmd(
            master_fd,
            "systemd-analyze security zethropol-recovery.service | grep -E 'CAP_SYS_MODULE|@module|Overall'",
            'SEC', 30, log_chunks,
        )
        pf_out, pf_rc = run_serial_cmd(
            master_fd,
            'busctl --json=short call org.zethropol.Recovery /org/zethropol/Recovery org.zethropol.Recovery.v1 PreflightRollback u 1',
            'PF', 30, log_chunks,
        )
        cli_out, cli_rc = run_serial_cmd(
            master_fd,
            '/usr/local/bin/zethropol-recovery',
            'CLI', 30, log_chunks,
        )

        # Clean poweroff
        os.write(master_fd, b'systemctl poweroff --no-block\n')
        try:
            proc.wait(timeout=45)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=10)

        results = {
            'iso_path': str(iso_path),
            'iso_size_bytes': iso_path.stat().st_size,
            'uefi_booted': efi_rc == 0 and 'EFI_OK' in efi_out,
            'hostname': host_out,
            'hostname_ok': host_rc == 0 and host_out.strip() == 'zethropol-recovery',
            'packages_rc': pkg_rc,
            'packages': [ln.strip() for ln in pkg_out.splitlines() if ln.strip()],
            'has_recovery_0_1_0_3': 'zethropol-recovery 0.1.0-3' in pkg_out,
            'dbus_get_state_rc': dbus_rc,
            'dbus_get_state_ok': dbus_rc == 0 and '"detectionStatus"' in dbus_out,
            'service_active': svc_rc == 0 and svc_out.strip() == 'active',
            'security_hardening': [ln.strip() for ln in sec_out.splitlines() if ln.strip()],
            'security_cap_and_module_ok': 'CAP_SYS_MODULE' in sec_out and '@module' in sec_out,
            'preflight_rc': pf_rc,
            'rollback_lock_preserved': '"rollbackExecutionEnabled":{"type":"b","data":false}' in pf_out,
            'cli_helper_rc': cli_rc,
            'cli_helper_ok': cli_rc == 0 and 'zethropol-recovery 0.1.0-3' in cli_out and 'GetState: OK' in cli_out,
            'cli_helper_output': cli_out,
        }
        results['all_passed'] = all([
            results['uefi_booted'],
            results['hostname_ok'],
            results['packages_rc'] == 0,
            results['has_recovery_0_1_0_3'],
            results['dbus_get_state_ok'],
            results['service_active'],
            results['security_cap_and_module_ok'],
            results['rollback_lock_preserved'],
            results['cli_helper_ok'],
        ])

        rendered = json.dumps(results, ensure_ascii=False, indent=2) + '\n'
        print(rendered)
        if args.out_json:
            args.out_json.write_text(rendered)
        return 0 if results['all_passed'] else 1
    except Exception as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        log_file = tmp_dir / 'serial.log'
        log_file.write_text(''.join(log_chunks))
        print(f'Serial tail:\n{strip_ansi("".join(log_chunks))[-2000:]}', file=sys.stderr)
        return 1
    finally:
        if proc and proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        if master_fd >= 0:
            try:
                os.close(master_fd)
            except OSError:
                pass
        shutil.rmtree(tmp_dir, ignore_errors=True)


if __name__ == '__main__':
    raise SystemExit(main())
