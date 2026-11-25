# Monitoring Service Deployment

Runtime installation layout:

- Binary: `/usr/lib/zethropol/zethropol-monitoring`
- D-Bus policy: `/etc/dbus-1/system.d/org.zethropol.Monitoring.conf`
- D-Bus activation: `/usr/share/dbus-1/system-services/org.zethropol.Monitoring.service`
- systemd unit: `/usr/lib/systemd/system/zethropol-monitoring.service`

The Cargo release binary is produced as `target/release/monitoring` and is installed as `zethropol-monitoring`.

## Version Dependency

- `org.zethropol.Monitoring.v1` exclusively consumes `org.zethropol.Hardware.v1.GetTelemetry` and does **not** fall back to heavy `Hardware.v1.GetState`.
- Future packaging (`zethropol-monitoring >= 0.1.0-3`) must declare a matching runtime dependency on the `zethropol-hardware` release that introduces `org.zethropol.Hardware.v1.GetTelemetry` (e.g., `zethropol-hardware >= 0.1.0-4`).
- If paired with an older `zethropol-hardware` binary lacking `GetTelemetry`, Monitoring reports `success = false`, `code = "unsupported"`, `readiness = "dependency-failed"`, empty `data = {}`, and backs off to a 30-second retry interval with deduplicated logging.

## D-Bus Contract (`org.zethropol.Monitoring.v1`)

- **Bus name:** `org.zethropol.Monitoring`
- **Object path:** `/org/zethropol/Monitoring`
- **Interface:** `org.zethropol.Monitoring.v1`

### Methods

- `GetState() -> a{sv}`
  - Returns the standard Zethropol structured result envelope:
    - `success` (`bool`)
    - `code` (`string`: `"ok"`, `"not-available"`, `"timeout"`, `"unsupported"`, `"failed"`)
    - `message` (`string`)
    - `operationId` (`string`)
    - `readiness` (`string`: `"initializing"`, `"ready"`, `"degraded"`, `"dependency-failed"`)
    - `data` (`a{sv}`: validated subset of the 25 normalized `f64` metrics on `success = true`; empty `{}` on `success = false`)

### 25 Normalized Metric Contract (`data`)

- **CPU (3):** `cpu_usage_percent` (`%`), `cpu_temperature_c` (`°C`), `cpu_frequency_ghz` (`GHz`)
- **GPU (7):** `gpu_usage_percent` (`%`), `gpu_memory_usage_percent` (`%`), `gpu_core_clock_mhz` (`MHz`), `gpu_vram_used_gb` (`GiB`, legacy `_gb` key), `gpu_vram_total_gb` (`GiB`, legacy `_gb` key), `gpu_temperature_c` (`°C`), `gpu_memory_clock_mhz` (`MHz`)
- **Memory (2):** `memory_used_gb` (`GiB`, legacy `_gb` key), `memory_total_gb` (`GiB`, legacy `_gb` key)
- **Storage (10 — decimal `GB` & `%`):**
  - Canonical: `storage_used_gb`, `storage_capacity_gb` (physical disk), `storage_available_gb`, `storage_temperature_c` (`°C`)
  - Explicit filesystem / physical: `storage_filesystem_total_gb`, `storage_filesystem_used_gb`, `storage_filesystem_free_gb`, `storage_filesystem_available_gb`, `storage_filesystem_usage_percent` (direct from Hardware), `storage_physical_capacity_gb`
- **Network (3):** `network_download_mbps` (`Mbps`), `network_upload_mbps` (`Mbps`), `network_ping_ms` (`ms`, omitted by default)

Missing, `NaN`, infinite, out-of-range, or malformed readings are omitted rather than masked as `0.0`. Genuine `0.0` measurements are preserved.

### Readiness Policy

- `initializing`: No sample collected yet (`success = false`, `code = "not-available"`, `data = {}`).
- `degraded`: Valid partial telemetry is available (`success = true`, `code = "ok"`), but either mandatory warming-up/core measurements are missing (`cpu_usage_percent`, `memory_{used,total}_gb`, root filesystem usage when storage is detected, or active-link `network_{download,upload}_mbps`) or a previously observed sensor on a still-present subsystem transiently disappeared.
- `ready`: All mandatory measurements for present/active subsystems are valid. Default disabled `network_ping_ms`, GPU-less systems (`gpu.detected = false`), offline networks (`operstate = down` or no default route), and unsupported optional sensors do **not** cause `degraded`.
- `dependency-failed`: Hardware is unreachable, timed out, incompatible (`UnknownMethod` → `code = "unsupported"`), returned an invalid/failed payload, or telemetry exceeded `max_sample_age` (`6 s`).

### Signals

- `StateChanged(state: a{sv})`
  - Emits the exact same structured result envelope as `GetState()`.
  - Emitted only on meaningful metric updates, readiness transitions, failure transitions, or proactive staleness expiration (watchdog timer at `max_sample_age`).
  - Timestamp-only refreshes and repeated identical failures do not emit redundant signals.

### Sampling & Resource Policy

- Default intervals: `sample_interval = 1 s`, `retry_interval = 1 s`, `incompatible_retry_interval = 30 s`, `sample_timeout = 3500 ms`, `max_sample_age = 6 s`.
- A single non-overlapping background sampler loop shares the server's persistent D-Bus connection and reuses an uncached proxy (`CacheProperties::No`).
- On system D-Bus transport closure (`connection.closed()`), the daemon exits with non-zero status so systemd (`BindsTo=dbus.socket`, `Restart=on-failure`) re-acquires `org.zethropol.Monitoring` cleanly.
