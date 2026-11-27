# Zethropol Hardware Package

This package builds and installs the production Zethropol Hardware D-Bus service.

## Runtime

The package installs:

- `/usr/lib/zethropol/zethropol-hardware`
- `/usr/lib/zethropol/zethropol-diagnostics-helper` (`0.1.0-5`+)
- `/etc/dbus-1/system.d/org.zethropol.Hardware.conf`
- `/usr/share/dbus-1/system-services/org.zethropol.Hardware.service`
- `/usr/lib/systemd/system/zethropol-hardware.service`
- `/usr/share/polkit-1/actions/org.zethropol.diagnostics.helper.policy` (`0.1.0-5`+)
- `/usr/share/licenses/zethropol-hardware/LICENSE`

The service uses system-level D-Bus activation through systemd and is not enabled through `multi-user.target`.

## P4B Release (`0.1.0-5` — Isolated Build Ready, Live Stays `0.1.0-4`)

- Packages the hardened `zethropol-diagnostics-helper` binary under `/usr/lib/zethropol/zethropol-diagnostics-helper` together with `/usr/share/polkit-1/actions/org.zethropol.diagnostics.helper.policy` (`auth_admin`, Arch path `/usr/lib/zethropol/` instead of `/usr/libexec/zethropol/`).
- Enforces strict whole block device path validation (`nvmeXnY`, `sdX`, `vdX`, `mmcblkX`), rejects symlinks and non-block targets, clears environment variables, and invokes `/usr/bin/smartctl -a --json -- <device>`. Periodic read-only `GetState`/`GetTelemetry` inventory queries continue to never prompt `pkexec`.
- Verified offline and rootlessly via `makepkg --clean --force` (`hardware` 37/37 + `dbus_disconnect` 1/1 + `diagnostics-helper` 4/4 = 42/42 PASS; `0` `/home/Zevor` path leaks in both ELF binaries; `packages/zethropol-hardware/SHA256SUMS`: 7/7 OK).
  - Source archive SHA-256: `855bcf7585f3bfffe378a8a0ce67be2a7213d16bc01fbf785b8fdbc61b881c8f`
  - Package archive SHA-256: `0c89ecbedcaf848f529407d19aaa609f30dc4819300c727fc3fb140912092c11`
- Not installed live yet (`pacman -Q` stays at `0.1.0-4` until separate explicit live upgrade approval); preserves `0.1.0-3` and `0.1.0-4` rollback/staging archives untouched.

## Stage 04.6 Release (`0.1.0-4`)

- Adds the zero-fork `org.zethropol.Hardware.v1.GetTelemetry` contract alongside backward-compatible `GetState`.
- Uses the locked `libc::statvfs` binding (`libc 0.2.189`) for Linux filesystem usage reporting while preserving `f_frsize`/`f_bsize` and Btrfs occupancy semantics.
- Built offline with `--locked --offline` and path remapping so no developer home or temporary build paths appear in the binary.
- Preserves the previous rollback archive `zethropol-hardware-0.1.0-3-x86_64.pkg.tar.zst` untouched.
- Production package artifact is stored under `packages/stage-04.6/` and deployed live during Stage 04.7 (`zethropol-hardware 0.1.0-4`, `pacman -Qkk`: 18 files, 0 modified; closure record: `docs/STAGE_04_SYSTEM_MONITOR_CLOSURE.md`).
