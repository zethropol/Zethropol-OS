# ZCI Türkçe sohbet kabulü 1.0.0

Bu, 15 Kasım 2026'da oluşturulmuş **yeni** dokuz turluk kümedir. Kaybolan
geçici `acceptance.json` dosyasının birebir kopyası veya eski beş/15 turluk
testin yeniden adlandırılması değildir. Girdiler ve tur bazlı ölçütler
`acceptance.json` içindedir; negatif örnekler test geçmişine enjekte edilmez.

Dokuz tur aynı konuşmada, kaynak `system.md` bir kez eklenerek çalıştırılır.
Her gerçek asistan yanıtı sonraki tura değiştirilmeden aktarılır. Yanıtlar
üretilmeden önce dosyanın SHA-256 değeri ölçüm kaydına yazılır. Sonuç gördükten
sonra eşikleri veya örnekleri değiştirmek yeni sürüm gerektirir.

Her tur için anlamsal başarı ve biçim başarısı ayrı değerlendirilir. Doğru
bilgi veya niyet, doğal Türkçe ve uydurmama anlamsal başarıya dahildir.
Tam başarı ikisini birlikte gerektirir. Kısmi başarı ve kesilmiş yanıt tam
geçiş değildir. `exact` yalnız dış boşluklar temizlendikten sonra birebir
karşılaştırılır; noktalama keyfi silinmez. Diğer biçimler ham metindeki
cümle/sözcük/liste yapısına göre değerlendirilir. Global selamlama ve tanıtım
kuralı da her turda kontrol edilir; ikinci turdaki istenmiş açılış örneği
selamlamaya ilişkin açık istisnadır.

Kabul eşiği: 9/9 anlamsal, 9/9 tam başarı ve sıfır kritik uydurma. Bu küçük
küme geniş üretim güvencesi veya genel uzun bağlam yeterliliği sağlamaz.
Kritik uydurma olayları ve bunların görüldüğü tur sayısı ayrı sayılır.
Hesap/mantık hataları ayrıca başarısızlık olarak raporlanır.

`validate.py` yapı, kapsam, tur sayısı ve iki negatif regresyonu doğrular.
Dar sayı dedektörü yalnız kayıtlı negatif örnekleri korur; başka sayıların
veya teknik özelliklerin uydurulmadığını kanıtlamaz. Anlamsal puanlama her
çıktının ölçütlerle incelenmesini gerektirir; test edilen model puanlayıcı
olarak kullanılmaz.

```sh
python3 -I -B services/zci-chat/tests/acceptance/tr-v1/validate.py
```

Yeni kümenin skor ve süreleri eski 12 senaryo/15 yanıt veya beş turluk
koşularla doğrudan karşılaştırılamaz. Geçerli bir doğrudan kıyas için aynı
küme, sistem istemi, geçmiş yöntemi, motor, kaynak koşulları ve sampling
ayarları gerekir. Açık beş cgroup testi bu kabul ile geçmiş sayılmaz.
