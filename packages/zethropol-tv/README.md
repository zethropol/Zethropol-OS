# Zethropol TV Arch Package (`0.1.0-1`)

Bu dizin, ZCI ve sistem D-Bus servislerinden tamamen bağımsız **Zethropol TV** (`Qt6/QML` + `libmpv`) canlı TV oynatıcısı için Arch Linux paket tarifini içerir.

> **Durum:** Paket tarifi (`PKGBUILD`, `.SRCINFO`) yalnızca izole geçici bir derleme dizininde root kullanmadan doğrulanmıştır; canlı sisteme kurulmamıştır. Canlı `pacman -U` kurulumu için ayrı kullanıcı onayı gerekir.

## Paket Üst Verileri ve Kurulum İçeriği

- **Paket:** `zethropol-tv 0.1.0-1` (`x86_64`, lisans: `MIT`)
- **Çalışma Zamanı Bağımlılıkları (`depends`):** `qt6-base`, `qt6-declarative`, `mpv`
- **Derleme Bağımlılıkları (`makedepends`):** `cmake`, `ninja`, `pkgconf`
- **İsteğe Bağlı (`optdepends`):** `plasma-integration`
- **ZCI / D-Bus / Polkit Bağımsızlığı:** Hiçbir ZCI bileşeni, systemd servisi, D-Bus kuralı veya Polkit politikası içermez.
- **Kurulum Dosyaları (`DESTDIR="$pkgdir" cmake --install`):**
  - `/usr/bin/zethropol-tv` (QML modülü ve pencere ikonu gömülü bağımsız ikili)
  - `/usr/share/applications/org.zethropol.tv.desktop`
  - `/usr/share/icons/hicolor/256x256/apps/zethropol-tv.png`
  - `/usr/share/icons/hicolor/512x512/apps/zethropol-tv.png`
  - `/usr/share/licenses/zethropol-tv/LICENSE`

## Deterministik Kaynak Arşivi Üretimi (Fish Uyumlu)

Kaynak arşivi (`zethropol-tv-0.1.0-1-src.tar.gz`) çalışma ağacında saklanmaz; depo kökünden izole bir geçici derleme dizininde aşağıdaki komutlarla deterministik olarak üretilebilir (SHA-256: `961c7a45f707adc27356bae0590fabaa47f8f69d5617fdb0df54b34e415fffe4`):

```fish
set BUILD_STAGE (mktemp -d)
mkdir -p "$BUILD_STAGE/stage/zethropol-tv-0.1.0/desktop/tv"
cp LICENSE "$BUILD_STAGE/stage/zethropol-tv-0.1.0/LICENSE"
cp -a desktop/tv/CMakeLists.txt desktop/tv/src desktop/tv/qml desktop/tv/data desktop/tv/tests "$BUILD_STAGE/stage/zethropol-tv-0.1.0/desktop/tv/"
tar --sort=name --mtime='UTC 2026-01-01' --owner=0 --group=0 --numeric-owner -C "$BUILD_STAGE/stage" -cf - zethropol-tv-0.1.0 | gzip -n > "$BUILD_STAGE/zethropol-tv-0.1.0-1-src.tar.gz"
cp packages/zethropol-tv/PKGBUILD "$BUILD_STAGE/PKGBUILD"
```

## İzole Doğrulama Özeti (Root'suz — Faz 2.3 + Özel İkon)

- **Varsayılan Kanal Listesi & EPG:** Yalnızca Türkçe yayın diline sahip 31 kanallı liste (`10 Ulusal, 12 Haber, 2 Spor, 2 Belgesel, 4 Çocuk, 1 Kültür`; Faz 2.3 ile eklenen Ülke TV, Kanal 24, DMAX, Cartoon Network Türkiye [yalnızca resmi yayıncı — ham IP yok], MinikaGO ve MinikaÇOCUK dahil), çoklu yedek URL zinciri (`fallbackUrls`), Turkuvaz (`securevideotoken`) & Demirören (`/action/media/`) resmi API çözümleyicileri, YouTube `/@.../live` `hlsManifestUrl` desteği, güvenli `Referer`/`User-Agent` aktarımı ve `state.json` v1/v2/v3/v4 → v5 otomatik yükseltmesi.
- **Ses Çıkışı (PipeWire/PulseAudio/ALSA):** `MpvItem` üzerinde açık `ao=pipewire,pulse,alsa`, `audio-client-name=Zethropol TV`, `aid=auto`, `alang=tr,tur,en`, `mute=no`, başlatma/kanal geçişi ses senkronizasyonu ve OSD ses teşhis göstergesi.
- `bash -n packages/zethropol-tv/PKGBUILD`: **PASS**
- `makepkg --printsrcinfo`: `.SRCINFO` birebir doğrulandı (**PASS**)
- `makepkg --nobuild` (izole geçici derleme dizininde): bağımlılıklar, deterministik SHA-256 (`961c7a45f707adc27356bae0590fabaa47f8f69d5617fdb0df54b34e415fffe4`) ve `bsdtar` açılımı **PASS**
- `ctest --test-dir desktop/tv/build --output-on-failure -V`: **4/4 paket (34/34 test) PASS**
- Geçici derleme dizini doğrulama sonunda tamamen temizlendi.
