use super::aur_fixture::Fixture;
const BOOT_A: &str = "11111111-1111-4111-8111-111111111111";
const BOOT_B: &str = "22222222-2222-4222-8222-222222222222";
fn plan() -> CanonicalPlan {
    CanonicalPlan::from_packages(vec![UpdatePackageInfo {
        package_name: "durable-fixture".into(),
        current_version: "1-1".into(),
        new_version: "2-1".into(),
        repository: "aur-attested".into(),
        download_size: 1,
        filename: "durable.pkg.tar".into(),
        sha256: "a".repeat(64),
        repository_metadata_sha256: "b".repeat(64),
    }])
    .unwrap()
    .with_policy_fingerprint("c".repeat(64))
    .unwrap()
}
fn review() -> ReviewedBuild {
    use crate::update_service::ReviewedOutput;
    ReviewedBuild {
        source_package: "durable-fixture".into(),
        source_version: "2-1".into(),
        architecture: "x86_64".into(),
        source_plan_sha256: "a".repeat(64),
        build_root_sha256: "b".repeat(64),
        outputs: vec![ReviewedOutput {
            name: "durable-fixture".into(),
            version: "2-1".into(),
            filename: "durable.pkg.tar".into(),
        }],
    }
}
#[test]
fn reboot_with_reset_clocks_preserves_unknown_commit_and_refuses_replay() {
    let f = Fixture::persistent(BOOT_A, 90);
    let q = f.queue(plan());
    f.store
        .claim_transaction(&q.operation_id, &q.plan, 102)
        .unwrap();
    f.store
        .checkpoint_aur_commit_start(&q.operation_id, &q.plan, "17", 103)
        .unwrap();
    let new = f.reboot(BOOT_B, 1).unwrap();
    let v = new.operation_for_owner(&f.owner, &q.operation_id).unwrap();
    assert_eq!(v.status, OperationStatus::Failed);
    assert_eq!(v.evidence.package_commit, CommitState::Unknown);
    assert_eq!(v.evidence.pre_snapshot, Some("17".into()));
    assert!(v.message.contains("No automatic retry"));
    assert!(new.claim_transaction(&q.operation_id, &q.plan, 2).is_err());
    assert!(new
        .consume_aur_approved_preview(
            f.grant(f.owner.clone(), PolicyAction::Install, 2),
            2,
            &q.plan.hash()
        )
        .is_err());
    assert_eq!(
        f.reboot(BOOT_B, 3)
            .unwrap()
            .operation_for_owner(&f.owner, &q.operation_id)
            .unwrap()
            .evidence
            .package_commit,
        CommitState::Unknown
    );
}
#[test]
fn same_boot_restart_respects_live_worker_lease_then_recovers_dead_worker() {
    let f = Fixture::persistent(BOOT_A, 90);
    let q = f.queue(plan());
    let lease = f.store.worker_lease(&q.operation_id).unwrap();
    f.store
        .claim_transaction(&q.operation_id, &q.plan, 102)
        .unwrap();
    f.store
        .checkpoint_aur_commit_start(&q.operation_id, &q.plan, "17", 103)
        .unwrap();
    assert_eq!(
        f.reboot(BOOT_A, 104)
            .unwrap()
            .operation_for_owner(&f.owner, &q.operation_id)
            .unwrap()
            .status,
        OperationStatus::Running
    );
    assert!(f.reboot(BOOT_B, 1).is_err()); // a contradictory live lease blocks recovery
    drop(lease);
    assert_eq!(
        f.reboot(BOOT_A, 105)
            .unwrap()
            .operation_for_owner(&f.owner, &q.operation_id)
            .unwrap()
            .evidence
            .package_commit,
        CommitState::Unknown
    );
}
#[test]
fn reboot_retires_preview_without_reviving_old_bus_name_or_monotonic_consent() {
    let f = Fixture::persistent(BOOT_A, 90);
    let c = f
        .store
        .reserve_aur_build(
            f.grant(f.owner.clone(), PolicyAction::Preview, 100),
            review(),
            100,
        )
        .unwrap();
    let new = f.reboot(BOOT_B, 1).unwrap();
    assert!(new
        .receipt_context_for_operation(&f.owner, &c.operation_id, 2)
        .is_err());
    assert!(new
        .consume_aur_approved_preview(
            f.grant(f.owner.clone(), PolicyAction::Install, 2),
            2,
            &plan().hash()
        )
        .is_err());
    let v = new
        .aur_history_for_owner(
            f.grant(f.other_peer(), PolicyAction::Preview, 2),
            &c.operation_id,
            2,
        )
        .unwrap();
    assert_eq!(v.status, OperationStatus::Failed);
    assert_eq!(v.evidence.package_commit, CommitState::NotStarted);
    assert!(new.aur_nonce_retained(&c.operation_id).unwrap());
    assert!(new.cleanup(10000, Duration::ZERO).is_err());
    assert!(new.aur_nonce_retained(&c.operation_id).unwrap());
}
#[test]
fn committed_result_and_recovery_ids_survive_boot_with_fresh_read_authorization() {
    let f = Fixture::persistent(BOOT_A, 90);
    let q = f.queue(plan());
    f.store
        .claim_transaction(&q.operation_id, &q.plan, 102)
        .unwrap();
    let mut e = f
        .store
        .checkpoint_aur_commit_start(&q.operation_id, &q.plan, "17", 103)
        .unwrap();
    e.package_commit = CommitState::Committed;
    e.post_snapshot = Some("18".into());
    f.store
        .record_execution_evidence(&q.operation_id, &e)
        .unwrap();
    f.store
        .finish_worker_result(
            &q.operation_id,
            OperationStatus::CompletedWithWarnings,
            "fixture durable outcome",
            104,
        )
        .unwrap();
    let new = f.reboot(BOOT_B, 1).unwrap();
    let fresh =
        VerifiedIdentity::test_identity(":1.999".into(), f.owner.uid, "fresh-boot-session".into())
            .unwrap();
    let v = new
        .aur_history_for_owner(f.grant(fresh, PolicyAction::Preview, 2), &q.operation_id, 2)
        .unwrap();
    assert_eq!(v.status, OperationStatus::CompletedWithWarnings);
    assert_eq!(v.evidence, e);
    let foreign = VerifiedIdentity::test_identity(
        ":1.888".into(),
        f.owner.uid.wrapping_add(1),
        "other".into(),
    )
    .unwrap();
    assert_eq!(
        new.aur_history_for_owner(
            f.grant(foreign, PolicyAction::Preview, 2),
            &q.operation_id,
            2
        ),
        Err(BrokerError::IdentityMismatch)
    );
    assert!(new.claim_transaction(&q.operation_id, &q.plan, 3).is_err());
}
#[test]
fn checksum_detects_valid_record_mutation_and_missing_or_corrupt_boot_anchor() {
    for damage in 0..5 {
        let f = Fixture::persistent(BOOT_A, 90);
        let q = f.queue(plan());
        if damage == 0 {
            let p = f.store.operation_path(&q.operation_id).unwrap();
            let mut envelope: serde_json::Value =
                serde_json::from_slice(&fs::read(&p).unwrap()).unwrap();
            let mut record =
                decode_record(envelope["payload"].as_str().unwrap().as_bytes()).unwrap();
            record.message = "valid record bytes changed without their checksum".into();
            envelope["payload"] = String::from_utf8(encode_record(&record)).unwrap().into();
            fs::write(&p, serde_json::to_vec(&envelope).unwrap()).unwrap();
        } else if damage == 1 {
            fs::remove_file(f.path.join(AUR_EPOCH)).unwrap();
        } else if damage == 2 {
            fs::write(f.path.join(AUR_EPOCH), b"{partial").unwrap();
        } else if damage == 3 {
            fs::remove_file(f.path.join(format!("aur-receipt-{}.spent", q.operation_id))).unwrap();
        } else {
            fs::remove_file(f.path.join(format!("operation-{}.record", q.operation_id))).unwrap();
        }
        assert!(f.reboot(BOOT_B, 1).is_err(), "damage {damage}");
        let retained = if damage == 4 {
            format!("aur-receipt-{}.spent", q.operation_id)
        } else {
            format!("operation-{}.record", q.operation_id)
        };
        assert!(f.path.join(retained).exists());
    }
}
#[test]
fn permissions_symlinks_hardlinks_and_wrong_owner_fail_closed() {
    for damage in 0..6 {
        let f = Fixture::persistent(BOOT_A, 90);
        let q = f.queue(plan());
        let p = f.store.operation_path(&q.operation_id).unwrap();
        match damage {
            0 => fs::set_permissions(&f.path, fs::Permissions::from_mode(0o755)).unwrap(),
            1 => fs::set_permissions(&p, fs::Permissions::from_mode(0o644)).unwrap(),
            2 => fs::set_permissions(f.path.join(".lock"), fs::Permissions::from_mode(0o666))
                .unwrap(),
            3 => {
                let target = f.path.parent().unwrap().join("external");
                fs::rename(&p, &target).unwrap();
                std::os::unix::fs::symlink(target, &p).unwrap();
            }
            4 => {
                fs::hard_link(&p, f.path.parent().unwrap().join("extra-link")).unwrap();
            }
            _ => assert!(BrokerStore::open_aur_persistent_at(
                f.path.clone(),
                unsafe { libc::geteuid() }.wrapping_add(1),
                BOOT_B,
                1,
                false
            )
            .is_err()),
        }
        if damage != 5 {
            assert!(f.reboot(BOOT_B, 1).is_err(), "unsafe case {damage}");
        }
        if damage == 2 {
            assert_eq!(
                fs::metadata(f.path.join(".lock"))
                    .unwrap()
                    .permissions()
                    .mode()
                    & 0o777,
                0o666
            );
        }
    }
}
#[test]
fn retirement_cutoff_is_idempotent_and_replaced_or_expired_previews_are_retained() {
    let f = Fixture::persistent(BOOT_A, 90);
    let c = f
        .store
        .reserve_aur_build(
            f.grant(f.owner.clone(), PolicyAction::Preview, 100),
            review(),
            100,
        )
        .unwrap();
    let original = f.store.preview_path(&f.owner);
    let retired = f
        .path
        .join(format!("retired-preview-{}.record", c.operation_id));
    f.store
        .aur_atomic(&retired, &fs::read(&original).unwrap(), true)
        .unwrap(); // crash after audit publication
    let new = f.reboot(BOOT_B, 1).unwrap();
    assert!(!original.exists());
    assert!(retired.exists());
    let c2 = new
        .reserve_aur_build(
            f.grant(f.owner.clone(), PolicyAction::Preview, 2),
            review(),
            2,
        )
        .unwrap();
    let c3 = new
        .reserve_aur_build(
            f.grant(f.owner.clone(), PolicyAction::Preview, 3),
            review(),
            3,
        )
        .unwrap();
    assert!(new.aur_nonce_retained(&c2.operation_id).unwrap());
    assert!(f
        .path
        .join(format!("retired-preview-{}.record", c2.operation_id))
        .exists());
    let hash = new
        .with_lock(|| Ok(new.read_record(&new.preview_path(&f.owner))?.plan.hash()))
        .unwrap();
    assert!(new
        .consume_approved_preview(
            f.grant(f.owner.clone(), PolicyAction::Install, 304),
            304,
            &hash,
            false
        )
        .is_err());
    assert!(f
        .path
        .join(format!("retired-preview-{}.record", c3.operation_id))
        .exists());
}
#[test]
fn persistence_high_water_guard_keeps_checkpoint_headroom_and_security_evidence() {
    let f = Fixture::persistent(BOOT_A, 90);
    let q = f.queue(plan());
    // Many individually valid sparse orphans cross the logical retention
    // threshold without allocating that amount of disk data.
    for _ in 0..14 {
        let junk = f.path.join(format!("aur-tmp-{}", Uuid::new_v4()));
        let file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(junk)
            .unwrap();
        file.set_len(7 * 1024 * 1024).unwrap();
    }
    assert!(f
        .store
        .reserve_aur_build(
            f.grant(f.owner.clone(), PolicyAction::Preview, 102),
            review(),
            102
        )
        .is_err());
    f.store
        .claim_transaction(&q.operation_id, &q.plan, 102)
        .unwrap();
    f.store
        .checkpoint_aur_commit_start(&q.operation_id, &q.plan, "17", 103)
        .unwrap();
    assert_eq!(
        f.store
            .operation_for_owner(&f.owner, &q.operation_id)
            .unwrap()
            .evidence
            .package_commit,
        CommitState::InProgress
    );
    assert!(f.store.cleanup(99999, Duration::ZERO).is_err());
}
#[test]
fn atomic_publication_and_checkpoint_crash_cutoffs_recover_in_subprocesses() {
    for point in [
        "after-file-fsync",
        "after-rename",
        "after-directory-fsync",
        "before-record-rename",
        "after-record-rename",
    ] {
        let f = Fixture::persistent(BOOT_A, 90);
        let out = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "broker_protocol::persistent_tests::persistent_crash_child",
                "--ignored",
                "--nocapture",
            ])
            .env("ZETHROPOL_AUR_PERSISTENT_DIR", &f.path)
            .env("ZETHROPOL_AUR_CRASH_TARGET", point)
            .output()
            .unwrap();
        assert_eq!(
            out.status.code(),
            Some(86),
            "{point}: {} {}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        );
        let new = f.reboot(BOOT_B, 1).unwrap();
        if point.contains("record-rename") {
            let path = fs::read_dir(&f.path)
                .unwrap()
                .map(|e| e.unwrap().path())
                .find(|p| {
                    p.file_name()
                        .unwrap()
                        .to_string_lossy()
                        .starts_with("operation-")
                })
                .unwrap();
            let r = new.read_record(&path).unwrap();
            assert_eq!(r.status, OperationStatus::Failed);
            assert_eq!(
                r.evidence.package_commit,
                if point == "before-record-rename" {
                    CommitState::NotStarted
                } else {
                    CommitState::Unknown
                }
            );
            assert!(new.claim_transaction(&r.operation_id, &r.plan, 2).is_err());
        } else {
            let marker = f
                .path
                .join("aur-review-33333333-3333-4333-8333-333333333333.json");
            assert_eq!(marker.exists(), point != "after-file-fsync");
            if marker.exists() {
                assert_eq!(
                    new.aur_create_companion(&marker, &serde_json::to_vec(&review()).unwrap()),
                    Err(BrokerError::InvalidToken)
                );
            }
        }
    }
}
#[test]
#[ignore = "subprocess crash fixture only; never runs host or package commands"]
fn persistent_crash_child() {
    let path = std::env::var_os("ZETHROPOL_AUR_PERSISTENT_DIR").expect("private fixture path");
    let store = BrokerStore::open_aur_persistent_at(
        PathBuf::from(path),
        unsafe { libc::geteuid() },
        BOOT_A,
        90,
        false,
    )
    .unwrap();
    let target = std::env::var("ZETHROPOL_AUR_CRASH_TARGET").unwrap();
    if target.contains("record-rename") {
        let owner = VerifiedIdentity::test_identity(
            ":1.400".into(),
            unsafe { libc::geteuid() },
            "fixture-session".into(),
        )
        .unwrap();
        store
            .create_preview(
                AuthorizedCaller::test_authorized(owner.clone(), PolicyAction::Preview, 100),
                plan(),
                100,
            )
            .unwrap();
        let id = store
            .read_record(&store.preview_path(&owner))
            .unwrap()
            .operation_id;
        let p = plan();
        let binding = AurReceiptBinding {
            schema: 1,
            operation_id: id.clone(),
            receipt_sha256: "b".repeat(64),
            plan_hash: p.hash(),
            expires_at: 1000,
            deadline: 400,
            trust_sha256: "d".repeat(64),
            signatures: vec![("durable-fixture".into(), "e".repeat(64))],
            review: review(),
        };
        store
            .aur_create_companion(
                &store.root.join(format!("aur-receipt-{id}.spent")),
                &serde_json::to_vec(&binding).unwrap(),
            )
            .unwrap();
        let q = store
            .consume_aur_approved_preview(
                AuthorizedCaller::test_authorized(owner, PolicyAction::Install, 101),
                101,
                &plan().hash(),
            )
            .unwrap();
        let _lease = store.worker_lease(&q.operation_id).unwrap();
        store
            .claim_transaction(&q.operation_id, &q.plan, 102)
            .unwrap();
        std::env::set_var("ZETHROPOL_AUR_CRASH_POINT", target);
        let _ = store.checkpoint_aur_commit_start(&q.operation_id, &q.plan, "17", 103);
    } else {
        std::env::set_var("ZETHROPOL_AUR_CRASH_POINT", target);
        let path = store
            .root
            .join("aur-review-33333333-3333-4333-8333-333333333333.json");
        let _ = store.aur_create_companion(&path, &serde_json::to_vec(&review()).unwrap());
    }
    panic!("crash point was not reached");
}

#[test]
fn accepted_receipt_tombstone_survives_reboot_and_nonce_reuse_is_rejected() {
    let f = Fixture::persistent(BOOT_A, 90);
    let p = plan();
    f.store
        .create_preview(
            f.grant(f.owner.clone(), PolicyAction::Preview, 100),
            p.clone(),
            100,
        )
        .unwrap();
    let id = f
        .store
        .read_record(&f.store.preview_path(&f.owner))
        .unwrap()
        .operation_id;
    // Synthetic ALREADY-VERIFIED admission state. No private key is generated;
    // cryptographic receipt validation was exercised in the preceding delivery.
    let binding = AurReceiptBinding {
        schema: 1,
        operation_id: id.clone(),
        receipt_sha256: "b".repeat(64),
        plan_hash: p.hash(),
        expires_at: 1000,
        deadline: 400,
        trust_sha256: "d".repeat(64),
        signatures: vec![("durable-fixture".into(), "e".repeat(64))],
        review: review(),
    };
    let marker = f.path.join(format!("aur-receipt-{id}.spent"));
    f.store
        .aur_create_companion(&marker, &serde_json::to_vec(&binding).unwrap())
        .unwrap();
    let q = f
        .store
        .consume_aur_receipt_preview(
            f.grant(f.owner.clone(), PolicyAction::Install, 101),
            101,
            900,
            &p.hash(),
            &"d".repeat(64),
        )
        .unwrap();
    f.store.claim_transaction(&q.operation_id, &p, 102).unwrap();
    f.store
        .checkpoint_aur_commit_start(&q.operation_id, &p, "17", 103)
        .unwrap();
    let bytes = fs::read(&marker).unwrap();
    let new = f.reboot(BOOT_B, 1).unwrap();
    assert_eq!(new.aur_unpack_live_bytes(&bytes), Err(BrokerError::Expired));
    assert_eq!(fs::read(&marker).unwrap(), bytes);
    assert_eq!(
        new.aur_create_companion(&marker, &serde_json::to_vec(&binding).unwrap()),
        Err(BrokerError::InvalidToken)
    );
    assert!(new
        .consume_aur_receipt_preview(
            f.grant(f.owner.clone(), PolicyAction::Install, 2),
            2,
            899,
            &p.hash(),
            &"d".repeat(64)
        )
        .is_err());
    assert_eq!(
        new.operation_for_owner(&f.owner, &id)
            .unwrap()
            .evidence
            .package_commit,
        CommitState::Unknown
    );
    struct ReusedNonce(String);
    impl TokenSource for ReusedNonce {
        fn next_token(&self) -> Result<String, BrokerError> {
            Ok(self.0.clone())
        }
    }
    let mut reuse = new;
    reuse.tokens = Arc::new(ReusedNonce(id.clone()));
    assert_eq!(
        reuse.create_preview(f.grant(f.owner.clone(), PolicyAction::Preview, 3), p, 3),
        Err(BrokerError::InvalidToken)
    );
}
#[test]
fn deleting_boot_anchor_cannot_downgrade_a_live_persistent_handle_to_runtime() {
    let f = Fixture::persistent(BOOT_A, 90);
    let q = f.queue(plan());
    fs::remove_file(f.path.join(AUR_EPOCH)).unwrap();
    assert!(f.store.cleanup(10000, Duration::ZERO).is_err());
    assert!(f
        .store
        .claim_transaction(&q.operation_id, &q.plan, 102)
        .is_err());
    assert!(f
        .path
        .join(format!("operation-{}.record", q.operation_id))
        .exists());
}
