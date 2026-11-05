> Source architecture update: see [WORKER_ARCHITECTURE.md](WORKER_ARCHITECTURE.md) for the new worker profile, evidence states and source-only validation. The historical host runs below describe the installed 0.1.0-9 profile, not validation of the new source profile. No new host worker was started.

# Worker güvenlik doğrulaması — 2026-10-26

> Güncel sonuç: aşağıdaki **Host üzerinde onaylı deneme** bölümü esas alınmalı.
> Önceki başarılı store/proses testleri, systemd sandbox testinin yerine geçmez.

Bu testler paket güncellemesi veya üretim worker çalıştırılması değildir.
Kurulu 0.1.0-9 ve Control Center kaynak değişiklikleri korunmuştur.

## Host üzerinde onaylı denemeler

Kurulu 0.1.0-9 ve Control Center değişiklikleri korunmuştur. Üretim worker'ı,
TriggerUpdate, paket işlemi, snapshot işlemi, mount değişikliği veya servis
restart'ı yapılmadı.

### Gerçek host ve Codex sandbox ayrımı

Salt okunur root incelemesi gerçek PID 1 ile KDE `plasmashell` PID 769'un
`mnt:[4026531832]` namespace'ini doğruladı. Gerçek host `/` ve `/boot` VFS
bayrakları `rw`; `/` Btrfs `@` subvolume ID 539, `/boot` vfat. `/etc/fstab`,
`-.mount`/`boot.mount` generator birimleri ve Limine'nin varsayılan girişi
`rw rootflags=subvol=/@` ile uyumlu. Çalışan `/proc/cmdline` da `rw` ve `@`
gösteriyor. Kernel journal'ında Btrfs hata/uyarı, storage I/O veya read-only
remount hatası yok. Önceki `ro` host sonucu gerçek host değil, Codex sandbox
namespace'inden (`mnt:[4026532620]`) alınmıştı.

### Onaylı geçici worker testi

Plan `/tmp/zethropol-host-worker-plan-9qr3stnr/` altında; yalnız `probe`
transient unit'i başladı. Privileged runner ve gerçek PID 1 namespace'i
`mnt:[4026531832]`, Codex sandbox `mnt:[4026532620]`, test süreci ise ayrı
`mnt:[4026532611]` idi. Test birimi üretim profilinin karşılaştırılan ayarlarıyla
eşleşti. D-Bus `ReadWritePaths` elemanları beklenen `/usr`, `/etc`,
`/var/lib/pacman`, `/var/cache/pacman`, `/boot`, `/run/zethropol-update` ve
`-/efi` listesiyle aynıydı. Buna rağmen test PID'sinin mountinfo'sunda `/usr`
ve `/boot` mount flag'ları `ro`; kaynak superblock seçenekleri `rw` idi.
Dolayısıyla `ProtectSystem=full` sandbox'ı bu testte yolları salt okunur
bıraktı ve `ReadWritePaths` istisnasının fiilî etkisi kanıtlanmadı. `/usr`,
`/boot` veya başka sistem yoluna yazma denenmedi.

İlk doğrulayıcı D-Bus Array'in `signature` alanındaki eleman türü `s` değerini
tam dizi imzası `as` ile karıştırıp erkenden durdu. Rapor yine namespace,
mountinfo ve tüm `ReadWritePaths` elemanlarını kaydetti; okunan mount flag'ları
zaten izin kontrolünün başarısız olduğunu gösteriyor. Doğrulayıcı yerel olarak
`dbus.Array` türünü tanıyacak şekilde düzeltildi; bu düzeltme hostta yeniden
çalıştırılmadı. SIGKILL/cgroup/restart aşamalarına geçilmedi. Birim ve /run
çalışma dizini temizlendi, cleanup hatası yok.

Ham rapor: `/tmp/zethropol-host-worker-plan-9qr3stnr/report.json`. Önce/sonra
broker PID 40049, active/running, NRestarts=0 kaldı. Üretim binary/birim ve
Limine hash'leri, snapshot 295–297 metadata hash/dev/inode değerleri aynı.

Testte `ReadWritePaths` listesi D-Bus'ta mevcutken mountların neden ro kaldığı
henüz açıklanmış değil; bu bir host fstab, snapshot veya Btrfs hata durumu
olarak görünmüyor. Gerçek worker'ın yazma izinleri bu sonuçla doğrulanmış
sayılmaz. Sonraki adım, aynı zararsız transient probe ile systemd'nin mount
kurulum davranışını bu D-Bus ayarları altında ayırmak olmalı; düzeltme veya
yaşam döngüsü testleri için yeniden açık host onayı gerekir. Bu test gerçek
paket commit'i, Snapper/Limine kancaları veya kurtarma başarısı için kanıt
değildir. Gerçek güncelleme için hazır olduğu söylenemez.

`systemd-analyze verify` worker unit'i için başarılıdır. `systemd-analyze
security` 8.0 EXPOSED statik risk puanı verdi; bu, fiilî yazma erişimini
ölçmez.

## Yeni izole testler

`src/worker_integration_tests.rs`, broker_protocol altından yalnız `cfg(test)`
ile yüklenir. Üretim davranışı değiştirilmedi; paket yeniden üretilmedi.

Test ortamı yalnız özel bir D-Bus daemon'ı ve /tmp altında UUID ile ayrılmış
0700 çalışma dizinidir. BrokerStore doğrudan bu geçici records/ deposuna açılır;
open_runtime, system bus, production run_worker, systemctl veya paket/snapshot
executor'ları çağrılmaz. Test sonlandırması yalnız kendi test alt süreçlerine
uygulanır. Testin kendi geçici dosyaları dışında dosya temizliği yapılmaz.

- Özel bus fixture'ı gerçek mesaj başlığından unique sender, bus'tan UID/PID
  alır. Session ve yetki kararları fixture'dır; canlı logind/PolicyKit kanıtı
  olarak sunulmaz. Fixture Policy servisi deny/allow kararı döndürür ve hangi
  özgün sender/action için kontrol edildiğini kaydeder.
- Preview reddi kayıt oluşturmaz; install reddi preview'ı tüketmez veya queued
  işlem oluşturmaz. Daha sonra fixture izniyle aynı preview tüketilebilir.
- Aynı bağlantıdan eşzamanlı iki consume isteğinden yalnız biri başarılı olur;
  tekrar istek NoPreview alır. Diğer gerçek bus bağlantısı aynı UID'de bile
  işlem durumuna erişemez. İkinci pending işlem OperationInProgress alır.
- Geçersiz ID özel bus/store'da InvalidToken ile reddedilir. Üretim worker'ına
  hiçbir ID verilmez; üretim D-Bus başlangıç metodu çağrılmaz.
- Plan değişiminde üretim BrokerStore.claim_worker PlanChanged döndürür,
  kayıt failed olur ve tekrar claim reddedilir.
- Test binary'sinin yalnız fixture_worker_process testi yeni bir alt süreçte
  çalıştırılır. Çalışma dizini /tmp altında özel fixture prefix'iyle sınırlıdır.
  Bu süreç sadece lease/queued/claim işlemlerini yapıp bekler. Pacman/Snapper
  çağrısı yoktur. İkinci alt süreç lease alamaz ve sonucu değiştirmez.
- BrokerStore yeniden açıldığında tutulan lease running kaydını korur. Test
  alt süreci gerçekten SIGKILL ile sonlanır; lease bırakılınca reconciliation
  kaydı terminal failed yapar. Yeniden claim ve otomatik tekrar yürütme yoktur.
- Var olmayan fixture executable başlatılamaz; store'da worker-start-failed
  kaydı oluşur, queued/claim yoluna kendiliğinden geçilemez.

Bu bir test broker/policy/worker kabuğudur; ortak üretim kayıt ve lease kodunu
kullanır. Üretim UpdateManager -> PolicyKit -> systemd -> libalpm zincirinin
uçtan uca çalıştırıldığı veya paket commit'inin güvenli olduğu iddia edilmez.

## Çalıştırma ve sonuçlar

```
cargo check --locked
cargo fmt --check
cargo build --release --locked
cargo test --locked
```

Check/format/release başarılı. Tüm testler izinli sandbox dışı koşuda:
47 geçti, 0 başarısız, 2 ignored (49 kayıt). Önceki testler korundu.
Ignored kayıtları:
- Önceden mevcut canlı KDE identity testi: bu koşuda otomatik çalıştırılmaz.
- fixture_worker_process: doğrudan suite çalıştırması için ignored; süreç
  testleri bu testi --exact/--ignored ile iki ayrı child'da gerçekten çalıştırır.

Yeni üç esas test:
- isolated_bus_denial_ownership_replay_concurrency_and_plan_change
- subprocess_crash_broker_reopen_and_duplicate_lease_are_terminal
- missing_fixture_executable_records_start_failure_without_claim

Önceki geçici libalpm DB kilit çakışması, tüm plan alanları, UID/session,
token replay ve broker uzlaştırma testleri de geçti. Özel D-Bus sözleşme testi
dahil tüm regular testler çalıştı. Socket gerektiren testler sandbox dışı
çalıştırıldı; socket bind kısıtı bir yetki reddi sonucu sayılmaz.

## Canlı sistemde doğrulananlar

- Broker active/running, PID 40049, NRestarts=0; etkin fragment mevcut
  /etc/systemd/system/zethropol-update.service, drop-in yok.
- Worker template root:root 0644; Restart=no, KillMode=control-group,
  ExecStopPost --worker-stopped, root/root ve paket/kayıt yolları mevcut.
  Bunlar yapılandırma okumalarıdır, worker aktivasyonu değildir.
- D-Bus introspection/state query salt okunur doğrulandı. GetUpdateState
  introspection (bbsu), liste a(sssst), yeni integrity alanlı preview
  a(sssstsss), durum a{ss}; başlangıç
  metodu introspection'da görülse de çağrılmadı.
- Gerçek kullanıcı unique bus subject'inde flags=0 ile preview authorized=true;
  install authorized=false/challenge=true (auth_admin) oldu. Bu kesin ret
  değildir, kimlik doğrulama gereksinimidir.
- Önceden kullanıcıya istem açılabileceği bildirildikten sonra yalnız PolicyKit
  CheckAuthorization AllowUserInteraction ile install authorized=true döndü.
  Test bus bağlantısı kapatıldı; broker'ın başlangıç metodu çağrılmadı.
  Bu kontrol gerçek eylem/policy/agent yolunu kanıtlar; broker grant tüketimi
  veya worker başlatılmasıyla bağlandığını kanıtlamaz.
- Bilinmeyen ID için GetOperationState not-found; üretim worker'ına sahte
  işlem kimliği verilmedi.
- Hiçbir aktif/başlayan worker görülmedi; pacman kilidi yok. Son root-only
  okuma /run/zethropol-update altında operation kaydı olmadığını doğruladı;
  yalnız bir preview kaydı vardı. Runtime dizini 0700, worker unit'i root:root
  0644. Token içerikleri gösterilmedi veya değiştirilmedi.

Control Center yeniden derlendi; 3/3 Qt CTest hedefi izinli host koşusunda
başarılı. Mevcut KDE oturumunda son Qt istemcisi status/list/preview ve
bilinmeyen işlem sorgusunu exit 0 ile tamamladı. Aynı QML/UpdateService kullanan
geçici UI probe'unda gerçek broker'dan önizleme açıldı. Test düğmesi yalnız
Preview Plan idi; güncelleme/kurulum onayı çağrılmadı. Depo metadata'sı bu sırada
yenilendiğinden önceki native sorgu 10, sonraki görsel probe 11 aday gösterdi;
probe kendi listesi/preview'ı için count=11, previewReady=true doğruladı.
Yeni görsel /tmp/zethropol-worker-safety-live-preview.png altında; gerçek KDE
penceresinden grabWindow ile alındı ve incelendi. Üretim main.cpp değiştirilmedi;
probe yalnız geçici giriş noktasıyla mevcut QML/UpdateService kullanır.

## Önceki çalışmanın sınırları (son systemd lab denemesinden önce)

Önceki istekte servis değişikliği yasak olduğu için test birimi veya ikinci
systemd manager başlatılmamıştı. Son istek kapsamında özel kapsayıcı ve yalnız
yeni geçici lab birimleriyle aşağıdaki gerçek systemd denemesi yapıldı.

Bu nedenle şunlar henüz gerçek systemd ortamında sınanmış değildir:
- StartUnit/ExecStart hata yolu, ExecStopPost çalışması ve cgroup'daki bütün
  worker/hook çocuklarının durdurulması;
- fiili broker process restart'ı sırasında worker'ın bağımsız sürmesi;
- etkin worker namespace'inde Snapper, pacman hooks ve ayrı /boot/EFI izinleri;
- gerçek libalpm commit kesintisinin paket DB/dosya sonuçları ve kurtarma/boot.

Bunlar ayrı, izole VM/container systemd ortamında ve ayrıca yetkilendirilmiş
kontrollü uçtan uca testle doğrulanmalıdır. Üretim hostunda sahte ID denemesi
veya korumaları gevşetmek bu doğrulamanın yerine geçmez. Canlı install policy
artık doğrulanmış olsa da üretim başlatma/grant/claim/commit/sonuç zinciri henüz
canlı sınanmadı. Gerçek güncelleme güvenliği veya kurtarma garantisi verilmez.

Kurulu paket ve yerel birim hash'leri değişmedi:
- 0.1.0-9: f71d9919ecef5447f3ce8ff2442477eb9115dadec45c2105cd0f4343eec0d6d2
- /etc unit: dd7b85adc80fd53920b2f654a4f2b2605b6b13717e9659f32515e3f8ef6ea951

TriggerUpdate, üretim worker çalıştırma, paket kurulumu, gerçek güncelleme,
servis restart/değişikliği, snapshot/rollback veya Git işlemi yapılmadı.

Bu tur değişen dosyalar: src/broker_protocol.rs (yalnız cfg(test) modül kaydı),
src/worker_integration_tests.rs (yeni) ve WORKER_VALIDATION.md (bu belge).
Control Center kaynakları ve kurulu paket yeniden düzenlenmedi.

## Son gerçek systemd denemesi

**Hazırlık kararı: kontrollü gerçek güncelleme için henüz yeterli kanıt yok.**

Gerçek systemd PID 1, özel user/PID/mount/network/IPC/UTS/cgroup namespace'leri
ve kopyalanmış minimal OS ağacıyla nspawn kapsayıcısı kuruldu. Üretim worker'ı,
pacman ve Snapper bu test köküne kopyalanmadı; hosttan yazılabilir bind yoktu.
Ayrı ve benzersiz geçici host lab birimleri dışında mevcut birim değişmedi.

İlk denemede özel /run altındaki fixture dizini eksikti. Sonraki temiz OS
ağaçlarında standart geçici dizinler, özel journal ve dlopen ile yüklenen
libseccomp kütüphanesi tamamlandı. Son olarak /usr, /etc, /boot ve /efi guest
user namespace'ine ait tmpfs'lere kopyalandı. Güvenlik flag'ları kaldırılmadı.

Son gerçek hata:

```
Failed to set up mount namespacing: /boot: Operation not supported
Failed at step NAMESPACE spawning /usr/bin/python3: Operation not supported
ExecStart: status=226
ExecStopPost: status=226
```

Bu, özel nspawn/private-userns ortamında worker hardening profilinin
çalıştırılamadığını kanıtlar; hosttaki üretim worker'ının aynı hatayı vereceğini
kanıtlamaz. Kesin alt kernel/mount yetenek sebebi bu çalışma ile ayrıştırılmadı.
Private-users veya koruma ayarları kaldırılıp daha zayıf profil başarılı
sayılmadı.

| Davranış | Son gerçek systemd sonucu |
|---|---|
| İzole PID 1 ve yedi ayrı namespace | Doğrulandı |
| Unit yükleme ve ExecStart hazırlığı | 226/NAMESPACE ile başarısız |
| Başlatma hatasından sonra ExecStopPost planlanması | Gözlendi; gövdesi aynı 226 hatasıyla çalışamadı |
| Eksik executable 203/EXEC testi | BLOCKED — worker namespace ön koşulu sağlanmadı |
| Cgroup ve SIGTERM-direnen çocuk sonlandırma | BLOCKED — yürütülmedi |
| Sandbox yazma izinleri/PrivateTmp/NNP/AF filtresi | BLOCKED — gövde çalışmadı |
| SIGKILL ve Restart=no | BLOCKED — bu systemd suite'inde yürütülmedi |
| Terminal kayıtla açık restart | BLOCKED — yürütülmedi |
| Sentetik lock/plan guard'ı, systemd içinde | BLOCKED — yürütülmedi |

Sentetik plan değişimi ve geçici gerçek libalpm DB lock çakışması ayrı Rust
regresyonlarında geçti. Bunlar systemd testinin başarısızlığını kapatmaz.

Yeni kaynaklar: tests/systemd_lab/run_lab.py, suite.py, worker.py ve README.md.
Son ham sonuçlar /tmp/zethropol-systemd-lab-l7pzz2ii/{host-result.json,
guest-result.json,console.log}; açık FAILED/BLOCKED özeti
/tmp/zethropol-systemd-lab-final-summary.json. Önceki başarısız denemelerin
ayrı /tmp OS ağaçları da korunuyor. Fixture Python sözdizimi/whitespace geçti;
ulaşılamayan fixture senaryoları doğrulanmış sayılmıyor.

Bu tur regresyonları: cargo check, fmt --check, release başarılı; Rust
47 geçti/2 ignored, Qt derleme ve 3/3 CTest hedefi başarılı. Ignored kayıtlar
önceki canlı KDE testi ve süreç testlerinin child olarak çağırdığı helper'dır.

Her lab çalıştırmasında broker binary'si, broker/worker unit ve Limine hash'leri
önce/sonra aynı; broker PID'si aynı kaldı. Son canlı okumada PID=40049,
NRestarts=0, active; kurulu sürüm 0.1.0-9. Geçici lab veya üretim worker birimi
aktif/yüklü kalmadı. Snapshot 295/296/297 korunuyor. Paket, snapshot/rollback,
üretim worker, TriggerUpdate veya Git işlemi yapılmadı.

Alternatif: tüm yazma yolları atılabilir VM diskinde olan ayrı bir systemd VM
ve aynı hardening profiliyle bu vakaların tamamlanması. Hostta KVM var; hazır
/var/lib/machines imajı ve PATH'te QEMU/Podman/Docker yok. Yeni paket/imaj
kurulmadı veya indirilmedi. Ayrı VM yürütücüsü sağlanmadan ve gerçek
libalpm/hook/snapshot yolu ayrıca sınanmadan üretim güncellemesi için hazır
olduğu iddia edilmez. Ayrıntılı güvenlik sınırı tests/systemd_lab/README.md'de.
