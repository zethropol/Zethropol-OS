# zethropol-performance

Arch Linux package definition for the Zethropol Performance D-Bus service.

> **Status:** `zethropol-performance 0.1.0-2` is **deployed live and verified** (2026-11-27): `pacman -Qkk` (`18` files, `0` modified), `/etc/systemd/system/zethropol-performance.service.d/99-zethropol-quarantine.conf` backed up under `backups/performance-quarantine-backup/` and removed, `systemctl is-active`: `active`, `busctl GetState`: `success=true`, `code="ok"` (`18` live metrics), `systemd-analyze security`: `3.6 OK :-)`. Built artifacts remain verified in `packages/zethropol-performance/SHA256SUMS` (`2/2` OK; `.pkg.tar.zst` SHA-256: `b421a05162576ec92ad318e126389d98d012ab4003e04050513274772c549b13`, `0` `/home/Zevor` leaks).

## D-Bus identity

- Bus name: `org.zethropol.Performance`
- Object path: `/org/zethropol/Performance`
- Interface: `org.zethropol.Performance.v1` (read-only `GetState` projection over `org.zethropol.Hardware.v1.GetState`)

## Runtime components

The service is D-Bus activated through systemd and is not enabled through `multi-user.target`.

- `/usr/lib/zethropol/zethropol-performance`
- `/etc/dbus-1/system.d/org.zethropol.Performance.conf`
- `/usr/share/dbus-1/system-services/org.zethropol.Performance.service`
- `/usr/lib/systemd/system/zethropol-performance.service`
- `/usr/share/licenses/zethropol-performance/LICENSE`

## Security hardening release (`0.1.0-2`)

- Restricts `org.zethropol.Performance` bus name ownership exclusively to `<policy user="root">`; `<policy context="default">` retains only `send_destination`.
- Hardens `zethropol-performance.service` with read-only sandboxing (`ProtectSystem=strict`, `ProtectKernelTunables=true`, `PrivateDevices=true`, `ProtectHome=true`, `NoNewPrivileges=true`, empty `CapabilityBoundingSet=`/`AmbientCapabilities=`, `RestrictAddressFamilies=AF_UNIX`, `MemoryDenyWriteExecute=true`, matching `zethropol-monitoring.service`; no `ReadWritePaths=` required).
- Declares runtime dependencies `gcc-libs` and `zethropol-hardware>=0.1.0-4`.
- Builds and tests offline (`--locked --offline`) with `RUSTFLAGS` `--remap-path-prefix` path sanitization and installs the MIT `LICENSE`.

## Deterministic source archive (Fish-compatible)

The source archive (`zethropol-performance-0.1.0-2-src.tar.gz`) is not tracked in Git and can be reproduced deterministically from the repository root (SHA-256: `9b65ecc476dcf7d62bcbae65b8293d391c5345c9853317ebfd46a8fb7238780e`):

```fish
set BUILD_STAGE (mktemp -d)
mkdir -p "$BUILD_STAGE/stage/zethropol-performance-0.1.0/services/performance"
cp LICENSE "$BUILD_STAGE/stage/zethropol-performance-0.1.0/LICENSE"
cp -a services/performance/Cargo.toml services/performance/Cargo.lock services/performance/src services/performance/deployment "$BUILD_STAGE/stage/zethropol-performance-0.1.0/services/performance/"
tar --sort=name --mtime='UTC 2026-01-01' --owner=0 --group=0 --numeric-owner -C "$BUILD_STAGE/stage" -cf - zethropol-performance-0.1.0 | gzip -n > "$BUILD_STAGE/zethropol-performance-0.1.0-2-src.tar.gz"
cp packages/zethropol-performance/PKGBUILD "$BUILD_STAGE/PKGBUILD"
```
