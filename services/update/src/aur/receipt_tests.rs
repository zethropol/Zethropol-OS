// Separate processes isolate native GPGME's cached keyring context. All signing
// keys, policies, broker records and native databases live in temporary roots.
macro_rules! receipt_case {
    ($name:ident, $case:literal) => {
        #[test]
        fn $name() {
            let out = Command::new(std::env::current_exe().unwrap())
                .args(["--exact", "update_service::aur_integration_tests::aur_signed_fd_intake_to_native_commit_persists_result_in_private_root", "--nocapture", "--test-threads=1"])
                .env("ZETHROPOL_AUR_SIGNED_CHILD", "receipt")
                .env("ZETHROPOL_AUR_RECEIPT_CASE", $case).output().unwrap();
            assert!(out.status.success(), "{} {}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
        }
    }
}
receipt_case!(receipt_schema_and_expiry_are_strict, "schema");
receipt_case!(
    receipt_retention_quota_does_not_replace_current_preview,
    "quota"
);
receipt_case!(receipt_approved_plan_reaches_signed_native_commit, "commit");
receipt_case!(
    receipt_wrong_user_operation_source_and_output_are_rejected,
    "identity"
);
receipt_case!(
    receipt_role_signatures_and_policy_pins_are_independent,
    "crypto"
);
receipt_case!(
    receipt_archive_metadata_and_signature_hash_are_bound,
    "archive"
);
receipt_case!(
    receipt_plan_requires_install_grant_and_rejects_replay_and_drift,
    "replay"
);
receipt_case!(receipt_crash_before_publication_burns_admission, "crash");
receipt_case!(receipt_seals_and_policy_snapshots_prevent_toctou, "toctou");
receipt_case!(
    receipt_final_resolution_keeps_official_upgrade_barrier,
    "final"
);

// Keep all independent trust artifacts explicit at the fixture boundary.
#[allow(clippy::too_many_arguments)]
fn receipt_fixture_chain(
    f: &Fixture,
    env: &AlpmFixture,
    home: &Path,
    gpg: &impl Fn(&[&str]) -> Vec<u8>,
    archive: &fs::File,
    signature: &fs::File,
    parent: &Path,
    handle: &mut Alpm,
) {
    use std::os::fd::AsFd;
    let now = receipt_unix_now().unwrap();
    let root = RootManifest {
        schema: 1,
        purpose: "zethropol-aur-build-root".into(),
        architecture: "x86_64".into(),
        expires: now + 600,
        packages: vec![RootPackage {
            name: "lab-tool".into(),
            version: "1-1".into(),
            sha256: "a".repeat(64),
        }],
    };
    let root_bytes = serde_json::to_vec(&root).unwrap();
    let review = ReviewedBuild {
        source_package: "aur-fixture".into(),
        source_version: "2-1".into(),
        architecture: "x86_64".into(),
        source_plan_sha256: "d".repeat(64),
        build_root_sha256: receipt_hash(&root_bytes),
        outputs: vec![ReviewedOutput {
            name: "aur-fixture".into(),
            version: "2-1".into(),
            filename: env.archive.identity.filename.clone(),
        }],
    };
    let context = f
        .store
        .reserve_aur_build(
            f.grant(f.owner.clone(), PolicyAction::Preview, 100),
            review.clone(),
            100,
        )
        .unwrap();
    let reopened_context = f
        .reopen()
        .receipt_context_for_operation(&f.owner, &context.operation_id, 101)
        .unwrap();
    assert_eq!(context, reopened_context);
    assert!(f
        .store
        .receipt_context_for_operation(&f.other_peer(), &context.operation_id, 101)
        .is_err());
    let sign = |bytes: &[u8]| {
        let path = f.path.join(format!("signed-{}.json", uuid::Uuid::new_v4()));
        fs::write(&path, bytes).unwrap();
        gpg(&["--detach-sign", path.to_str().unwrap()]);
        let signature = fs::read(format!("{}.sig", path.display())).unwrap();
        (
            seal_receipt_bytes(bytes).unwrap(),
            seal_receipt_bytes(&signature).unwrap(),
        )
    };
    let (manifest, manifest_sig) = sign(&root_bytes);
    let policy_dir = f.path.join("lab-receipt-policy");
    fs::create_dir(&policy_dir).unwrap();
    let keys = String::from_utf8(gpg(&["--with-colons", "--list-keys"])).unwrap();
    let fingerprint = keys
        .lines()
        .find(|l| l.starts_with("fpr:"))
        .unwrap()
        .split(':')
        .nth(9)
        .unwrap();
    let keyring = gpg(&["--export"]);
    fs::write(policy_dir.join("lab.gpg"), &keyring).unwrap();
    let role = || ReceiptRoleConfig {
        keyring: "lab.gpg".into(),
        keyring_sha256: receipt_hash(&keyring),
        signers: vec![fingerprint.into()],
    };
    let config = ReceiptPolicyConfig {
        schema: 1,
        purpose: "zethropol-aur-trust".into(),
        manifest: role(),
        receipt: role(),
        archive: role(),
    };
    let policy = policy_dir.join("policy.json");
    fs::write(&policy, serde_json::to_vec(&config).unwrap()).unwrap();
    let mut trust = ReceiptTrust::load_owned(&policy, unsafe { libc::geteuid() }, false).unwrap();
    let base = BuildReceipt {
        schema: 1,
        purpose: "zethropol-aur-build-receipt".into(),
        operation_id: context.operation_id.clone(),
        uid: context.uid,
        bus_name: context.bus_name.clone(),
        session_id: context.session_id.clone(),
        source_package: review.source_package.clone(),
        source_version: review.source_version.clone(),
        architecture: review.architecture.clone(),
        build_root_sha256: review.build_root_sha256.clone(),
        source_plan_sha256: review.source_plan_sha256.clone(),
        issued_at: now - 1,
        expires_at: now + 120,
        packages: vec![ReceiptOutput {
            name: "aur-fixture".into(),
            version: "2-1".into(),
            architecture: "any".into(),
            filename: env.archive.identity.filename.clone(),
            sha256: env.archive.identity.sha256.clone(),
            signature_sha256: receipt_hash(
                &read_receipt_fd(signature.as_fd(), 1024 * 1024).unwrap(),
            ),
        }],
    };
    let pairs = [(archive.as_fd(), signature.as_fd())];
    let case = std::env::var("ZETHROPOL_AUR_RECEIPT_CASE").unwrap();
    // The callback models the server's native policy stamp, not client input.
    let stamp = "c".repeat(64);
    let mut verify_raw = |bytes: &[u8], sig: &fs::File, c: &ReceiptContext, t: &ReceiptTrust| {
        let data = seal_receipt_bytes(bytes).unwrap();
        verify_aur_receipt(
            c,
            ReceiptFds {
                receipt: data.as_fd(),
                receipt_signature: sig.as_fd(),
                manifest: manifest.as_fd(),
                manifest_signature: manifest_sig.as_fd(),
                packages: &pairs,
            },
            t,
            parent,
            handle,
            now,
            || Ok(stamp.clone()),
        )
    };
    let mut verify = |r: &BuildReceipt, c: &ReceiptContext, t: &ReceiptTrust| {
        let bytes = serde_json::to_vec(r).unwrap();
        let (_, sig) = sign(&bytes);
        verify_raw(&bytes, &sig, c, t)
    };
    match case.as_str() {
        "quota" => {
            let path = f.path.join("aur-review-quota.json");
            let file = fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .mode(0o600)
                .open(path)
                .unwrap();
            file.set_len(64 * 1024 * 1024).unwrap(); // sparse; no allocation or host mutation
            assert!(f
                .store
                .reserve_aur_build(
                    f.grant(f.owner.clone(), PolicyAction::Preview, 101),
                    review.clone(),
                    101
                )
                .is_err());
            assert_eq!(
                f.store
                    .receipt_context_for_operation(&f.owner, &context.operation_id, 102)
                    .unwrap(),
                context
            );
        }
        "schema" => {
            for change in 0..8 {
                let mut r = base.clone();
                match change {
                    0 => r.schema = 2,
                    1 => r.purpose = "worker-says-ok".into(),
                    2 => r.expires_at = now,
                    3 => r.issued_at = now + 1,
                    4 => r.expires_at = now + 301,
                    5 => r.packages.clear(),
                    6 => r.packages.push(r.packages[0].clone()),
                    _ => r.build_root_sha256 = "0".repeat(64),
                }
                assert!(
                    verify(&r, &context, &trust).is_err(),
                    "schema/lifetime mutation {change}"
                );
            }
            drop(verify);
            for bytes in [b"{}".to_vec(), br#"{"schema":1,"schema":1}"#.to_vec(), {
                let mut json = serde_json::to_value(&base).unwrap();
                json["unexpected"] = true.into();
                serde_json::to_vec(&json).unwrap()
            }] {
                let (_, sig) = sign(&bytes);
                assert!(verify_raw(&bytes, &sig, &context, &trust).is_err());
            }
        }
        "identity" => {
            for change in 0..11 {
                let mut r = base.clone();
                match change {
                    0 => r.uid = r.uid.wrapping_add(1),
                    1 => r.bus_name = ":1.999".into(),
                    2 => r.session_id = "different-session".into(),
                    3 => r.operation_id = uuid::Uuid::new_v4().to_string(),
                    4 => r.source_package = "other-source".into(),
                    5 => r.source_version = "99-1".into(),
                    6 => r.source_plan_sha256 = "0".repeat(64),
                    7 => r.architecture = "aarch64".into(),
                    8 => r.packages[0].name = "other-output".into(),
                    9 => r.packages[0].version = "3-1".into(),
                    _ => r.packages[0].filename = "other.pkg.tar".into(),
                }
                assert!(
                    verify(&r, &context, &trust).is_err(),
                    "identity mutation {change}"
                );
            }
        }
        "crypto" => {
            drop(verify);
            let bytes = serde_json::to_vec(&base).unwrap();
            let (_, sig) = sign(&bytes);
            let mut changed_receipt = base.clone();
            changed_receipt.expires_at -= 1; // valid JSON/identity/lifetime; wrong signature
            let changed = serde_json::to_vec(&changed_receipt).unwrap();
            assert!(verify_raw(&changed, &sig, &context, &trust).is_err());
            let bad = seal_receipt_bytes(b"invalid detached signature").unwrap();
            assert!(verify_raw(&bytes, &bad, &context, &trust).is_err());
            for role in 0..3 {
                let pin = if role == 0 {
                    &mut trust.receipt.signers
                } else if role == 1 {
                    &mut trust.manifest.signers
                } else {
                    &mut trust.archive.signers
                };
                let original = pin.clone();
                *pin = vec!["A".repeat(40)];
                assert!(
                    verify_raw(&bytes, &sig, &context, &trust).is_err(),
                    "wrong role signer {role}"
                );
                if role == 0 {
                    trust.receipt.signers = original;
                } else if role == 1 {
                    trust.manifest.signers = original;
                } else {
                    trust.archive.signers = original;
                }
            }
            fs::write(policy_dir.join("lab.gpg"), b"changed keyring").unwrap();
            assert!(ReceiptTrust::load_owned(&policy, unsafe { libc::geteuid() }, false).is_err());
            fs::write(policy_dir.join("lab.gpg"), &keyring).unwrap();
            fs::set_permissions(&policy, fs::Permissions::from_mode(0o666)).unwrap();
            assert!(ReceiptTrust::load_owned(&policy, unsafe { libc::geteuid() }, false).is_err());
        }
        "archive" => {
            for change in 0..4 {
                let mut r = base.clone();
                match change {
                    0 => r.packages[0].sha256 = "0".repeat(64),
                    1 => r.packages[0].signature_sha256 = "0".repeat(64),
                    2 => r.packages[0].architecture = "x86_64".into(),
                    _ => r.packages[0].sha256 = "not-a-digest".into(),
                }
                assert!(
                    verify(&r, &context, &trust).is_err(),
                    "archive mutation {change}"
                );
            }
            let mut wrong_base = context.clone();
            wrong_base.review.source_package = "other-base".into();
            let mut r = base.clone();
            r.source_package = "other-base".into();
            assert!(verify(&r, &wrong_base, &trust).is_err());
        }
        "commit" => {
            let verified = verify(&base, &context, &trust).unwrap();
            let plan = f
                .store
                .admit_aur_receipt(
                    f.grant(f.owner.clone(), PolicyAction::Preview, 101),
                    &verified,
                    101,
                    now,
                )
                .unwrap();
            assert!(f
                .store
                .aur_commit_evidence(&context.operation_id, &plan)
                .is_err());
            let q = f
                .store
                .consume_aur_receipt_preview(
                    f.grant(f.owner.clone(), PolicyAction::Install, 102),
                    102,
                    now,
                    &plan.hash(),
                    &trust.digest,
                )
                .unwrap();
            drop(verify);
            drop(verify_raw);
            let locked = prepare_aur_locked(handle, &verified.intake.archives, &stamp).unwrap();
            assert_eq!(locked.plan, plan);
            let binding = f
                .store
                .aur_receipt_binding(&q.operation_id, &plan, 103, now, &trust.digest)
                .unwrap();
            verify_receipt_signature_delivery(&plan, &binding.signatures, &pairs).unwrap();
            locked.claim(&f.store, &q.operation_id, 103).unwrap();
            aur_checkpoint_after_snapshot(&f.store, &q.operation_id, &plan, Ok("17".into()), 104)
                .unwrap();
            aur_commit_started_with_post(locked, &f.store, &q.operation_id, |_| Ok("18".into()))
                .unwrap();
            assert_eq!(
                fs::read(env.root.join("aur-lab-payload")).unwrap(),
                b"signed isolated payload\n"
            );
            let view = f
                .reopen()
                .operation_for_owner(&f.owner, &q.operation_id)
                .unwrap();
            assert_eq!(view.evidence.package_commit, CommitState::Committed);
            assert_eq!(view.status, OperationStatus::CompletedWithWarnings);
            assert!(f
                .store
                .claim_transaction(&q.operation_id, &plan, 105)
                .is_err());
        }
        "replay" => {
            let verified = verify(&base, &context, &trust).unwrap();
            assert_eq!(
                verified.plan().packages()[0].sha256,
                base.packages[0].sha256
            );
            assert_eq!(
                verified.plan().packages()[0].repository_metadata_sha256,
                verified.receipt_sha256()
            );
            assert!(f
                .store
                .admit_aur_receipt(
                    f.grant(f.other_peer(), PolicyAction::Preview, 101),
                    &verified,
                    101,
                    now
                )
                .is_err());
            assert!(f
                .store
                .admit_aur_receipt(
                    f.grant(f.owner.clone(), PolicyAction::Install, 101),
                    &verified,
                    101,
                    now
                )
                .is_err());
            let plan = f
                .store
                .admit_aur_receipt(
                    f.grant(f.owner.clone(), PolicyAction::Preview, 101),
                    &verified,
                    101,
                    now,
                )
                .unwrap();
            assert!(f
                .reopen()
                .admit_aur_receipt(
                    f.grant(f.owner.clone(), PolicyAction::Preview, 102),
                    &verified,
                    102,
                    now
                )
                .is_err());
            assert!(f
                .store
                .consume_aur_receipt_preview(
                    f.grant(f.owner.clone(), PolicyAction::Preview, 102),
                    102,
                    now,
                    &plan.hash(),
                    &trust.digest
                )
                .is_err());
            assert!(f
                .store
                .consume_aur_receipt_preview(
                    f.grant(f.owner.clone(), PolicyAction::Install, 102),
                    102,
                    now,
                    &"0".repeat(64),
                    &trust.digest
                )
                .is_err());
            let q = f
                .reopen()
                .consume_aur_receipt_preview(
                    f.grant(f.owner.clone(), PolicyAction::Install, 102),
                    102,
                    now,
                    &plan.hash(),
                    &trust.digest,
                )
                .unwrap();
            assert_eq!(q.operation_id, context.operation_id);
            assert!(f
                .store
                .aur_receipt_binding(&q.operation_id, &plan, 103, now + 121, &trust.digest)
                .is_err());
            assert!(f
                .store
                .aur_receipt_binding(&q.operation_id, &plan, 103, now, &"e".repeat(64))
                .is_err());
            assert!(f
                .store
                .aur_receipt_binding(&q.operation_id, &plan, 250, now - 100, &trust.digest)
                .is_err());
            let binding = f
                .store
                .aur_receipt_binding(&q.operation_id, &plan, 103, now, &trust.digest)
                .unwrap();
            verify_receipt_signature_delivery(&plan, &binding.signatures, &pairs).unwrap();
            let bad = seal_receipt_bytes(b"changed signature").unwrap();
            assert!(verify_receipt_signature_delivery(
                &plan,
                &binding.signatures,
                &[(archive.as_fd(), bad.as_fd())]
            )
            .is_err());
            let mut different = plan.packages().to_vec();
            different[0].new_version = "9-1".into();
            let different = CanonicalPlan::from_packages(different)
                .unwrap()
                .with_policy_fingerprint(stamp.clone())
                .unwrap();
            assert!(f
                .store
                .claim_transaction(&q.operation_id, &different, 103)
                .is_err());
            assert!(f
                .store
                .claim_transaction(&q.operation_id, &plan, 104)
                .is_err()); // drift failure is terminal
                            // Independent operation exercises durable start -> process-stop uncertainty.
            let g = Fixture::new();
            let q = g.queue(plan.clone());
            g.store
                .claim_transaction(&q.operation_id, &q.plan, 102)
                .unwrap();
            aur_checkpoint_after_snapshot(&g.store, &q.operation_id, &q.plan, Ok("17".into()), 103)
                .unwrap();
            g.store
                .worker_process_stopped(&q.operation_id, 104)
                .unwrap();
            assert_eq!(
                g.reopen()
                    .operation_for_owner(&g.owner, &q.operation_id)
                    .unwrap()
                    .evidence
                    .package_commit,
                CommitState::Unknown
            );
            assert!(g
                .store
                .claim_transaction(&q.operation_id, &q.plan, 105)
                .is_err());
        }
        "crash" => {
            let verified = verify(&base, &context, &trust).unwrap();
            // Simulate the durable cutoff AFTER O_EXCL admission reservation but
            // BEFORE preview publication. Reopening must never replay it.
            let marker = f
                .path
                .join(format!("aur-receipt-{}.spent", context.operation_id));
            let file = fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .mode(0o600)
                .open(marker)
                .unwrap();
            file.sync_all().unwrap();
            fs::File::open(&f.path).unwrap().sync_all().unwrap();
            assert!(f
                .reopen()
                .admit_aur_receipt(
                    f.grant(f.owner.clone(), PolicyAction::Preview, 101),
                    &verified,
                    101,
                    now
                )
                .is_err());
            assert!(f
                .store
                .consume_aur_approved_preview(
                    f.grant(f.owner.clone(), PolicyAction::Install, 102),
                    102,
                    &verified.plan().hash()
                )
                .is_err());
        }
        "toctou" => {
            assert!(archive.set_len(1).is_err());
            assert!(signature.write_at(b"x", 0).is_err());
            let verified = verify(&base, &context, &trust).unwrap();
            fs::write(&env.archive.path, b"caller path replaced after sealing").unwrap();
            let frozen = verify(&base, &context, &trust).unwrap();
            assert_eq!(verified.plan(), frozen.plan());
            fs::write(
                policy_dir.join("lab.gpg"),
                b"keyring changed after verification",
            )
            .unwrap();
            assert!(ReceiptTrust::load_owned(&policy, unsafe { libc::geteuid() }, false).is_err());
            // Held keyring snapshots still verify the original bytes; the real
            // worker reloads policy and checks its stamp again before commit.
            assert!(verify(&base, &context, &trust).is_ok());
            let replacement = f
                .store
                .reserve_aur_build(
                    f.grant(f.owner.clone(), PolicyAction::Preview, 102),
                    review.clone(),
                    102,
                )
                .unwrap();
            assert_ne!(replacement.operation_id, context.operation_id);
            assert!(f
                .store
                .admit_aur_receipt(
                    f.grant(f.owner.clone(), PolicyAction::Preview, 103),
                    &verified,
                    103,
                    now
                )
                .is_err());
        }
        "final" => {
            drop(verify);
            drop(verify_raw);
            env.official_upgrade();
            handle.register_syncdb("core", SigLevel::NONE).unwrap();
            let bytes = serde_json::to_vec(&base).unwrap();
            let (data, sig) = sign(&bytes);
            let result = verify_aur_receipt(
                &context,
                ReceiptFds {
                    receipt: data.as_fd(),
                    receipt_signature: sig.as_fd(),
                    manifest: manifest.as_fd(),
                    manifest_signature: manifest_sig.as_fd(),
                    packages: &pairs,
                },
                &trust,
                parent,
                handle,
                now,
                || Ok(stamp.clone()),
            );
            assert!(result.err().unwrap().contains("official full upgrade"));
            assert!(!env.db.join("db.lck").exists());
        }
        _ => panic!("unknown receipt test"),
    }
    let _ = home; // all GPG state belongs to the existing temporary fixture
}
