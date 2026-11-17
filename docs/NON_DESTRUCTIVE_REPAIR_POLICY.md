# Zethropol OS — Veri Kaybı Oluşturmayan Onarım Politikası (03.2.10)

Bu belge, Zethropol OS kurtarma ve onarım katmanında veri kaybını önlemeyi önceliklendiren üç sınıflı onarım güvenlik politikasını tanımlar.

## 1. Üç Sınıflı Onarım Politikası (`RepairSafetyClass`)

1. **`read-only-safe` (Salt-okunur ve güvenli)**:
   - Yalnızca teşhis, durum okuma, bütünlük ve politika değerlendirmesi (`diagnose-recovery-state`, `inspect-subvolumes`, `inspect-boot-and-limine`, `assess-post-update`, `verify-package-integrity-readonly`).
   - Disk, paket veritabanı, snapshot veya `/boot` üzerine hiçbir yazma yapmaz.
2. **`conditional-authorized` (Koşullu ve yetkilendirme gerektiren)**:
   - Teorik olarak yalnızca sıkı ön koşullar altında değerlendirilebilen onarım planları (`guided-snapshot-rollback-plan`, `guided-chroot-package-repair-plan`, `reinstall-kernel-initramfs-plan`).
   - Zorunlu koşullar:
     - Sunucu tarafında doğrulanmış bağımsız haricî yedek (istemcinin `has_verified_external_backup=true` beyanı kanıt sayılmaz; Snapper snapshot'ı bağımsız yedek değildir).
     - D-Bus çağıran kimliği doğrulaması + işlem bazlı Polkit yetkilendirmesi (`org.zethropol.recovery.rollback` veya `org.zethropol.recovery.modify`).
     - Açık kullanıcı onayı (`explicit_user_confirmation` tek başına yetkilendirme yerine geçmez).
     - Aktif `pacman` kilidi (`/var/lib/pacman/db.lck`), `active_operation` veya `AbortedOrOrphaned` belirsizliği olmaması.
     - `/home` izolasyonu ve (rollback/boot planları için) `/boot` + Limine + modül uyumluluğunun `pass` olması.
   - **Yürütme sınırı**: Bu aşamada `repair_execution_enabled = false`, `allowed = false` ve sunucu tarafı haricî yedek/Polkit kanıtı bulunmadığından `planReady = false` tutulur.
3. **`blocked` (Engellenen)**:
   - `/home` (`/@home`) geri sarma, bölümleme, formatlama, kullanıcı verisi silme (`rollback-home-subvolume`, `format-partition`, `repartition-disk`, `auto-delete-user-data`).
   - `pacman` DB tahminle onarma veya `/var/lib/pacman/db.lck` körlemesine silme (`guess-repair-pacman-db`, `delete-pacman-lock`).
   - Snapshot silme veya doğrulanmamış kernel/initramfs üzerine yazma (`delete-snapshots-for-space`, `overwrite-boot-kernels-unverified`).
   - Bilinmeyen veya veri kaybı oluşturmayacağı kanıtlanamayan tüm eylemler (`unknown-repair-action` -> asla `read-only-safe` sınıfına alınmaz).

## 2. Korunan Sınırlar

- `/home` kullanıcı verilerinin korunması (`homeDataPreserved`) ile uygulama verilerinin (`~/.config`, `~/.local/share`) sürüm uyumluluğu riski (`appDataCompatibilityRisk`) ayrı raporlanır.
- Eksik erişim yetkisi (`PermissionDenied` / `unknown`) hiçbir zaman başarılı doğrulama sayılmaz.
- **Üç yürütme sınırı kapalıdır**:
  - Canlı Update yürütmesi: `m_liveExecutionEnabled = false`
  - Gerçek rollback yürütmesi: `rollback_execution_enabled = false`
  - Gerçek onarım yürütmesi: `repair_execution_enabled = false`
