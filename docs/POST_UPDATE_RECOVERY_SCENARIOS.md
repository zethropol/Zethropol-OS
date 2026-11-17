# Zethropol OS — Başarısız Güncelleme Sonrası Kurtarma Senaryoları (03.2.09)

Bu belge, 03.2.01–03.2.08 kapsamında kurulan Recovery (`org.zethropol.Recovery.v1`) ve `UpdateInstaller` (`arch-update` / `cachy-update`) mimarisi üzerinde başarısız veya kesintiye uğramış güncelleme sonrası salt-okunur kurtarma değerlendirmesini ve güvenlik sınırlarını tanımlar.

## 1. Temel Güvenlik İlkeleri

- **Sahte sağlık kabulü yok**: İstemcinin gönderdiği `health_check_ok=true` veya `update_outcome="ok"` tek başına sistem sağlığını kanıtlamaz; sunucu tarafında doğrulanmış sağlık kanıtı yoksa sonuç `unknown` olarak sınıflandırılır.
- **Otomatik `point_id=0` seçimi yok**: `point_id=0` gönderildiğinde en son snapshot otomatik seçilmez; sonuç `unknown` / `snapshot-unavailable` döner. Belirli `point_id > 0` ile `update_op_id` ilişkisi kanıtlanmalıdır.
- **Belirsiz durum kilidi korunur**: `AbortedOrOrphaned` durumu, `/var/lib/pacman/db.lck` dosyası bulunmasa bile otomatik olarak sıfırlanmaz (`AcknowledgeUncertainPreUpdate` etkinleştirilmemiştir).
- **Otomatik rollback veya onarım yok**: `rollback_execution_enabled = false` ve `m_liveExecutionEnabled = false` korunur. `pacman` veritabanı tahminle onarılmaz, `db.lck` otomatik silinmez.
- **Koşullu rehberlik**: Snapshot varlığı Limine snapshot boot veya `limine-snapper-restore` başarısını garanti etmez; `ready` / `blocked` / `unknown` ayrımı korunur ve haricî kurtarma / `arch-chroot` yalnızca koşullu rehberlik olarak sunulur.
- **`/home` ve uygulama verisi ayrımı**: `/home` (`/@home`) kullanıcı verilerinin root rollback ile geri sarılmaması ile sistem paketi–uygulama verisi sürüm uyumsuzluğu riski birbirinden ayrı raporlanır.

## 2. 12 Başarısız Güncelleme Senaryosu Matrisi

| # | Senaryo | Bilinen Durum | Değerlendirme (`assessmentStatus` / `recommendedLevel`) | Engellenen İşlemler |
|---|---|---|---|---|
| 1 | İndirme sırasında hata | `CompletedFailed`, `db.lck` yok | `recovery-recommended` / `snapshot-preflight` (veya `unknown`) | Otomatik rollback, tahminle DB onarımı |
| 2 | Pacman başlamadan iptal | `CompletedFailed` / iptal, paket değişimi yok | `recovery-recommended` (koşullu) / `none` veya `snapshot-preflight` | Otomatik rollback, snapshot silme |
| 3 | Paket kurulumu sırasında hata | Kısmi değişiklik olasılığı | `recovery-recommended` / `snapshot-preflight` | Otomatik rollback, zorla paket silme |
| 4 | Kısmen uygulanmış yükseltme (`db.lck` var) | Aktif veya kalıntı kilit belirsiz | `blocked-lock` / `external-arch-chroot` | `db.lck` otomatik silme, yeni güncelleme, rollback |
| 5 | Kernel/initramfs güncelleme hatası | `/boot` ve `/@` uyumsuz olabilir | `external-recovery-required` / `external-arch-chroot` (preflight `blocked`/`unknown` ise) | `/boot` doğrulanmadan devam, otomatik reboot |
| 6 | Limine veya `/boot` uyumsuzluğu | `boot_mount_verified` / `limine_boot_integration` / `kernel_modules_compatibility` başarısız | `external-recovery-required` / `external-arch-chroot` | Yerinde otomatik rollback, `limine-update` |
| 7 | Güç kesintisi / sistem çökmesi | Bellek rezervasyonu yok; diskte `[update-op:<id>]` snapshot ve olası `db.lck` | `blocked-lock` veya `recovery-recommended` / `limine-snapshot-boot` | Bellek sıfır olduğu için "temiz" varsayımı |
| 8 | `UpdateInstaller` veya Recovery çökmesi / D-Bus kopması | `AbortedOrOrphaned` (`uncertain-state`) | `blocked-uncertain` / `snapshot-preflight` veya `external-arch-chroot` | D-Bus kopmasını işlemin durması sayma, belirsiz durum sıfırlama |
| 9 | Güncelleme sonrası sistemin açılmaması | D-Bus oturumu yok | Seviye 1 (Limine Snapshot Boot) veya Seviye 3/4 (`arch-chroot`) koşullu rehberlik | — |
| 10 | Snapshot kayıp veya doğrulanamıyor | `snapshot_exists` / `subvolume` başarısız | `snapshot-unavailable` / `external-arch-chroot` | Eksik snapshot'a rollback |
| 11 | `/home` ve uygulama verisi uyumsuzluğu | `/home` (`/@home`) korunur; `~/.config` sürüm farkı riski | `homeDataPreserved=true` + `homeCompatibilityWarning` | `/@home` geri alma |
| 12 | `code=0` ama sağlık kontrolü başarısız/bilinmiyor | Sunucu sağlık kanıtı yok | `unknown` (sahte `healthy` üretilmez) | Snapshot otomatik silme |
