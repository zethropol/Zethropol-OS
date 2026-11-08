**Control Center — Update düzenlemeleri ve çalışırlık raporu**

Tarih: 8 Kasım 2026. Kapsam: yerel kaynak Control Center, güncelleme taraması, uygulama içi kurulum, kurulum sonrası bakım ve eski servis temizliği.

Kaynak uygulama derleniyor; otomatik tarama ve gömülü kurulum etkileşimi doğrulandı. Yeni arayüz üzerinden gerçek paket kurulumu uçtan uca tamamlanarak doğrulanmış değildir. Bu nedenle mevcut durum, çalışan geliştirme sürümü olarak değerlendirilmelidir.

Çalışılan kaynak dizini `/home/Zevor/Zethropol-OS/desktop/control-center/`, çalıştırılabilir dosya `build/control_center` konumundadır. Kurulu Control Center değiştirilmedi; sistem geneline uygulama kurulumu yapılmadı.

Yapılan düzenlemeler ve doğrulama kapsamı:

| Düzenleme | Mevcut davranış | Çalışırlık / kanıt |
|---|---|---|
| Update navigasyonu | Mevcut navigasyon girişi korundu; güncelleme raporu eklendi. | Kaynak uygulama açıldı; arayüz derlemesi başarılı. |
| Açılış taraması | Uygulama açılınca, sekmeye geçilmesi beklenmeden ilk tarama başlıyor. | Otomatik başlangıç testi geçti; önceki canlı taramada paket sonuçları görüldü. |
| Resmî depo taraması | `checkupdates` ayrı geçici veritabanıyla çalışıyor. | Komut ve geçici veritabanı kullanımı test edildi. Tarama ana paket veritabanını eşitlemiyor. |
| AUR taraması | Yapılandırılmış yardımcı veya mevcut yardımcılar arasından seçilen araçla güncellemeler listeleniyor. | Paket ayrıştırma, yok sayılan paketlerin elenmesi ve kaynak kapatma ayarları test edildi. |
| Flatpak taraması | Uzak kaynaklardaki güncellemeler listeleniyor. | Komut, sonuç ayrıştırma ve sürüm bilgisi boş satırlar test edildi. |
| Rapor görünümü | Kaynaklar ayrı kartlarda; paket adı, mevcut/yeni sürüm, durum ve son tarama zamanı gösteriliyor. | Canlı arayüz ve kontrollü QML incelemesi yapıldı. |
| Tarama hataları | Bağlantı hatası, başlatma hatası, okunamayan çıktı ve zaman aşımı ayrı bildiriliyor. | Hataların “güncel” olarak gösterilmediği test edildi. |
| Yeniden tarama | Kullanıcı yeniden tarama başlatabiliyor; eski sonuçlar temizleniyor. | Yinelenen isteklerin engellenmesi ve sonuçların yenilenmesi test edildi. |
| Kurulum başlangıcı | “Güncellemeleri kur…” düğmesi ayrı onay penceresi açıyor. | Onaysız başlamama, vazgeçme ve açık onayla başlama QML testinde doğrulandı. |
| Uygulama içi kurulum | Harici terminal penceresi açılmıyor; mevcut sistem güncelleme akışı bir PTY üzerinden çalıştırılıyor. | Gerçek PTY üzerinde kontrollü yardımcı programlarla test edildi. |
| Canlı kurulum çıktısı | İşlem çıktısı Update sekmesinde gösteriliyor; renk/kontrol dizileri ayıklanıyor. | Çıktı aktarımı ve ANSI ayıklama test edildi. |
| Parola ve yanıt alanı | Giriş maskeli; gönderilen ve işlem sonunda gönderilmeden kalan metin temizleniyor. | Test parolasının çıktıya yansımadığı ve alanın temizlendiği doğrulandı. Gerçek yönetici parolasıyla kurulum testi yapılmadı. |
| Tek tuş etkileşimi | Satır yanıtı, tek tuş ve Esc gönderimi mevcut. | Tek tuşun sonraki soruya fazladan Enter bırakmadığı test edildi. |
| Eşzamanlı işlem kontrolü | Kurulum sürerken yeni tarama ve ikinci kurulum başlatılamıyor. | Yinelenen başlangıç ve tarama isteklerinin engellenmesi test edildi. |
| Normal pencere kapatma | Kurulum sürerken normal kapatma isteği reddediliyor ve açıklama gösteriliyor. | Kodda mevcut. Elektrik kesintisi, SIGKILL veya uygulama çökmesine karşı koruma olarak değerlendirilmemeli. |
| İşlem sonu | Başarılı çıkış, iptal, hata ve beklenmedik sonlanma ayrı durumlarla bildiriliyor; ardından tarama yenileniyor. | Kontrollü çıkış, iptal, kesinti ve başlatma hatası senaryoları test edildi. |
| Adlandırmalar | İstenmeyen araç adı arayüzden kaldırıldı; çıktı alanında da sadeleştiriliyor. | Kaynak incelemesi yapıldı. |

Önceki canlı taramada resmî depolardan `glances`, AUR’dan `google-chrome` güncellemesi listelendi; Flatpak güncellemesi bildirilmedi. Bu gözlem güncel paket envanteri olarak kullanılmamalıdır: rapor hazırlanırken yeni bir tarama veya kurulum başlatılmadı.

Kurulum sırası mevcut sistem araçlarına dayanır: güncellemelerin tekrar kontrolü ve kullanıcı incelemesi, resmî depoların tam sistem yükseltmesi, etkin AUR yardımcısı ve Flatpak güncellemeleri. Control Center yeni bir root servis veya ayrı paket yöneticisi uygulamaz. Yetkilendirme mevcut sistem araçları üzerinden gerçekleşir.

Kurulum sonrası bakım davranışları:

| Aşama | Koşul ve kullanıcı onayı | Doğrulama düzeyi |
|---|---|---|
| Yetim paket kontrolü | `pacman -Qtdq` ile listeleme; açık kabul halinde `pacman -Rns`. Boş yanıt/Enter kaldırmayı reddeder. | Kurulu betik incelendi; gerçek kaldırma yapılmadı. |
| Yetim kontrolünün zamanı | AUR güncellemesi varsa önce onun öncesinde; sonrasında liste değişmişse tekrar. AUR güncellemesi yoksa son bakım aşamasında. | Kurulu akış incelendi. |
| Paket önbelleği | Önce adaylar belirlenir, ardından temizleme sorulur. Boş yanıt/Enter kabul anlamındadır. Varsayılan olarak kurulu paketler için son üç önbellek sürümü tutulur; kaldırılmış paketler için saklama sayısı sıfırdır. | Kurulu betik ve varsayılan ayarlar incelendi; gerçek temizlik yapılmadı. |
| AUR derleme önbelleği | Yukarıdaki paket arşivi temizliğinin kapsamı dışındadır; yardımcının kendi davranışına bağlıdır. | Ayrı kapsam olarak belirlendi. |
| Kullanılmayan Flatpak bileşenleri | Destek etkinse ayrı bakım aşamasında değerlendirilir. | Kurulu akış incelendi; gerçek kaldırma yapılmadı. |
| Servis yeniden başlatma / reboot önerileri | Mevcut bakım akışında kullanıcı etkileşimine bağlıdır. | Akış incelendi; bu geliştirmeleri doğrulamak için servis yeniden başlatma veya bilgisayarı yeniden başlatma yapılmadı. |

Bakım aşamasına ulaşılması bütün kaynakların başarılı olduğunu kanıtlamaz. Resmî paket güncellemesi hatası akışı durdururken AUR veya Flatpak hatası sonrasında bakım adımları devam edebilir. Ayrıca alttaki aracın sıfır çıkış kodu her paketin kurulduğunu garanti etmez. Bu nedenle arayüz oturumun bittiğini bildirir; kesin paket başarısı yerine yenilenmiş raporu ve gerçek işlem çıktısını esas alır.

Kurtarma ve kesinti bakımından tespit edilen durum:

| Konu | Mevcut durum |
|---|---|
| Paket işlemi öncesi snapshot | Sistemde `snap-pac` ön hook’u ve Snapper `root` yapılandırması mevcut. Snapshot, paket dosyaları değiştirilmeden önce alınacak şekilde yapılandırılmıştır; düğmeye basılır basılmaz uygulanan ayrı bir Control Center adımı değildir. |
| Geçmiş snapshot kanıtı | 8 Kasım’daki önceki sistem yükseltmesinin loglarında işlem öncesi `306`, işlem sonrası `307` numarası bildirilmiştir. Bu, yeni arayüzün bir kurulum yaptığına veya bu snapshot’ların halen saklandığına dair kanıt değildir. |
| Zorunlu snapshot doğrulaması | Control Center snapshot kimliğini ayrıca alıp doğrulamıyor. Kurulu yardımcı betik Snapper alt komutunun çıkış durumunu kontrol etmediğinden hook’taki `AbortOnFail` tek başına mutlak güvence vermiyor. |
| Ortam etkisi | Kurulum süreci sistem ortamını devralıyor; snapshot atlama değişkeni uygulama tarafından ayrıca temizlenmiyor. |
| Flatpak’a özel güncelleme | Yalnızca Flatpak işlemi yapılırsa pacman snapshot hook’u çalışmaz. |
| Otomatik rollback | Control Center’da hata veya kesinti üzerine otomatik geri dönüş uygulanmıyor. |
| Kapsam | `root` hedefi `/`; `/boot` ayrı dosya sistemi, `/home` ayrı alt birimdir. Root snapshot’ı tek başına bu alanların korunduğunu kanıtlamaz. Ek boot kurtarma mekanizmaları bu çalışma kapsamında doğrulanmadı. |
| Zorunlu kesinti | Elektrik kesintisi, zorla süreç sonlandırma ve gerçek paket işlemi ortasında çökme senaryoları uygulanmadı; sistemin bu durumlarda bozulmayacağı iddia edilemez. |

Gömülü çıktı alanı tam ekran terminal emülatörü değildir. Metin soruları, parola girişi ve tek tuş yanıtları test edilmiştir; tam ekran editörlerle paket tarifi düzenleme gibi etkileşimler doğrulanmamıştır. Control Center’ın gösterdiği işlem çıktısı bellekte 256 × 1024 karakterle sınırlıdır ve ayrı kalıcı çıktı dosyasına yazılmaz. Paket yöneticilerinin kendi sistem kayıtları bundan bağımsızdır.

Test ve derleme kanıtı:

| Kontrol | Sonuç |
|---|---|
| Kaynak uygulama derlemesi | Başarılı. `build/control_center` mevcut; son derleme 8 Kasım 2026, 12:28. |
| Tarama test hedefi | Geçti: 5 işlevsel senaryo. |
| Kurulum test hedefi | Geçti: veri satırları dahil 7 işlevsel senaryo. |
| Toplam | İki test hedefi, 12 işlevsel senaryo; başarısız test yok. Hazırlık/kapanış yordamları bu sayıya dahil değildir. |
| Arayüz etkileşimi | Onay, vazgeçme, çıktı gösterimi, maskeli giriş ve yanıt gönderimi kontrollü testte geçti. |
| Diğer sekmeler | 11 navigasyon sayfası korundu; Update dışındaki dokuz ayrıntı sayfasının kaynak içeriği önceki kayıtla karşılaştırılarak aynı bulundu. Overview’daki Update’e özel eski bağlantılar önceki temizleme aşamasında kaldırılmıştı. |
| Gerçek paket kurulumu | Yeni gömülü akışta uçtan uca doğrulanmadı. Chrome güncellemesinin bu akışla başarıyla kurulduğuna dair kanıt yok. |
| Testlerin niteliği | Kurulum etkileşimi gerçek PTY ve kontrollü yardımcı programlarla sınandı; testler sistem paketlerini güncellemedi veya kaldırmadı. |

Test kanıtı mevcut `build/Testing/Temporary/LastTest.log` kaydından alınmıştır. Bu rapor için testler, temizlik, snapshot veya paket kurulumu yeniden çalıştırılmadı.

Eski servis ve korunan bileşenler:

- Eski `zethropol-update` paketi kaldırıldı. Ana servis ve Worker birimleri, çalıştırılabilir dosyası, D-Bus etkinleştirme/politika dosyaları ve ona özel PolicyKit dosyası kaldırıldı.
- Rapor hazırlanırken ana servis `LoadState=not-found`, `ActiveState=inactive` olarak doğrulandı; `org.zethropol.Update` D-Bus sahipliği bulunmuyor.
- Mevcut Control Center tarama ve kurulum kaynakları bu servise bağlı değil.
- Pacman, paru, Flatpak, Snapper ve `snap-pac` korundu. Doğrulanan sürümler sırasıyla `7.1.0.r9.g54d9411-5`, `2.2.0-1`, `1:1.18.4-2.1`, `0.13.2-1.1` ve `3.0.1-3`.
- Eski işlem/kurtarma kayıtları, özel anahtarlar ve kullanıcı yedekleri servis kaldırma kapsamında silinmedi.
- Hardware, Recovery, ZCI ve diğer modüller için yeni geliştirme veya sistem dağıtımı yapılmadı.

Kaynak ve yayın durumu:

Yeni uygulama kodu `src/UpdateScanner.cpp`, `src/UpdateInstaller.cpp`, bunların başlıkları ve `qml/UpdateReport.qml` dosyalarındadır. Bağlantılar `src/main.cpp`, `qml/Main.qml` ve `CMakeLists.txt` içinde; testler `tests/UpdateScannerTest.cpp` ve `tests/UpdateInstallerTest.cpp` dosyalarındadır.

Bu raporun yayın tabanı, önceki Update kaynaklarının temizliğini içeren `d958660c662718c6ea22f4913eddef7244ffdf9b` commit’idir. Yeni tarama, uygulama içi kurulum kaynakları, ilgili testler ve bu rapor aynı yayın commit’ine dahil edilmiştir. İlgisiz yerel değişiklikler yayın kapsamı dışındadır. Nihai commit kimliği Git geçmişinden görülebilir.

Mevcut değerlendirme: otomatik tarama, raporlama ve uygulama içi kurulum etkileşimi çalışır geliştirme düzeyindedir. Gerçek paket kurulumunun uçtan uca başarısı, zorunlu kurtarma noktası doğrulaması ve kesinti sonrası geri dönüş güvencesi henüz doğrulanmış kapsamda değildir.
