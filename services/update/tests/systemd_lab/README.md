# İzole gerçek systemd worker laboratuvarı

**Sonuç (2026-10-26): başarısız / altyapı engelli.** Systemd PID 1 ve yedi
ayrı namespace doğrulandı; worker profili `/boot: Operation not supported`
ile `226/NAMESPACE` aşamasında durdu. Bu suite geçmiş veya skip edilmiş
başarılı test olarak sunulmamalıdır.

## Güvenlik sınırı

- `run_lab.py` yalnız systemd/Python ve gerekli kütüphaneleri yeni /tmp OS
  ağacına kopyalar; paket kurmaz. Pacman, Snapper ve üretim worker'ı bu ağaca
  girmez. Testte sentetik JSON planı ve özel DB lock dosyası kullanılır.
- Ayrı private user/PID/mount/net/IPC/UTS/cgroup namespace'leri, ayrı init,
  private-network, register=no, settings=no ve link-journal=no zorunludur.
  Hosttan yazılabilir bind mount yoktur. Korunan yollar son denemede guest
  user namespace'inin tmpfs'lerindedir; host verisi değildir.
- Yalnız benzersiz `zethropol-worker-lab-<uuid>` geçici host birimi oluşturulur;
  `Delegate=yes`, 150 saniye üst sınırı ve otomatik collection kullanır.
  Mevcut üretim birimleri yeniden yüklenmez/durdurulmaz/değiştirilmez.
- Host broker binary/unit, worker unit ve Limine hash'leri ile broker PID'si
  önce/sonra kontrol edilir. Özel kökten dışarı kaçan symlink reddedilir.
- Worker template üretim unit'inden alınır. Yalnız ExecStart/ExecStopPost test
  Python programına çevrilir; stdout/stderr özel sonuç dosyasına yönlendirilir.
  Güvenlik bayrakları, Restart=no, KillMode=control-group ve 30 saniyelik stop
  süresi korunur. Özel journal yalnız kapsayıcı içinde çalışır.
- Driver tüm namespace ve private OS marker kontrollerini geçmeden herhangi
  bir `systemctl start/stop/kill/restart/exit` işlemi yapmaz. Bu komutlar
  yalnız kapsayıcının özel /run/systemd/private soketine gider.

## Komutlar

```
python3 services/update/tests/systemd_lab/run_lab.py
# Çıktıdaki YENİ özel /tmp dizini ile, yetkili bakım oturumunda:
pkexec --disable-internal-agent /usr/bin/python3 \
  "$PWD/services/update/tests/systemd_lab/run_lab.py" --run /tmp/zethropol-systemd-lab-<yeni-dizin>
```

Bir testi düzeltmek için `--private-users` kaldırılmamalı veya üretim unit'inin
ProtectSystem/ProtectHome/ProtectControlGroups ayarları gevşetilmemelidir.
Başarısız OS ağaçları ve JSON/console logları inceleme için korunur. Aynı kirli
OS ağacı tekrar kullanılmak yerine yeni prepare çıktısı kullanılmalıdır.

## Planlanan ancak bu ortamda bloke olan vakalar

- Normal ExecStart ve terminal sonucu koruyan ExecStopPost
- Eksik executable için 203/EXEC ve stop-post
- RW istisnaları, PrivateTmp, ProtectHome/kernel/cgroup, NNP, AF_PACKET reddi
- Sentetik pacman kilidi ve plan değişiminde sıfır sentetik işlem
- SIGKILL sonrası stop-post ve Restart=no
- Terminal kaydı açıkça restart etmenin tekrar yürütme yapmaması
- SIGTERM'i yoksayan çocuğun gerçek 30 saniyelik stop timeout sonunda cgroup'dan
  tamamen çıkarılması

Son deneme ilk worker'ın gövdesine ulaşmadı. Yalnız unit yükleme, ExecStart ve
ExecStopPost için **namespace hazırlık hatası** gerçek systemd ile gözlendi.
Sonraki vakalar `BLOCKED / yürütülmedi`; bu fixture senaryoları henüz bütünüyle
çalıştırılmış/kanıtlanmış uygulama kodu olarak değerlendirilmemeli.

Son kanıtlar:
- /tmp/zethropol-systemd-lab-l7pzz2ii/host-result.json
- /tmp/zethropol-systemd-lab-l7pzz2ii/guest-result.json
- /tmp/zethropol-systemd-lab-l7pzz2ii/console.log
- /tmp/zethropol-systemd-lab-final-summary.json

## Alternatif altyapı

Tam hardening profiliyle bind/mount namespace RW geçişlerini destekleyen tek
kullanımlık bir systemd VM gerekir. Gerçek host diskleri paylaşılmamalı;
`/boot`/EFI/pacman DB/cache dahil tüm yazma yolları VM'nin atılabilir diskine
ait olmalı. Aynı fixture unit'i, sentetik plan/lock ve child süreçleri orada
çalıştırılmalı; namespace guard'ı kaldırılmak yerine VM izolasyonunu doğrulayan
ayrı, denetlenmiş bir VM yürütücüsü hazırlanmalıdır. Bu repo harness'i şu an
nspawn backend'idir; çalışan bir VM backend'i varmış gibi sunulmaz.

Hostta KVM aygıtı mevcut, ancak PATH'te QEMU/Podman/Docker ve hazır
/var/lib/machines imajı bulunmadı. Bu çalışma bunları kurmadı veya indirmedi.
Gerçek libalpm/hook/snapshot/restore yolu bu sentetik suite'ten ayrıca,
yetkilendirilmiş VM uçtan uca doğrulaması gerektirir.
