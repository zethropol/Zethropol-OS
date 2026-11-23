# Zethropol OS — Control Center 0.1.0-3 Resmî Kapanış ve Kabul Kaydı

- **Tarih:** 2026-11-24
- **Aşama F Durumu:** `COMPLETED / ACCEPTED`
- **Bileşen Durumu:** `Zethropol Control Center 0.1.0-3: COMPLETED / ACCEPTED — kabul edilmiş güvenli işlev kapsamıyla`
- **Referans Başlangıç Sürümü:** `zethropol-control-center 0.1.0-3` (ve bütünleşik 8 paketlik dağıtım tabanı)
- **Resmî Kabul Kaynağı:** Daha önce tamamlanan ve kullanıcı tarafından incelenen `final_live_acceptance_report.md`

> **Kanıt Sınırı Notu:** `final_live_acceptance_report.md` bu Git deposu içinde yer almayan haricî oturum kabul raporudur. Bu belgede söz konusu rapordan bilinen ve onaylanan kabul sonuçları özetlenmiş; rapora doğrudan erişim olmaksızın ham test çıktısı üretilmemiştir. Paket `SHA256SUMS` özetleri, geri dönüş arşivleri, kaynak güvenlik kilitleri ve `zethropol-performance` politika durumu doğrudan depo çalışma ağacından doğrulanmıştır.

---

## 1. Resmî Kabul Özeti (`final_live_acceptance_report.md`)

| Kontrol Alanı | Kabul Sonucu | Kaynak / Dayanak |
|---|---|---|
| Control Center sürümü | `zethropol-control-center 0.1.0-3` (`COMPLETED / ACCEPTED`) | `final_live_acceptance_report.md` + `packages/zethropol-control-center/` |
| Bütünleşik paket güncellemesi | **8/8** paket dağıtıldı | `final_live_acceptance_report.md` + `packages/*/SHA256SUMS` |
| Paket bütünlüğü | **9/9** paket bütünlüğü doğrulandı (8 dağıtım + 1 karantina) | `final_live_acceptance_report.md` + `sha256sum -c` |
| Sistem servisleri | **7/7** aktif veya D-Bus ile etkinleşebilen servis kabul edildi | `final_live_acceptance_report.md` + `services/*/deployment/` |
| Güvenlik / senaryo kabul kapsamı | **S-01–S-09** kabul kapsamı tamamlandı | `final_live_acceptance_report.md` |
| Masaüstü canlı kabulü | **KDE Plasma Wayland 12/12 sekme PASS** | `final_live_acceptance_report.md` (`TabRegressionTest.cpp` 12 sekme kapsamıyla uyumlu) |
| Anlık görüntü koruması | **Snapper 340–347** korundu | `final_live_acceptance_report.md` |
| Karantina sınırı | `zethropol-performance 0.1.0-1` karantinası korundu | `final_live_acceptance_report.md` + `ServiceService` engeli |

### Doğrulanan 12 KDE Plasma Wayland Sekmesi

`Overview`, `Hardware`, `Diagnostics`, `System`, `Performance`, `Power`, `Updates`, `Recovery`, `Security`, `Services`, `Applications`, `Processes`.

---

## 2. Sekiz Paketlik Dağıtım ve 9/9 Paket Envanteri

Depodaki `packages/*/SHA256SUMS` dosyalarının tamamı salt-okunur `sha256sum -c` ile doğrulanmıştır (`Tamam`).

| # | Paket | Sürüm | Durum | Kaynak Arşivi SHA-256 (`*-src.tar.gz`) | Paket Arşivi SHA-256 (`*.pkg.tar.zst`) |
|---|---|---|---|---|---|
| 1 | `zethropol-control-center` | `0.1.0-3` | Dağıtıldı · Referans Taban | `93f89fc923cb7d04f0c33c5fbae2ac03b8f46881dfef97d0e112fbfe94e5d6c7` | `c92a2ac78404484172d4a16beaf28cb8750d47a14381276776cfdada62601de3` |
| 2 | `zethropol-diagnostics` | `0.1.0-3` | Dağıtıldı (Servis 1/7) | `cd86907c4fef43ae058f49e2b01e67266176ca5da638dfb3f5676164af0fca7b` | `aaf9a4ea9c386911282deb84de598fa401d42b8fcea12aad04710488d324d651` |
| 3 | `zethropol-hardware` | `0.1.0-3` | Dağıtıldı (Servis 2/7) | `7245ce0c01cfc5676c93c772d3c2e7f08231ac6293486aefcc8a4b0242adba67` | `4d0391cbcdecb559a5d16b16db069f92246318b905506501f5d50631e150e54f` |
| 4 | `zethropol-monitoring` | `0.1.0-2` | Dağıtıldı (Servis 3/7) | `25e3a1a5d6158fbbbd84c9c7191087a9e700d58924d9d6f6d04ae56745c338ef` | `5e60b3ed7a3d120dd51d70cda1bd447b27c82a40d498753f918182a4946d8a97` |
| 5 | `zethropol-power` | `0.1.0-2` | Dağıtıldı (Servis 4/7) | `58016e28feea74ac9ca88f12d5f598d99b5503299a41351e39538d17dac33185` | `7a53f7b4410e652ffe44fca9911c5c04b3578f2e0c7e8e0934dd4d67a992bad4` |
| 6 | `zethropol-recovery` | `0.1.0-2` | Dağıtıldı (Servis 5/7) | `13d0bcb9d95ea68fcaf7f5c4f6cbe742f4909b004cb2e73671fafafc7d4229c3` | `0d929d11c8662c6bf3b9aa1c1feceaaad5c3faf9357ade03657caf8d3b65c4f5` |
| 7 | `zethropol-security` | `0.1.0-2` | Dağıtıldı (Servis 6/7) | `423686aee13dc33a5f302c58135b8a850808d54468f52c780ce087a4ec206a49` | `2b2dab822c7c77291a15a70099d238a9419ee827f5babe37c14f33785e88c0b0` |
| 8 | `zethropol-services` | `0.1.0-2` | Dağıtıldı (Servis 7/7) | `60f73642781e89d042ab62d44e3f080d83f1c9df8d7df90957d07528a39c6bea` | `118459cf70b6adec7413d6bdac8b2431948e3ba1b8eec28a8fa572f3b5331444` |
| 9 | `zethropol-performance` | `0.1.0-1` | **KARANTİNADA** (Yükseltilmedi) | — (kurulu `0.1.0-1` commit `814a1d5c`) | — (yeni paket üretilmedi) |

### 7/7 Kabul Edilen Servis ve Yetkilendirme Sınırı

- Salt-okunur / D-Bus kök sahipliği sınırlandırılmış servisler: `org.zethropol.Diagnostics`, `org.zethropol.Hardware`, `org.zethropol.Monitoring`.
- D-Bus kök sahipliği + Polkit `auth_admin` yetki sınırı uygulanmış servisler:
  - `org.zethropol.Power` (`org.zethropol.power.modify`)
  - `org.zethropol.Recovery` (`org.zethropol.recovery.modify`, `org.zethropol.recovery.rollback`)
  - `org.zethropol.Security` (`org.zethropol.security.modify`)
  - `org.zethropol.Services` (`org.zethropol.services.modify`)

---

## 3. Korunan Snapper Kayıtları ve Geri Dönüş Arşivleri

- **Snapper anlık görüntü aralığı:** `340–347` dokunulmadan korunmaktadır.
- **Doğrulanmış geri dönüş arşivleri (`sha256sum -c`: Tamam):**
  - `packages/zethropol-control-center/rollback-0.1.0-1/installed-state-repack/zethropol-control-center-0.1.0-1-x86_64.pkg.tar.zst` — `852ded9e5fb01907bad0afdf927799305d8526c8034794ed0963163ef09c16c4`
  - `packages/zethropol-control-center/zethropol-control-center-0.1.0-2-x86_64.pkg.tar.zst` — `bf36e274f010720f08ddb68f6848e8a42c05c21d2eca2f2e1aa5e95661e4ceb5`
  - `packages/zethropol-diagnostics/rollback-0.1.0-1-live-state/` (çift katmanlı `0.1.0-1` yedeği, 8/8 `SHA256SUMS` Tamam) + `zethropol-diagnostics-0.1.0-1-x86_64.pkg.tar.zst` (`503f7d83412e89e03f486bf4c380f931e946784f4ba88a30f60fdc56b82479f7`) + `zethropol-diagnostics-0.1.0-2-x86_64.pkg.tar.zst` (`1e0e6089e1f14970127416fdeb8fb7fdb085df72cc8025941924348f5cb1cc1b`)
  - `packages/zethropol-power/zethropol-power-0.1.0-1-x86_64.pkg.tar.zst` — `6d6e421d8dfebb45d4352648baadf72f83ecfbbc26fd34250f59cc9f2c4fbf28`
  - `packages/zethropol-update/rollback-0.1.0-2/installed-state-repack/zethropol-update-0.1.0-2-x86_64.pkg.tar.zst` — `558fb820d9d806da43278655d02d05a7a742343c91dbf960ea63769d78cf7a42`

---

## 4. `NOT VERIFIED` Kapsamı ve Korunan Güvenlik Kilitleri

Aşağıdaki canlı sistem durumunu değiştiren işlemler bu kabul kapsamında canlı ortamda uygulanıp doğrulanmamıştır ve açıkça **`NOT VERIFIED`** olarak muhafaza edilir:

- **Updates canlı paket kurulumu / sistem yükseltmesi:** `NOT VERIFIED` — yürütme bilinçli olarak kapalıdır.
- **Recovery canlı snapshot oluşturma, silme, rollback ve onarım yürütmesi:** `NOT VERIFIED` — yürütme kapalıdır (`rollback_execution_enabled = false`, `repair_execution_enabled = false`).
- **Power canlı profil değişikliği:** `NOT VERIFIED`.
- **Security canlı kural / güvenlik duvarı / durum değişikliği:** `NOT VERIFIED`.
- **Services canlı birim başlatma, durdurma, yeniden başlatma, etkinleştirme, devre dışı bırakma:** `NOT VERIFIED`.
- **Applications canlı paket kaldırma:** `NOT VERIFIED`.
- **Processes canlı süreç sonlandırma / öldürme:** `NOT VERIFIED`.

### Değiştirilemez Kaynak Kilitleri

- `desktop/control-center/src/UpdateInstaller.h` (`m_liveExecutionEnabled = false`) ve `desktop/control-center/src/UpdateInstaller.cpp` içindeki canlı yürütme kilidi **kesinlikle korunmuştur** ve kaldırılmamıştır.
- `services/recovery/src/recovery_service.rs` içindeki `rollback_execution_enabled = false` ve `repair_execution_enabled = false` sınırları korunmuştur.

---

## 5. Karantina ve Ayrı Takip Kayıtları

1. **`zethropol-performance 0.1.0-1` Karantinası (Yüksek Öncelikli Ayrı Takip):**
   - Karantina kaldırılmamıştır; yeni paket üretilmemiş ve dağıtılmamıştır.
   - Kurulu `0.1.0-1` paketindeki eski D-Bus politikası (`<policy context="default">` altında `<allow own="org.zethropol.Performance"/>`) ayrı güvenlik borcu olarak takip edilmektedir.
   - `desktop/control-center/src/ServiceService.cpp`, `zethropol-performance.service` için tüm eylemleri engellemeye devam etmektedir (`SecurityRemediationTest.cpp` ile doğrulandı).
   - Çalışma ağacındaki `services/performance/deployment/` sertleştirme taslağı paketlenip doğrulanmadığı için `0.1.0-3` kabul kapsamı dışındadır.
2. **`UpdateInstallerTest` Zamanlama Yarışı (Ayrı Takip):**
   - `desktop/control-center/tests/UpdateInstallerTest.cpp` içindeki aralıklı tarama zamanlama yarışı ayrı takip bulgusu olarak korunmaktadır.
3. **Çalışma Ağacı Bütünlüğü:**
   - Mevcut Git çalışma ağacındaki tüm kullanıcı değişiklikleri, derleme/yedek çıktıları ve ilgisiz modül çalışmaları dokunulmadan korunmuştur.
