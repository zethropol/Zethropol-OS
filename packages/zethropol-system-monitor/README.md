# Zethropol System Monitor Package (`0.1.0-1`)

Arch Linux package definition for the five-pod KDE Plasma 6 Zethropol System Monitor Plasmoid (`org.zethropol.systemmonitor`) and its standalone Qt6/QML D-Bus data bridge (`org.zethropol.monitoring 1.0`).

## Runtime Layout

- KPackage Plasmoid: `/usr/share/plasma/plasmoids/org.zethropol.systemmonitor/`
  - `metadata.json`
  - `contents/ui/main.qml`
  - `contents/ui/config/Appearance.qml`
  - `contents/config/main.xml`
  - `contents/config/config.qml`
- Qt6/QML Module: `/usr/lib/qt6/qml/org/zethropol/monitoring/`
  - `libzethropol_monitoring_qml.so`
  - `qmldir`
  - `zethropol_monitoring_qml.qmltypes`
- License: `/usr/share/licenses/zethropol-system-monitor/LICENSE`

## Runtime Dependencies & Licensing

- Verified runtime dependencies: `glibc`, `gcc-libs`, `qt6-base`, `qt6-declarative`, `libplasma`, `kirigami`, `kpackage`, `zethropol-monitoring>=0.1.0-3` (transitively requiring `zethropol-hardware>=0.1.0-4`).
- Legacy `plasma5support` and CLI `--monitor` dependencies are completely absent.
- Licensing: `license=("GPL-3.0-or-later" "MIT")` — preserves `GPL-3.0-or-later` on the KPackage Plasmoid (`metadata.json`) and repository `MIT` license on the original Qt6/QML bridge sources without unauthorized relicensing. Compatible with dynamic linking to Qt6 (`LGPL-3.0-only` / `GPL-3.0-only` / `Qt-GPL-exception-1.0`).

## Stage 04.6 Artifact Note

- Built offline and rootlessly via `makepkg`; production archive is stored in `packages/stage-04.6/` (unsigned development build; not installed live during Stage 04.6).
- Before live installation in Stage 04.7, any user-local copy at `~/.local/share/plasma/plasmoids/org.zethropol.systemmonitor/` must be backed up and moved aside so it does not shadow the system KPackage while preserving `backgroundOpacity=90` in `~/.config/plasma-org.kde.plasma.desktop-appletsrc`.
