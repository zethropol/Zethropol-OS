# zethropol-security (`0.1.0-3` — Aşama 08 ZCI Security & Policy Engine)

Arch Linux package definition for the Zethropol Security & ZCI Policy Engine D-Bus service.

> **Status (2026-11-28):** Live installed baseline is `zethropol-security 0.1.0-2`. Stage 08 release **`zethropol-security 0.1.0-3`** (preserving `org.zethropol.Security.v1` and adding `org.zethropol.Security.Policy.v1` at `/org/zethropol/Security/Policy` for `08.01`–`08.08`: default-deny 5-tier action classification, per-agent allowlists for `zci-chat`/`zekans-code`/`zci-command`/`zci-vision`, untrusted content isolation & injection blocking, hardcoded engine-level self-modification lock, persistent progressive abuse penalties/lockout gated by Polkit `org.zethropol.security.policy.admin`, and SHA-256 hash-chained structured JSONL audit logging) is **built and verified rootlessly** (`16/16` Rust tests including private D-Bus & Polkit checks, `0` `/home/Zevor` ELF leaks, `0` RPATH/RUNPATH, source SHA-256 `1ea8d9f835c1d639430d6045cc08be040b7ca8d8af0448baa3e8aa2014521af2`, `.pkg.tar.zst` SHA-256 `ee44bc20d51a3c0775538b5eae22fd8d2d7d1e8e8308e87a41f2f1696a073058`) and indexed in `packages/repo/x86_64/`; live `sudo pacman -U` awaits operator execution.

## D-Bus & Polkit Identity

- Bus name: `org.zethropol.Security`
- Preserved interface: `/org/zethropol/Security` (`org.zethropol.Security.v1`: `GetState`, `ToggleFirewall`, `StateChanged`)
- Added Stage 08 interface: `/org/zethropol/Security/Policy` (`org.zethropol.Security.Policy.v1`: `EvaluateAction`, `WrapUntrustedData`, `GetAgentStatus`, `GetPolicySummary`, `GetAuditEvents`, `ResetAgentLockout`, `PolicyDecisionRecorded`)
- Polkit actions (`org.zethropol.security.policy`):
  - `org.zethropol.security.modify` (`auth_admin`)
  - `org.zethropol.security.policy.admin` (`auth_admin` — required for `ResetAgentLockout`)

## Runtime Files & Hardening

- `/usr/lib/zethropol/zethropol-security`
- `/etc/dbus-1/system.d/org.zethropol.Security.conf`
- `/usr/share/dbus-1/system-services/org.zethropol.Security.service`
- `/usr/lib/systemd/system/zethropol-security.service` (`ProtectSystem=strict`, `ProtectHome=true`, `StateDirectory=zethropol-security`, `LogsDirectory=zethropol-security`, `NoNewPrivileges=true`, `RestrictAddressFamilies=AF_UNIX`, `RestrictNamespaces=true`, `RestrictRealtime=true`, `LockPersonality=true`, `MemoryDenyWriteExecute=true`)
- `/usr/share/polkit-1/actions/org.zethropol.security.policy`
- `/usr/share/licenses/zethropol-security/LICENSE`

Persistent state (`/var/lib/zethropol-security/zci-policy-state.json`, `0600`) and hash-chained audit logs (`/var/log/zethropol-security/zci-audit.jsonl`, `0640`) are owned by root under systemd `StateDirectory`/`LogsDirectory`, isolating them at the OS level from unprivileged ZCI user-space agents.
