# Zethropol System Monitor

Five-pod Plasma 6 QML system telemetry monitor (`CPU LOAD`, `RAM USAGE`, `GPU LOAD`, `NETWORK`, `STORAGE`) packaged as a standard KDE Plasma 6 KPackage (`org.zethropol.systemmonitor`) and powered by `org.zethropol.Monitoring.v1` through the standalone Qt6/QML data bridge (`import org.zethropol.monitoring 1.0`).

## Standard KPackage & Source Layout

- `services/system-monitor/metadata.json` — Plasma 6 KPackage metadata (`Plasma/Applet`, `org.zethropol.systemmonitor`, `X-Plasma-API-Minimum-Version: 6.0`)
- `services/system-monitor/contents/ui/main.qml` — 5-pod Plasmoid UI
- `services/system-monitor/contents/config/main.xml` — KConfigXT schema (`backgroundOpacity`, default `60`, range `0..100`)
- `services/system-monitor/contents/config/config.qml` — Plasma configuration model
- `services/system-monitor/contents/ui/config/Appearance.qml` — Appearance settings page
- `services/system-monitor/bridge/` — Standalone Qt6/QML C++ module (`org.zethropol.monitoring 1.0`)
- `services/system-monitor/tests/` — Offscreen QML & integration test suite (excluded from production packages)
- `services/system-monitor/CMakeLists.txt` — Unified build, test, and packaging install rules

## Data Flow

`org.zethropol.Hardware.v1.GetTelemetry` -> `org.zethropol.Monitoring.v1` (`GetState` + `StateChanged`) -> `MonitoringBridge` (`services/system-monitor/bridge/`) -> `services/system-monitor/contents/ui/main.qml` (5 pods).

- Legacy `Plasma5Support.DataSource` (`executable`) and CLI `--monitor` paths have been removed with no CLI fallback.
- A single `MonitoringBridge` instance in `contents/ui/main.qml` feeds all five pods reactively via `metricsChanged` and `stateChanged`.
- No external processes, CLI tools, periodic QML timers, or direct `org.zethropol.Hardware` calls are performed by the UI.

## Pod Metric Contracts

| Pod | Metrics | Unit & Semantics |
| --- | --- | --- |
| **CPU LOAD** | `cpu_usage_percent`, `cpu_temperature_c`, `cpu_frequency_ghz` | `%` (`0..100`), `°C`, `GHz * 1000 -> MHz`; neutral Canvas when missing |
| **RAM USAGE** | `memory_used_gb`, `memory_total_gb` | Binary `GiB`, `% = used / total * 100` (guards `total <= 0` and `used > total`) |
| **GPU LOAD** | `gpu_usage_percent`, `gpu_temperature_c`, `gpu_core_clock_mhz`, `gpu_vram_used_gb`, `gpu_vram_total_gb` | `%`, `°C`, `MHz`, VRAM in `GiB` (guards missing GPU / `used > total` / `total <= 0`) |
| **NETWORK** | `network_download_mbps`, `network_upload_mbps`, `network_ping_ms` | Decimal `Mbps`, optional `ms` (`— ms  •  PING` by default; zero external ping) |
| **STORAGE** | `storage_filesystem_usage_percent`, `storage_filesystem_used_gb`, `storage_filesystem_total_gb`, `storage_temperature_c` | Direct Btrfs-aware filesystem `%` (never recalculated, never falls back to physical capacity), decimal `GB`, `°C` |

## Configuration & Visual Scaling

- Preserves `Plasmoid.title: "Zethropol System Monitor"`, `Plasmoid.backgroundHints: PlasmaCore.Types.NoBackground`, and `Plasmoid.configuration.backgroundOpacity` (`0..100`, default `60` in `contents/config/main.xml`, affecting only card background alpha while keeping text, borders, Canvas arcs, and accent bars opaque).
- Reference layout: five `180×180` pods (`radius: 24`, `spacing: 12`, total `948×180`), unified title alignment across all five pods, unified bottom blue accent bar across all five pods (including `GPU LOAD`), `Text.HorizontalFit` overflow guards, and proportional aspect-preserving container scaling down to `minReadableScale = 0.55` (`522×99` logical minimum).

## Licensing & Compatibility

- **KPackage Plasmoid (`org.zethropol.systemmonitor`):** Preserves the existing `GPL-3.0-or-later` license declared in `metadata.json`. No relicensing has been performed.
- **Qt6/QML Bridge (`services/system-monitor/bridge/`):** Original Zethropol OS C++/QML sources developed in this repository under the repository-level `MIT` license (`LICENSE`), dynamically linked against system Qt6 (`qt6-base`, `qt6-declarative`: `LGPL-3.0-only` / `GPL-3.0-only` / `Qt-GPL-exception-1.0`).
- **Distribution compatibility:** `MIT` is permissive and compatible with dynamic linking to `LGPL-3.0-only` Qt6 libraries as well as combined packaging alongside `GPL-3.0-or-later` Plasmoid QML files (`libplasma`: `LGPL-2.0-or-later`, `kirigami`/`kpackage`: `LGPL-2.0-only` / `LGPL-3.0-only`). The Arch package (`zethropol-system-monitor`) therefore declares `license=('GPL-3.0-or-later' 'MIT')` and installs `/usr/share/licenses/zethropol-system-monitor/LICENSE`.
- **Provenance note:** Individual QML/C++ source files do not carry per-file SPDX headers; license attributions reflect the repository root `LICENSE` (`MIT`) and the existing Plasmoid `metadata.json` (`GPL-3.0-or-later`).

## Production Installation Layout

- KPackage Plasmoid: `/usr/share/plasma/plasmoids/org.zethropol.systemmonitor/`
- Qt6/QML Plugin: `/usr/lib/qt6/qml/org/zethropol/monitoring/` (`libzethropol_monitoring_qml.so`, `qmldir`, `zethropol_monitoring_qml.qmltypes`)

## Out-of-Tree Build & Isolated Testing

```bash
cmake -S services/system-monitor -B /tmp/zethropol-sm-build -G Ninja -DBUILD_TESTING=ON
cmake --build /tmp/zethropol-sm-build
ZETHROPOL_HARDWARE_RELEASE_BIN=/path/to/hardware \
ZETHROPOL_MONITORING_RELEASE_BIN=/path/to/monitoring \
ctest --test-dir /tmp/zethropol-sm-build --output-on-failure -V
```
