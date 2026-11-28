# Zethropol Hava Durumu Arch Package (`zethropol-weather 0.1.0-1`)

KDE Plasma 6 için Zethropol OS koyu cam/kart tasarımlı, API anahtarsız, Türkiye il/ilçe uyumlu, **3 / 5 / 7 günlük tahmin seçicili** ve **ayarlanabilir opaklıklı (`%15`–`%100`)** hava durumu plasmoid paketi (`org.zethropol.plasma.weather`).

> **Durum (2026-11-28):** `zethropol-weather 0.1.0-1` root kullanmadan derlenip test edildi (`2/2` hedef, `12/12` QTest PASS, sıfır QML uyarısı, `xmllint` PASS, `ELF_HOME_LEAKS=0`, `RPATH_HITS=0`, kaynak SHA-256 `762a33837dd4fa617f1c37607111a9f2c2538bffae21aa112566a5d8a01febb0`, `.pkg.tar.zst` SHA-256 `4504945b000a8d9222bd3e704a600f12a95585c12079cff4c6e697e2127af485`) ve yerel `pacman` deposuna (`packages/repo/x86_64/`, `14/14` paket) indekslendi; canlı kurulum (`sudo pacman -U`) kullanıcı onayındadır.

## Paket İçeriği

- **Plasma 6 KPackage (`/usr/share/plasma/plasmoids/org.zethropol.plasma.weather/`):**
  - `metadata.json`
  - `contents/config/main.xml`, `contents/config/config.qml`
  - `contents/ui/main.qml`, `CompactRepresentation.qml`, `FullRepresentation.qml`, `configGeneral.qml`
- **Qt6/QML Eklentisi (`/usr/lib/qt6/qml/org/zethropol/weather/`):**
  - `libzethropol_weather_qml.so` (`Qt6::Network`, yalnızca izin listeli HTTPS — `QProcess`/`eval`/`Qt.createQmlObject` yok)
  - `qmldir`, `zethropol_weather_qml.qmltypes`
- **Lisans:** `/usr/share/licenses/zethropol-weather/LICENSE`

## Deterministik Kaynak Arşivi (Fish Uyumlu)

```fish
set BUILD_STAGE (mktemp -d)
mkdir -p "$BUILD_STAGE/stage/zethropol-weather-0.1.0/desktop/weather"
cp LICENSE "$BUILD_STAGE/stage/zethropol-weather-0.1.0/LICENSE"
cp -a desktop/weather/CMakeLists.txt desktop/weather/metadata.json desktop/weather/contents desktop/weather/plugin desktop/weather/tests "$BUILD_STAGE/stage/zethropol-weather-0.1.0/desktop/weather/"
tar --sort=name --mtime='UTC 2026-01-01' --owner=0 --group=0 --numeric-owner -C "$BUILD_STAGE/stage" -cf - zethropol-weather-0.1.0 | gzip -n > "$BUILD_STAGE/zethropol-weather-0.1.0-1-src.tar.gz"
```

Ayrıntılı kurulum, masaüstü/panel ekleme ve isteğe bağlı `org.kde.plasma.weather` yerine geçirme adımları için `docs/ZETHROPOL_WEATHER.md` belgesine bakın.
