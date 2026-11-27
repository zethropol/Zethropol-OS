# ZCI — Gemma 4 E4B ve yenilenmiş Qwen3-4B değerlendirmesi

15 Kasım 2026. **Gemma mevcut donanım ve motorla uyumlu; bu örneklemde
Qwen3-4B'den daha başarılı, fakat 9/9 tam kabul eşiğini geçmiyor.** Kalıcı
model/servis geçişi yapılmadı. Başka model araştırılmadı veya indirilmedi.

## Küme ve yöntem

Kayıp geçici dosyanın kopyası olmayan yeni [Türkçe kabul kümesi 1.0.0](../tests/acceptance/tr-v1/acceptance.json)
oluşturuldu. Dokuz tur aynı konuşmada çalışır. Girdiler, tur bazlı beklenen
davranış, tam/anlamsal kabul ölçütleri ve iki negatif regresyon kalıcıdır.
Özgün “12 bayt” çıktısı ve kayıtlı “928 bayt” hatasının kısa örneği korunur;
bunlar model geçmişine eklenmez. Küme doğrulaması geçti: dokuz tur, benzersiz
kimlikler, kapsam ve iki negatif örnek doğrulandı. Çıkarımdan önce sabitlenen
SHA-256: `5056cbdd31ec4445b378324f24f0403f21fa8256ad840c99ca80c0d981cd89e5`.

Ryzen 5 3600, 32 GiB RAM, swap yok. Mevcut llama.cpp `0.9.0-dev`, commit
`cd959bae99ff4bed22bef2604f45241c09bbb8c8` kullanıldı; derleme değiştirilmedi.
Her model kendi gömülü şablonunu kullandı. Gemma şablonu Google Gemma 4
Canonical Chat Template olarak tanımlanıyor; gerçek render üzerinde Gemma 4
tur işaretleri, tek sistem mesajı ve düşünmesiz mod doğrulandı.
[Google biçim sözleşmesi](https://ai.google.dev/gemma/docs/core/prompt-formatting-gemma4)
ile uyumlu tur yapısı kullanıldı. Qwen'de boş think öneki doğrulandı.

İki nihai koşu aynı kaynak `system.md`, aynı küme, altı CPU iş parçacığı,
GPU=0, tek slot, 32768 bağlam, 768 çıktı rezervi, temperature=0.2, seed=7,
top_k=64, top_p=0.95, min_p=0.05 ile sırayla çalıştı. İstem önbelleği ve
context shift kapalıydı. Diğer etkin sampler ayarları da karşılaştırılarak
eşleşti. Gerçek asistan çıktılarının sonraki turlara değiştirilmeden aktarımı,
tek sistem istemi ve sıfır cached token tüm 18 yanıtta doğrulandı.

Gemma'nın top_k varsayılanı 64, Qwen'in 40 olduğundan başlayan Qwen ön denemesi
altı tamamlanmış yanıttan sonra durduruldu; ayrı `qwen3-4b-topk40-incomplete/`
altında korunur ve skorlara dahil değildir. Nihai Qwen koşusunda top_k açıkça
64 yapıldı. Gemma koşusu tekrarlanmadı. Bu düzeltme dışındaki kalite testleri
tekrarlanmadı. Eski beş/15 turluk sonuçlar bu skorlarla doğrudan kıyaslanmadı.

## Nihai ölçümler

| Ölçüt | Gemma 4 E4B Q4_K_M | Qwen3-4B Q4_K_M |
|---|---:|---:|
| Tam başarı: anlam + talimat | 7/9 | 2/9 |
| Tam anlamsal başarı | 9/9 | 2/9 |
| Kısmi anlamsal başarı, geçiş sayılmaz | 0/9 | 3/9 |
| Kritik uydurma olayı / etkilenen tur | 0 / 0 | 0 / 0 |
| Medyan tam yanıt | 8,274 sn | 11,379 sn |
| Medyan ilk görünür token gecikmesi | 7,112 sn | 10,946 sn |
| Medyan sunucu üretim hızı | 9,94 token/sn | 11,53 token/sn |
| Süreç tepe RSS / HWM | 7,509 GiB | 8,546 GiB |
| En düşük kullanılabilir sistem RAM'i | 24,158 GiB | 20,587 GiB |

Token/sn, sunucunun `predicted_per_second` alanının medyanıdır; üretilen özel
tokenlar dahildir. Ham yanıtlarda ağ üzerinden gözlenen hız ayrıca saklanır;
özellikle çok kısa yanıtlarda bu değer parser paketlemesinden etkilenebilir.
İlk token ölçümü HTTP isteğinden ilk dolu içerik delta'sına kadardır; içeride
ilk çözümlenen tokenın ayrı ölçümü değildir. Süreler kullanıcı/asistan geçmişi
dahil soğuk istemlerle ölçülür. Modellerin tokenizerları ve kendi önceki
yanıtları farklıdır: girişler Gemma'da 419–947, Qwen'de 487–1040 token;
toplam üretilen tokenlar sırasıyla 283 ve 304'tür. Tablo aynı konuşma protokolünün
uçtan uca karşılaştırmasıdır; sabit token sayılı saf motor hız kıyası değildir.
Arka plan masaüstü yükü ve donanım saatleri sabitlenmedi.

Gemma ilk turda açık yasağa rağmen kendini tanıttı ve dört cümle üretti;
ikinci turda tek açılış cümlesinin yanına açıklama ekledi. Diğer yedi tur tam
geçti. İş arkadaşı bilgisi ve cuma düzeltmesi doğru izlendi; hesap 12,
mantık cevabı doğru, teknik özellik verilmediğinde belirsizlik açıklandı.
Sonraki turlarda tekrarlayan selamlama/tanıtım görülmedi.

Qwen'in tam geçen turları kısa onay ve bilinmeyen protokol sorusudur. İlk
tur, geçmiş sorusu ve pratik öneri kısmi anlamsal başarıdır: bozuk Türkçe
soru kalıbı, cevap yerine soru ve sözcük/uyum sorunları vardır. Gün düzeltmesi
yerine “Çarşamba.”, hesapta 12 yerine “16” üretmiş, geçersiz ters çıkarımı
kabul etmiştir. Son turda yasaklanan listeyi ve altı cümleyi üretmiştir.
Bu koşuda “12 bayt” veya “928 bayt” uydurması iki modelde de görülmedi;
bu, kayıtlı eski hataları geçersiz kılmaz.

Her turun ham yanıtı, ayrı anlamsal/biçim puanı ve gerekçesi [review.json](review.json)
içindedir. Puanlama modelin kendisine yaptırılmadı. Kısmi başarı geçiş sayılmaz;
doğal Türkçe de önceden sabitlenen anlamsal ölçütün parçasıdır.

## Dosya ve kaynak doğrulaması

Yalnız `gemma-4-E4B-it-Q4_K_M.gguf` indirildi: **5.335.291.936 bayt**.
[LM Studio yayımlayıcı deposu](https://huggingface.co/lmstudio-community/gemma-4-E4B-it-GGUF),
sabit revizyon `99210a71094c6aa8559f764e0cadedd7d89de94d`.
Yayımlanan LFS SHA-256 ve iki yerel doğrulama (indirme akışı, diski tekrar okuma)
eşleşti: `0ffb122c8b6921f13cbc34186e052524d0b5803b17f4867b7197a561400b3770`.
Metadata ve tam URL `source.json` / `download-result.json` içindedir.
Projektör, görsel/ses encoder dosyası veya başka model indirilmedi.

Model yeni `~/.local/share/zci/models/evaluation/gemma4-e4b-q4-k-m/` dizininde
korunuyor. Doğrulama yeniden gerektiğinde `sha256sum` ile bu dosyanın özeti
yukarıdaki sabit değerle karşılaştırılabilir. Qwen3-4B'nin mevcut dosyası da
önceden kayıtlı SHA-256 ile doğrulandı; yeniden indirilmedi.

## Bellek, kapanış ve karar

İki model de 32768 slotla yüklendi. Başlangıçta en az 12 GiB kullanılabilir
RAM arandı; 6 GiB altı kullanılabilir RAM veya 18 GiB üstü süreç RSS sınırında
yalnız test sürecini durduracak gözetim vardı. Eşikler tetiklenmedi. Tam dolu
32K girişle çıkarım veya genel uzun bağlam kalite testi yapılmadı; yalnız
32K ayırmanın ve bu kısa konuşmanın güvenli RAM ile çalışmasının kabulü vardır.

Test yalnız `127.0.0.1:28089` üzerinde çalıştı. İki nihai sunucu ve yarıda kesilen
ön deneme temiz kapandı; son kontrolde llama-server süreci ve test dinleyicisi
yoktu. 8087/8088 başlangıçta ve sonda bağlantı reddediyordu. WebUI veritabanı
açılmadı/değiştirilmedi. 1.7B çalıştırılmadı; iki mevcut modelin ve motorun
inode/boyut/mtime kayıtları ile kaynak sistem istemi, yapılandırma ve AGENTS.md
özetleri değişmedi. Root/sudo, kalıcı kurulum veya servis geçişi yapılmadı.
**Açık beş cgroup testi açık kalır; burada çalıştırılmadı veya başarılı sayılmadı.**

Küme doğrulaması, ölçüm incelemesinin geçmiş/ayar/temizlik kontrolleri ve
`git diff --check` geçti. Önceden var olan dört izlenen dosya değişikliği
korundu; bu görev yalnız `services/zci-chat/tests/acceptance/tr-v1/` ve bu
değerlendirme dizinine ekler yaptı. Git add/commit/push yapılmadı.

**Karar:** Gemma bu donanımda ZCI için daha güçlü bir adaydır; Türkçe, bağlam,
hesap ve mantık bu küçük kümede olumlu. Ancak istenmeyen tanıtım ve biçim
ihlalleri nedeniyle tam sohbet kabulü sağlanmadı. Varsayılan modele geçiş
önerisi henüz onaylanmış kabul olarak sunulamaz. Mevcut durum korunarak
değerlendirme burada durduruldu; başka modele otomatik geçilmedi.
