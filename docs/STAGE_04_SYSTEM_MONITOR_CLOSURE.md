# Zethropol OS — Aşama 04 (System Monitor) Resmî Kapanış ve Canlı Kurulum Kaydı

- **Tarih:** 2026-11-27
- **Canlı Dağıtım Tarihi (Aşama 04.7):** 2026-11-26 (`backups/system-monitor-user-backup-stage047-20261126-001200/`)
- **Aşama 04 Durumu:** `COMPLETED / DEPLOYED LIVE`
- **Referans Taban:** `zethropol-control-center 0.1.0-3` (korundu) + `zethropol-hardware 0.1.0-4` + `zethropol-monitoring 0.1.0-3` + `zethropol-system-monitor 0.1.0-1`

> **Kanıt Sınırı Notu:** Bu belge, 2026-11-27 tarihinde çalışma ağacı ve canlı sistemde salt-okunur doğrulanan paket sürümleri (`pacman -Q`), dosya bütünlüğü (`pacman -Qkk`), KPackage çözümlemesi (`kpackagetool6`), paket ve kullanıcı yedeği `SHA256SUMS` özetleri üzerinden hazırlanmıştır. 2026-11-26 tarihli canlı dağıtım penceresindeki etkileşimli masaüstü görsel/KConfigDialog adımlarına ait ham ekran görüntüsü veya ayrı haricî kabul raporu bu depoda saklı değildir ve üretilmemiştir.

---

## 1. Canlı Kurulu Paket Envanteri ve Bütünlük Doğrulaması

2026-11-27 tarihinde `pacman -Q`, `pacman -Qkk` ve `packages/stage-04.6/SHA256SUMS` (`sha256sum -c`: 3/3 Tamam) ile doğrulanmıştır:

| Paket | Kurulu Sürüm | `pacman -Qkk` Sonucu | Arşiv (`packages/stage-04.6/`) SHA-256 | Rollback Arşivi |
| --- | --- | --- | --- | --- |
| `zethropol-hardware` | `0.1.0-4` | 18 toplam dosya, 0 değiştirilmiş | `cbd755d10fbe03be44e77bfbcfbe74226c8a05b6c18a7d9d38a0dae00cb4c02c` | `packages/zethropol-hardware/zethropol-hardware-0.1.0-3-x86_64.pkg.tar.zst` |
| `zethropol-monitoring` | `0.1.0-3` | 18 toplam dosya, 0 değiştirilmiş | `c5e6eddefeb13fe27ed617a3137436f3841377e7d45bf8237408154dec724afb` | `packages/zethropol-monitoring/zethropol-monitoring-0.1.0-2-x86_64.pkg.tar.zst` |
| `zethropol-system-monitor` | `0.1.0-1` | 26 toplam dosya, 0 değiştirilmiş | `7bab6f337ec257eb67b2c1212135d15a04de500784dca27ed95f05ca0fbc4da3f` | Kaldırma + kullanıcı Plasmoid yedeği geri yükleme |

### Bağımlılık Zinciri

`zethropol-system-monitor 0.1.0-1` -> `zethropol-monitoring>=0.1.0-3` -> `zethropol-hardware>=0.1.0-4` (tamamı canlı sistemde kurulu ve bütünlüğü doğrulanmıştır).

---

## 2. Kullanıcı Plasmoid Geçişi ve Gölgeleme Temizliği

- **KPackage Çözümlemesi (`kpackagetool6 -t Plasma/Applet -s org.zethropol.systemmonitor`):**
  - Eklenti: `org.zethropol.systemmonitor`
  - Açıklama: `Five-pod Zethropol OS system telemetry monitor for CPU, RAM, GPU, Network, and Storage`
  - Çözümlenen Yol: `/usr/share/plasma/plasmoids/org.zethropol.systemmonitor/` (sistem paketi etkin; kullanıcı dizini gölgelemesi kaldırılmış).
- **Korunan Kullanıcı Yedeği (`backups/system-monitor-user-backup-stage047-20261126-001200/`):**
  - `pre-move-copy/org.zethropol.systemmonitor/` için `sha256sum -c ../SHA256SUMS`: **12/12 Tamam**.
  - `moved-user-plasmoid/` ve `plasma-org.kde.plasma.desktop-appletsrc.pre-04.7` arşivleri korundu.

---

## 3. Korunan Güvenlik ve Taban Sınırları

- `zethropol-control-center 0.1.0-3` (23 dosya, 0 değiştirilmiş) ve `m_liveExecutionEnabled = false` kilidi korundu.
- `services/recovery/src/recovery_service.rs` içindeki `rollback_execution_enabled = false` ve `repair_execution_enabled = false` kilitleri korundu.
- `zethropol-performance 0.1.0-1` (15 dosya, 0 değiştirilmiş) ve `/etc/systemd/system/zethropol-performance.service.d/99-zethropol-quarantine.conf` karantinası Aşama 04.7 kapsamında değiştirilmedi; yükseltme ayrı güvenlik borcu (`P2`) olarak takip edilir.
- `zethropol-tv 0.1.0-1` bağımsız canlı TV uygulaması (17 dosya, 0 değiştirilmiş) ayrıca kurulu durumdadır.
