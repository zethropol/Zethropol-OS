# Bütünleşik Zethropol OS + ZCI Yol Haritası — İlerleme Kaydı

- **Güncelleme Tarihi:** 2026-11-24
- **Aktif Referans Taban:** `Zethropol Control Center 0.1.0-3` (+ 7 kabul edilmiş D-Bus servisi = 8 paketlik dağıtım, 9/9 paket bütünlüğü)
- **Not:** Mevcut `docs/DEVELOPMENT_STAGES.md` belgesine dokunulmamıştır; bu dosya bütünleşik geliştirme sırası ve güncel aşama durumları için kısa ilerleme kaydıdır.

---

## 1. Ana Geliştirme Sırası ve Güncel Durum

| Aşama | Kapsam | Durum | Referans / Not |
|---|---|---|---|
| **01–02** | Sistem Kimliği, Temel Mimari, D-Bus/Polkit ve Güvenlik İlkeleri | `COMPLETED` | `docs/ARCHITECTURE.md`, `docs/SECURITY.md`, `docs/SERVICES.md` |
| **03 / Aşama F** | **Zethropol Control Center (`0.1.0-3`) ve 7/7 Çekirdek Servis Kabulü** | **`COMPLETED / ACCEPTED`** | Kabul edilmiş güvenli işlev kapsamıyla resmen kapatıldı (`docs/CONTROL_CENTER_CLOSURE_0.1.0-3.md`, `final_live_acceptance_report.md`). Yeni geliştirmelerde referans başlangıç sürümüdür. |
| **04** | **Zethropol System Monitor (5 podlu yapı) nihai kabulü** | **`PLANNING / OPEN`** | **Sıradaki ana geliştirme aşaması.** Yalnızca planlama düzeyinde açılmıştır; uygulama başlatılmamıştır. |
| **Sonraki Fazlar** | Masaüstü Entegrasyonu, Kurulum / Paket Deposu / ISO Altyapısı ve Çoklu Donanım Doğrulaması | `PLANNED` | Ana geliştirme sırası korunur. |

---

## 2. Sıradaki Ana Aşama — `04 — Zethropol System Monitor (5 podlu yapı) nihai kabulü` (`PLANNING / OPEN`)

- **Durum:** `PLANNING / OPEN` (uygulama, kod değişikliği, paketleme veya canlı test döngüsü başlatılmadı).
- **Mevcut Çalışma Ağacı Gözlemi:** `services/system-monitor/main.qml` ve `services/system-monitor/contents/` altında 5 podlu plasmoid yapısı (`CPU LOAD`, `RAM USAGE`, `GPU LOAD`, `NETWORK`, `STORAGE`) ile görünüm/arka plan opaklık yapılandırması yer almaktadır; çalışma ağacındaki mevcut kullanıcı değişiklikleri dokunulmadan korunmuştur.
- **Planlama Düzeyi Önkoşulları:**
  - `zethropol-control-center 0.1.0-3` referans tabanına, 7/7 kabul edilmiş servislere, Snapper `340–347` kayıtlarına, `m_liveExecutionEnabled=false` kilidine ve `zethropol-performance` karantinasına dokunulmadan izole planlama yürütülür.
  - Veri kaynağı mimari sınırı (`docs/ARCHITECTURE.md`, `docs/CONTROL_CENTER_ARCHITECTURE.md`), KDE Plasma Wayland görsel/yerleşim kabul kriterleri ve paketleme/geri dönüş sınırları uygulama öncesinde ayrı planla netleştirilir.

---

## 3. Korunan Ayrı Takip Kayıtları (Ana Sırayı Değiştirmez)

1. **Performance Güvenlik Borcu (`zethropol-performance 0.1.0-1`):**
   - Karantina yürürlüktedir; kurulu paketteki eski D-Bus politikası (`<policy context="default"><allow own="org.zethropol.Performance"/>`) ayrı güvenlik takip maddesi olarak korunur.
2. **Control Center Kilitli / `NOT VERIFIED` Kapsamı:**
   - Updates `m_liveExecutionEnabled=false` kilidi ve Recovery `rollback_execution_enabled=false` / `repair_execution_enabled=false` sınırları korunur.
   - Canlı durum değiştiren işlemler `NOT VERIFIED` olarak muhafaza edilir; `UpdateInstallerTest` zamanlama yarışı ayrı takiptedir.
3. **ZCI / ZEKANS Takip Kaydı:**
   - `services/zci-chat/`, `services/local-inference/` ve `services/zekans-code/` üzerindeki mevcut geliştirme kayıtları korunur; ana sırayı değiştirmez.
   - `AGENTS.md` ve `docs/SECURITY.md` gereği ZCI/ZEKANS için ayrıcalık veya güvenlik istisnası varsayılmaz. Açık kalan 5 delegasyon/cgroup testi, Gemma 4 E4B Türkçe kabul eşiği (`7/9 < 9/9`) ve uygulanmamış WebUI `8088` geçişi ayrı takip kapsamındadır.
4. **Kurulum ve ISO Altyapısı:**
   - Tekrarlanabilir paket deposu, kurulum aracı ve ISO üretim altyapısı sonraki faz takip kaydı olarak korunur.
