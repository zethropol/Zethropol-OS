# Zethropol OS — Hata, İptal ve Kesinti Test Matrisi (03.2.11)

Bu belge, 03.2.01–03.2.11 kapsamındaki zorunlu güvenlik alanlarının gerçek test fonksiyonlarıyla eşleştirmesini ve kapsam durumunu belgeler.

## 1. 03.2.11 Zorunlu Güvenlik Alanları (14 Alan)

| # | Zorunlu Alan | Gerçek Test Fonksiyonları | Durum |
|---|---|---|---|
| 1 | Snapper komut hatası, eksik snapshot, bozuk alt hacim | `scenario_6_backend_unavailable`, `scenario_7_malformed_snapper_output`, `create_fails_verification_when_id_missing_or_unlisted`, `pre_update_5_snapper_failure_or_bad_btrfs_transitions_to_completed_failed` | **Doğrudan test edildi** (izole mock) |
| 2 | Polkit reddi, iptali ve D-Bus sahiplik ihlali | `polkit_denial_and_cancellation_are_distinguished`, `polkit_caller_identity_validation_rejects_malformed_senders`, `polkit_failure_modes_timeout_and_missing_helper_fail_safely`, `rollback_and_snapshot_permissions_are_strictly_separated`, `cancel_operation_enforces_caller_identity_and_ownership` | **Doğrudan test edildi** (izole mock) |
| 3 | Eşzamanlı Recovery veya Update istekleri | `pre_update_3_coordination_storage_polkit_and_critical_boot_blocks`, `pre_update_6_concurrent_create_rejected_without_clobbering_reservation`, `repair_policy_4_blocks_when_active_op_or_uncertain_reservation_exists`, `cancelledAndUnknownIsolatedUpdatePreserveSnapshotAndGuidance` | **Doğrudan test edildi** (izole) |
| 4 | D-Bus bağlantısının kopması ve servis çökmesi | `pre_update_2_disconnect_and_uncertain_state_blocks_subsequent_updates`, `pre_update_7_unrelated_sender_disconnect_does_not_orphan_owner_reservation`, `post_update_4_restart_power_loss_and_unverifiable_boot_guidance`, `dbusUnavailablePreGateFailsClosedAndNeverRunsUpdater` | **Doğrudan test edildi** (simülasyon) |
| 5 | Yarım kalmış paket yükseltmesi ve belirsiz pacman durumu | `post_update_3_aborted_orphaned_without_dblock_and_dblock_present`, `repair_policy_3_blocks_home_db_lck_missing_snap_and_boot_mismatch`, `failedIsolatedUpdatePreservesSnapshotAndReportsFailedOutcome` | **Kısmen test edildi** (canlı `libalpm` kesintisi kapsam dışı; izole) |
| 6 | `db.lck` mevcut, eksik veya bayat göründüğündeki davranışlar | `pre_update_3_coordination_storage_polkit_and_critical_boot_blocks`, `post_update_3_aborted_orphaned_without_dblock_and_dblock_present`, `post_update_5_stale_empty_dblock_and_no_side_effects_in_assessment_or_policy` | **Doğrudan test edildi** (izole) |
| 7 | Snapshot oluşturma ile doğrulama arasında durum değişmesi (TOCTOU) | `boot_11_toctou_boot_or_snapshot_change_during_preflight_blocks`, `pre_update_8_toctou_between_create_and_verify_blocks_execution` | **Doğrudan test edildi** (izole) |
| 8 | `/home` izolasyonunun bozulması | `preflight_blocks_shared_root_subvolume_for_home`, `preflight_blocks_missing_home_mount`, `preflight_blocks_nested_or_conflicting_home_subvolume`, `preflight_returns_unknown_when_mount_and_fstab_unverifiable`, `pre_update_8_toctou_between_create_and_verify_blocks_execution` | **Doğrudan test edildi** (izole) |
| 9 | `/boot`, Limine, kernel ve modül uyumsuzluğu | `boot_1_valid_limine_kernel_initramfs_layout_passes` .. `boot_10_unverifiable_snapshot_modules_prevents_ready`, `pre_update_4_critical_boot_update_blocks_on_boot_mismatch` | **Doğrudan test edildi** (izole; canlı `/boot` `umask=0077` nedeniyle **Canlı doğrulama bekliyor**) |
| 10 | Yetersiz depolama, Snapper temizlik sınırları | `pre_update_3_coordination_storage_polkit_and_critical_boot_blocks` | **Doğrudan test edildi** (izole) |
| 11 | Sahte/doğrulanamayan haricî yedek ve kullanıcı onayı beyanları | `repair_policy_2_client_backup_and_confirmation_claims_never_grant_ready_or_exec`, `post_update_2_client_healthy_claim_and_completed_ok_yield_unknown` | **Doğrudan test edildi** (izole) |
| 12 | Update, rollback veya onarım yürütme bayraklarının kapalı kalması | `rollback_and_snapshot_permissions_are_strictly_separated`, `preUpdateGateDefaultsToLiveExecutionDisabledAndBlocksUpdater`, `repair_policy_2_client_backup_and_confirmation_claims_never_grant_ready_or_exec` | **Doğrudan test edildi** |
| 13 | Kurtarma rehberliğinin yanlışlıkla gerçek işlem başlatmaması | `readonly_operations_do_not_invoke_pkcheck`, `post_update_5_stale_empty_dblock_and_no_side_effects_in_assessment_or_policy`, `cancelledAndUnknownIsolatedUpdatePreserveSnapshotAndGuidance` | **Doğrudan test edildi** |
| 14 | 03.2.09 ve 03.2.10 senaryolarının test kapsamı | `post_update_1` .. `post_update_5`, `repair_policy_1` .. `repair_policy_4` | **Doğrudan test edildi** (canlı sistem senaryoları hariç) |

## 2. 03.2.09 (12 Senaryo) ve 03.2.10 (10 Senaryo) Kapsam Özeti

- **03.2.09 Senaryoları 1–8, 10–12**: `post_update_1`..`post_update_5`, `failedIsolatedUpdatePreservesSnapshotAndReportsFailedOutcome`, `cancelledAndUnknownIsolatedUpdatePreserveSnapshotAndGuidance` ile **Doğrudan test edildi** (izole); Senaryo 9 (*sistemin normal açılmaması — canlı oturum dışı*) yalnızca **Canlı doğrulama bekliyor** (Limine / `arch-chroot` ortamında doğrulanabilir).
- **03.2.10 Senaryoları 1–10**: `repair_policy_1`..`repair_policy_4`, `post_update_5` ve `rollback_and_snapshot_permissions_are_strictly_separated` ile **Doğrudan test edildi** (izole).

## 3. Açık Kapsam Boşlukları ve Canlı Doğrulama Bekleyenler

- Canlı sistemde `/boot` (`vfat`, `umask=0077`) okuma, gerçek `snapper`/`btrfs` alt hacim oluşturma, gerçek `pacman`/`snap-pac` `PreTransaction`/`PostTransaction` kancaları ve açılışta Limine snapshot boot + `limine-snapper-restore` adımları **Canlı doğrulama bekliyor**.
- `UpdateInstaller` ile `RecoveryManager` arasındaki gerçek `zbus` <-> `QDBusConnection` sistem bus entegrasyonu canlı D-Bus daemon üzerinde çalıştırılmamıştır; birimler ayrı ayrı izole testlerle doğrulanmıştır.
