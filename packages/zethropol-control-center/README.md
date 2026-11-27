# Zethropol Control Center Package (`0.1.0-5` İzole Hazırlık — Kurulu Taban: `0.1.0-4`)

> **Güncel Durum (2026-11-28):** Canlı sistemde kurulu referans sürüm **`zethropol-control-center 0.1.0-4`** paketidir (`pacman -Qkk`: `23` dosya, `0` değiştirilmiş). Yeni **`zethropol-control-center 0.1.0-5`** paketi (Aşama 06 / Dilim 6B: ayrıcalıksız `CompatibilityManager` + `CompatibilityPage.qml` 13. sekmesi, `optdepends` altında `zethropol-compatibility` ve korunan `m_liveExecutionEnabled=false` kilidi) root'suz `makepkg --clean --force` ile **izole olarak derlenip doğrulandı** (`7/7` QTest hedefi, toplam `69/69` senaryo PASS, `24` paket girdisi, ELF `/home/Zevor` sızıntısı `0`, `RPATH`/`RUNPATH` yok, `desktop-file-validate` PASS, `.pkg.tar.zst` SHA-256 `e02ea52132961d10920ae932d9727b6b3c46d1bb34b10f7af5edb835950564b8`); canlı `sudo pacman -U` kurulumu ayrı onay bekler. Aşağıdaki bölüm tarihsel `0.1.0-2` (03.2.17) paket hazırlık kaydını korur.

Bu dizin, kurulu `zethropol-control-center 0.1.0-1` paketine dokunmadan 03.2.17 kapsamında kabul edilen **7 Control Center dosyasından** root'suz olarak hazırlanan `zethropol-control-center 0.1.0-2` paketini ve doğrulama kayıtlarını içerir.

> **Durum:** Paket yalnızca hazırlanmış ve izole testlerden geçirilmiştir; sisteme kurulmamıştır. Canlı kurulum (`pacman -U`) için ayrı kullanıcı onayı gerekir. 03.2.17 açıktır; 03.2.18'e geçilmemiştir.

## Kaynak ve Sürüm İzlenebilirliği

- **Paket:** `zethropol-control-center 0.1.0-2` (`x86_64`, `vercmp 0.1.0-2 0.1.0-1` = `1`)
- **Taban Commit:** `4888e47` (`LICENSE` + `desktop/control-center/` izlenen dosyaları)
- **Eklenen 03.2.17 Değişiklikleri (yalnızca 7 dosya):**
  1. `desktop/control-center/qml/Main.qml`
  2. `desktop/control-center/qml/SystemState.qml`
  3. `desktop/control-center/src/HardwareServiceClient.h`
  4. `desktop/control-center/src/HardwareServiceClient.cpp`
  5. `desktop/control-center/src/core/zethropol_power_client.h`
  6. `desktop/control-center/src/core/zethropol_power_client.cpp`
  7. `desktop/control-center/tests/HardwareTest.cpp`
- **Hariç Tutulan ve Çalışma Ağacında Korunanlar:**
  - `desktop/control-center/zethropol-control-center.desktop` çalışma ağacı değişikliği (`#!/usr/bin/env xdg-open`, `0755`) pakete alınmadı; pakette `HEAD` (`4888e47`) sürümü (`0644`, SHA-256 `b194b2d8ac3ef04329418a8d637db1e22b75a2dc90ac4c3b1b584e5d8c5b20b1`) kullanıldı.
  - `desktop/control-center/AGENTS.md`, `*.plan-preview.bak` yedekleri, `desktop/control-center/build/` ve diğer modül/Git değişiklikleri pakete alınmadı ve korundu.
- **Kaynak Arşivi:** `zethropol-control-center-0.1.0-2-src.tar.gz` (deterministik, SHA-256: `ca97e4bf6b48be4ec55dab6415b32b4ed7ce29d3593077b1473bb3e868afcea9`, `PKGBUILD` içinde sabitli)
- **Paket Arşivi:** `zethropol-control-center-0.1.0-2-x86_64.pkg.tar.zst` (423044 bayt, SHA-256: `bf36e274f010720f08ddb68f6848e8a42c05c21d2eca2f2e1aa5e95661e4ceb5`)

## Paket İçeriği (Kurulu `0.1.0-1` ile Birebir 23 Girdi)

- `/usr/bin/control_center` (stripped PIE ELF, `RPATH`/`RUNPATH` yok, `/home/Zevor` sızıntısı yok)
- `/usr/bin/zethropol-control-center -> control_center`
- `/usr/lib/qt6/qml/Zethropol/ControlCenter/control_center.qmltypes`
- `/usr/lib/qt6/qml/Zethropol/ControlCenter/qmldir`
- `/usr/lib/qt6/qml/Zethropol/ControlCenter/qml/{HardwarePage,Main,ProcessesPage,SystemState,UpdateReport}.qml`
- `/usr/share/applications/zethropol-control-center.desktop` (`desktop-file-validate` geçti)
- `/usr/share/licenses/zethropol-control-center/LICENSE`

Bağımlılıklar (`13 depends`, `14 optdepends`, `Kurulum Betiği: Hayır`) kurulu `0.1.0-1` ile birebir korunmuştur.

## İzole Doğrulama Sonuçları (Root'suz)

- `makepkg --clean` (`BUILDDIR=/tmp/zethropol-cc-makepkg-0.1.0-2`):
  - `UpdateScannerTest`: **10/10**
  - `ProcessServiceTest`: **7/7**
  - `TabRegressionTest` (özel D-Bus + offscreen Qt): **5/5**
  - `HardwareTest` (hedefli 5 test + init/cleanup, özel D-Bus + offscreen Qt): **7/7**
- Çıkarılan paket ikilisi (`/usr/bin/zethropol-control-center`) headless smoke testi (özel D-Bus, boş `PATH`, `QT_QPA_PLATFORM=offscreen`): QML hatasız başlatma ve temiz `SIGTERM` kapanışı.
- Genişletilmiş izole CTest doğrulaması (`/tmp` üzerinde):
  - `HardwareTest` (`ZETHROPOL_HARDWARE_SERVICE_BINARY=/usr/lib/zethropol/zethropol-hardware`): **8/8** (`realServiceAndAllTabs`, `systemInfoNormalizationAndFormatting`, `powerClientStateChangedSignal` dahil)
  - `RecoveryTest` (`ZETHROPOL_RECOVERY_SERVICE_BINARY=/usr/lib/zethropol/zethropol-recovery`): **35/35**
  - `UpdateInstallerTest` (gözlemsel, kapsam dışı): bu koşuda **22/22** geçti; ancak aşağıdaki aralıklı zamanlama yarışı ayrı takip bulgusu olarak korunmaktadır.

## Ayrı Takip Bulgusu — `UpdateInstaller` Zamanlama Yarışı

- **Konum:** `desktop/control-center/tests/UpdateInstallerTest.cpp:624-632` (`aurAndFlatpakErrorsAndIncompleteScansAreClassifiedSafely`), `desktop/control-center/src/UpdateInstaller.cpp:506-507` (`finish()` içindeki `m_scanner->scan()`), `desktop/control-center/src/UpdateScanner.cpp:62-63` (`if (m_busy || m_installationRunning) return;`).
- **Kanıt:** Önceki `desktop/control-center/build/Testing/Temporary/LastTest.log:60-63` koşusunda `Actual (scanned.count()): 4, Expected (5): 5` hatası gözlenmiştir; izole koşuda 22/22 geçmesi zamanlamaya bağlı aralıklı yarış (flaky race) niteliğini doğrulamaktadır.
- **Kök Neden:** `UpdateInstaller::finish()` sinyali (`sessionFinished()`) yaymadan hemen önce asenkron `m_scanner->scan()` başlatmakta; test ise 3. oturum bitiminde (`finished.count() == 3`) bu taramanın tamamlanmasını (`scanned.count() == 4` / `!scanner.busy()`) beklemeden 631. satırda yeniden `scanner.scan()` çağırmaktadır. Tarama hâlâ `m_busy` durumundaysa yeni çağrı sessizce düşmekte ve sayaç 4'te kalmaktadır.
- **Kapsam Kararı:** 03.2.17 kapsamı dışındadır; kod değişikliği yapılmamıştır. Ayrı bir takip görevinde ele alınmalıdır.

## Güvenli Kurulum Önkoşulları (İlerideki Onaylı `pacman -U` İçin)

1. **Açık Kurulum Onayı:** `sudo pacman -U packages/zethropol-control-center/zethropol-control-center-0.1.0-2-x86_64.pkg.tar.zst` yalnızca açık kullanıcı onayıyla çalıştırılmalıdır.
2. **Bütünlük ve Üst Veri Kontrolü:** Kurulum öncesi `sha256sum -c packages/zethropol-control-center/SHA256SUMS`, `pacman -Qip` ve `pacman -Qlp` doğrulamaları yapılmalıdır.
3. **Kilit ve Süreç Kontrolü:** `/var/lib/pacman/db.lck` dosyasının mevcut olmadığı ve çalışan `control_center` oturumu bulunmadığı doğrulanmalıdır.
4. **Snapshot ve Geri Dönüş Hazırlığı:**
   - Kurulu `0.1.0-1` paketi geçici `/run/zethropol-pkg-migrate.*` yolundan kurulduğu için özgün `.pkg.tar.zst` arşivi diskte saklı değildir.
   - Snapper 340/341 snapshot'larının ve `snap-pac` kancalarının (`05-snap-pac-pre.hook`, `zz-snap-pac-post.hook`) korunduğu, `SKIP_AUTOSNAP` değişkeninin set edilmediği doğrulanmalıdır. Doğrudan paket downgrade seçeneği de istenirse kurulum öncesi `0.1.0-1` yerel durum geri dönüş paketi ayrıca alınmalıdır.
5. **İzolasyon ve Yan Etki Sınırı:** Bu paket yalnızca Control Center ikilisi, QML modülü, `.desktop` ve `LICENSE` dosyalarını günceller; systemd birimi, D-Bus/Polkit politikası, `/usr/local`, `zethropol-diagnostics` ve Performance karantinası (`/etc/systemd/system/zethropol-performance.service.d/99-zethropol-quarantine.conf`) üzerinde hiçbir değişiklik yapmaz.
6. **Kurulum Sonrası Doğrulama (Onaylı Kurulumdan Sonra):**
   - `pacman -Qi zethropol-control-center` (`0.1.0-2`) ve `pacman -Qkk zethropol-control-center` (`0` değiştirilmiş dosya)
   - Performance karantinası ve Snapper 340/341'in korunduğunun doğrulanması
   - `zethropol-control-center` açılarak System sekmesinde (`Version: Rolling`, `KDE Plasma`, `Plasma Version`, `Session: Wayland`) ve Power sekmesinde QtDBus uyarısız canlı durum güncellemesinin doğrulanması.
