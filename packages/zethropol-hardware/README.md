# Zethropol Hardware Package

This package builds and installs the production Zethropol Hardware D-Bus service.

## Runtime

The package installs:

- `/usr/lib/zethropol/zethropol-hardware`
- `/etc/dbus-1/system.d/org.zethropol.Hardware.conf`
- `/usr/share/dbus-1/system-services/org.zethropol.Hardware.service`
- `/usr/lib/systemd/system/zethropol-hardware.service`
- `/usr/share/licenses/zethropol-hardware/LICENSE`

The service uses system-level D-Bus activation through systemd and is not enabled through `multi-user.target`.

## Stage 04.6 Release (`0.1.0-4`)

- Adds the zero-fork `org.zethropol.Hardware.v1.GetTelemetry` contract alongside backward-compatible `GetState`.
- Uses the locked `libc::statvfs` binding (`libc 0.2.189`) for Linux filesystem usage reporting while preserving `f_frsize`/`f_bsize` and Btrfs occupancy semantics.
- Built offline with `--locked --offline` and path remapping so no developer home or temporary build paths appear in the binary.
- Preserves the previous rollback archive `zethropol-hardware-0.1.0-3-x86_64.pkg.tar.zst` untouched.
- Production package artifact is stored under `packages/stage-04.6/` (unsigned development build; not installed live during Stage 04.6).
