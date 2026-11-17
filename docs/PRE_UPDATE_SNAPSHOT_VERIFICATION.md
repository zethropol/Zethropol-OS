# Zethropol OS — Güncelleme Öncesi Snapshot Başarı Denetimi (03.2.08)

Bu belge, onaylanmış `UpdateInstaller` (`arch-update` / `cachy-update`) güncelleme mimarisi ile `org.zethropol.Recovery.v1` D-Bus servisi arasındaki güncelleme öncesi snapshot doğrulama zincirini, rezervasyon durum makinesini ve açık güvenlik sınırlarını tanımlar.

## 1. D-Bus Sözleşmesi ve Rezervasyon Durum Makinesi

- Mevcut `CreateRecoveryPoint(name, description)` sözleşmesi manuel kurtarma noktaları için korunur.
- Güncelleme öncesi akış için ayrık yöntemler kullanılır:
  1. `CreatePreUpdateRecoveryPoint(update_op_id, description)` — Polkit `org.zethropol.recovery.modify`, çağıran kimliği (`validate_dbus_sender`), `pacman` kilidi ve depolama/kota denetimi uygular; snapshot'ı `--userdata zethropol=pre-update,important=yes` etiketiyle oluşturur ve `snapper list` + `btrfs subvolume show` ile doğrular.
  2. `VerifyPreUpdateSnapshot(update_op_id, point_id)` — Salt-okunur doğrulama yapar; `owner_sender`, `update_op_id`, `snapshot_id`, `subvolume_id`, `/home` izolasyonu, `/boot` bağlama durumu ve (kritik güncelleme ise) Limine/kernel/modül uyumluluğunu kontrol eder. Rezervasyonu erken tüketmez.
  3. `BeginPreUpdateExecution(update_op_id, point_id)` — Tek kullanımlık `VerifiedReady -> ActiveUpdate` geçişini yapar.
  4. `CompletePreUpdateExecution(update_op_id, outcome)` — `CompletedOk`, `CompletedFailed` veya `AbortedOrOrphaned` terminal durumuna geçirir.

### Durumlar (`PreUpdateReservationState`)
- `Creating` -> `VerifiedReady` -> `ActiveUpdate` -> `CompletedOk` | `CompletedFailed` | `AbortedOrOrphaned`.
- **Sabit 120 sn TTL kullanılmaz**: `ActiveUpdate` durumu D-Bus `owner_sender` bağlantısına ve açık tamamlama çağrısına bağlıdır.
- **D-Bus kopması / çökme**: `owner_sender` bağlantısı koptuğunda rezervasyon `AbortedOrOrphaned` durumuna alınır; ancak D-Bus kopması gerçek `pacman` işleminin durduğu anlamına gelmez. Durum belirsizken (`ActiveUpdate` veya `AbortedOrOrphaned`) yeni bir güncelleme başlatılması engellenir ve oluşturulan snapshot diskte korunur.

## 2. Sistem Genelinde Koordinasyon Sınırı

- `RecoveryManager` bellek içi kilitleri (`active_operation`, `active_pre_update_reservation`) yalnızca D-Bus istemcilerini serileştirir; **gerçek sistem genelinde kalıcı güvenlik garantisi oluşturmaz** (servis yeniden başlatıldığında veya harici root CLI işlemlerinde bellek durumu sıfırlanır).
- `UpdateInstaller` harici etkileşimli `script -- ... -- arch-update` (`sudo pacman`) başlattığından, `BeginPreUpdateExecution` ile `pacman`'in `/var/lib/pacman/db.lck` alması arasında kullanıcı uzayında tam atomik geçiş yoktur. İşlem anındaki libalpm kilidi `05-snap-pac-pre.hook` (`AbortOnFail`) ve `10-limine-snapper-lock.hook` tarafından sağlanır.

## 3. Snapshot Saklama, Depolama ve Boot Sınırları

- `important=yes` etiketi Snapper'ın `NUMBER_LIMIT_IMPORTANT` (15) havuzunu kullanır, ancak disk alanı (`FREE_LIMIT=0.2`, `SPACE_LIMIT=0.5`) tükendiğinde Snapper temizliği tarafından silinmezlik garantisi vermez. Bu nedenle ön-kapı Btrfs boş alanını ve `important=yes` sayısını önceden denetler.
- `snap-pac` `pre`/`post` hook'ları korunur; tek bir güncelleme için `Recovery pre-update` + `snap-pac pre/post` (3 snapshot) depolama maliyeti hesaba katılır.
- **Kritik Boot Güncellemeleri**: `UpdateScanner` raporları `linux*`, `mkinitcpio*`, `dracut*`, `limine*`, `grub*`, `efibootmgr`, `systemd*`, `btrfs-progs`, `snapper`, `snap-pac`, `limine-snapper-sync`, `pacman`, `glibc` içeriyorsa veya tarama `error`/`warning`/`pending` durumundaysa oturum kritik boot-etkili kabul edilir. `/boot` (`vfat`, `umask=0077`) ve Limine/modül uyumluluğu kanıtlanamıyorsa işlem engellenir.

## 4. Canlı Yürütme Varsayılanı

> [!IMPORTANT]
> Geliştirme/doğrulama aşaması `03.2.08` kapsamında `UpdateInstaller` üzerinde gerçek paket işlemi başlatan yol (`liveExecutionEnabled = false`) **varsayılan olarak kapalıdır** ve gerçek rollback yürütmesi (`rollback_execution_enabled = false`) kapalıdır.
