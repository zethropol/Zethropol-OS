# Zethropol ZCI Runtime Arch Package (`zethropol-zci-runtime 0.1.0-2`)

Aşama 11 (**ZCI Yerel AI Çalışma Motoru**, `org.zethropol.ZciRuntime.v1`) için root kullanmadan derlenen Arch Linux paketi.

> **Durum (2026-11-28 — `0.1.0-2` Sohbet Kalitesi, Token & Örnekleme Optimizasyonu):** `dev-baseline` `max_output_tokens` tavanı `2048` (`ancha-production` `4096`), varsayılan `request_timeout_ms` `120_000` ms ve `/v1/chat/completions` Türkçe örnekleme parametreleri (`temperature: 0.7`, `top_p: 0.9`, `repeat_penalty: 1.08`) ile root kullanmadan derlenip test edildi (`services/zci-runtime` **7/7 PASS**, `0` uyarı; `services/local-inference` **33/33 PASS**, `-W error`; `makepkg --clean --force` `check()` **7/7 PASS**, `ELF_HOME_LEAKS=0`, `RPATH_HITS=0`, kaynak SHA-256 `980614a2fb50f73d18419a449bccf0f58208b33fc5999a8cf30d8d9501259bc3`, `.pkg.tar.zst` SHA-256 `f11c9d6c12ae23dfbd1d0518a9980935b9d2f341cb40d4863fedf678fdf4e821`) ve yerel `pacman` deposuna (`17/17` paket, `19/19` SHA-256 Tamam) indekslendi; canlı kurulum (`sudo pacman -U`) kullanıcı onayındadır.

## Paket İçeriği

- `/usr/lib/zethropol/zethropol-zci-runtime` — Loopback-only, pasif bekleme, donanım profili algılama (`dev-baseline` / `ancha-production`), akışlı SHA-256 model manifest doğrulama ve çift yönlü güvenilmeyen içerik mühürleme motoru.
- `/usr/lib/systemd/system/zethropol-zci-runtime.service` — Sertleştirilmiş `systemd` birimi (`IPAddressDeny=any`, `IPAddressAllow=localhost`, `ProtectSystem=strict`, `ProtectHome=read-only`, `NoNewPrivileges=yes`, `MemoryDenyWriteExecute=yes`).
- `/etc/dbus-1/system.d/org.zethropol.ZciRuntime.conf` & `/usr/share/dbus-1/system-services/org.zethropol.ZciRuntime.service` — Sistem D-Bus politikası ve etkinleştirme tanımı.
- `/usr/share/polkit-1/actions/org.zethropol.zci.runtime.policy` — Model yükleme/boşaltma (`org.zethropol.zci.runtime.load`) ve manifest/standby yönetimi (`org.zethropol.zci.runtime.admin`) Polkit kapıları.
- `/usr/share/zethropol/zci-models-manifest.example.json` — Örnek model manifest şablonu.
- `/usr/share/licenses/zethropol-zci-runtime/LICENSE` — MIT lisansı.

Ayrıntılı mimari ve güvenlik sınırları için `docs/ZCI_RUNTIME.md` belgesine bakın.
