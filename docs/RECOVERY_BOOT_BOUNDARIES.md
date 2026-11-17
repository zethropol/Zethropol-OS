# Zethropol OS — Boot, Limine, Kernel ve Recovery Koruma Sınırları

Bu belge, Zethropol OS'te `/boot` (ESP), Limine önyükleyici yapılandırması, çekirdek (`kernel`), `initramfs`, çekirdek modülleri (`/usr/lib/modules`) ve `limine-snapper-sync` arasındaki kurtarma sınırlarını tanımlar.

## 1. Gerçek Sistemde Doğrulanan Boot Düzeni

Canlı sistemdeki `/etc/fstab`, `/etc/limine-snapper-sync.conf`, `uname -r` ve `/usr/lib/modules/` üzerinden salt-okunur doğrulanan yapı:

- **`/boot` (ESP) bağlaması** (`/etc/fstab`):
  - `UUID=FB94-FF19` -> `/boot`, dosya sistemi `vfat`, seçenekler `defaults,umask=0077 0 2`.
  - Root (`/@`) Btrfs alt hacminden **tamamen ayrı** bir FAT32/EFI bölümüdür. `umask=0077` nedeniyle yalnızca `root` erişebilir; sandbox veya ayrıcalıksız kullanıcı bağlamında `/boot` doğrudan okunamaz.
- **Aktif çekirdek ve modül ağaçları**:
  - Çalışan çekirdek (`uname -r`): `7.3.4-1-cachyos`.
  - Mevcut modül dizinleri (`/usr/lib/modules/`): `7.3.4-1-cachyos` ve `6.18.60-2-cachyos-lts`.
- **`limine-snapper-sync` yapılandırması** (`/etc/limine-snapper-sync.conf`):
  - `LIMIT_USAGE_PERCENT=85` (FAT32 `/boot` doluluk sınırı; aşılırsa yeni snapshot boot girdisi eklenmez).
  - `RESTORE_METHOD=replace` (root alt hacim tabanlı geri yükleme yöntemi).
  - `SNAPSHOT_FORMAT_CHOICE=2`.

## 2. `/boot` ve Root Snapshot Ayrımından Doğan Sınırlar

1. **`/boot` root snapshot kapsamında değildir**:
   - Snapper `root` yapılandırması yalnızca `/` (`/@`) alt hacmini yakalar. `/boot` ayrı bir `vfat` bölümü olduğu için root snapshot geri yüklemesi `/boot` altındaki `vmlinuz`, `initramfs` ve `limine.conf` dosyalarını otomatik olarak geri almaz.
2. **Kernel–modül sürüm uyuşmazlığı riski**:
   - Sistem güncellemesi `/boot` üzerindeki çekirdeği yükseltip eski bir root snapshot'ı (`/@`) eski `/usr/lib/modules/<sürüm>` ile bıraktığında, `/boot` içindeki çekirdek ile snapshot içindeki modüller uyuşmayabilir. Bu durumda sistem açılışta Btrfs/NVMe veya çevre birimi modüllerini yükleyemez.
   - Bir `vmlinuz` veya `initramfs` dosyasının diskte var olması tek başına eski bir root snapshot'ının o çekirdekle açılabileceğini **kanıtlamaz**.
3. **Aktif ve alternatif kurtarma girdilerinin korunması**:
   - `limine.conf` içinde hem mevcut çalışan sistemi açabilen birincil girdi hem de en az bir alternatif/fallback veya snapshot kurtarma girdisi bulunmalı, tüm `kernel_path` ve `module_path` hedefleri `/boot` içinde mevcut ve boş olmayan dosyalara işaret etmelidir.
4. **Hatalı veya eksik `limine-snapper-sync` durumu**:
   - `limine-snapper-sync` ikilisinin yokluğu, boş/hatalı `/etc/limine-snapper-sync.conf`, geçersiz `RESTORE_METHOD` veya `/boot` doluluk sınırı aşımları kurtarma menüsü senkronizasyonunu bozar.

## 3. Rollback Preflight (`run_rollback_preflight`) Güvenlik Kuralları

`services/recovery/src/recovery_service.rs` içindeki ön kontrol şu koşullardan herhangi biri sağlanmadığında veya doğrulanamadığında `ready` üretmez (`blocked` veya `unknown`, `canProceed=false`):

- `boot_mount_verified`: `/boot` kalıcı ve tutarlı bir dosya sisteminde (`mountinfo` / `/etc/fstab`) bağlı değilse veya `tmpfs` gibi geçici bir bağlama ise engellenir.
- `limine_boot_integration`: `limine-snapper-sync` yoksa (`unknown`), yapılandırma geçersizse, `limine.conf` eksik/boşsa, yoltraversal (`..`) içeriyorsa, kernel veya initramfs dosyaları eksik/boşsa ya da alternatif kurtarma/fallback girdisi yoksa engellenir.
- `kernel_modules_compatibility`: Çalışan çekirdek veya `/usr/lib/modules/<sürüm>` doğrulanamıyorsa, hedef snapshot'ın `usr/lib/modules` dizini okunamıyor/eksikse (`unknown`/`blocked`) ya da snapshot modül sürümleri `/boot` içindeki doğrulanmış bir çekirdek girdisiyle eşleşmiyorsa engellenir.
- **TOCTOU yeniden kontrolü**: Preflight çalışması sırasında snapshot listesi, `limine.conf`, `limine-snapper-sync.conf` veya kernel/modül uyumluluğu değişirse sonuç `blocked` olarak işaretlenir.

> [!NOTE]
> Canlı sistemde `/boot` bölümü `umask=0077` ile korunduğu için ayrıcalıksız süreçler veya sandbox ortamı `/boot` içeriğini okuyamaz; bu durumda preflight sahte başarı üretmez, `blocked` veya `unknown` ile güvenli biçimde durur.
