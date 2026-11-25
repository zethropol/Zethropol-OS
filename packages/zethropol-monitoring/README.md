# Zethropol Monitoring Package

Arch Linux package definition for the Zethropol Monitoring system service.

## Runtime Components

- `/usr/lib/zethropol/zethropol-monitoring`
- `/etc/dbus-1/system.d/org.zethropol.Monitoring.conf`
- `/usr/share/dbus-1/system-services/org.zethropol.Monitoring.service`
- `/usr/lib/systemd/system/zethropol-monitoring.service`
- `/usr/share/licenses/zethropol-monitoring/LICENSE`

The service uses system-level D-Bus activation through systemd and is not enabled through `multi-user.target`.

## Stage 04.6 Release (`0.1.0-3`)

- Consumes `org.zethropol.Hardware.v1.GetTelemetry` and serves the normalized 25-metric `org.zethropol.Monitoring.v1` contract (`GetState` + `StateChanged`) with staleness invalidation and deduplication.
- Declares versioned runtime dependency `zethropol-hardware>=0.1.0-4`.
- Moves test-only `serde_json` to `[dev-dependencies]` so the production release binary omits it.
- Built offline with `--locked --offline` and path remapping; preserves the previous rollback archive `zethropol-monitoring-0.1.0-2-x86_64.pkg.tar.zst` untouched.
- Production package artifact is stored under `packages/stage-04.6/` (unsigned development build; not installed live during Stage 04.6).
