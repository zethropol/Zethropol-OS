# Zethropol ZCI Chatbot (`org.zethropol.ZciChat.v1` & `zci-chat`) — Aşama 12 Mimarisi

**ZCI Chatbot** (`services/zci-chat-service` + `desktop/zci-chat`, Arch paketi `zethropol-zci-chat 0.1.0-1`), Zethropol OS için tamamen yerel, dış ağsız, kimlik doğrulamalı şifreli geçmişe sahip ve **ZCI Runtime (`org.zethropol.ZciRuntime.v1`) / ZCI Daemon (`org.zethropol.ZciDaemon.v1`) / Security Policy Engine (`org.zethropol.Security.Policy.v1`)** zinciri üzerinden çalışan yerel yapay zekâ sohbet servisi ile Qt6/QML masaüstü + CLI istemcisidir.

## 1. Kapsam (`12.01`–`12.07`)

| Bileşen | Uygulama Özeti |
|---|---|
| **`12.01` Yerel Sohbet Oturumu & Bağlam Yönetimi** | `services/zci-chat-service` (`zethropol-zci-chatd`) çok turlu sohbet oturumlarını yönetir, konuşma geçmişini `zci-runtime`'ın etkin `max_context_tokens` bütçesine göre en eski turlardan kırpar ve yalnızca yerel `AF_UNIX` D-Bus soketleriyle iletişim kurar (`IPAddressDeny=any`). |
| **`12.02` Salt-Okunur Sistem Bağlamı Toplayıcı** | Yalnızca kullanıcının açık izniyle (`include_system_context=true` veya arayüzdeki geçiş anahtarı) `zci-runtime` / `zci-daemon` üzerinden salt-okunur donanım profili (`dev-baseline` / `ancha-production`), pasif/aktif durum (`Standby`/`Active`) ve sınırlı son olay özetini toplayıp `WrapUntrustedData` (`source_kind="system_context"`) ile mühürleyerek bağlama ekler. |
| **`12.03` Prompt-Injection & Aşama 13 Komut Engeli** | Kullanıcı iletileri, sistem bağlamı ve model çıktıları çift yönlü olarak `WrapUntrustedData` (`executable=false`, `policy_authority="none"`) ile izole edilir. Doğrudan kabuk komutu (`sudo`, `pkexec`, `pacman -S/-R/-U`, `systemctl`, `rm -rf`, `bash -c` vb.) veya politika/kurtarma kilidi geçersiz kılma (`rollback_execution_enabled`, `AGENTS.md`, prompt override) girişimleri fail-closed reddedilir (`command-execution-denied-stage13` / `prompt-injection-blocked`). |
| **`12.04` D-Bus Sohbet Arayüzü (`org.zethropol.ZciChat.v1`)** | Kullanıcı oturumu D-Bus servisi (`org.zethropol.ZciChat` @ `/org/zethropol/ZciChat`): `CreateSession`, `SendMessage`, `GetSessionHistory`, `ListSessions`, `ClearSession`, `GetChatStatus` / `GetState` metotları ile `MessageReceived` ve `SessionCleared` sinyalleri. |
| **`12.05` Masaüstü & CLI İstemcisi (`zci-chat`)** | `desktop/zci-chat/`: Zethropol koyu cam (`#0b111d`) tasarımlı Qt6/QML masaüstü penceresi (`zci-chat.desktop` + `zethropol-zci-chat.svg`) ve headless terminal kipi (`zci-chat --cli`, `--status`, `--send`, `--history`, `--clear`). Canlı `Standby`/`Active` model durumu, VRAM, donanım profili ve güvenlik rozetlerini gösterir; `Qt6::Network` veya `QProcess` bağlamaz. |
| **`12.06` Kalıcılık & Gizlilik Denetimi** | Sohbet geçmişi yalnızca kullanıcının ev dizininde (`~/.local/share/zethropol/zci-chat/`) `0700` dizin ve `0600` dosya izinleriyle, saf-Rust **SHA-256-CTR + HMAC-SHA256** (*Encrypt-then-MAC*) kimlik doğrulamalı şifrelemeyle atomik olarak saklanır; bozulmuş dosya MAC kontrolünde fail-closed reddedilir; tek tıkla geçmiş temizleme ve dış telemetri kapalı garantisi vardır. |
| **`12.07` Doğrulama** | `services/zci-chat-service` (`cargo test --offline --locked`), `desktop/zci-chat` (`ctest` / `zci_chat_qml_test`: `0` QML uyarısı), `desktop-file-validate`, ve `services/local-inference/test_zci_chat_service.py` (gerçek `security` + `zci-daemon` + `zci-runtime` + `zci-chat-service` + gerçek `zci-chat` CLI 4 servisli özel D-Bus entegrasyonu). |

## 2. Güvenlik Sınırları ve Korunan Kilitler

- `zethropol-zci-chatd` ayrıcalıksız kullanıcı oturumu servisi (`systemd --user`, `RestrictAddressFamilies=AF_UNIX`, `IPAddressDeny=any`, `NoNewPrivileges=yes`, `MemoryDenyWriteExecute=yes`, `UMask=0077`) olarak çalışır.
- Doğrudan kabuk komutu yürütme veya sistem durumu değişikliği yetkisi yoktur (Aşama 13 kapsamındadır).
- Mevcut `services/zci-chat/` kabul/değerlendirme dosyaları ve `services/recovery/src/recovery_service.rs` içindeki `rollback_execution_enabled=false` / `repair_execution_enabled=false` kilitleri korunur.
