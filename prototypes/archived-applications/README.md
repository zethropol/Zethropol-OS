# Archived Prototype — `applications` D-Bus Service (`P4A`)

- **Arşivleme Tarihi:** 2026-11-27
- **Durum:** `ARCHIVED / DO NOT DEPLOY`

Bu dizin, daha önce `services/applications/` altında bulunan erken ve tamamlanmamış Rust D-Bus prototipini yalnızca tarihsel referans olarak saklar.

## Neden Arşivlendi?

- `uninstall_application` çağrısında Polkit yetkilendirmesi (`_header` yoksayılıyordu), paket adı doğrulaması ve korumalı sistem paketi engeli bulunmamakta; `pacman -R --noconfirm` güvensiz biçimde çağrılmaktaydı.
- D-Bus/systemd/Polkit dağıtım tanımları (`deployment/`) ve Arch paket tarifi (`packages/zethropol-applications/`) hiçbir zaman oluşturulmamıştı.

## Kanonik Uygulama Kaldırma Akışı

2026-11-27 tarihli `P4A` mimari kararı doğrultusunda kanonik akış `desktop/control-center/src/ApplicationService.cpp` üzerinde korunmaktadır:
- Toplu `LC_ALL=C pacman -Qo` paket sahipliği çözümü,
- Korumalı sistem ve Zethropol paket listesi (`zethropol-*`, `linux*`, `systemd*`, `pacman`, `plasma-*` vb.),
- Açık kullanıcı onayı (`userConfirmed == true`) ve salt-okunur `pacman -R --print -- <paket>` bağımlılık ön kontrolü,
- `--noconfirm`/`--nodeps`/`--force` olmadan `pkexec /usr/bin/pacman -R -- <paket>` çağrısı.
