# Hardware geliştirme ve doğrulama raporu — 2026-11-09

## Sonuç

Hardware veri toplama, D-Bus aktarımı, Qt köprüsü ve Hardware ekranı kaynak ağacında düzeltildi. Sistem geneline kurulum, paket güncellemesi veya servis dağıtımı yapılmadı. Kurulu Hardware servisi değiştirilmedi; yeni servis gerçek sistem kaynaklarını okuyarak özel D-Bus üzerinde çalıştırıldı ve test sonunda kapatıldı.

## İnceleme ve düzeltmeler

- Göreve başlanırken Hardware dahil çok sayıda yerel değişiklik vardı. Başlangıç diff'i ve dosya özetleri karşılaştırılarak ilgisiz değişikliklerin, mevcut testlerin ve `.bak` dosyalarının korunduğu doğrulandı. Commit/push yapılmadı.
- Köprü yalnızca ilk açılışta Hardware verisi okuyordu. Asenkron ilk okuma, 10 saniyelik yenileme, elle yenileme, 5 saniyelik D-Bus zaman aşımı, üst üste istek engeli ve hata sonrası otomatik toparlanma eklendi. Başarısız okumada eski veriler güncelmiş gibi gösterilmiyor.
- D-Bus adı, nesne yolu, `org.zethropol.Hardware.v1.GetState` yöntemi, `a{sv}` sonuç zarfı ve birincil `gpu` bölümü korundu. `gpus` listesi, `motherboard` ve ağ arayüzü bilgileri eklendi. Mevcut tüketicilerin kullandığı alan adları korundu; okunamayan ölçümler JSON'da null, D-Bus sözlüklerinde eksik anahtar olarak temsil ediliyor.
- GPU keşfi ilk kartta duruyor, eksik alt sistem/sürücü alanlarında kartı atlıyordu. Tüm DRM kartları ile DRM sürücüsü bağlanmamış PCI görüntü aygıtları taranıyor; PCI/DRM tekrarları eleniyor, kart sırası kararlı tutuluyor. Kart başına sürücü ve mevcut sensörler gösteriliyor. Çip kimliğinden perakende model veya VRAM tahmini kaldırıldı; yerel PCI kimlik veritabanı ve bilinmeyen aygıtlarda gerçek PCI kimliği kullanılıyor.
- CPU çekirdek sayısı soket/çekirdek çiftlerinden hesaplanıyor. AMD ve Intel CPU hwmon sensörleri destekleniyor; eksik hwmon adı sonraki sensörlerin okunmasını engellemiyor. CPU kullanımında guest zamanlarının iki kez sayılması düzeltildi. Eksik procfs verileri panik veya sahte kullanım yüzdesi üretmiyor.
- RAM ölçümleri eksik veya tutarsızsa sıfır ya da tamamen dolu RAM olarak gösterilmiyor.
- Kök depolama aygıtı tüm disk üzerinde dosya sistemi ve device-mapper üst aygıtları için de çözümleniyor. Diskin bulunması firmware sürümü alanına bağımlı değil. Linux `size` alanındaki 512 bayt sektör birimi kullanılıyor; mantıksal blok boyutuyla kapasiteyi hatalı çarpma kaldırıldı. Dosya sistemi kullanım alanları disk fiziksel kapasitesinden ayrı etiketlendi. Disk sıcaklığı yalnızca seçilen diskin hwmon sahibi eşleştiğinde alınıyor.
- Ağ hızları gerçek örnekleme süresinden Mbps olarak hesaplanıyor; bağlantı durumu veri yapısının varlığından çıkarılmıyor. Eski düz metin `--monitor` alan sırası ve ağ hızı birimi korundu. Mevcut ping ölçümü korundu; başarısız ölçüm sıfıra dönüştürülmüyor.
- Anakart ve BIOS bilgileri okunabilir DMI alanlarından alınıyor; seri numarası toplanmıyor.
- fwupd `updatable` bayrağı güncelleme bulunduğu anlamına gelmediği için sahte firmware güncelleme bildirimi kaldırıldı. GPU'nun veya diskin varlığı tek başına hızlandırma/güç yönetimi desteği sayılmıyor. Doğrulanmamış yetenekler açıkça belirtiliyor. Bilinmeyen sağlık durumu OK olarak sunulmuyor.
- Periyodik envanter okuması `pkexec` ile yetki istemiyor; SMART mevcut işlem yetkileriyle salt okunur deneniyor. Ayrı Diagnostics servisi, helper ve PolicyKit dosyaları değiştirilmedi. Erişilemeyen SMART alanları bilinmiyor olarak kalıyor.
- Hardware bölümü tema paletini kullanan, kaydırılabilir ve dar genişlikte tek sütuna geçen `HardwarePage.qml` bileşenine taşındı. Diğer 10 sekmenin Main.qml kaynak bölümleri başlangıç sürümüyle birebir aynı. Ortak SystemState biçimlendirmesi eksik/null alanları güvenli gösteriyor.

## Doğrulama

1. `cargo test --manifest-path services/hardware/Cargo.toml --offline --locked`: **17/17 başarılı**.
2. `cargo build --manifest-path services/hardware/Cargo.toml --offline --locked`: başarılı.
3. Control Center CMake yapılandırması ve `cmake --build desktop/control-center/build -j 2`: başarılı. Yeni bağımlılık yüklenmedi. Mevcut Qt policy/Vulkan başlık tespit uyarıları derlemeyi engellemiyor.
4. `ctest --test-dir desktop/control-center/build --output-on-failure`: **3/3 test paketi başarılı**; mevcut UpdateScanner ve UpdateInstaller testleri de geçti.
5. Hardware Qt testi gerçek Rust servisi → özel D-Bus → gerçek C++ köprüsü → paketlenmiş QML zincirini çalıştırdı. CPU modeli, anakart modeli, GPU PCI üreticisi ve disk kapasitesi doğrudan sistem dosyalarıyla karşılaştırıldı. Gerçek geliştirme `build/control_center` çalıştırılabilir dosyasının açılışı da sınandı.
6. Paketlenmiş Main.qml içindeki 11 sekme offscreen yazılım çizicisiyle gezildi; QML uyarı/hatası bulunmadı. Hardware sayfasının ekran görüntüsü alınıp incelendi. Hata sonrası otomatik toparlanma ve iGPU+dGPU/eksik sensör QML senaryoları da geçti.
7. `git diff --check`: başarılı. Görev başlangıcına göre yalnızca ilgili değişikliklerin diff'i ayrıca çıkarılıp incelendi.

## Gerçek sistemde okunan donanım

- CPU: AMD Ryzen 5 3600 6-Core Processor — 6 çekirdek / 12 iş parçacığı.
- GPU: Advanced Micro Devices, Inc. [AMD/ATI] Baffin [Radeon RX 460/560D / Pro 450/455/460/555/555X/560/560X]; sürücü `amdgpu`, sysfs VRAM 4.00 GiB.
- RAM: 31.26 GiB kullanılabilir fiziksel toplam (`/proc/meminfo`).
- Depolama: KIOXIA-EXCERIA SSD, `/dev/nvme0n1`, 500.108 GB.
- Anakart: Micro-Star International Co., Ltd. A520M-A PRO (MS-7C96).
- Ağ: `enp34s0`, bağlantı durumu `up`.

## Sınırlar ve kayıtlar

Bu fiziksel sistemde tek AMD GPU var. Intel iGPU + NVIDIA dGPU, eksik sürücü/alt sistem alanları, erişilemeyen sensörler ve PCI/DRM tekrarları kontrollü test verileriyle doğrulandı; fiziksel çift GPU üzerinde test yapılmadı. Diğer 10 sekme için açılış/bağlama regresyonu sınandı; güncelleme kurulumu, kurtarma, servis yönetimi veya paket kaldırma eylemleri tetiklenmedi.

Firmware güncelleme kullanılabilirliği, UEFI capsule desteği, aygıt başına Vulkan/OpenGL hızlandırması ve depolama güç yönetimi doğrulanmadığında destek varmış gibi gösterilmiyor. Birden fazla fiziksel diske dayanan belirsiz kök depolama eşleştirmesi bilinmiyor olarak bırakılıyor. Kök aygıt dışındaki tüm diskleri listeleyen yeni bir depolama yönetimi kapsamı eklenmedi. Kurulu servis eski sürümünde kaldığından yeni `gpus`/anakart alanları burada doğrulanan geliştirme servisine aittir.

Kayıtlar `build/hardware-verification-2026-11-09/` altında:

- `hardware-live.png`: Hardware ekranı.
- `hardware-live-bridge.json`: gerçek D-Bus verilerinin C++ köprüsünden alınmış anlık görüntüsü.
- `ctest.log`, `build.log`, `configure.log`: doğrulama çıktıları.
- `scope-review.json`, `task-changes.diff`: başlangıçla kapsam karşılaştırması ve yalnızca bu görevdeki değişiklikler.
