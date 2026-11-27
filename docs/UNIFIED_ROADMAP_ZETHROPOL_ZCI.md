# Bütünleşik Zethropol OS + ZCI Yol Haritası — İlerleme Kaydı

- **Güncelleme Tarihi:** 2026-11-27
- **Aktif Referans Taban:** `Zethropol Control Center 0.1.0-3` + Aşama 04.7 canlı dağıtımı (`zethropol-hardware 0.1.0-4`, `zethropol-monitoring 0.1.0-3`, `zethropol-system-monitor 0.1.0-1`) + 6 kabul edilmiş çekirdek servis (`diagnostics 0.1.0-3`, `performance 0.1.0-2`, `power 0.1.0-2`, `recovery 0.1.0-2`, `security 0.1.0-2`, `services 0.1.0-2`) + bağımsız `zethropol-tv 0.1.0-1`
- **Not:** Mevcut `docs/DEVELOPMENT_STAGES.md` belgesine dokunulmamıştır; bu dosya bütünleşik geliştirme sırası, güncel aşama durumları ve onaylı mimari kararlar için aktif ilerleme kaydıdır.

---

## 1. Ana Geliştirme Sırası ve Güncel Durum

| Aşama | Kapsam | Durum | Referans / Not |
|---|---|---|---|
| **01–02** | Sistem Kimliği, Temel Mimari, D-Bus/Polkit ve Güvenlik İlkeleri | `COMPLETED` | `docs/ARCHITECTURE.md`, `docs/SECURITY.md`, `docs/SERVICES.md` |
| **03 / Aşama F** | **Zethropol Control Center (`0.1.0-3`) ve Çekirdek Servis Kabulü** | **`COMPLETED / ACCEPTED`** | Kabul edilmiş güvenli işlev kapsamıyla resmen kapatıldı (`docs/CONTROL_CENTER_CLOSURE_0.1.0-3.md`). |
| **04 (04.6 + 04.7)** | **Zethropol System Monitor (5 podlu yapı) + `Hardware 0.1.0-4` + `Monitoring 0.1.0-3`** | **`COMPLETED / DEPLOYED LIVE`** | 2026-11-26 tarihinde canlıya alındı; 2026-11-27 tarihinde paket bütünlüğü (`pacman -Qkk`: 0 değiştirilmiş), sistem KPackage çözümlemesi ve kullanıcı yedek özetleri doğrulanarak kapatıldı (`docs/STAGE_04_SYSTEM_MONITOR_CLOSURE.md`). |
| **Bağımsız Uygulama** | **Zethropol TV (`0.1.0-1` Linux + Win7 Portable)** | **`DEPLOYED LIVE`** | ZCI ve sistem D-Bus servislerinden bağımsız (`packages/zethropol-tv/README.md`, `pacman -Qkk`: 17 dosya, 0 değiştirilmiş). |
| **Kapanış Dilimi 1 & 2** | `P0`, `P1`, `P2A-Live`, `P2B`, `P3B`, `P4A`, `P4B` | **`COMPLETED`** | `zethropol-performance 0.1.0-2` canlıda aktif ve karantina kaldırıldı; `P2B` kod temizliği, `P4A` arşivleme ve `P4B` (`zethropol-hardware 0.1.0-5` izole paket) doğrulandı. |
| **Sonraki Fazlar** | `P3A`/`P3C` (ZCI Gemma 4 E4B `9/9` + WebUI `8088`), `P4C` (`Performance` `GetTelemetry` evrimi), `P5` (`NOT VERIFIED` VM Matrisi), `P6` (Aşama 7: Yerel Repo/ISO) ve `P7` (Aşama 8: Çoklu Donanım) | `OPEN / PLANNED` | Dış donanım gerektirmeyen borçlar bağımlılık sırasıyla kapatılır; Aşama 8 dış donanım bekler. |

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
7. **`P4B` — `diagnostics-helper` Sertleştirme ve `/usr/lib/zethropol/` Paketlemesi (`BUILT / AWAITING LIVE APPROVAL`):**
   - `services/hardware/diagnostics-helper/src/main.rs` bütün blok aygıt doğrulaması (`nvmeXnY`, `sdX`, `vdX`, `mmcblkX`), symlink/non-block reddi, temiz ortam ve `--` ayırıcılı `smartctl` çağrısı ile sertleştirildi; Polkit yolu `/usr/lib/zethropol/zethropol-diagnostics-helper` olarak düzeltildi.
   - `packages/zethropol-hardware` `0.1.0-5` paketi root'suz derlenip doğrulandı (`hardware` 37/37 + `dbus_disconnect` 1/1 + `diagnostics-helper` 4/4 = **42/42 PASS**, iki ikilide `0` `/home/Zevor` sızıntısı, `.pkg.tar.zst` SHA-256 `0c89ecbedcaf848f529407d19aaa609f30dc4819300c727fc3fb140912092c11`, `SHA256SUMS` 7/7 Tamam). Canlı kurulum (`0.1.0-4` -> `0.1.0-5`) ayrı onay bekler.

---

## 3. Açık Takip Kayıtları

1. **Canlı Paket Yükseltme Kuyruğu (Kaynakta Hazır, Ayrı Canlı Onay Bekler):**
   - `zethropol-hardware 0.1.0-5` (`zethropol-diagnostics-helper` + Polkit politikası dahil; canlıda `0.1.0-4`).
   - `zethropol-services` ve `zethropol-control-center` sonraki sürümleri (`P2B` karantina temizliği dahil; canlıda `services 0.1.0-2` ve `control-center 0.1.0-3`).
2. **ZCI / ZEKANS Takip Kaydı (`P3A` / `P3C`):**
   - `services/zci-chat/`, `services/local-inference/` ve `services/zekans-code/` üzerindeki geliştirme kayıtları korunur; `AGENTS.md` ve `docs/SECURITY.md` gereği ZCI/ZEKANS için ayrıcalık veya güvenlik istisnası varsayılmaz.
   - `P3A` (Gemma 4 E4B `gemma-system.md` ile dondurulmuş `tr-v1/acceptance.json` üzerinde `9/9` resmî kabul + ZEKANS profil mutabakatı) ve `P3C` (uygulanmamış WebUI `8088` geçişi) açık takip kapsamındadır.
3. **`P4C` — `Performance` Servisi `GetTelemetry` Evrimi & Depo Temizliği:**
   - `services/performance/src/dbus.rs` içindeki ağır `Hardware.v1.GetState` çağrısının `GetTelemetry` üzerine taşınması, `Power` ile rol ayrımı ve `Configuration Service` / kök dizin artıklarının (`src/main.rs`, boş `control-center/src/core/`) değerlendirilmesi.
4. **Control Center Kilitli / `NOT VERIFIED` Kapsamı (`P5` — İzole KVM/QEMU Btrfs + Limine VM):**
   - Canlı masaüstündeki `m_liveExecutionEnabled=false`, `rollback_execution_enabled=false` ve `repair_execution_enabled=false` kilitleri korunur; 7 canlı durum değiştiren işlem ile `docs/RECOVERY_FAILURE_TEST_MATRIX.md` (`/boot umask=0077`, `snap-pac`, Limine snapshot boot) doğrulamaları izole **KVM/QEMU Btrfs + Limine VM** üzerinde yürütülecektir.
5. **Kurulum ve ISO Altyapısı (`P6` / Aşama 7) ve Çoklu Donanım (`P7` / Aşama 8):**
   - Tekrarlanabilir yerel paket deposu, `build/recovery/` ArchISO güncellemesi + QEMU/OVMF testi (`P6`) ve dış donanım bağımlı çoklu donanım doğrulaması (`P7`) sonraki faz kayıtlarıdır.
