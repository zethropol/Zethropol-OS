# Bütünleşik Zethropol OS + ZCI Yol Haritası — İlerleme Kaydı

- **Güncelleme Tarihi:** 2026-11-27
- **Aktif Referans Taban:** `Zethropol Control Center 0.1.0-3` + Aşama 04.7 canlı dağıtımı (`zethropol-hardware 0.1.0-5`, `zethropol-monitoring 0.1.0-3`, `zethropol-system-monitor 0.1.0-1`) + 6 kabul edilmiş çekirdek servis (`diagnostics 0.1.0-3`, `performance 0.1.0-2`, `power 0.1.0-2`, `recovery 0.1.0-2`, `security 0.1.0-2`, `services 0.1.0-2`) + bağımsız `zethropol-tv 0.1.0-1`
- **Not:** Mevcut `docs/DEVELOPMENT_STAGES.md` belgesine dokunulmamıştır; bu dosya bütünleşik geliştirme sırası, güncel aşama durumları ve onaylı mimari kararlar için aktif ilerleme kaydıdır.

---

## 1. Ana Geliştirme Sırası ve Güncel Durum

| Aşama | Kapsam | Durum | Referans / Not |
|---|---|---|---|
| **01–02** | Sistem Kimliği, Temel Mimari, D-Bus/Polkit ve Güvenlik İlkeleri | `COMPLETED` | `docs/ARCHITECTURE.md`, `docs/SECURITY.md`, `docs/SERVICES.md` |
| **03 / Aşama F** | **Zethropol Control Center (`0.1.0-3`) ve Çekirdek Servis Kabulü** | **`COMPLETED / ACCEPTED`** | Kabul edilmiş güvenli işlev kapsamıyla resmen kapatıldı (`docs/CONTROL_CENTER_CLOSURE_0.1.0-3.md`). |
| **04 (04.6 + 04.7)** | **Zethropol System Monitor (5 podlu yapı) + `Hardware 0.1.0-4` + `Monitoring 0.1.0-3`** | **`COMPLETED / DEPLOYED LIVE`** | 2026-11-26 tarihinde canlıya alındı; 2026-11-27 tarihinde paket bütünlüğü (`pacman -Qkk`: 0 değiştirilmiş), sistem KPackage çözümlemesi ve kullanıcı yedek özetleri doğrulanarak kapatıldı (`docs/STAGE_04_SYSTEM_MONITOR_CLOSURE.md`). |
| **Bağımsız Uygulama** | **Zethropol TV (`0.1.0-1` Linux + Win7 Portable)** | **`DEPLOYED LIVE`** | ZCI ve sistem D-Bus servislerinden bağımsız (`packages/zethropol-tv/README.md`, `pacman -Qkk`: 17 dosya, 0 değiştirilmiş). |
| **Kapanış Dilimi 1 & 2** | `P0`, `P1`, `P2A-Live`, `P2B`, `P3B`, `P4A`, `P4B` | **`COMPLETED / COMMITTED (63ac8a8)`** | `zethropol-performance 0.1.0-2` ve `zethropol-hardware 0.1.0-5` canlıda aktif ve doğrulandı; `P2B` kod temizliği, `P3B` legacy kapanışı ve `P4A` arşivleme yerel commit'e alındı. |
| **Kapanış Dilimi 3** | `P3A` (Gemma 4 E4B `9/9` + İzole WebUI + Taşınabilir Yol/Log Temizliği) ve `P4C` (`Performance` `GetTelemetry` Geçişi) | **`COMPLETED / VERIFIED`** | Gemma 4 E4B `gemma-system.md` ile **9/9 tam kabul** + izole Firefox WebUI **PASS**; `0` sabit `/home/Zevor` yolu; `services/performance` `GetTelemetry` **10/10 Rust testi PASS**. |
| **Sonraki Fazlar** | `P3C` (Canlı WebUI `8088` geçişi), `P5` (`NOT VERIFIED` KVM/QEMU Btrfs+Limine VM Matrisi), `P6` (Aşama 7: Yerel Repo/ISO) ve `P7` (Aşama 8: Çoklu Donanım) | `OPEN / PLANNED` | Canlı servis geçişi (`P3C`) ve kaynakta hazır paket yükseltmeleri ayrı canlı onay bekler; Aşama 8 dış donanım bekler. |

---

## 2. Tamamlanan Kapanış Dilimleri (2026-11-27)

1. **`P0` — Aşama 04.7 Kapanışı ve Belge Eşitlemesi (`CLOSED`):**
   - `docs/STAGE_04_SYSTEM_MONITOR_CLOSURE.md` oluşturuldu; `packages/stage-04.6/STAGE_04.7_DEPLOYMENT_AND_MIGRATION_PLAN.md` ve ilgili paket README'leri canlı kurulu sürümlerle eşitlendi.
2. **`P1` — `UpdateInstallerTest` Zamanlama Yarışı Kapanışı (`CLOSED`):**
   - `control_center_update_installer_test` hedefi güncel kaynaktan derlenip offscreen ortamda **20/20 ardışık PASS** (`23/23` senaryo/tur, `317,00 sn`, `0` hata) vererek kapatıldı. `m_liveExecutionEnabled = false` kilidi korundu.
3. **`P2A` + `P2A-Live` — `zethropol-performance 0.1.0-2` Güvenlik Borcu Kapanışı (`DEPLOYED LIVE / VERIFIED`):**
   - `services/performance` birim testleri **6/6 PASS**, root'suz `makepkg` **PASS**, ELF `/home/Zevor` sızıntısı **`0`**.
   - Canlıya yükseltildi (`pacman -Qkk`: `18` dosya, `0` değiştirilmiş), `99-zethropol-quarantine.conf` `backups/performance-quarantine-backup/` altına yedeklenerek kaldırıldı, `<allow own>` yalnızca `user="root"` altında doğrulandı.
   - `systemctl is-active zethropol-performance.service`: **`active`**; `busctl GetState`: `success=true`, `code="ok"` (`18` canlı metrik); `systemd-analyze security`: **`3.6 OK :-)`**.
4. **`P2B` — Kod İçi Karantina Sabitlerinin Temizlenmesi (`CLOSED IN SOURCE`):**
   - `services/services/src/services_service.rs` (`QUARANTINED_SERVICES` boşaltıldı; `cargo test --offline --locked`: **5/5 PASS**) ve `desktop/control-center/src/ServiceService.cpp` + `SecurityRemediationTest.cpp` (`control_center`, `control_center_security_remediation_test`, `control_center_tabs_test`: **2/2 hedef PASS**) güncellendi. (Canlı `zethropol-services`/`zethropol-control-center` paket yükseltmesi ayrı onay penceresine bırakıldı.)
5. **`P3B` — 5 Açık cgroup Testinin Kaynak Tespiti ve Legacy Kapanışı (`CLOSED — LEGACY SCOPE`):**
   - Kaynak doğrulandı: `/home/Zevor/.local/share/zci/verification-planner/test_executor.py` (`LiveDelegatedTests`: `test_live_success`, `test_live_wrong_stdout`, `test_live_wrong_exit`, `test_live_timeout`, `test_live_isolation_failure`).
   - Aşama `1.10.4` ile varsayılan yürütücü `--backend systemd` (geçici kullanıcı servisi, `Delegate=no`, `test_systemd_live.py`) aktif olduğundan, açık `ZCI_TEST_CGROUP_PARENT` delegasyonu gerektiren bu 5 eski `--backend direct` testi **legacy kapsam** olarak kapatıldı.
6. **`P4A` — `services/applications` Prototipinin Arşivlenmesi (`CLOSED`):**
   - Tamamlanmamış `services/applications/` prototipi `prototypes/archived-applications/` altına taşındı (`README.md` + `docs/SERVICE_INTERFACE_MODEL.md` durum notu eklendi). Kanonik uygulama kaldırma akışı `desktop/control-center/src/ApplicationService.cpp` (`pkexec /usr/bin/pacman -R --`) olarak korundu.
7. **`P4B` — `diagnostics-helper` Sertleştirme ve `/usr/lib/zethropol/` Paketlemesi (`DEPLOYED LIVE / VERIFIED`):**
   - `services/hardware/diagnostics-helper/src/main.rs` bütün blok aygıt doğrulaması (`nvmeXnY`, `sdX`, `vdX`, `mmcblkX`), symlink/non-block reddi, temiz ortam ve `--` ayırıcılı `smartctl` çağrısı ile sertleştirildi; Polkit yolu `/usr/lib/zethropol/zethropol-diagnostics-helper` olarak düzeltildi.
   - `packages/zethropol-hardware` `0.1.0-5` paketi root'suz derlenip canlıya kuruldu (`hardware` 37/37 + `dbus_disconnect` 1/1 + `diagnostics-helper` 4/4 = **42/42 PASS**, iki ikilide `0` `/home/Zevor` sızıntısı, `.pkg.tar.zst` SHA-256 `0c89ecbedcaf848f529407d19aaa609f30dc4819300c727fc3fb140912092c11`).
   - Canlı doğrulama: `pacman -Q zethropol-hardware` = **`0.1.0-5`**, `pacman -Qkk` = **`22` dosya / `0` değiştirilmiş**, `/usr/lib/zethropol/zethropol-diagnostics-helper` + `/usr/share/polkit-1/actions/org.zethropol.diagnostics.helper.policy` (`exec.path` eşleşiyor), root'suz geçersiz girdi testleri (`--scan`, `/etc/shadow` -> çıkış `2`) ve canlı `busctl GetTelemetry` (`success=true`, `code="ok"`, 5 bölüm) **PASS**.
8. **`P3A` — ZCI / ZEKANS Taşınabilir Yol & Geçici Dizin Temizliği ve Gemma 4 E4B `9/9` Türkçe Kabulü (`COMPLETED / VERIFIED`):**
   - `services/local-inference/shared_gemma.py`, `services/zci-chat/{live_acceptance.py,zci_chat.py}` ve `services/zekans-code/{profile.json,zekans_code.py}` içindeki tüm sabit `/home/Zevor` yolları `Path.home()` / `~` genişletmesine taşındı (`grep -rn '/home/Zevor'`: **`0` eşleşme**); varsayılan geçici `/tmp` dizin temizliği (`--keep-logs` ile korunabilir) eklendi.
   - Birim testleri: `services/local-inference` **22/22 PASS**, `services/zci-chat` **27/27 PASS** (+ `tr-v1/validate.py` SHA-256 `5056cbdd...`), `services/zekans-code` **11/11 PASS** (`AdapterTests` dahil, `0` atlama).
   - Gerçek Gemma 4 E4B kabulü (`services/zci-chat/evaluation-gemma4-20261127/REPORT.md` & `review.json`): dondurulmuş `tr-v1/acceptance.json` üzerinde `gemma-system.md` ile **9/9 tam başarı**, **9/9 anlamsal başarı**, **3/3 exact**, **0/0 kritik uydurma** (medyan `6,061 sn`, `10,48 tok/sn`, tepe RSS `7,509 GiB`, en düşük kullanılabilir RAM `24,017 GiB`).
   - İzole Open WebUI + headless Firefox kabulü (`gemma_webui_acceptance.py --run`, `webui-result.json`): `real_user_database_opened: false`, `actual_firefox_ui: true`, `sse_bubble: true`, `reasoning_control_tokens_absent: true`, `owned_engine_unavailable_error_visible: true`, `default_unchanged: true`, tüm süreçler temiz kapandı (`PASS`).
9. **`P4C` — `Performance` Servisi `Hardware.v1.GetTelemetry` Geçişi (`CLOSED IN SOURCE / VERIFIED`):**
   - `services/performance/src/dbus.rs` ağır `Hardware.v1.GetState` çağrısı yerine doğrudan sıfır-fork `org.zethropol.Hardware.v1.GetTelemetry` çağrısına geçirildi; mevcut D-Bus bağlantısının yeniden kullanımı (`#[zbus(connection)]`), `CacheProperties::No`, zarf doğrulaması (`success`/`code`/`data`), tamsayı->`f64` dönüşümü ve `NaN`/sonsuz değer süzme eklendi. `services/performance/src/main.rs` `connection.closed().await` ile systemd kurtarma çıkışına geçirildi.
   - Doğrulama: `cargo test --offline --locked` -> **10/10 PASS** (özel `dbus-daemon` testi `GetTelemetry == 1` ve ağır `GetState == 0` çağrısını kanıtladı).

---

## 3. Açık Takip Kayıtları

1. **Canlı Paket Yükseltme Kuyruğu (İzole Paketler Üretildi & Doğrulandı, Canlı `pacman -U` Onayı Bekler):**
   - `zethropol-performance 0.1.0-3` (`P4C` `GetTelemetry` + bağlantı kopma kurtarması; `10/10` Rust testi, `0` `/home/Zevor` sızıntısı, `.pkg.tar.zst` SHA-256 `85bed4d2c34dc33b46aa32fd700016b15c8edc03cb08a969d9d5107ed6410830`; canlıda `0.1.0-2`).
   - `zethropol-services 0.1.0-3` (`P2B` karantina temizliği + `--remap-path-prefix`; `5/5` Rust testi, `0` `/home/Zevor` sızıntısı, `.pkg.tar.zst` SHA-256 `d4255ef7122b9f4067390875fa729e8189695fb031b203083510a984d3375fe3`; canlıda `0.1.0-2`).
   - `zethropol-control-center 0.1.0-4` (`P2B` karantina temizliği + `m_liveExecutionEnabled=false` korunarak; `6/6` QTest hedefi / `62/62` senaryo PASS, `0` `/home/Zevor` sızıntısı, `.pkg.tar.zst` SHA-256 `8ae82fcc4c604c6acf2f47d72f356dc70fcde28c4cc897f26e4442febdddfdf7`; canlıda `0.1.0-3`).
2. **ZCI / ZEKANS Takip Kaydı (`P3C` — Canlı `8088` WebUI Ön Kontrolü Tamamlandı):**
   - `P3A` commit edildi (`203286c`). Ön kontrol bulgusu: `~/.local/share/zci/{start.sh,start-chat.py}` ve `~/.local/bin/zci-code` başlatıcıları 2026-11-16 tarihinde `shared_gemma.py` (`19086`/`29016`) üzerine geçirilmiş ve `backups/gemma-live-transition-20261116` yedeği alınmış durumda; şu an hiçbir ZCI portu (`8087`/`8088`/`19086`/`29016`) dinlemiyor ve `~/.local/share/zci/webui/{documents,model,webui}.pid` bayat PID dosyaları mevcut. `webui/settings.json` ve `webui/data/webui.db` gizlilik sınırı gereği okunmadı; canlı `8088` doğrulaması ve olası ayar/başlatıcı güncellemesi açık kullanıcı onayı bekler.
3. **İsteğe Bağlı Depo Temizliği (Düşük Öncelikli Mimari Bakım):**
   - `Performance` ile `Power` servisi rol sınırı değerlendirmesi ve kök dizin artıklarının (`src/main.rs`, boş `desktop/control-center/src/core/`) gözden geçirilmesi.
4. **Control Center Kilitli / `NOT VERIFIED` Kapsamı (`P5` — İzole KVM/QEMU Btrfs + Limine VM):**
   - Canlı masaüstündeki `m_liveExecutionEnabled=false`, `rollback_execution_enabled=false` ve `repair_execution_enabled=false` kilitleri korunur; 7 canlı durum değiştiren işlem ile `docs/RECOVERY_FAILURE_TEST_MATRIX.md` (`/boot umask=0077`, `snap-pac`, Limine snapshot boot) doğrulamaları izole **KVM/QEMU Btrfs + Limine VM** üzerinde yürütülecektir.
   - Ortam ön kontrolü (2026-11-27): `/dev/kvm`, `btrfs`, `limine` ve `mkarchiso` **mevcut**; **`qemu-system-x86_64`, `qemu-img` ve `edk2-ovmf` (`/usr/share/edk2/x64`) eksik** (tek komutluk kurulum: `sudo pacman -S --needed qemu-base edk2-ovmf`, isteğe bağlı `swtpm`).
5. **Kurulum ve ISO Altyapısı (`P6` / Aşama 7) ve Çoklu Donanım (`P7` / Aşama 8):**
   - Tekrarlanabilir yerel paket deposu, `build/recovery/` ArchISO güncellemesi + QEMU/OVMF testi (`P6`) ve dış donanım bağımlı çoklu donanım doğrulaması (`P7`) sonraki faz kayıtlarıdır.
