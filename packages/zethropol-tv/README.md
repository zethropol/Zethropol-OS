# Zethropol TV Arch Package (`0.1.0-2`)

Bu dizin, ZCI ve sistem D-Bus servislerinden tamamen bağımsız **Zethropol TV** (`Qt6/QML` + `libmpv`) canlı TV oynatıcısı için Arch Linux paket tarifini içerir.

> **Durum:** `zethropol-tv 0.1.0-1` canlı sistemde kuruludur; `zethropol-tv 0.1.0-2` paketi root kullanmadan derlenip test edilerek yerel `pacman` deposuna (`packages/repo/`) işlenmiştir. Canlı sistem yükseltmesi (`sudo pacman -U`) ayrı kullanıcı onayına tabidir.

## Paket Üst Verileri ve Kurulum İçeriği

- **Paket:** `zethropol-tv 0.1.0-2` (`x86_64`, lisans: `MIT`)
- **Çalışma Zamanı Bağımlılıkları (`depends`):** `qt6-base`, `qt6-declarative`, `mpv`, `yt-dlp`, `deno`, `ffmpeg`
- **Derleme Bağımlılıkları (`makedepends`):** `cmake`, `ninja`, `pkgconf`
- **İsteğe Bağlı (`optdepends`):** `plasma-integration`
- **ZCI / D-Bus / Polkit Bağımsızlığı:** Hiçbir ZCI bileşeni, systemd servisi, D-Bus kuralı veya Polkit politikası içermez.
- **Kurulum Dosyaları (`DESTDIR="$pkgdir" cmake --install`):**
  - `/usr/bin/zethropol-tv` (QML modülü ve pencere ikonu gömülü bağımsız ikili)
  - `/usr/share/applications/org.zethropol.tv.desktop`
  - `/usr/share/icons/hicolor/256x256/apps/zethropol-tv.png`
  - `/usr/share/icons/hicolor/512x512/apps/zethropol-tv.png`
  - `/usr/share/licenses/zethropol-tv/LICENSE`

## YouTube Tabanlı Kanallar ve Bağımlılık Bakımı

`0.1.0-2` sürümüyle birlikte **Bilim ve Teknoloji**, **Spor** ve **Müzik** kategorilerine resmi HLS (`.m3u8`) ve resmi YouTube canlı/kanal yayınları eklendi. Son kullanıcı kurulumlarında YouTube tabanlı kanalların sorunsuz çalışması için `PKGBUILD` içindeki `depends` dizisine şu üç paket eklendi:

- **`yt-dlp`:** Gömülü `libmpv` güvenlik kilitleri (`load-scripts=no`, `ytdl=no`, `load-unsafe-playlists=no`) korunurken, yalnızca doğrulanmış YouTube alan adları için kabuksuz `QProcess` üzerinden geçerli HLS (`hls_variant`/`.m3u8`) yayın adresini çözer.
- **`deno`:** YouTube'un akış URL'lerini koruyan JavaScript sınamalarını (*JS challenge* / imza doğrulaması) çözmek için `yt-dlp --js-runtimes deno` çalışma zamanı olarak kullanılır.
- **`ffmpeg`:** Ayrık video/ses akış profilleri ve HLS akış çözümleme uyumluluğu için gereklidir.

**Uygulama içi uyarılar ve bakım:**
- Sistemde `yt-dlp`, `deno` veya `ffmpeg` eksikse uygulama kullanıcıya eksik bağımlılık uyarısı gösterir.
- Üç bileşen kurulu olduğu hâlde YouTube akışı çözülemiyorsa YouTube tarafındaki oynatıcı değişiklikleri nedeniyle çözücü güncelliğini yitirmiş olabilir; sistem güncellemelerini (`sudo pacman -Syu` veya **Zethropol Control Center -> Updates**) çalıştırarak **`yt-dlp`** paketini güncel tutmanız önerilir.
- Ayrıntılı kullanıcı ve mimari rehberi için `docs/ZETHROPOL_TV.md` belgesine bakın.

## Deterministik Kaynak Arşivi Üretimi (Fish Uyumlu)

Kaynak arşivi (`zethropol-tv-0.1.0-2-src.tar.gz`) çalışma ağacında saklanmaz; depo kökünden aşağıdaki komutlarla deterministik olarak üretilebilir (SHA-256: `b5e00dcd3a722a8759e93f779d99da000a0834bc515d2d61c5c9f775f0f2defa`):

```fish
set BUILD_STAGE (mktemp -d)
mkdir -p "$BUILD_STAGE/stage/zethropol-tv-0.1.0/desktop/tv"
cp LICENSE "$BUILD_STAGE/stage/zethropol-tv-0.1.0/LICENSE"
cp -a desktop/tv/CMakeLists.txt desktop/tv/src desktop/tv/qml desktop/tv/data desktop/tv/tests "$BUILD_STAGE/stage/zethropol-tv-0.1.0/desktop/tv/"
tar --sort=name --mtime='UTC 2026-01-01' --owner=0 --group=0 --numeric-owner -C "$BUILD_STAGE/stage" -cf - zethropol-tv-0.1.0 | gzip -n > "$BUILD_STAGE/zethropol-tv-0.1.0-2-src.tar.gz"
cp packages/zethropol-tv/PKGBUILD "$BUILD_STAGE/PKGBUILD"
```

## İzole Doğrulama Özeti (Root'suz — `0.1.0-2`)

- **Varsayılan Kanal Listesi & EPG (`v6` — 41 Kanal):** `10 Ulusal, 10 Haber, 5 Spor, 5 Bilim ve Teknoloji, 2 Belgesel, 3 Çocuk, 5 Müzik, 1 Kültür`. **Sözcü TV**, **Tele1** ve **Cartoon Network Türkiye** kaldırıldı; **NASA TV**, **DW Planet A**, **TeknoSeyir**, **Hardware Plus (HWP)**, **DonanımHaber**, **Red Bull TV**, **FIFA+**, **beIN SPORTS Türkiye**, **PowerTürk TV**, **Power TV**, **Kral Pop TV**, **Number1 Türk TV** ve **Number1 TV** eklendi. `state.json` v1–v5 → v6 otomatik yükseltmesi kullanıcı özel kanallarını ve favorilerini korur.
- **Ses Çıkışı (PipeWire/PulseAudio/ALSA):** `MpvItem` üzerinde açık `ao=pipewire,pulse,alsa`, `audio-client-name=Zethropol TV`, `aid=auto`, `alang=tr,tur,en`, `mute=no` ve güvenlik kilitleri (`ytdl=no`, `load-scripts=no`, `load-unsafe-playlists=no`) korunur.
- `bash -n packages/zethropol-tv/PKGBUILD` ve `makepkg --printsrcinfo`: **PASS**
- `makepkg --clean --force` (`build()` + `check()` 4/4 hedef, 37/37 QTest + `package()`): **PASS**
- ELF `/home/Zevor` sızıntı kontrolü (`ELF_HOME_LEAKS=0`) ve `RPATH`/`RUNPATH` (`0`): **PASS**
