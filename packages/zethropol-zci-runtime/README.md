# Zethropol ZCI Runtime Arch Package (`zethropol-zci-runtime 0.1.0-1`)

Aşama 11 (**ZCI Yerel AI Çalışma Motoru**, `org.zethropol.ZciRuntime.v1`) için root kullanmadan derlenen Arch Linux paketi.

> **Durum (2026-11-28):** `zethropol-zci-runtime 0.1.0-1` (`11.01`–`11.08`) root kullanmadan derlenip test edildi (`services/zci-runtime` **7/7 PASS**, `0` uyarı; `services/local-inference` **31/31 PASS**, `-W error`, gerçek 3 servisli `security` + `zci-daemon` + `zci-runtime` D-Bus entegrasyonu dâhil; `makepkg --clean --force` `check()` **7/7 PASS**, `ELF_HOME_LEAKS=0`, `RPATH_HITS=0`, kaynak SHA-256 `022c5e52adcee4bb31d253c2c286233ea142f00467e693e4a47131e568e6ca93`, `.pkg.tar.zst` SHA-256 `9842a25c6e6ebd45be921691eae740b51e0d17aab4286881a2d983bf481bcebe`) ve yerel `pacman` deposuna (`15/15` paket, `17/17` SHA-256 Tamam) indekslendi; canlı kurulum (`sudo pacman -U`) kullanıcı onayındadır.

## Paket İçeriği

- `/usr/lib/zethropol/zethropol-zci-runtime` — Loopback-only, pasif bekleme, donanım profili algılama (`dev-baseline` / `ancha-production`), akışlı SHA-256 model manifest doğrulama ve çift yönlü güvenilmeyen içerik mühürleme motoru.
- `/usr/lib/systemd/system/zethropol-zci-runtime.service` — Sertleştirilmiş `systemd` birimi (`IPAddressDeny=any`, `IPAddressAllow=localhost`, `ProtectSystem=strict`, `ProtectHome=read-only`, `NoNewPrivileges=yes`, `MemoryDenyWriteExecute=yes`).
- `/etc/dbus-1/system.d/org.zethropol.ZciRuntime.conf` & `/usr/share/dbus-1/system-services/org.zethropol.ZciRuntime.service` — Sistem D-Bus politikası ve etkinleştirme tanımı.
- `/usr/share/polkit-1/actions/org.zethropol.zci.runtime.policy` — Model yükleme/boşaltma (`org.zethropol.zci.runtime.load`) ve manifest/standby yönetimi (`org.zethropol.zci.runtime.admin`) Polkit kapıları.
- `/usr/share/zethropol/zci-models-manifest.example.json` — Örnek model manifest şablonu.
- `/usr/share/licenses/zethropol-zci-runtime/LICENSE` — MIT lisansı.

Ayrıntılı mimari ve güvenlik sınırları için `docs/ZCI_RUNTIME.md` belgesine bakın.
