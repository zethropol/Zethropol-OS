# ZCI — Gemma 4 E4B `gemma-system.md` 9/9 Türkçe Kabul ve İzole WebUI Raporu

- **Tarih:** 2026-11-27
- **Durum:** **`9/9` Tam Kabul (`strict_acceptance: true`) + İzole Open WebUI / Firefox Kabulü `PASS`**
- **Kapsam Sınırı:** Kalıcı canlı `8087`/`8088` servis veya varsayılan model geçişi (`P3C`) yapılmadı; mevcut kullanıcı `webui.db` ve `settings.json` açılmadı.

## 1. Yöntem ve Sabitler

- **Dondurulmuş Küme:** `services/zci-chat/tests/acceptance/tr-v1/acceptance.json` (`1.0.0`, SHA-256 `5056cbdd31ec4445b378324f24f0403f21fa8256ad840c99ca80c0d981cd89e5`) — değiştirilmeden doğrulandı (`validate.py`: 9 tur, 2 negatif regresyon reddi).
- **Sistem İstemi:** `services/zci-chat/gemma-system.md` (SHA-256 `d2380f3602bb57563e540e5e4f1d678774ec3743598cce78ba6390f22159dff1`).
- **Model ve Şablon:** `gemma-4-E4B-it-Q4_K_M.gguf` (SHA-256 `0ffb122c8b6921f13cbc34186e052524d0b5803b17f4867b7197a561400b3770`), yerel Gemma 4 kanonik şablonu (SHA-256 `603a42db292c25278e9b23d94c7abbe77453f13e26a053c43c5c2f900b4136f7`), `enable_thinking: false`, `temperature: 0.2`, `top_k: 64`, `top_p: 0.95`, `min_p: 0.05`, `seed: 7`, `ctx: 32768`, `max_tokens: 768`, `cache_prompt: false`.
- **Koşu Aracı:** `python3 -I -B services/zci-chat/profile_acceptance.py --stage full` (portlar `19086`/`19087`, `ModelLease` aktif, `8087`/`8088` dokunulmadı).

## 2. Ölçümler ve 2026-11-15 Karşılaştırması

| Ölçüt | 2026-11-15 (`system.md`) | 2026-11-27 (`gemma-system.md`) |
|---|---:|---:|
| Tam başarı (anlam + biçim) | 7/9 | **9/9** |
| Tam anlamsal başarı | 9/9 | **9/9** |
| Kısmi anlamsal başarı | 0/9 | **0/9** |
| Birebir (`exact`) denetim | 3/3 | **3/3** |
| Kritik uydurma olayı / etkilenen tur | 0 / 0 | **0 / 0** |
| Medyan tam yanıt süresi | 8,274 sn | **6,061 sn** |
| Medyan ilk içerik gecikmesi | 7,112 sn | **5,792 sn** |
| Medyan sunucu üretim hızı | 9,94 tok/sn | **10,48 tok/sn** |
| Tepe süreç RSS / HWM | 7,509 GiB | **7,509 GiB** (`7873732 KiB`) |
| En düşük kullanılabilir sistem RAM'i | 24,158 GiB | **24,017 GiB** (`25787961344 B`) |

## 3. 9 Tur Özeti (Model Kendi Kendini Puanlamadı)

Tüm 9 turun ham yanıtları, ölçüt kontrolleri ve gerekçeleri [review.json](review.json) ile [result.json](result.json) dosyalarında kayıtlıdır:

1. **`natural` (PASS):** `"Selam. Anlıyorum, dinlerim seni."` — 2 kısa cümle, tanıtım/ek soru yok.
2. **`intent` (PASS):** `"Bu aralar en çok neyle ilgileniyorsun?"` — yalnızca tek doğal açılış cümlesi, giriş/açıklama yok.
3. **`memory` (PASS):** `"Yeni tanıştığın iş arkadaşınla konuşacaksın."` — tek kısa cümlede doğru hatırlama.
4. **`update` (PASS):** `"Kaydettim."` — `exact_pass: true`.
5. **`latest_context` (PASS):** `"Cuma"` — `exact_pass: true`.
6. **`arithmetic` (PASS):** `"12"` — `exact_pass: true`.
7. **`logic` (PASS):** `"Ece'nin bir kartı olması, üye olduğu anlamına gelmez. Kartın üyelik şartı olup olmadığı bilinmiyor."` — geçersiz ters çıkarımı reddeden çelişkisiz tam 2 kısa cümle.
8. **`unknown` (PASS):** `"Bu protokol hakkında teknik bir bilgi verilmediği için paket başlığının kaç bayt olduğunu söyleyemem."` — sıfır uydurma, tek kısa cümle.
9. **`practical_format` (PASS):** 3 tamamlanmış, farklı ve pratik cümle; madde listesi, tekrar veya kapanış eki yok.

Tüm 9 turda `history_exact: true`, `cached_tokens: 0`, `finish_reason: "stop"`, `zci_status: "completed"`, `protected_sources_unchanged: true`, `existing_ports_unchanged: true`, `engine_stopped: true`, `gateway_stopped: true` doğrulandı.

## 4. İzole Open WebUI + Headless Firefox Kabulü (`PASS`)

`gemma_webui_acceptance.py --run` ile taze geçici SQLite DB ve ayrılmış portlar (`19685`, `19688`, `19692`) üzerinde doğrulanan özet ([webui-result.json](webui-result.json)):

- `real_user_database_opened: false`
- `available_models`: `["zci-test-preserved-default", "zci-chat-gemma4-e4b"]`
- `default_unchanged: true`, `default_fixture_inference_attempts: 0`
- `json_routed_to_gemma: true` ve SSE akış (`stream: true`, `thinking: false`) yönlendirmesi doğrulandı.
- `browser`: `actual_firefox_ui: true`, `selected_model: "zci-chat-gemma4-e4b"`, `sse_bubble: true` (tarayıcı çıktısı: `TAMAM`), `reasoning_control_tokens_absent: true`, `owned_engine_unavailable_error_visible: true`.
- `cleanup`: `ui_stopped: true`, `engine_stopped: true`, `gateway_stopped: true`, `fixture_stopped: true`, `active_upstreams: 0`.
