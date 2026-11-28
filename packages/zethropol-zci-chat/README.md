# Zethropol ZCI Chatbot Arch Package (`zethropol-zci-chat 0.1.0-3`)

Aşama 12 (**ZCI Chatbot**, `org.zethropol.ZciChat.v1` + `zci-chat` Qt6/QML masaüstü ve CLI istemcisi) için root kullanmadan derlenen Arch Linux paketi.

> **Durum (2026-11-28 — `0.1.0-3` Asenkron Masaüstü UI, Oturum İleti Sayacı & Görünür Hata Bildirimi):** Tam asenkron `QDBusPendingCallWatcher` (`125_000` ms) gönderimi, canlı `"Yerel model yanıt hazırlıyor..."` göstergesi ve çift gönderim kilidi, `ListSessions`/`GetSession` gerçek `message_count`/`messageCount` sol panel senkronizasyonu ve hata/zaman aşımı/politika engeli durumunda kullanıcı mesajını kaybetmeyen görünür Türkçe bildirimle root'suz `makepkg --clean --force` ile derlendi, `check()` (**4/4 Rust PASS** + **1/1 CTest / 6/6 QTest PASS**, `0` derleyici/QML uyarısı), **33/33 `services/local-inference` PASS**, **27/27 `services/zci-chat` PASS**, `desktop-file-validate`, `ELF_HOME_LEAKS=0`, `RPATH_HITS=0`, `QT_NETWORK_LINKS=0` ile doğrulandı ve yerel depoda (`17/17`) indekslendi. Canlı kurulum `sudo pacman -U` onay kapısındadır.

## Doğrulanan Özetler (`SHA256SUMS`)

- `zethropol-zci-chat-0.1.0-3-src.tar.gz`: `86d1b9cf167855878d201054f535311905b6e395c257eda8998ccbad3edca6ac`
- `zethropol-zci-chat-0.1.0-3-x86_64.pkg.tar.zst`: `726a94eac8bc381a20f888b9dba82d2dd313beefc8f93a12c39aa6a5d9bf3039`

## Paket İçeriği

- `/usr/lib/zethropol/zethropol-zci-chatd` — Kullanıcı oturumu D-Bus servisi (`org.zethropol.ZciChat.v1`), token bütçesi kırpma, opt-in salt-okunur sistem bağlamı, çift yönlü `WrapUntrustedData` mühürleme, Aşama 13 komut engeli ve `0700`/`0600` şifreli yerel geçmiş (`SHA-256-CTR + HMAC-SHA256`).
- `/usr/lib/systemd/user/zethropol-zci-chat.service` & `/usr/share/dbus-1/services/org.zethropol.ZciChat.service` — Sertleştirilmiş kullanıcı oturumu birimi (`RestrictAddressFamilies=AF_UNIX`, `IPAddressDeny=any`).
- `/usr/bin/zci-chat` — Zethropol koyu cam (`#0b111d`) Qt6/QML masaüstü sohbet penceresi ve terminal kipi (`--cli` / `--status` / `--send` / `--history` / `--clear`).
- `/usr/share/applications/zci-chat.desktop` & `/usr/share/icons/hicolor/scalable/apps/zethropol-zci-chat.svg` — Masaüstü başlatıcı ve simge.
- `/usr/share/licenses/zethropol-zci-chat/LICENSE` — MIT lisansı.

Ayrıntılı mimari ve güvenlik sınırları için `docs/ZCI_CHAT.md` belgesine bakın.
