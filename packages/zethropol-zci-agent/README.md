# Zethropol ZCI Command Agent & ZEKANS Arch Package (`zethropol-zci-agent 0.1.0-1`)

Aşama 13 (**ZCI Komut Ajanı & ZEKANS Entegrasyonu**, `org.zethropol.ZciAgent.v1` + `zci-agent` Qt6/QML onay arayüzü ve CLI istemcisi) için root kullanmadan derlenen Arch Linux paketi.

> **Durum (2026-11-28):** Root'suz `makepkg --clean --force` ile derlendi, `check()` (**3/3 Rust PASS** + **1/1 CTest / 5/5 QTest PASS**, `0` CMake/derleyici/QML uyarısı), 4 servisli çapraz D-Bus + gerçek CLI testi (**33/33 `services/local-inference` PASS**, **8/8 `services/zekans-code` PASS**), `desktop-file-validate`, `ELF_HOME_LEAKS=0`, `RPATH_HITS=0`, `QT_NETWORK_LINKS=0` ile doğrulandı ve yerel depoda (`17/17`) indekslendi. Canlı kurulum `sudo pacman -U` onay kapısındadır.

## Doğrulanan Özetler (`SHA256SUMS`)

- `zethropol-zci-agent-0.1.0-1-src.tar.gz`: `f515adefb28405cbbd488c5a6ab0df745442a16a096e6981c6bfb021a18d7c3d`
- `zethropol-zci-agent-0.1.0-1-x86_64.pkg.tar.zst`: `e53f7457ef7588a1b95cf62dad5adeced53a770d2283479da48f4c29a8afb0c1` (`1521,02 KiB`)

## Paket İçeriği

- `/usr/lib/zethropol/zethropol-zci-agentd` — Kullanıcı oturumu D-Bus servisi (`org.zethropol.ZciAgent.v1`), ham kabuk yasaklı allowlist planlayıcı, `zci-daemon` eylem geçidi, salt-okunur Snapper/kurtarma ön-kontrolü ve `0700`/`0600` SHA-256 hash-zincirli denetim günlüğü.
- `/usr/lib/systemd/user/zethropol-zci-agent.service` & `/usr/share/dbus-1/services/org.zethropol.ZciAgent.service` — Sertleştirilmiş kullanıcı oturumu birimi (`RestrictAddressFamilies=AF_UNIX`, `IPAddressDeny=any`).
- `/usr/bin/zci-agent` — Zethropol koyu cam (`#0b111d`) Qt6/QML onay ve adım izleme penceresi ile headless terminal kipi (`--plan`, `--status`, `--approve`, `--execute`, `--cancel`, `--audit`).
- `/usr/share/applications/zci-agent.desktop` & `/usr/share/icons/hicolor/scalable/apps/zethropol-zci-agent.svg` — Masaüstü başlatıcı ve simge.
- `/usr/share/licenses/zethropol-zci-agent/LICENSE` — MIT lisansı.

Ayrıntılı mimari ve güvenlik sınırları için `docs/ZCI_AGENT.md` belgesine bakın.
