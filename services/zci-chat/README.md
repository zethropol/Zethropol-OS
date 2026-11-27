# ZCI bağımsız sohbet bileşeni

Kaynak bileşeni, kurulu llama.cpp CPU motorunu ve mevcut Qwen3-4B GGUF dosyasını
kullanır. Model veya paket indirmez; sistem servisi kurmaz. ZEKANS'ın 1.7B
başlatıcısını, 8087 bağlantısını ve güvenlik mekanizmalarını kullanmaz/değiştirmez.
Bu dizin kaynak geliştirmesidir; mevcut masaüstü kısayolu veya WebUI ayarları
otomatik değiştirilmez.

## Bağlantı ve kullanım

```text
Open WebUI → 127.0.0.1:18086/v1 → yalnız Qwen3-4B, 127.0.0.1:18087
ZEKANS     → mevcut 127.0.0.1:8087/v1 → mevcut Qwen3-1.7B
```

Portlar `config.example.json` üzerinden seçilir; yalnız loopback dinlenir.
8087, 8088 ve mevcut belge okuyucusunun 8089 portu ayrılmıştır ve reddedilir.
Aktif bir dinleyici yeniden kullanılmaz. Kapanmış bağlantıların TIME_WAIT durumu
standart SO_REUSEADDR ile karşılanır; SO_REUSEPORT kullanılmaz.

Gerekli yerel yolları gözden geçirerek, normal kullanıcıyla ön planda çalıştırma:

```sh
python3 -I -B services/zci-chat/zci_chat.py --config services/zci-chat/config.example.json
```

Bu komut yalnız 4B motorunu ve sohbet bağlantı katmanını başlatır. Ctrl-C/SIGTERM,
başlatma hatası ve normal kapanış, yalnız sahip olunan süreç grubunu ve açık
model bağlantılarını temizler. SIGKILL, sistem çökmesi ve kötü niyetli aynı-UID
host süreçlerine karşı tam temizlik/kimlik güvencesi verilmez. `/tmp/zci-chat-*`
altındaki küçük motor günlükleri tanı için tutulur; sohbet metinleri ve HTTP
kimlik doğrulama başlıkları gateway tarafından günlüğe yazılmaz.

32.768 bağlam, bir slot, altı CPU iş parçacığı ve GPU=0 korunur. Başlatmada en az
12 GiB kullanılabilir RAM gerekir. Çalışırken kullanılabilir RAM 6 GiB altına
veya sahip olunan motor RSS'i 18 GiB üstüne çıkarsa yalnız o motor durdurulur.
İkinci modeli otomatik başlatan yol yoktur. İki modelin RAM ölçümü önceki
prototipte açık test seçeneğiyle yapılmıştır; bu bileşen o testi tekrar etmez.

## API sözleşmesi

- `GET /health`, `GET /v1/models`, `POST /v1/chat/completions`.
- İki açık profil: `zci-chat-qwen3-4b` ve `zci-chat-qwen3-4b-thinking`.
  Her ikisi aynı sabit 4B dosyasına gider; başka alias veya 8087 fallback'i yoktur.
- Model alias'ı, dosya yolu, 32K bağlam ve sohbet şablonu kontrol edilir.
  Başlatıcı, zorunlu ve açıkça yapılandırılmış SHA-256 ile model dosyasını doğrular.
  Kontroller güvenilir aynı-UID host varsayımına dayanır; HTTP kontrolleri arasında
  kötü niyetli motor değişimini atomik olarak engelleyen bir imza sistemi değildir.
- JSON ve SSE desteklenir. Düşünme alanları kullanıcı yanıtına aktarılmaz;
  içerikteki düşünme etiketleri, SSE parçaları arasında bölünmüş olsa da tutulur.
- Gerçek şablon ve tokenizer aynı 4B endpoint'indedir. Düşünme anahtarı sayım ve
  üretimde aynıdır. Giriş + ayrılan çıktı ≤32768; çıktı rezervi 1–768 token.
- Özgün `system.md` bir kez eklenir; özel sistem bağlamı ve son kullanıcı
  mesajları korunur. Otomatik özet, sahte hafıza, budama veya yeniden deneme yoktur.
- Desteklenen giriş düz metindir. Araçlar, görseller ve keyfi şablon/endpoint
  alanları reddedilir. `temperature`, `top_p`, `top_k`, `min_p`, `seed`, frekans/
  varlık cezaları ve sınırlı `stop` dizileri doğrulanarak aktarılır.
- Varsayılan sıcaklık düşünmesiz profilde 0.2, düşünmelide 0.6'dır. Bunlar kalite
  garantisi değildir; kullanıcının desteklenen açık sampling ayarları korunur.
- Boş veya kesilmiş yanıt `zci_status=incomplete` alır ve metne görünür Türkçe
  bildirim eklenir. Böylece özel alanı bilmeyen WebUI de durumu gösterir.
  Ağ kesilmesinden önce aktarılmış kısmi yanıt geri alınamaz; kesilme bildirimi
  eklenir. Bağlantı ilk başta kurulamazsa anlaşılır HTTP hatası döner.
- Aynı anda bir çıkarım kabul edilir; ikinci istek 429 alır. İstemci iptalinde
  model akışı kapatılır. Gizli düşünme sırasında yalnız metin içermeyen SSE
  bekleme yorumları kullanılır. İptal, ağın yeni veri/yazma bildiriminde gözlenir;
  tam atomik veya sıfır gecikmeli iptal iddiası yoktur.

## Open WebUI 0.13.0 doğrulaması

Kurulu Open WebUI kaynakları ve salt okunur, yalnız herkese açık yönlendirme
alanları incelendi. Mevcut WebUI sağlayıcısı 8087'yi, varsayılanı 1.7B alias'ını
kullanıyor. Model bazlı eski filtre 1.7B'ye bağlı. Özgün istem kaynakla eşleşiyor
(yalnız son satır sonu farklı). Gerçek kullanıcı sohbet/hesap kayıtları okunmadı.

`live_acceptance.py --run`, kurulu WebUI Python ortamıyla çalışır. Yeni `/tmp`
veritabanı, statik dosya hedefi, HOME ve Firefox profili oluşturur. Dotenv yükleme
kapalıdır; gerçek ayarlar dosyası ve veritabanı kullanılmaz. Yalnız sentetik bir
test hesabı oluşturulur; onun test oturum değeri raporlanmaz.

```sh
~/.local/share/zci/webui/venv/bin/python -I -B services/zci-chat/live_acceptance.py --run
```

İzole WebUI 18088, Firefox kontrolü 18092 üzerinde çalışır. Gerçek 4B ile JSON,
tarayıcı üzerinden SSE sohbeti, profil seçimi, tek-token rezerviyle kontrollü
kesilme bildirimi ve yalnız test motoru kapatıldıktan sonra görünür bağlantı
hatası doğrulanır. Tek-token testi modelin sohbet kalitesi kabulü değildir.
Başlangıç/kapanış, istemci iptali ve akış kopması ayrıca geçici HTTP fikstürleriyle
test edilir. Başarısız/pahalı 15 turluk kalite kıyasları tekrarlanmaz.

```sh
python3 -I -B services/zci-chat/test_zci_chat.py
```

## Kalıcı etkinleştirme için hazırlanmış, uygulanmamış adımlar

1. `config.example.json` yerel motor/model yollarını ve çakışmayan portları seçer.
   Çalıştırma normal kullanıcı ve ön plan komutuyla yapılabilir; systemd kurulumu yoktur.
2. Yetki verildiğinde, gerçek WebUI'ye `webui-connection.example.json` içindeki
   ayrı sohbet sağlayıcısı eklenir; mevcut 8087 bağlantısı ve hesaplar korunur.
   Yerel bağlantı gerçek bir API anahtarı gerektirmez. WebUI boş alanı kabul
   etmezse yalnız yeni bağlantıya anlamsız `local` değeri verilir; mevcut anahtarlar
   kopyalanmaz veya raporlanmaz.
3. Yeni ZCI varsayılanı açıkça seçilir; eski 1.7B filtresi yeni modele bağlanmaz.
   Sayım ve düşünme ayrımı gateway'dedir. Özgün istem bir kez eklenir; sohbet
   özetleme/hafıza ve araç özellikleri açılmaz.
4. ZCI masaüstü başlatıcısı ayrıca yetkiyle değiştirilmelidir: mevcut
   `start-chat.py` sohbet için 8087 modelini otomatik başlatıyor. Yeni yol yalnız
   bağımsız sohbet bileşenini kullanmalı; ZEKANS gerektiğinde kendi yoluyla açılmalı.

Bu adımların hiçbiri mevcut 8087/8088'e uygulanmadı. Beş açık delegasyon/cgroup
testi bu bileşenin kabulüyle geçmiş sayılmaz; yeni yetki veya delegasyon eklenmez.

## Türkçe kalite sınırı

Başarısız kısa istem veya ek güvenlik paragrafı kullanılmadı. “12 bayt” uydurma
yanıtı negatif regresyon verisi olarak korunur. Doğrudan model çıktısındaki niyet,
doğal Türkçe, mantık ve biçim sorunları ile taşıma/işleme hataları ayrıdır.
Bu bileşen sayı doğruluğunu tahmin eden veya modelin cevabını sonradan uydurarak
onaran bir filtre değildir. Gerçek modelin genel Türkçe kalite kabulü hâlâ açıktır.

### 2026-11-15 konuşma bağlamı incelemesi

Kullanıcının izin verdiği, iki örnek sorunun geçtiği 4B deneme sohbeti salt okunur
incelendi. Geçici WebUI'de aynı ilk iki yanıt yeniden üretildi: model ilk soruyu
yanıtlamak yerine kendini tanıttı; ikinci turda tanıtımı ve kapanış sorusunu
tekrarladı. İlk dört turda WebUI'nin gönderdiği kullanıcı/asistan geçmişi ile
gateway'in şablona ve modele verdiği geçmiş birebir eşleşti. Sistem istemi bir
kez eklendi; dördüncü turda iş arkadaşı bilgisi hatırlandı. Bağlamın silindiğini
gösteren kanıt bulunmadı.

Özgün istemin yetenek sınırları korunarak kimlik, selamlama, bağlam ve belirsizlik
kuralları için geçici bir aday denendi. Kendini tanıtma azaldı; selamlama, tekrar,
istenen tek cümle biçimi ve uydurma bilgi sorunları sürdü. Her iki istemde de
hayalî protokol sorusuna dayanağı olmayan “928 bayt” yanıtı verildi. Aday kaynak
`system.md` dosyasına veya çalışan hizmete uygulanmadı. Bu ölçüm düşünmesiz
profilde, sıcaklık 0.2, seed 7 ve 384 çıktı tokenı ile yapıldı; kapsamlı bir model
kıyaslaması değildir.

Tarayıcı testinin eski baloncukları sayması ve Markdown metnini ham SSE metniyle
karşılaştırması iki yanlış zaman aşımına yol açtı. Model yanıtları tamamlanmıştı.
Test artık yeni mesaj baloncuğunun kimliğini kullanır. Adayın ilk üç turu gerçek
geçici WebUI/SSE üzerinden, kalan iki tur aynı geçmiş korunarak JSON üzerinden
tamamlandı; beş turun tamamı için tarayıcı kabulü iddia edilmez. Düzeltilen
tarayıcı bekleme yöntemiyle tam test yeniden çalıştırılmadı.

`conversation_acceptance.py --run` yeni geçici WebUI/Firefox/veritabanında iki
beş turluk diziyi kaydeder; `--candidate-only` yalnız geçici adayı çalıştırır.
Doğrulanmış mevcut 4B motoru varsa onu durdurmadan kullanır; yoksa yalnız kendi
geçici motorunu başlatır. 8087/8088 ve gerçek kullanıcı verileri değişmez.
Üretilen raporlar geçmiş aktarımını ve süreç temizliğini doğrular; yanıt kalitesi
ayrıca anlamsal olarak incelenmelidir. Birim testlerine beş turluk geçmişin JSON
ve SSE'de değişmeden aktarılması eklendi; toplam 21 test geçti.
