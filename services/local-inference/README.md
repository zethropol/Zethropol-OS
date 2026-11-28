# Ortak CPU model koordinasyonu ve Gemma 4 E4B günlük kullanım

`inference_guard.ModelLease` ZCI ve ZEKANS profillerinin aynı kullanıcı
oturumunda gereksiz eşzamanlı CPU modeli başlatmasını engeller. Güvenilir
başlatıcı, model yüklemeden önce lease alır; `Popen` sonrası hemen
`attach_process(process.pid)` çağırır; yalnız kendi modeli durup `wait()`
tamamlandıktan sonra lease bırakır. Kilit FD'si modele miras verilmez.

Üretim yolu sabittir: `/run/user/<uid>/zethropol-inference/cpu.lock`. Kullanıcıya
özel dizin ve dosya sahipliği/türü kontrol edilir; symlink, hardlink, herkese
açık izinler ve bozuk kayıt reddedilir. Başlangıçta varsayılan en az 12 GiB RAM
payı gerekir. Mevcut motorların çalışma sırasındaki RAM gözetimi ayrıca korunur.
Çakışmada başka motor kapatılmaz veya ona bağlanılmaz; açık hata verilir.

Başlatıcı kapansa da izlenen çocuk süreç yaşıyorsa süreç PID/start-time bağı
ikinci başlatmayı reddeder. Çocuk bittikten sonra kayıt geri alınabilir. Bu
mekanizma aynı UID'deki kötü niyetli host sürecine karşı yetkilendirme veya
cgroup yalıtımı değildir. Korunan eski 8087 başlatıcısını değiştirmez veya
yönetmez; o motor lease'e katılmaz.

`runtime_dir` yalnız güvenilir test bağımlılığıdır; model/HTTP girdisinden
seçilmez. `ModelLease` hiçbir servis, PolicyKit/D-Bus politikası, kullanıcı
veritabanı veya sistem kurulumu oluşturmaz. Beş açık cgroup testinin yerine
geçmez; onları başarılı saymaz.

## Günlük kullanım komutları (Gemma 4 E4B)

ZCI sohbeti ve ZEKANS kodlama ajanı için hedef model **Gemma 4 E4B**
(`gemma-4-E4B-it-Q4_K_M.gguf`) olarak belirlenmiştir.

### 1. Ortak Gemma 4 E4B Motoru ve Çift Gateway Başlatıcısı (Önerilen)
```sh
sh services/local-inference/start-shared-gemma4.sh
```
- Modeli belleğe **tek kez** yükler (`127.0.0.1:29087`, `--api-key-file` ile iç port korumalı).
- **ZCI Sohbet Gateway (`127.0.0.1:19086/v1`):** `zci-chat-gemma4-e4b` (`Bearer local`).
- **ZEKANS Kodlama Gateway (`127.0.0.1:29016/v1`):** `zekans-gemma4-e4b` (görev süreci bağlı tek kullanımlık `Bearer` belirteci ve `reflection=1` bütçesi).
- Durum kontrolü:
  ```sh
  sh services/local-inference/start-shared-gemma4.sh --status
  ```

### 2. ZEKANS Gemma 4 E4B Kodlama Başlatıcısı
```sh
sh services/local-inference/zci-code-gemma4-shared --project /path/to/project --message "Görev metni" main.py
```
- Ortak motor (`start-shared-gemma4.sh`) çalışıyorsa doğrudan `29016` kapısına bağlanır; çalışmıyorsa görev süresince tek seferlik Gemma motorunu başlatıp görev sonunda kapatır.
- `task_guard.py` ile belirsiz veya çelişkili görevlerde dosya değiştirmeden soru sorar (`Exit code: 2`).
- Ön kontrol için `--check-task`, bağımsız anlamsal doğrulama için `--verify-spec` ve `--verify-allow-file` desteklenir.

### 3. Bağımsız ZCI Gemma 4 E4B Sohbet Başlatıcısı (Yedek Yol)
```sh
sh services/zci-chat/start-zci-gemma4.sh
```
- Open WebUI bağlantı şablonu: `services/zci-chat/webui-gemma4-connection.example.json`

### 4. ZCI Security & Policy Engine İstemcisi (`Aşama 08`)
- `services/local-inference/zci_policy_client.py` (`ZciPolicyClient`), `zci-chat`, `zekans-code`, `zci-command` ve `zci-vision` ajanları için `/usr/bin/busctl` üzerinden `org.zethropol.Security` (`/org/zethropol/Security/Policy`, `org.zethropol.Security.Policy.v1`) arayüzüne bağlanan **fail-closed** Python köprüsüdür.
- Servis erişilemez, zaman aşımına uğrar veya bozuk yanıt dönerse istemci her zaman `allowed=False` (`CLIENT_FAIL_CLOSED`) döndürür; hiçbir ayrıcalık yükseltmesi yapmaz.

### 5. Testler
```sh
python3 -I -B -m unittest discover -s services/local-inference -v
```
