# Control Center geliştirme

Nihai sürüm tamamlanana kadar uygulama kurulmadan build dizininden çalıştırılır.

Depo kökünden:

```sh
cmake -S desktop/control-center -B desktop/control-center/build -DCMAKE_SKIP_INSTALL_RULES=ON -DBUILD_TESTING=ON
cmake --build desktop/control-center/build -j 2
ctest --test-dir desktop/control-center/build --output-on-failure
./desktop/control-center/build/control_center
```

Kaynak: `/home/Zevor/Zethropol-OS/desktop/control-center/`

Build: `/home/Zevor/Zethropol-OS/desktop/control-center/build/`


`cmake --install`, `/usr/local` kurulumu, paket kurulumu ve sistem geneline dağıtım yapılmaz. Servisler, D-Bus/PolicyKit yapılandırmaları, kullanıcı verileri ve yedekler korunur.

Önceki build çalıştırılabilir dosyası `build/control_center.before-dev-workflow-20261027` olarak saklanmıştır. `build/update-ready-2026-10-27/` çıktısı da korunur.

Hardware dahil tüm servis bağlantıları standart sistem D-Bus üzerinden yapılır.
Özel başlatıcı veya Hardware'e özel veri yolu yönlendirmesi kullanılmaz. Donanım
alanları için güncel `zethropol-hardware` sistem paketi gerekir; paket tarifi
`packages/zethropol-hardware/PKGBUILD` içindedir. Control Center build dizininden
çalışmaya devam eder ve sistem geneline kurulmaz. Hardware sistem paketi güncellemesi
ayrıca kullanıcı onayı gerektirir; uygulama kendiliğinden servis kurmaz veya güncellemez.
