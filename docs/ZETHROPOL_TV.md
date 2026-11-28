# Zethropol TV — Kullanıcı ve Mimari Rehberi

**Zethropol TV** (`zethropol-tv`), Zethropol OS için `Qt6/QML` ve gömülü `libmpv` tabanlı olarak geliştirilmiş bağımsız canlı TV ve açık erişimli yayın oynatıcısıdır.

## 1. Mimari ve Güvenlik Sınırları

- **Tam Bağımsızlık:** Zethropol TV hiçbir ZCI bileşenine, `systemd` sistem servisine, D-Bus arayüzüne veya Polkit politikasına bağlı değildir; tamamen kullanıcı oturumunda ayrıcalıksız çalışır.
- **Sertleştirilmiş `libmpv` Oynatıcı:** Gömülü oynatıcı (`MpvItem`) her zaman `load-scripts=no`, `ytdl=no` ve `load-unsafe-playlists=no` kilitleriyle başlatılır.
- **URL İzin Listesi (`StreamSecurity`):** Yalnızca geçerli `http://` / `https://` medya ve yayıncı adresleri kabul edilir; `file://`, `exec://`, `lua://`, `javascript:` veya kimlik bilgisi/kontrol karakteri içeren adresler engellenir.
- **İki Aşamalı Yayın Çözümleyici (`StreamResolver`):**
  1. Doğrudan `.m3u8` / HLS yayınları güvenlik doğrulamasından hemen sonra oynatılır.
  2. Resmi yayıncı canlı sayfaları, yayıncı API'leri (Turkuvaz `securevideotoken`, Demirören `/action/media/`) ve doğrulanmış YouTube canlı/kanal adresleri sıralı yedek (`fallbackUrls`) zinciriyle çözümlenir.

## 2. Varsayılan Kanal Listesi (v6 — 41 Kanal)

Uygulama ilk açılışta (veya önceki `state.json` v1–v5 sürümlerinden otomatik yükseltmede kullanıcının kendi eklediği özel kanallar ve favoriler korunarak) **8 kategoride 41 resmi/açık erişimli kanal** yükler:

| Kategori | Kanal Sayısı | Örnek Kanallar |
|---|---|---|
| **Ulusal** | 10 | TRT 1, ATV, Show TV, Star TV, Kanal D, NOW, TV8, Kanal 7, Beyaz TV, TV8.5 |
| **Haber** | 10 | TRT Haber, NTV, CNN Türk, Habertürk, Halk TV, A Haber, Haber Global, TGRT Haber, Ülke TV, Kanal 24 |
| **Spor** | 5 | TRT Spor, TRT Spor Yıldız, Red Bull TV, FIFA+, beIN SPORTS Türkiye |
| **Bilim ve Teknoloji** | 5 | NASA TV, DW Planet A, TeknoSeyir, Hardware Plus (HWP), DonanımHaber |
| **Belgesel** | 2 | TRT Belgesel, DMAX |
| **Çocuk** | 3 | TRT Çocuk, MinikaGO, MinikaÇOCUK |
| **Müzik** | 5 | PowerTürk TV, Power TV, Kral Pop TV, Number1 Türk TV, Number1 TV |
| **Kültür** | 1 | TRT 2 |

> **Not:** `0.1.0-2` (şema `v6`) ile birlikte **Sözcü TV**, **Tele1** ve **Cartoon Network Türkiye** varsayılan listeden ve yerleşik EPG tablosundan kaldırılmıştır. Bazı YouTube tabanlı bilim/teknoloji ve spor kanalları 7/24 canlı yayın yapmadığı için, canlı yayın kapalıyken otomatik olarak ilgili kanalın resmi güncel yayın/video yedeklerine geçilir.

## 3. YouTube Tabanlı Kanallar: Neden `yt-dlp`, `deno` ve `ffmpeg` Gerekir?

`zethropol-tv` Arch Linux paketinde (`0.1.0-2` itibarıyla) **`yt-dlp`**, **`deno`** ve **`ffmpeg`** doğrudan zorunlu çalışma zamanı bağımlılıkları (`depends`) olarak tanımlanmıştır. Böylece son kullanıcı `zethropol-tv` paketini kurduğunda bu bileşenler otomatik olarak yüklenir.

Her bileşenin üstlendiği teknik rol şöyledir:

| Paket | Rolü | Neden Gerekli? |
|---|---|---|
| **`yt-dlp`** | YouTube akış çözümleyicisi | `libmpv` güvenlik gereği `ytdl=no` kilidiyle çalıştığı için YouTube canlı (`/live`) ve kanal (`/streams`, `/videos`) sayfalarından geçerli master HLS (`hls_variant` / `.m3u8`) akış URL'si yalnızca doğrulanmış YouTube alan adları için kabuksuz `QProcess` üzerinden `yt-dlp` ile çözülür. |
| **`deno`** | JavaScript sınama (*JS challenge*) çözücüsü | YouTube, yayın URL'lerini üretirken dinamik JavaScript imza ve *n-challenge* doğrulamaları uygular. `yt-dlp --js-runtimes deno` çağrısı bu JavaScript doğrulamalarını çözerek 403/imza hatalarını önler. |
| **`ffmpeg`** | Medya biçim/akış yardımcısı | Yüksek kaliteli ayrık video/ses akışlarının çözümlenmesi ve `yt-dlp` HLS/akış işlem hattının tam uyumluluğu için gereklidir. |

### Uygulama İçi Akıllı Uyarılar

YouTube tabanlı bir kanal açılırken sorun yaşanırsa Zethropol TV genel bir hata yerine doğrudan sorunun kaynağına işaret eden Türkçe bir uyarı gösterir:

- **Sistemde `yt-dlp`, `deno` veya `ffmpeg` eksikse:**
  > `YouTube tabanlı kanalların oynatılması için sistemde yt-dlp, deno ve ffmpeg paketlerinin kurulu olması gerekir.`
- **Üç bileşen de kurulu olduğu hâlde YouTube akışı çözülemiyorsa:**
  > `YouTube yayın çözücüsü güncelliğini yitirmiş olabilir; sistem güncellemelerini (yt-dlp) denetleyin.`

### Güncelleme ve Bakım Önerisi

YouTube, oynatıcı betiklerini ve akış imza mekanizmalarını dönemsel olarak günceller. YouTube tabanlı kanallarda çözümleme hatası görürseniz **Zethropol Control Center -> Updates** bölümünden veya terminalde sistem güncellemelerini (`sudo pacman -Syu`) uygulayarak özellikle **`yt-dlp`** ve **`deno`** paketlerinin güncel sürümde olduğundan emin olun.
