# Zethropol OS — Btrfs Alt Hacim Düzeni ve `/home` Koruma Sınırları

Bu belge, Zethropol OS kurtarma (Recovery) katmanındaki Btrfs alt hacim düzenini, root snapshot geri yüklemesinin (rollback) etki sınırlarını, `/home` veri koruma garantilerini ve rollback preflight tarafından uygulanan güvenlik engellerini tanımlar.

## 1. Doğrulanmış Btrfs Alt Hacim ve Bağlama Düzeni

Canlı sistemdeki `/etc/fstab`, `/proc/self/mountinfo` (`findmnt`) ve `/etc/snapper/configs/root` yapılandırmasından doğrulanan düzen aşağıdaki gibidir:

| Bağlama Noktası | Dosya Sistemi | Kaynak / UUID | Btrfs Alt Hacmi (`subvol`) | Doğrulanan `subvolid` | Rollback Kapsamı |
|---|---|---|---|---|---|
| `/` | `btrfs` | `UUID=43b2952b-efab-43cb-a84a-1c9d97227f2d` (`/dev/nvme0n1p2`) | `/@` | `539` | **Geri yüklenir** (hedef kök alt hacim) |
| `/home` | `btrfs` | `UUID=43b2952b-efab-43cb-a84a-1c9d97227f2d` | `/@home` | `257` | **Korunur** (ayrı alt hacim) |
| `/root` | `btrfs` | `UUID=43b2952b-efab-43cb-a84a-1c9d97227f2d` | `/@root` | — | **Korunur** (ayrı alt hacim) |
| `/srv` | `btrfs` | `UUID=43b2952b-efab-43cb-a84a-1c9d97227f2d` | `/@srv` | `259` | **Korunur** (ayrı alt hacim) |
| `/var/cache` | `btrfs` | `UUID=43b2952b-efab-43cb-a84a-1c9d97227f2d` | `/@cache` | `260` | **Korunur** (ayrı alt hacim) |
| `/var/tmp` | `btrfs` | `UUID=43b2952b-efab-43cb-a84a-1c9d97227f2d` | `/@tmp` | `261` | **Korunur** (ayrı alt hacim) |
| `/var/log` | `btrfs` | `UUID=43b2952b-efab-43cb-a84a-1c9d97227f2d` | `/@log` | `262` | **Korunur** (ayrı alt hacim) |
| `/boot` | `vfat` | `UUID=FB94-FF19` | — | — | **Ayrı EFI bölümü** (kernel/initramfs/Limine uyumu ayrıca doğrulanır) |
| `/tmp` | `tmpfs` | `tmpfs` | — | — | Geçici bellek dosya sistemi |

- **Snapper Yapılandırması** (`/etc/conf.d/snapper` ve `/etc/snapper/configs/root`):
  - `SNAPPER_CONFIGS="root"`, `SUBVOLUME="/"`, `FSTYPE="btrfs"`.
  - Snapper `root` yapılandırması `/` (`subvol=/@`) alt hacminin anlık görüntülerini alır.
  - `/etc/fstab` içinde `/.snapshots` için ayrı bir bağlama satırı bulunmamaktadır; `/.snapshots` kök yapılandırması altında yer alır.

## 2. Root Snapshot Geri Yüklemesinin Etki Sınırları

### Kullanıcı Dosyaları (`/home` / `/@home`)
- `/home`, `/` (`/@`) ile aynı Btrfs havuzunda ancak bağımsız bir üst düzey alt hacim (`/@home`, `subvolid=257`) olarak bağlandığından, `/@` alt hacminin bir snapshot'a geri döndürülmesi `/@home` alt hacmindeki kullanıcı belgelerini, kişisel dosyaları ve kullanıcı dizinlerini **geri almaz ve üzerine yazmaz**.
- Ancak bu koruma yalnızca `/home` dizininin gerçekten `/@` dışındaki ayrı bir alt hacme veya ayrı bir dosya sistemine bağlı olması koşulunda geçerlidir.

### Diğer Ayrı Alt Hacimler
- `/@root` (`/root`), `/@srv` (`/srv`), `/@cache` (`/var/cache`), `/@tmp` (`/var/tmp`) ve `/@log` (`/var/log`) ayrı alt hacimler olduğu için root snapshot geri yüklemesinden doğrudan etkilenmez.
- `/var/log` ve `/var/lib/zethropol/aur-transactions` (`/@srv/zethropol/aur-transactions`) kayıtları root rollback sonrasında da korunur; bu sayede hata ayıklama ve işlem izleri kaybolmaz.

### Uygulama Verileri ve Sistem Durumu
- Kullanıcı düzeyindeki uygulama ayarları ve verileri (`~/.config`, `~/.local/share`, `~/.cache`) `/@home` altında bulunduğu için root rollback ile **eski haline dönmez**.
- Sistem düzeyindeki yapılandırma ve servis verileri (`/etc`, `/usr`, ve `/@` altında kalan `/var/lib` dizinleri) root snapshot kapsamındadır; rollback sonrasında sistem paketi/uygulama sürümü ile `/@home` içindeki kullanıcı uygulama veritabanı biçimleri arasında sürüm uyumsuzluğu oluşabilir.

### Kurtarma Kayıtları (`/.snapshots`)
- Snapper `root` kayıtları (`/.snapshots/<id>/info.xml` ve `snapshot` alt hacimleri) sistem kurtarma geçmişini oluşturur. Rollback işlemi sırasında mevcut kök alt hacim ve Snapper meta verilerinin tutarlılığı korunmalıdır; tutarsız veya erişilemeyen snapshot dizinleri preflight tarafından engellenir.

## 3. Haricî Yedekleme İlkesi

> [!WARNING]
> **Snapper snapshot'ları haricî yedekleme yerine geçmez.**
>
> Root snapshot'ları yalnızca aynı NVMe/Btrfs depolama birimi üzerindeki sistem alt hacminin (`/@`) noktasal durumunu yakalar. Disk/NVMe donanım arızası, Btrfs dosya sistemi bozulması, bölümleme hatası, fiziksel kayıp veya `/@home` içindeki kullanıcı dosyalarının yanlışlıkla silinmesi/şifrelenmesi durumlarına karşı koruma sağlamaz. Kullanıcı verileri için haricî/ayrı ortamda düzenli yedekleme zorunludur.

## 4. Veri Kaybı Riskleri ve Rollback Preflight Engelleme Koşulları

`services/recovery/src/recovery_service.rs` içindeki `run_rollback_preflight` (`home_subvolume_isolation` kontrolü) aşağıdaki durumlarda rollback ön kontrolünü **engeller (`blocked` / `unknown`, `canProceed=false`)**:

1. **Ortak kök alt hacim**: `/home` dizini `/@` (veya `/`) ile aynı Btrfs alt hacmini paylaşıyorsa; bu durumda root rollback doğrudan kullanıcı verilerini geri sarar.
2. **Eksik `/home` bağlama noktası**: `mountinfo` veya `/etc/fstab` içinde ayrı bir `/home` bağlaması yoksa (`/home` yalnızca `/` altında sıradan bir dizin olarak duruyorsa).
3. **İç içe (nested) alt hacim**: `/home` alt hacmi kök alt hacmin altına yuvalanmışsa (ör. `subvol=/@/home`); root alt hacim değiştirildiğinde alt hacim yetim kalabilir veya bağlama tutarsızlığı doğar.
4. **Çelişkili / belirsiz bağlama düzeni**: Çalışan sistem (`mountinfo`) ile kalıcı yapılandırma (`/etc/fstab`) farklı `/home` alt hacimleri veya dosya sistemleri bildiriyorsa, ya da `/home` geçici (`tmpfs`) bir bağlama ise.
5. **Doğrulanamayan durum**: `mountinfo` ve `/etc/fstab` kaynaklarının her ikisi de okunamıyor veya ayrıştırılamıyorsa (`unknown` — güvenli başarısızlık).
