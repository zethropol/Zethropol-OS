# Zethropol ZCI Daemon (`org.zethropol.ZciDaemon.v1`) — Aşama 10 Mimarisi

**ZCI Daemon** (`services/zci-daemon`, Arch paketi `zethropol-zci-daemon 0.1.0-1`), ZCI ajanlarının (`zci-chat`, `zekans-code`, `zci-command`, `zci-vision`) tüm eylem taleplerini **ZCI Security & Policy Engine** (`org.zethropol.Security.Policy.v1`) kapısından geçirmeden çalıştırmayan, varsayılan-kapalı (*fail-closed*) bir sistem D-Bus geçididir.

## 1. Kapsam (`10.01`–`10.07`)

| Bileşen | Uygulama Özeti |
|---|---|
| **`10.01` Çekirdek Servis & `systemd` Sertleştirme** | `org.zethropol.ZciDaemon` (`/org/zethropol/ZciDaemon`, `org.zethropol.ZciDaemon.v1`); `ProtectSystem=strict`, `ProtectHome=read-only`, `NoNewPrivileges=yes`, `PrivateTmp=yes`, `MemoryDenyWriteExecute=yes`, `CapabilityBoundingSet=`, `IPAddressDeny=any`, `IPAddressAllow=localhost`, `SystemCallFilter=@system-service` + tehlikeli çağrı gruplarının engellenmesi. |
| **`10.02` Policy-Gated Action Gateway** | `ExecuteAction` her talepte (dosya okuma `read_file`, proje analizi `project_analysis`, yerel model sorgusu `model_query`, salt-okunur sistem tanılama `system_diagnostics`, kullanıcı-alanı ön-kontrol `userspace_preflight`) zorunlu olarak `org.zethropol.Security.Policy.v1.EvaluateAction` çağrısı yapar. Politika Motoru erişilemezse veya yanıt `ReadOnly` / `UserSpaceSafe` dışındaysa işlem kesin olarak reddedilir (*fail-closed*). |
| **`10.03` Güvenilmeyen İçerik Sarma Geçidi** | Ajanlara dönen tüm dosya içerikleri, proje analiz özetleri, tanılama çıktıları, kullanıcı istemleri ve yerel model çıktıları `org.zethropol.Security.Policy.v1.WrapUntrustedData` (`executable=false`, `policy_authority="none"`) ile mühürlenir; doğrudan `WrapUntrustedContent` metodu da fail-closed çalışır. |
| **`10.04` Oturum, Hız/Eşzamanlılık ve Pasif Bekleme** | `Idle`, `Busy`, `Standby`, `LockedOut` durum makinesi; ajan başına kayan pencere hız sınırı, ajan/genel eşzamanlılık kotaları ve RAII yuvalı zaman aşımı koruması; `Gemma 4B` (`gemma-4-E4B-it-Q4_K_M`) pasif bekleme (*passive standby*) uyumu (`Standby` modunda `estimated_vram_mb = 0`, yalnızca aktif loopback istek süresince `Active`). |
| **`10.05` Polkit Yetki Sınırları** | Yerel oturum salt-okunur sorguları (`GetDaemonStatus`, `GetState`, `ListSessions`) yetkisiz mutasyon içermez; `TerminateSession` ve `ReloadConfig` işlemleri `org.zethropol.zci.daemon.admin` (`auth_admin`) Polkit kapısına bağlıdır. |
| **`10.06` Sağlık & Gözlemlenebilirlik** | `GetDaemonStatus` / `GetState`: aktif/meşgul/standby/kilitli oturum sayıları, Politika Motoru bağlantı sağlığı, pasif model bellek/VRAM özeti, onaylanan/engellenen/fail-closed/hız-sınırı/eşzamanlılık/zaman aşımı sayaçları ve son karar halka arabelleği. |
| **`10.07` Doğrulama** | `services/zci-daemon` (`cargo test --offline --locked`: 8/8 birim ve özel `dbus-daemon` testi) + `services/local-inference/test_zci_daemon_client.py` (gerçek `zethropol-security` + gerçek `zethropol-zci-daemon` çapraz süreç testi). |

## 2. Değişmez Güvenlik Kuralları

- `zci-daemon`, `/home` veya sistem dosya ağacına asla yazma yapmaz (`ProtectHome=read-only`, `ProtectSystem=strict`).
- `services/recovery/src/recovery_service.rs` içindeki `rollback_execution_enabled=false` ve `repair_execution_enabled=false` kilitleri korunur; `rollback`/`repair` eylemleri hem Politika Motorunda hem geçitte reddedilir.
- Yerel model sorguları yalnızca `127.0.0.1` / `::1` loopback adreslerine izin verir (`IPAddressDeny=any`, `IPAddressAllow=localhost`); dış ağ bağlantısı yapılmaz.
