# Zethropol System Monitor — Qt6/QML D-Bus Data Bridge

Standalone, asynchronous, read-only Qt6/QML module (`org.zethropol.monitoring 1.0`) for consuming normalized system telemetry from `org.zethropol.Monitoring.v1`.

## Architecture

- **D-Bus interface:** `org.zethropol.Monitoring` · `/org/zethropol/Monitoring` · `org.zethropol.Monitoring.v1` (`GetState() -> a{sv}`, `StateChanged(a{sv})`).
- **Process-local shared backend (`MonitoringSharedClient`):** Multiple `MonitoringBridge` QML instances inside a host process (e.g., `plasmashell`) share one `MonitoringSharedClient` via a `std::weak_ptr` registry, guaranteeing at most one `QDBusServiceWatcher`, one `StateChanged` subscription, and zero multiplied D-Bus calls.
- **Strictly asynchronous & read-only:** Uses only `QDBusConnection::asyncCall` and D-Bus signals; never blocks the QML main thread, never spawns external processes or CLI tools, never connects directly to `org.zethropol.Hardware`, and exposes no writable properties or privileged actions.
- **Race protections:**
  - `m_ownerEpoch` + sender unique-name (`reply.service()` / `message.service()`) guards reject delayed replies and signals from previous service owners after restart/owner change.
  - `m_signalRevision` prevents an in-flight `GetState()` reply from overwriting newer `StateChanged` telemetry.
- **Staleness & deduplication:** Consumes Monitoring's proactive staleness `StateChanged` notifications (clearing `metrics` immediately), preserves valid metrics in `degraded` state without faking zeros, avoids redundant QML redraw signals when observable state is unchanged, and uses a rare `30 s` safety sync timer (no 1 Hz polling loop).

## QML Contract (`import org.zethropol.monitoring 1.0`)

`MonitoringBridge` exposes strictly read-only properties:

- `connected` (`bool`)
- `stateAvailable` (`bool`)
- `readiness` (`"initializing"` | `"ready"` | `"degraded"` | `"dependency-failed"`)
- `errorCode` (`""` | `"not-available"` | `"timeout"` | `"failed"` | `"unsupported"`)
- `errorMessage` (`string`)
- `metrics` (`QVariantMap`: validated subset of the 25 normalized `double` metrics)
- `lastUpdate` (`QDateTime`) / `lastUpdateMs` (`qint64`)
- `hasMetric(key)` (`bool`) / `metricValue(key)` (`double` or `undefined`)

### Unit Semantics (25 Metrics)

- CPU frequency: `GHz`
- Memory (`memory_*_gb`) and GPU VRAM (`gpu_vram_*_gb`): binary `GiB` (legacy `_gb` keys preserved)
- Storage (`storage_*_gb`): decimal `GB` (`10^9` bytes)
- Network throughput: decimal `Mbps`
- Temperatures: `°C` (`-100..250`)
- Clocks: `MHz` (`>= 0`)
- Percentages: `0..100` `%`

## Packaging & Licensing

- **Build dependencies:** `cmake`, `qt6-base`, `qt6-declarative` (test only: `qt6-declarative` Quick + `qt6-base` Test, `dbus`).
- **Runtime installation target:** `${QT_INSTALL_QML}/org/zethropol/monitoring/` (`/usr/lib/qt6/qml/org/zethropol/monitoring/` containing `libzethropol_monitoring_qml.so`, `qmldir`, `zethropol_monitoring_qml.qmltypes`).
- **Runtime dependencies:** `glibc`, `gcc-libs`, `qt6-base`, `qt6-declarative`, `zethropol-monitoring>=0.1.0-3` (which requires `zethropol-hardware>=0.1.0-4`).
- **Licensing:** Original Zethropol OS C++/QML bridge sources under the repository `MIT` license (`LICENSE`), dynamically linked against Qt6 (`LGPL-3.0-only` / `GPL-3.0-only` / `Qt-GPL-exception-1.0`) and packaged cleanly alongside the `GPL-3.0-or-later` System Monitor Plasmoid in `zethropol-system-monitor` (`license=('GPL-3.0-or-later' 'MIT')`).
