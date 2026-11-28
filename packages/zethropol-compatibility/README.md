# Zethropol Compatibility Arch Package (`0.1.0-2`)

Bu dizin, Zethropol OS Aşama 06 / Dilim 6A + Aşama 07 kullanıcı-alanı Windows uyumluluk ve gaming katmanı (`zethropol-compatibility`) için Arch Linux paket tarifini içerir. (`0.1.0-2`: `gaming.py` GPU yetenek matrisi, `gaming-probe`/`gaming-status`/`install-dxvk`/`remove-dxvk` alt komutları, zorunlu anti-cheat/EULA onay koruması; `17/17` unittest PASS, `0` `/home/Zevor` sızıntısı, `.pkg.tar.zst` SHA-256 `c6430b7d783e9018473d6e6078701ac34f404fdbe3b3ee4c708f1c5b3f63e403`.)

## Paket İçeriği ve Güvenlik Sınırları

- **Tamamen Ayrıcalıksız Kullanıcı Alanı:** Root servisi, sistem D-Bus politikası veya Polkit kuralı içermez (`docs/SECURITY.md` en az yetki ilkesi).
- **Kurulan Dosyalar:**
  - `/usr/bin/zethropol-prefix-ctl` — İzole Wine prefix yönetimi (`~/.local/share/zethropol/wine-prefixes/<profil>/`), `default-win64` ve `classic-win32` (Wine 11 saf WoW64 uyumlu), Türkçe `tr_TR.UTF-8` + `FontSubstitutes` enjeksiyonu, `Z:\` ve `$HOME` symlink izolasyonu + denetim.
  - `/usr/bin/zethropol-wine-run` — `.exe` / `.msi` / `.lnk` çalıştırıcısı, otomatik donanım Vulkan 1.3+ / DXVK vs. `wined3d` seçimi, varsayılan `bubblewrap` (`bwrap`) salt-okunur kök/ev yazma izolasyonu (`--no-sandbox` ile isteğe bağlı devredışı bırakılabilir) ve çalışma sonrası `.lnk` → `.desktop` dönüştürme kancası.
  - `/usr/lib/zethropol/compatibility/zethropol_compat/*.py` — Çekirdek Python modülleri.
  - `/usr/share/applications/org.zethropol.wine-run.desktop` — MIME işleyici kaydı (`application/x-ms-dos-executable`, `application/x-msi`, `application/x-ms-shortcut`; mevcut `wine.desktop` dosyasına dokunmaz).
  - `/usr/share/licenses/zethropol-compatibility/LICENSE` — MIT lisansı.

## Bağımlılıklar

- **Zorunlu (`depends`):** `python`, `wine`, `ttf-liberation`, `desktop-file-utils`, `shared-mime-info`, `xdg-utils`
- **İsteğe Bağlı (`optdepends`):** `bubblewrap`, `dxvk-mingw-git`, `vkd3d-proton-mingw-git`, `vulkan-tools`, `winetricks`, `wine-gecko`, `wine-mono`, `icoutils`

## Yeniden Üretim ve Doğrulama (Fish Uyumlu, Root Gerektirmez)

```fish
bash -n packages/zethropol-compatibility/PKGBUILD
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest discover -s services/compatibility/tests -v
```
