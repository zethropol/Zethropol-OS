# Host üzerinde zararsız worker sandbox/yaşam döngüsü testi

Bu, üretim worker'ı veya paket güncellemesi değildir. Her yetkili çalıştırma
öncesinde kullanıcıdan kapsamı belirtilmiş açık onay alınmalıdır.

## İncelenebilir parçalar

- inspect_host.py: yalnız root okumaları; birim/durum/hash/snapshot varlığı.
- probe.c: statik test programı. Dosya okumaları yalnız /proc mount/status ve
  önceden açılan özel sonuç dizini. /usr, /boot, varsa /efi yalnız statvfs ve
  mountinfo ile okunur; bu dizinlere hiçbir dosya yazılmaz.
- run_host.py: onaylanmış manifest ile yalnız benzersiz geçici birimleri
  başlatır/sinyaller/stop/restart yapar ve finally içinde kaldırır.

Program bir interpreter kullanmaz. Başlangıçtan sonra seccomp exec, socket,
mount, namespace ve başka dosya erişimlerini reddeder; openat yalnız önceden
alınmış sonuç dizini FD'sine izin verir. Üretilen dosya adları sabit önek,
sınırlı test etiketi ve systemd invocation ID'sidir. Programda paket API'si,
external command, system(), popen() veya başka bir programa exec yolu yoktur.
Seccomp engeli EPERM olarak doğrulanır; /usr/bin/pacman dosyası açılmaz.
Programın kendi ek kısıtı unit'in namespace veya sandbox ayarını gevşetmez.

## Hazırlık (yetkisiz, çalışma ağacı ve /tmp)

```
cc -static -O2 -Wall -Wextra -Werror \
  services/update/tests/host_worker/probe.c -o /tmp/zethropol-host-worker-probe
python3 services/update/tests/host_worker/run_host.py
```

Manifest /tmp/zethropol-host-worker-plan-*/manifest.json içindedir. Binary hash'i
sabitlenir; root çalıştırıcısı aynı byte'ları root-only /run test dizinine
kopyalar. Yetkisiz kullanıcı test dosyasını çalışma sırasında değiştiremez.

## Onay gerektiren işlem grubu

- Önce/sonra broker PID/durum, üretim binary/unit, Limine/katalog hash'leri ve
  snapshot 295/296/297 metadata/inode okumaları.
- Yalnız manifestteki /run/zethropol-host-worker-test-<id> dizinini oluşturmak,
  root:root 0700 sonuç dizini ve doğrulanmış statik programı oraya kopyalamak.
- systemd-run ile manifestteki dört benzersiz geçici unit'i oluşturmak.
  Üretim ExecStart/ExecStopPost çağrılmaz. Daemon-reload veya /etc dosyası yok.
- Namespace/sandbox ve yaşam döngüsü özelliklerini etkin üretim profiliyle
  karşılaştırmak. Fark veya beklenmeyen hata varsa hemen durmak.
- probe: yalnız /usr, /boot, /efi mount hazırlığı; normal çıkış ve stop-post.
- probe çalışırken PID 1, Codex sandbox ve test süreci mount namespace ID'leri;
  test PID'sinin /proc mountinfo satırları ve systemd D-Bus `ReadWritePaths`
  değerinin `as` imzası ile tek tek elemanları kaydedilir. `/usr` ve `/boot`
  VFS bayrakları ile üretim birimi profili doğrulanmadan sonraki aşama başlamaz.
- sigkill: yalnız kendi test unit'inin main sürecine SIGKILL; Restart=no,
  sonuç ve gerçek stop-post kaydı.
- cgroup: yalnız test programının SIGTERM'i yok sayan çocuğu; gerçek 30 saniye
  TimeoutStopSec ve KillMode=control-group ile grubun boşalması.
- restart: yalnız kendi test unit'ini bir kez açıkça restart edip iki farklı
  invocation gözlemek; sonra stop. Bu bir üretim operation retry testi değildir.
- Her unit için ExecStopPost yine aynı zararsız statik programdır.
- Nihayet yalnız oluşturulan test unit'lerine stop/reset-failed/unref;
  transient tanımları ve yalnız oluşturulan /run dizinini kaldırmak.
- Yalnız bu unit'lerin journal'ını ve sonuç dosyalarını manifestin /tmp
  dizinine kopyalamak. Üretim dosyası veya snapshot silinmez.

Unit'ler üretimin Type=exec, User/Group=root, Restart=no,
KillMode=control-group, TimeoutStopSec=30s, TimeoutStartSec=infinity ve bütün
namespace/hardening flag'larını kullanır. Teste özel farklar yalnız açıklama,
ExecStart/ExecStopPost programı ve güvenlik amaçlı RuntimeMaxSec=90s sınırıdır.
Etkin özelliklerin geniş listesi tekrar karşılaştırılır. Systemd kendi
PrivateTmp/cgroup/runtime/journal kaynaklarını geçici olarak yönetebilir.

## Açık onaydan sonra

```
pkexec --disable-internal-agent /usr/bin/python3 \
  "$PWD/services/update/tests/host_worker/run_host.py" \
  --run /tmp/zethropol-host-worker-plan-<onaylanan-dizin>
```

Çalıştırıcı yanlış sürüm, üretim unit hash'i, broker durumu, aktif worker,
profil farkı veya namespace/izin hatasında devam etmez. Hata durumunda daha
gevşek ayarla otomatik tekrar yoktur. Temizlik yalnız kendi isimleriyle yapılır;
herhangi bir temizlik hatası başarısız sonuçtur. Rapor/journal saklanır.

Bu test hostun gerçek worker sandbox'ını hazırlayıp hazırlayabildiğini ve
systemd'nin zararsız süreci yönetmesini gösterir. Gerçek pacman transaction,
Snapper/Limine hook'ları, paket veritabanı tutarlılığı, rollback veya boot
kurtarması doğrulanmış sayılmaz.

## Son onaylı host çalıştırması

`/tmp/zethropol-host-worker-plan-e5_x_pp4/` rapor/journal/ham sonuçları içerir.
Probe ExecStart çalıştı; 226/NAMESPACE görülmedi. Ancak `/usr` ve `/boot`
statvfs/mountinfo çıktısı ro olduğundan suite durdu. Üretimle metinsel profil
eşleşti. Ek salt okunur inceleme `/proc/1/mountinfo` dahil `/` ve `/boot`
VFS mount flag'ının ro, backing superblock seçeneklerinin rw olduğunu gösterdi.
Bu ortamın üst mount namespace'i salt okunur; Btrfs superblock ro değil.
`ProtectSystem=full` yolları birim içinde ayrıca koruyor ve ilgili test/üretim
ayar farkı bulunmadı. Hiçbir yazma testi veya güvenlik gevşetmesi yapılmadı.
Yeni onaylı denemede host PID 1 ve KDE `plasmashell` `mnt:[4026531832]`,
Codex sandbox `mnt:[4026532620]`, test prosesi `mnt:[4026532611]` idi. Gerçek
host `/` ve `/boot` rw; testin `ProtectSystem=full` namespace'inde `/usr` ve
`/boot` ro kaldı. D-Bus `ReadWritePaths` elemanları beklenen listeyle aynıydı,
ama yazılabilir mount istisnası fiilen oluşmadı. Probe durdu; diğer yaşam
döngüsü testleri çalışmadı. Ham sonuçlar `/tmp/zethropol-host-worker-plan-9qr3stnr/`
altında. D-Bus imza doğrulayıcısı düzeltildi fakat yeniden çalıştırılmadı.
Temizlik SIGTERM sonrasında ExecStopPost çalıştı; SIGKILL/cgroup/restart
testleri başlatılmadı. Geçici kaynaklar kaldırıldı, üretim önce/sonra aynı.
Tam suite sonucu başarısız/eksiktir; yeni host denemesi yeniden açık onay ister.
