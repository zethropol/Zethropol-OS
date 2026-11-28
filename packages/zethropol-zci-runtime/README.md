# Zethropol ZCI Runtime Arch Package (`zethropol-zci-runtime 0.1.0-3`)

Aşama 11 (**ZCI Yerel AI Çalışma Motoru**, `org.zethropol.ZciRuntime.v1`) için root kullanmadan derlenen Arch Linux paketi.

> **Durum (2026-11-28 — `0.1.0-3` HTTP Chunked/JSON Ayrıştırma & `think=false` Hızlandırma):** RFC 9112 bayt-düzeyli `Transfer-Encoding: chunked` çözücü (`dechunk_http_body`), ham chunk/JSON/`reasoning` sızıntısını önleyen kesin `choices[0].message.content` (`message.content` / `response`) ayrıştırıcısı ve Gemma 4 / Ollama için `"think": false` parametresiyle root kullanmadan derlenip test edildi (`services/zci-runtime` **8/8 PASS**, `0` uyarı; `services/local-inference` **33/33 PASS**, `-W error`; `makepkg --clean --force` `check()` **8/8 PASS**, `ELF_HOME_LEAKS=0`, `RPATH_HITS=0`, kaynak SHA-256 `e4b90ba67038bd2fe14dd3758f5cb448a979a50391ff641e87a80f68cf62a45d`, `.pkg.tar.zst` SHA-256 `d4b8ea50308d2581b8412a23f4e60a2259be0f59335ac98201195d9e2d7ef903`) ve yerel `pacman` deposuna (`17/17` paket, `19/19` SHA-256 Tamam) indekslendi; canlı kurulum (`sudo pacman -U`) kullanıcı onayındadır.

## Paket İçeriği

- `/usr/lib/zethropol/zethropol-zci-runtime` — Loopback-only, pasif bekleme, donanım profili algılama (`dev-baseline` / `ancha-production`), akışlı SHA-256 model manifest doğrulama ve çift yönlü güvenilmeyen içerik mühürleme motoru.
- `/usr/lib/systemd/system/zethropol-zci-runtime.service` — Sertleştirilmiş `systemd` birimi (`IPAddressDeny=any`, `IPAddressAllow=localhost`, `ProtectSystem=strict`, `ProtectHome=read-only`, `NoNewPrivileges=yes`, `MemoryDenyWriteExecute=yes`).
- `/etc/dbus-1/system.d/org.zethropol.ZciRuntime.conf` & `/usr/share/dbus-1/system-services/org.zethropol.ZciRuntime.service` — Sistem D-Bus politikası ve etkinleştirme tanımı.
- `/usr/share/polkit-1/actions/org.zethropol.zci.runtime.policy` — Model yükleme/boşaltma (`org.zethropol.zci.runtime.load`) ve manifest/standby yönetimi (`org.zethropol.zci.runtime.admin`) Polkit kapıları.
- `/usr/share/zethropol/zci-models-manifest.example.json` — Örnek model manifest şablonu.
- `/usr/share/licenses/zethropol-zci-runtime/LICENSE` — MIT lisansı.

Ayrıntılı mimari ve güvenlik sınırları için `docs/ZCI_RUNTIME.md` belgesine bakın.
