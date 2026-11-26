# zethropol-performance

Arch Linux package definition for the Zethropol Performance D-Bus service.

> **Status:** Package recipe (`0.1.0-2`) is verified in an isolated rootless temporary build directory. Live package installation (`pacman -U`) and lifting of the `zethropol-performance` quarantine require separate explicit authorization.

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
