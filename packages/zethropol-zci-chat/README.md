# Zethropol ZCI Chatbot Arch Package (`zethropol-zci-chat 0.1.0-2`)

Aşama 12 (**ZCI Chatbot**, `org.zethropol.ZciChat.v1` + `zci-chat` Qt6/QML masaüstü ve CLI istemcisi) için root kullanmadan derlenen Arch Linux paketi.

> **Durum (2026-11-28 — `0.1.0-2` Sohbet Kalitesi, Akıcılık & Token Optimizasyonu):** Zenginleştirilmiş doğal Türkçe `BASE_SYSTEM_PROMPT`, `2048` çıktı token bütçesi ve `120_000` ms (`zci-chat` D-Bus `125_000` ms) hibrit yanıt zaman aşımıyla root'suz `makepkg --clean --force` ile derlendi, `check()` (**4/4 Rust PASS** + **1/1 CTest / 5/5 QTest PASS**, `0` derleyici/QML uyarısı), 4 servisli çapraz D-Bus + gerçek CLI testi (**33/33 `services/local-inference` PASS**), `ELF_HOME_LEAKS=0`, `RPATH_HITS=0`, `QT_NETWORK_LINKS=0` ile doğrulandı ve yerel depoda (`17/17`) indekslendi. Canlı kurulum `sudo pacman -U` onay kapısındadır.

## Doğrulanan Özetler (`SHA256SUMS`)

- `zethropol-zci-chat-0.1.0-2-src.tar.gz`: `5b861341c9a018acaf4d0ebc6bf196b6ccb016e00357831f80cdde0fd83602d7`
- `zethropol-zci-chat-0.1.0-2-x86_64.pkg.tar.zst`: `2e0856aa597df504268a0eeb3cc6999cfc9340f6533be0153aa92df2f697a93e`

## Paket İçeriği

- `/usr/lib/zethropol/zethropol-zci-chatd` — Kullanıcı oturumu D-Bus servisi (`org.zethropol.ZciChat.v1`), token bütçesi kırpma, opt-in salt-okunur sistem bağlamı, çift yönlü `WrapUntrustedData` mühürleme, Aşama 13 komut engeli ve `0700`/`0600` şifreli yerel geçmiş (`SHA-256-CTR + HMAC-SHA256`).
- `/usr/lib/systemd/user/zethropol-zci-chat.service` & `/usr/share/dbus-1/services/org.zethropol.ZciChat.service` — Sertleştirilmiş kullanıcı oturumu birimi (`RestrictAddressFamilies=AF_UNIX`, `IPAddressDeny=any`).
- `/usr/bin/zci-chat` — Zethropol koyu cam (`#0b111d`) Qt6/QML masaüstü sohbet penceresi ve terminal kipi (`--cli` / `--status` / `--send` / `--history` / `--clear`).
- `/usr/share/applications/zci-chat.desktop` & `/usr/share/icons/hicolor/scalable/apps/zethropol-zci-chat.svg` — Masaüstü başlatıcı ve simge.
- `/usr/share/licenses/zethropol-zci-chat/LICENSE` — MIT lisansı.

Ayrıntılı mimari ve güvenlik sınırları için `docs/ZCI_CHAT.md` belgesine bakın.
