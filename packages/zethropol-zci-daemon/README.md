# zethropol-zci-daemon (`0.1.0-1` — Aşama 10 ZCI Daemon & Policy-Gated Action Gateway)

Arch Linux package definition for the Zethropol ZCI Daemon (`org.zethropol.ZciDaemon`, `org.zethropol.ZciDaemon.v1`).

> **Status (2026-11-28):** Stage 10 release **`zethropol-zci-daemon 0.1.0-1`** (`10.01`–`10.07`) is built and verified rootlessly (`8/8` Rust unit & private D-Bus tests with `0` warnings, `28/28` `services/local-inference` Python tests including real cross-service `zethropol-security` + `zethropol-zci-daemon` D-Bus integration, `0` `/home/Zevor` ELF leaks, `0` RPATH/RUNPATH, source SHA-256 `d330cf4aad22b839a64125fd2fea9f8e5d19387b7d75d1b296e19e4bc75db539`, `.pkg.tar.zst` SHA-256 `f80337a42959d89cd9650049e3440c393ce4fb37a3e359886985ec2432db56f2`) and indexed in `packages/repo/x86_64/` (`13/13` packages); live `sudo pacman -U` awaits operator execution.

## D-Bus & Polkit Identity

- **Bus name:** `org.zethropol.ZciDaemon`
- **Object path:** `/org/zethropol/ZciDaemon`
- **Interface:** `org.zethropol.ZciDaemon.v1`
  - **Session Lifecycle (`10.04`):** `OpenSession`, `SetSessionStandby`, `CloseSession`
  - **Policy-Gated Action Gateway (`10.02`):** `ExecuteAction` (mandatory fail-closed gate via `org.zethropol.Security.Policy.v1.EvaluateAction`, strictly limited to `ReadOnly` and `UserSpaceSafe` classes)
  - **Untrusted Sealing Bridge (`10.03`):** `WrapUntrustedContent` (mandatory fail-closed bridge via `org.zethropol.Security.Policy.v1.WrapUntrustedData`)
  - **Local Read-Only Health & Observability (`10.05`, `10.06`):** `GetDaemonStatus`, `GetState`, `ListSessions`
  - **Polkit Admin (`10.05` — `org.zethropol.zci.daemon.admin`, `auth_admin`):** `TerminateSession`, `ReloadConfig`
  - **Signals:** `SessionStateChanged`, `ActionAudited`

## Runtime Files & Systemd Hardening (`10.01`)

- `/usr/lib/zethropol/zethropol-zci-daemon`
- `/etc/dbus-1/system.d/org.zethropol.ZciDaemon.conf` (`<allow own>` strictly limited to `user="root"`)
- `/usr/share/dbus-1/system-services/org.zethropol.ZciDaemon.service`
- `/usr/lib/systemd/system/zethropol-zci-daemon.service` (`ProtectSystem=strict`, `ProtectHome=read-only`, `NoNewPrivileges=yes`, `PrivateTmp=yes`, `PrivateDevices=yes`, `MemoryDenyWriteExecute=yes`, `CapabilityBoundingSet=`, `AmbientCapabilities=`, `RestrictAddressFamilies=AF_UNIX AF_INET`, `IPAddressDeny=any`, `IPAddressAllow=localhost`, `SystemCallArchitectures=native`, `SystemCallFilter=@system-service`, `SystemCallFilter=~@privileged @resources @mount @raw-io @module @reboot @swap @clock @cpu-emulation @debug @obsolete`)
- `/usr/share/polkit-1/actions/org.zethropol.zci.daemon.policy` (`org.zethropol.zci.daemon.admin`, `auth_admin`)
- `/usr/share/licenses/zethropol-zci-daemon/LICENSE`

## Security Invariants

- Strictly depends on `zethropol-security>=0.1.0-3` (`org.zethropol.Security.Policy.v1`); fails closed whenever the Policy Engine is unreachable, times out, or returns an unexpected class outside `ReadOnly`/`UserSpaceSafe`.
- Never writes to `/home` directly (`ProtectHome=read-only`); `UserSpaceSafe` requests are attested as `delegated-userspace-sandbox` for unprivileged user-space execution.
- Loopback-only (`127.0.0.1`/`::1`) local Gemma 4B passive standby coordination (zero VRAM while in `Standby`).
- Preserves `services/recovery/src/recovery_service.rs` safety locks (`rollback_execution_enabled=false`, `repair_execution_enabled=false`).

## Deterministic Source Archive (Fish Compatible)

```fish
set BUILD_STAGE (mktemp -d)
mkdir -p "$BUILD_STAGE/stage/zethropol-zci-daemon-0.1.0/services/zci-daemon"
cp LICENSE "$BUILD_STAGE/stage/zethropol-zci-daemon-0.1.0/LICENSE"
cp -a services/zci-daemon/Cargo.toml services/zci-daemon/Cargo.lock services/zci-daemon/src services/zci-daemon/deployment "$BUILD_STAGE/stage/zethropol-zci-daemon-0.1.0/services/zci-daemon/"
tar --sort=name --mtime='UTC 2026-01-01' --owner=0 --group=0 --numeric-owner -C "$BUILD_STAGE/stage" -cf - zethropol-zci-daemon-0.1.0 | gzip -n > "$BUILD_STAGE/zethropol-zci-daemon-0.1.0-1-src.tar.gz"
```
