# Zethropol Hava Durumu Arch Package (`zethropol-weather 0.1.0-2`)

KDE Plasma 6 için Zethropol OS koyu cam/kart tasarımlı, API anahtarsız, Türkiye il/ilçe uyumlu, **3 / 5 / 7 günlük tahmin seçicili** ve **ayarlanabilir opaklıklı (`%15`–`%100`)** hava durumu plasmoid paketi (`org.zethropol.plasma.weather`).

> **Durum (2026-11-28):** `zethropol-weather 0.1.0-2` (masaüstü/`plasmawindowed` doğrudan tam kart, 3/5/7 güne göre taşmasız boyut, açılışta anında İstanbul Open-Meteo isteği, alt durum çubuğunda gerçek hata + `Tekrar Dene`) root kullanmadan derlenip test edildi (`2/2` hedef, `15/15` QTest PASS, sıfır QML uyarısı, `ELF_HOME_LEAKS=0`, `RPATH_HITS=0`, kaynak SHA-256 `63319a8101b2b67fedc9d7d7f649819eb5e5043bbc879830ccf715f1e8208651`, `.pkg.tar.zst` SHA-256 `0537f532566f60d7a6a8c2c46e3ef37e0ffe5ba4f9e5b69004f6ace4006eea0d`) ve yerel `pacman` deposuna (`14/14` paket) indekslendi; canlı kurulum (`sudo pacman -U`) kullanıcı onayındadır.

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
tar --sort=name --mtime='UTC 2026-01-01' --owner=0 --group=0 --numeric-owner -C "$BUILD_STAGE/stage" -cf - zethropol-weather-0.1.0 | gzip -n > "$BUILD_STAGE/zethropol-weather-0.1.0-2-src.tar.gz"
```

Ayrıntılı kurulum, masaüstü/panel ekleme ve isteğe bağlı `org.kde.plasma.weather` yerine geçirme adımları için `docs/ZETHROPOL_WEATHER.md` belgesine bakın.
