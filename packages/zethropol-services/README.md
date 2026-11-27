# zethropol-services

Arch Linux package definition for the Zethropol Services D-Bus service.

> **Status (2026-11-27):** Live installed version is `zethropol-services 0.1.0-2`. Release **`zethropol-services 0.1.0-3`** (`P2B`: clears obsolete `zethropol-performance.service` quarantine constant after `P2A-Live` remediation; adds `--remap-path-prefix` path sanitization, `gcc-libs` dependency, and MIT `LICENSE` install) is **built and verified rootlessly** (`5/5` Rust tests PASS, `0` `/home/Zevor` ELF leaks, `.pkg.tar.zst` SHA-256 `d4255ef7122b9f4067390875fa729e8189695fb031b203083510a984d3375fe3`) and awaits explicit live `pacman -U` approval.

## Runtime

Installs:

- `/usr/lib/zethropol/zethropol-services`
- `/etc/dbus-1/system.d/org.zethropol.Services.conf`
- `/usr/share/dbus-1/system-services/org.zethropol.Services.service`
- `/usr/lib/systemd/system/zethropol-services.service`
- `/usr/share/polkit-1/actions/org.zethropol.services.policy`
- `/usr/share/licenses/zethropol-services/LICENSE`

The service uses D-Bus activation through systemd and is not enabled through `multi-user.target`.
