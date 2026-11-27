# Zethropol OS — Tekrarlanabilir Yerel Pacman Deposu (`zethropol-local`)

Bu dizin, Zethropol OS çekirdek servisleri, masaüstü bileşenleri ve bağımsız uygulamaları için tekrarlanabilir yerel `pacman` deposu (`[zethropol-local]`) tanımını içerir.

## Yapı

- `manifest.txt`: Depoya dahil edilecek güncel `x86_64` paketlerinin kanonik listesi (`12` paket).
- `x86_64/` *(Git tarafından izlenmez)*: `scripts/build-local-repo.sh` tarafından üretilen `.pkg.tar.zst` dosyaları, `zethropol-local.db.tar.gz` / `zethropol-local.files.tar.gz` veritabanları ve `SHA256SUMS`.

## Yeniden Üretim ve Doğrulama (Fish Uyumlu, Root Gerektirmez)

```fish
bash scripts/build-local-repo.sh
begin; cd packages/repo/x86_64; and sha256sum -c SHA256SUMS; end
```

## ArchISO (`build/recovery/`) Entegrasyonu

`build/recovery/pacman.conf` geliştiriciye özel mutlak yollar (`/home/<user>`) içermez. ISO derleme hazırlığında `scripts/prepare-recovery-iso.sh` betiği yerel depo yolunu çalışma zamanında geçici bir `pacman.conf` dosyasına enjekte eder ve `mkarchiso -C <runtime-pacman.conf>` ile kullanım için tek komutluk onay çıktısı üretir.
