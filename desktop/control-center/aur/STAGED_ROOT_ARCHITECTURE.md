# Tek root aktarımı — 2026-11-01

**Üretim etkin değildir.** Yeni kod kaynak adayıdır; sistem servisi, keyring,
kurulum veya yetkili manifest imzası değiştirilmedi/üretilmedi.

## Neden önceki aktarım ölçeklenmiyordu?

`FrozenTree` her dosyayı ayrı memfd'ye kopyalayıp mühürlüyordu. Bu, kaynak
path'inin değiştirilmesini, dosyanın doğrulamadan sonra yazılmasını ve symlink
üzerinden host dosyalarının bind edilmesini önlüyordu. Bwrap her memfd'yi ayrı
`--ro-bind-data` ile alıyordu. Güvenlik özelliği değerliydi; maliyeti dosya sayısıyla
artan FD, mount, argv ve potansiyel çift içerik kopyasıydı. Yay provasında
48.895 dosya / 275.228 mount argümanı bu yolun sınırlarını aşıyordu.

Sıradan bir dizini hash kontrolünden sonra bind etmek aynı değişmezliği sağlamaz.
Yeni mimaride değişmezlik dosya başına seal yerine **private tmpfs superblock'unun
salt okunur hâle getirilmesiyle** sağlanır.

## Akış

1. Mevcut pipeline, yetkili manifesti ve dağıtım paketlerinin sealed byte'larını
   sabit public politikalarla doğrular. Mevcut scriptsiz provisioner bunları
   private staging dizininde açar; signed .PKGINFO'dan yerel ALPM metadata üretir.
   Üretim root sahipliği, package/provenance eşleşmesi ve review hash'i korunur.
   Yeni modül bu doğrulamaların veya paket extraction kurallarının yerine geçmez.
2. `root_stage.c` **host root'u olmadan** yeni kullanıcı/mount namespace'i içinde
   `fsopen/fsconfig/fsmount` ile detached tmpfs yaratır. Host mountpoint'i yoktur.
   Caller path'i, filesystem tipi, namespace veya keyring seçtirilmez. Yalnız
   mount FD gönderilir; namespace/context FD'leri gönderilmez. Helper dumpable
   değildir, parent ölürse sonlanır ve setup süresi 300 saniyeyle sınırlıdır.
3. `staged_root.py` authenticate edilmiş tam tree'den dosyaları tek tek, directory
   FD'lerinden no-follow ile kopyalar. İçeriği streaming SHA-256 ile karşılaştırır;
   boyut/mtime/ctime değişimini reddeder. Kullanılan açık dosyalar içerik sayısıyla
   artmaz. Tüm arşiv paketlerinin execution root'u bu tek filesystem'dir.
4. Bütün writer FD'leri kapanınca helper private reconfiguration context'iyle
   **superblock'u readonly** yapar, mount'a readonly/nosuid/nodev uygular ve çıkar.
   Mount-level readonly tek başına yeterli sayılmaz: başka writable alias aynı
   superblock byte'larını değiştirememelidir. Helper'ın context/namespace yetkisi
   yayımlanmaz. Supervisor readonly tree'yi tekrar ölçüp beklenen dönüşümle eşler.
5. Her fazda helper'ın `--run` yolu yeni private namespace'te `open_tree` ile
   readonly klon oluşturur. Yalnız o namespace'teki küçük attach tmpfs'i üzerinden
   klonu bwrap'a sunar. Bu, bwrap'ın proc-FD path ve inode/device yarış kontrolünü
   korur. Hosta mount yapılmaz. İlk fsmount open-file-description'ı fazlar boyunca
   tutulur; erken kapatılırsa kernel anonim mount namespace'ini yok eder.
6. Bwrap **bir root FD / bir root bind** ile başlar; `/proc`, `/dev`, `/tmp` ve
   kotalı `/build` için sabit sayıda ek işlem sürer. Ağ izolasyonu, nested-userns
   engeli, cap-drop, no-new-privs, timeout ve child limitleri korunur.

`QuotaWorker`, backend'i yalnız pipeline'ın authenticated manifestinden gelen
`root_schema=2` ile seçer. Schema 1 eski `FrozenTree` yolunda kalır. İstemciye
backend/helper path override'ı veya mutable-directory fallback eklenmedi.
`quota_mount.c --enter` yalnız bwrap ve sabit yeni helper executable'ını kabul
eder; setns bittikten sonra namespace FD'lerini exec öncesinde kapatır.

## Sınırlar ve riskler

| Risk | Kontrol |
|---|---|
| Traversal / symlink parent | Canonical relative path, 4096 byte / 128 bileşen tavanı; bütün parent'lar reviewed directory; no-follow directory FD erişimi |
| Symlink hedefi | Kopyalama sırasında takip edilmez; `/proc`, `/dev`, `/tmp`, `/build` symlink olamaz; diğer linkler final root namespace'inde çözülür |
| TOCTOU | Açık source FD'nin bytes/hash ve stat kontrolü; filesystem freeze sonrası ikinci tam tree ölçümü; bwrap'ın inode/device kontrolü |
| Hardlink | Provisioner schema 2'de yalnız aynı paketteki önceki regular dosyayı kopyalar; runtime staging de alias'ları bağımsız dosya olarak materialize eder |
| Sahiplik / privilege | Execution tree mapped builder UID'sine aittir; setid ve file write bitleri atılır; xattr/capability/ACL kopyalanmaz; nosuid/nodev ve cap-drop korunur |
| Özel dosyalar | Yalnız file/dir/link descriptor kabul edilir; gerçek FD type kontrolü; device/FIFO/socket reddi; private `/dev` bwrap tarafından hazırlanır |
| Byte/inode amplification | Signed schema 2: en fazla 2 GiB tmpfs, 65.536 giriş; tek dosya 256 MiB; synthetic mountpoint'ler de giriş bütçesine dahildir |
| İptal / hata | Chunk bazlı cancel ve 240 saniyelik staging deadline; helper kill/wait, socket/mount FD close; bozuk tree yayımlanmaz |
| Yeniden yazılabilir clone | Superblock readonly + mount readonly; başka userns'de clone/thaw/reconfigure/yazma negatif testi |

Kernel block allocation ve mantıksal içerik farklıdır: ölçülen root 1.366.293.539
logical byte, detached tmpfs'te 1.492.725.760 tahsis edilmiş byte kullanmıştır.
2 GiB tavanı inode/dizin/allocation maliyetini de kernel düzeyinde sınırlar;
çok küçük dosya bütçeyi daha erken tüketirse ENOSPC güvenli ret olur.
512 MiB build workspace'i ve 2G build scope sınırı artırılmadı. Root staging
belleği build scope başlamadan tahsis edilir; çoklu provider/eşzamanlı staging
aggregate memory kabulü henüz yapılmadı. Sonlu root bütçesi, cgroup ölçümü yerine
geçmez.

## Plan ve dağıtım uyumluluğu

897-byte source-plan schema 2 commitment'i aynen korunur. Plan orijinal reviewed
tree'ye bağlı kalır. Runtime tree'nin directory/file write/setid bitleri
normalleştirilir ve eksik sandbox mountpoint dizinleri eklenir; bunlar deterministik
bir dönüşümdür, yeni manifest veya receipt değildir. Source-plan schema 1 de
desteklenir. Signed manifest kapasite sürümüyle source-plan sürümü ayrıdır.

Yeni helper ve Python modülü yeni release staging girdilerine eklendi. Stage CLI
artık `--root-stage-helper` ister; önceki payload sessizce yeniden yazılmaz.
Preflight yardımcıların sabit root-owned executable path'lerini arar. Yeni kaynak
ve helper'ın dağıtımı ayrı paket/hash incelemesi gerektirir. Mevcut kurulu worker
bu yeni davranışı sunuyor sayılmaz.

## Gerçek test sonuçları ve sınırları

Kanıt dizini: `../build/aur-staged-root-20261101-accepted-tests/`. Adındaki
“accepted-tests” yalnız test koşusudur; JSON açıkça production_authorized=false,
signed_manifest_verified=false ve independent_rebuild=false içerir.

- 97 Python testi başarılı. Path/type/hash/kota/iptal retleri, 65.000 dosyalık
  descriptor ölçeği, hardlink alias ayrımı ve backend seçimi kapsanır.
- C yardımcıları `-Wall -Wextra -Werror` ile derlendi.
- Gerçek küçük-root probe'u mevcut quota namespace'iyle birlikte geçti: parent
  yazması EROFS; source freeze sonrası değişse de payload etkilenmedi; host FD,
  capability, host dosyası/PID/ağ erişimi yok; root salt okunur, workspace yazılır.
- Başka namespace'in readonly clone'u yazılabilir hâle getirme/superblock'u
  yeniden yapılandırma girişimi reddedildi.
- Gerçek **tarihsel imzasız yay prova root'u**: 60.393 reviewed giriş, 60.394 staged
  giriş, tek root aktarım FD'si, supervisor'da iki kalıcı root handle'ı, 53 toplam
  komut argümanı. Hazırlık ve iki `/usr/bin/true` başlatması yaklaşık 8,4 saniye.
- İki ayrı hazırlık aynı staged hash'i verdi:
  `2b8b5c3aa1042ab658ad45f56e5404ac7e2b34c0f8bd050d976bf6ace859cf54`.
  İkisi aynı kullanıcıyla yapıldı; **bağımsız attestor rebuild'i değildir**.
- Başarısız hash kopyasında helper/socket/root FD sızıntısı görülmedi.

İlk başarısız denemeler bwrap detached-FD çözümü, fsmount descriptor ömrü ve quota
namespace FD sızıntısını ortaya çıkardı; kontroller kaldırılmadan düzeltildi.
Testler execution sandbox dışına açık izinle çıktı; bütün yardımcılar hostta
non-root UID ile çalıştı. Gerçek paket kurulumu veya host mount değişikliği yoktur.

## Kalan üretim engelleri

FD/argv ölçek engeli kaynakta ve gerçek root provasında aşılmıştır. Yetkili güncel
manifest imzası, owner-approved review/template, root-owned signed provisioning,
tam makepkg paket build'i, ayrı hesaplarda bağımsız eş çıktı/metadata/cleanup ve
signed receipt hâlâ zorunludur. Native pacman archive trust, PolicyKit,
snapshot/boot/recovery ve çoklu provider kaynak kabulü hâlâ gerekir.
Hiçbir test bu kanıtların yerine geçirilmedi; üretim kapısı kapalıdır.

API kaynakları: [Linux fsopen/fsmount](https://man7.org/linux/man-pages/man2/fsopen.2.html),
[mount_setattr](https://man7.org/linux/man-pages/man2/mount_setattr.2.html).
