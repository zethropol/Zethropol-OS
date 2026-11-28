# Zethropol Hava Durumu (`org.zethropol.plasma.weather`) — Kullanıcı ve Mimari Rehberi

**Zethropol Hava Durumu** (`zethropol-weather 0.1.0-1`), KDE Plasma 6 için Zethropol OS koyu cam/kart (*glassmorphic dark surface*) tasarım kimliğiyle geliştirilmiş, API anahtarsız, Türkiye il/ilçe uyumlu, **3 / 5 / 7 günlük tahmin seçicili** ve **ayarlanabilir opaklıklı** masaüstü / panel hava durumu aracıdır.

## 1. Özellikler ve Tasarım Kimliği

- **Zethropol Koyu Cam Yüzeyi:** `desktop/tv` ve `zethropol-system-monitor` ile uyumlu koyu cam kart yüzeyi (`#0b111d`), hava durumuna göre değişen atmosferik üst gradyanlar ve `Plasmoid.backgroundHints: PlasmaCore.Types.NoBackground` desteğiyle duvar kağıdı üzerinde çerçevesiz temiz görünüm.
- **3 / 5 / 7 Günlük Tahmin Seçici:** Hem ayarlar penceresinden (`configGeneral.qml`) hem de doğrudan kart/açılır pencere üzerindeki kompakt sekme çubuğundan (**3 Gün** | **5 Gün** | **7 Gün**) anlık dilimleme yapılır; tercih `plasmoid.configuration.forecastDays` içinde kalıcı saklanır (varsayılan: **5 Gün**).
- **Ayarlanabilir Opaklık (`%15` – `%100`) ve Kontrast Koruması:**
  - **Masaüstü Kartı Opaklığı:** `%15`–`%100` (varsayılan `%82`).
  - **Açılır Pencere (Popup) Opaklığı:** `%15`–`%100` (varsayılan `%92`).
  - Opaklık `%55`'in altına indiğinde metin dış çizgi kontrast koruması (`Text.Outline`) ve cam kenarlığı otomatik güçlendirilerek açık renk duvar kağıtlarında dahi okunabilirlik korunur.
- **Panel & Masaüstü Çift Görünüm:**
  - **Masaüstü (`Planar`):** Doğrudan tam Zethropol hava durumu kartı (`FullRepresentation.qml`).
  - **Panel / Sistem Tepsisi (`Horizontal` / `Vertical`):** Kompakt hava ikonu + anlık sıcaklık (`18°`, `CompactRepresentation.qml`); tıklandığında tam cam açılır pencere açılır.

## 2. API Anahtarsız Veri Mimarisi ve Güvenlik

| Katman | Kaynak / Mekanizma | Ayrıntı |
|---|---|---|
| **Birincil Tahmin & Konum Arama** | **Open-Meteo** (`api.open-meteo.com`, `geocoding-api.open-meteo.com`) | API anahtarı gerektirmez; `language=tr` ile Türkçe il/ilçe arama (Türkiye sonuçları ve yüksek nüfuslu merkezler önceliklendirilir; örn. *İstanbul*, *Kadıköy*, *Çankaya*, *Karşıyaka*, *Nilüfer*), anlık ölçüm + 24 saatlik akış + 7 günlük tahmin (`forecast_days=7`). |
| **İsteğe Bağlı MGM Zenginleştirmesi** | **MGM** (`servis.mgm.gov.tr/web/merkezler` & `sondurumlar`) | Türkiye (`TR`) konumlarında açık MGM anlık gözlem verisi (`-9999` eksik değer filtreli) ile sıcaklık, nem, rüzgâr, basınç ve hadise kodu zenginleştirilir; MGM erişilemezse sorunsuz biçimde Open-Meteo verisi kullanılır. |
| **Çevrimdışı Önbellek** | `~/.cache/zethropol/weather/cache.json` (`QSaveFile`) | Ağ kesintisinde son başarılı ölçüm otomatik yüklenir ve kart üzerinde **`Çevrimdışı önbellek`** etiketi gösterilir. |
| **Güvenlik Sınırları** | `org.zethropol.weather 1.0` (`Qt6::Network`) | Hiçbir harici süreç/kabuk çağrısı (`QProcess`), `eval` veya `Qt.createQmlObject` içermez; yalnızca izin listeli `https://` uç noktalarına (`api.open-meteo.com`, `geocoding-api.open-meteo.com`, `servis.mgm.gov.tr`) 512 KiB yanıt sınırı ve 10 sn zaman aşımı ile bağlanır. ZCI/D-Bus/Polkit ayrıcalığı gerektirmez. |

## 3. Kurulum ve Masaüstüne / Panele Ekleme

> **Not:** Paket kurulumu kullanıcının mevcut `~/.config/plasma-org.kde.plasma.desktop-appletsrc` dosyasına otomatik dokunmaz.

### Masaüstüne veya Panele Ekleme

1. Paket kurulumundan sonra masaüstüne veya Plasma paneline sağ tıklayıp **Düzenleme Moduna Gir → Araç Ekle...** (*Add Widgets*) menüsünü açın.
2. Arama kutusuna **`Zethropol Hava Durumu`** yazın ve masaüstüne (tam cam kart) veya panele (kompakt simge + sıcaklık) sürükleyin.
3. Araca sağ tıklayıp **Zethropol Hava Durumu Uygulamacığını Yapılandır...** penceresinden il/ilçe araması, **3 / 5 / 7 Gün** tahmini ve **Masaüstü / Açılır Pencere Opaklığı (`%15`–`%100`)** ayarlarını özelleştirin.

### İsteğe Bağlı: Mevcut `org.kde.plasma.weather` Aracının Yerine Geçirme

Mevcut KDE standart hava durumu aracını (`org.kde.plasma.weather`) Zethropol Hava Durumu ile değiştirmek için önerilen en güvenli yol grafik arayüzdür:

1. Masaüstünde veya panelde **Düzenleme Modu**'nu açın.
2. Eski **Hava Durumu** (`org.kde.plasma.weather`) aracının üzerine gelip **Kaldır** düğmesine tıklayın.
3. **Araç Ekle...** menüsünden **Zethropol Hava Durumu** (`org.zethropol.plasma.weather`) aracını aynı konuma yerleştirin.

*(İsteğe bağlı olarak Plasma yeni kurulan eklentiyi hemen listelemezse kullanıcı oturumunda `kquitapp6 plasmashell; kstart plasmashell` ile masaüstü kabuğu yeniden yüklenebilir.)*
