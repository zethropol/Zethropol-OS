# Zethropol ZCI Chatbot Arch Package (`zethropol-zci-chat 0.1.0-1`)

Aşama 12 (**ZCI Chatbot**, `org.zethropol.ZciChat.v1` + `zci-chat` Qt6/QML masaüstü ve CLI istemcisi) için root kullanmadan derlenen Arch Linux paketi.

> **Durum (2026-11-28):** Root'suz `makepkg --clean --force` ile derlendi, `check()` (**4/4 Rust PASS** + **1/1 CTest / 5/5 QTest PASS**, `0` derleyici/QML uyarısı), 4 servisli çapraz D-Bus + gerçek CLI testi (**32/32 `services/local-inference` PASS**, **27/27 `services/zci-chat` PASS**), `desktop-file-validate`, `ELF_HOME_LEAKS=0`, `RPATH_HITS=0`, `QT_NETWORK_LINKS=0` ile doğrulandı ve yerel depoda (`16/16`) indekslendi. Canlı kurulum `sudo pacman -U` onay kapısındadır.

## Doğrulanan Özetler (`SHA256SUMS`)

- `zethropol-zci-chat-0.1.0-1-src.tar.gz`: `8ad04775f9c3cba4d2b9a08a34a768b038113a9061ed93e7cbf01acbae776617`
- `zethropol-zci-chat-0.1.0-1-x86_64.pkg.tar.zst`: `a2a8e45b732b81a2ee312d20bc4512677e0faa26650ef4ab5a6f680cc255837f` (`1445,61 KiB`)

## Paket İçeriği

- `/usr/lib/zethropol/zethropol-zci-chatd` — Kullanıcı oturumu D-Bus servisi (`org.zethropol.ZciChat.v1`), token bütçesi kırpma, opt-in salt-okunur sistem bağlamı, çift yönlü `WrapUntrustedData` mühürleme, Aşama 13 komut engeli ve `0700`/`0600` şifreli yerel geçmiş (`SHA-256-CTR + HMAC-SHA256`).
- `/usr/lib/systemd/user/zethropol-zci-chat.service` & `/usr/share/dbus-1/services/org.zethropol.ZciChat.service` — Sertleştirilmiş kullanıcı oturumu birimi (`RestrictAddressFamilies=AF_UNIX`, `IPAddressDeny=any`).
- `/usr/bin/zci-chat` — Zethropol koyu cam (`#0b111d`) Qt6/QML masaüstü sohbet penceresi ve terminal kipi (`--cli` / `--status` / `--send` / `--history` / `--clear`).
- `/usr/share/applications/zci-chat.desktop` & `/usr/share/icons/hicolor/scalable/apps/zethropol-zci-chat.svg` — Masaüstü başlatıcı ve simge.
- `/usr/share/licenses/zethropol-zci-chat/LICENSE` — MIT lisansı.

Ayrıntılı mimari ve güvenlik sınırları için `docs/ZCI_CHAT.md` belgesine bakın.
