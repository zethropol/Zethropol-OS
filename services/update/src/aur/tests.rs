use super::*;
use crate::broker_protocol::aur_fixture::Fixture;

fn plan() -> CanonicalPlan {
    CanonicalPlan::from_packages(vec![UpdatePackageInfo {
        package_name: "aur-fixture".into(),
        current_version: "1-1".into(),
        new_version: "2-1".into(),
        repository: "aur-attested".into(),
        download_size: 1,
        filename: "aur-fixture.pkg.tar".into(),
        sha256: "a".repeat(64),
        repository_metadata_sha256: "b".repeat(64),
    }])
    .unwrap()
    .with_policy_fingerprint("c".repeat(64))
    .unwrap()
}
#[test]
fn aur_operation_cannot_be_read_or_cancelled_by_another_bus_peer() {
    let f = Fixture::new();
    let queued = f.queue(plan());
    assert_eq!(
        f.store
            .operation_for_owner(&f.other_peer(), &queued.operation_id),
        Err(BrokerError::IdentityMismatch)
    );
    let denied = f.store.cancel_aur_before_commit(
        f.grant(f.other_peer(), PolicyAction::Preview, 102),
        &queued.operation_id,
        102,
    );
    assert_eq!(denied, Err(BrokerError::IdentityMismatch));
    f.store
        .cancel_aur_before_commit(
            f.grant(f.owner.clone(), PolicyAction::Preview, 102),
            &queued.operation_id,
            102,
        )
        .unwrap();
    let reopened = f.reopen();
    let view = reopened
        .operation_for_owner(&f.owner, &queued.operation_id)
        .unwrap();
    assert_eq!(view.status, OperationStatus::Failed);
    assert_eq!(view.evidence.package_commit, CommitState::NotStarted);
    assert!(f
        .store
        .claim_transaction(&queued.operation_id, &queued.plan, 103)
        .is_err());
}
#[test]
fn aur_cancel_and_commit_start_are_atomic_and_commit_start_cannot_replay() {
    let f = Fixture::new();
    let queued = f.queue(plan());
    f.store
        .claim_transaction(&queued.operation_id, &queued.plan, 102)
        .unwrap();
    let evidence = aur_checkpoint_after_snapshot(
        &f.store,
        &queued.operation_id,
        &queued.plan,
        Ok("17".into()),
        103,
    )
    .unwrap();
    assert_eq!(evidence.package_commit, CommitState::InProgress);
    assert!(f
        .store
        .cancel_aur_before_commit(
            f.grant(f.owner.clone(), PolicyAction::Preview, 104),
            &queued.operation_id,
            104
        )
        .is_err());
    assert!(aur_checkpoint_after_snapshot(
        &f.store,
        &queued.operation_id,
        &queued.plan,
        Ok("18".into()),
        104
    )
    .is_err());
    f.store
        .worker_process_stopped(&queued.operation_id, 105)
        .unwrap();
    let reopened = f.reopen();
    let view = reopened
        .operation_for_owner(&f.owner, &queued.operation_id)
        .unwrap();
    assert_eq!(view.status, OperationStatus::Failed);
    assert_eq!(view.evidence.package_commit, CommitState::Unknown);
    assert_eq!(view.evidence.pre_snapshot, Some("17".into()));
    assert!(view.message.contains("No automatic retry"));
    assert!(f
        .store
        .claim_transaction(&queued.operation_id, &queued.plan, 106)
        .is_err());
}
#[test]
fn aur_snapshot_failure_and_expired_plan_never_checkpoint_commit() {
    let f = Fixture::new();
    let q = f.queue(plan());
    f.store
        .claim_transaction(&q.operation_id, &q.plan, 102)
        .unwrap();
    assert!(aur_checkpoint_after_snapshot(
        &f.store,
        &q.operation_id,
        &q.plan,
        Err("snapshot fixture failure".into()),
        103
    )
    .is_err());
    assert_eq!(
        f.store
            .operation_for_owner(&f.owner, &q.operation_id)
            .unwrap()
            .evidence
            .package_commit,
        CommitState::NotStarted
    );
    assert!(
        aur_checkpoint_after_snapshot(&f.store, &q.operation_id, &q.plan, Ok("0".into()), 103)
            .is_err()
    );
    assert!(aur_checkpoint_after_snapshot(
        &f.store,
        &q.operation_id,
        &q.plan,
        Ok("17".into()),
        401
    )
    .is_err());
    assert_eq!(
        f.reopen()
            .operation_for_owner(&f.owner, &q.operation_id)
            .unwrap()
            .evidence
            .package_commit,
        CommitState::NotStarted
    );
}
#[test]
fn aur_plan_drift_is_rejected_by_existing_store_before_start() {
    let f = Fixture::new();
    let q = f.queue(plan());
    let mut pkgs = q.plan.packages().to_vec();
    pkgs[0].sha256 = "e".repeat(64);
    let changed = CanonicalPlan::from_packages(pkgs)
        .unwrap()
        .with_policy_fingerprint("c".repeat(64))
        .unwrap();
    assert_eq!(
        f.store.claim_transaction(&q.operation_id, &changed, 102),
        Err(BrokerError::PlanChanged)
    );
    assert_eq!(
        f.store
            .operation_for_owner(&f.owner, &q.operation_id)
            .unwrap()
            .evidence
            .package_commit,
        CommitState::NotStarted
    );
}
#[test]
fn aur_cancel_before_start_checkpoint_forbids_commit_record() {
    let f = Fixture::new();
    let q = f.queue(plan());
    f.store
        .claim_transaction(&q.operation_id, &q.plan, 102)
        .unwrap();
    f.store
        .cancel_aur_before_commit(
            f.grant(f.owner.clone(), PolicyAction::Preview, 103),
            &q.operation_id,
            103,
        )
        .unwrap();
    assert!(aur_checkpoint_after_snapshot(
        &f.store,
        &q.operation_id,
        &q.plan,
        Ok("17".into()),
        104
    )
    .is_err());
}

struct AlpmFixture {
    root: PathBuf,
    db: PathBuf,
    archive: AurArchive,
}
impl AlpmFixture {
    fn new(f: &Fixture, extra: &str, arch: &str) -> Self {
        let root = f.path.join("root");
        let db = f.path.join("db");
        fs::create_dir_all(&root).unwrap();
        fs::create_dir_all(db.join("local")).unwrap();
        fs::create_dir_all(db.join("sync")).unwrap();
        fs::write(db.join("local/ALPM_DB_VERSION"), "9\n").unwrap();
        let metadata = f.path.join("pkg");
        fs::create_dir(&metadata).unwrap();
        fs::write(metadata.join(".PKGINFO"),format!("pkgname = aur-fixture\npkgbase = aur-fixture\npkgver = 2-1\narch = {arch}\nsize = 0\npkgdesc = temporary fixture\n{extra}")).unwrap();
        let path = f.path.join("aur-fixture.pkg.tar");
        assert!(Command::new("/usr/bin/bsdtar")
            .args(["-cf"])
            .arg(&path)
            .arg("-C")
            .arg(&metadata)
            .arg(".PKGINFO")
            .status()
            .unwrap()
            .success());
        let identity = UpdatePackageInfo {
            package_name: "aur-fixture".into(),
            current_version: String::new(),
            new_version: "2-1".into(),
            repository: "aur-attested".into(),
            download_size: fs::metadata(&path).unwrap().len(),
            filename: "aur-fixture.pkg.tar".into(),
            sha256: crate::integrity::hash_file(&path).unwrap(),
            repository_metadata_sha256: "b".repeat(64),
        };
        Self {
            root,
            db,
            archive: AurArchive { path, identity },
        }
    }
    fn handle(&self) -> Alpm {
        let mut handle = Alpm::new(self.root.to_str().unwrap(), self.db.to_str().unwrap()).unwrap();
        handle.set_architectures(["x86_64"].into_iter()).unwrap();
        handle
    }
    fn official_upgrade(&self) {
        self.official_upgrade_arch("x86_64");
    }
    fn official_upgrade_arch(&self, arch: &str) {
        let old = self.db.join("local/official-1-1");
        fs::create_dir_all(&old).unwrap();
        fs::write(
            old.join("desc"),
            "%NAME%\nofficial\n\n%VERSION%\n1-1\n\n%ARCH%\nx86_64\n",
        )
        .unwrap();
        fs::write(old.join("files"), "%FILES%\n").unwrap();
        let repo = self.root.join("repo/official-2-1");
        fs::create_dir_all(&repo).unwrap();
        fs::write(repo.join("desc"),format!("%NAME%\nofficial\n\n%VERSION%\n2-1\n\n%ARCH%\nx86_64\n\n%FILENAME%\nofficial.pkg.tar\n\n%SHA256SUM%\n{}\n\n%CSIZE%\n1\n\n%ISIZE%\n1\n\n%DESC%\nfixture\n","a".repeat(64))).unwrap();
        let desc = repo.join("desc");
        fs::write(
            &desc,
            fs::read_to_string(&desc)
                .unwrap()
                .replace("%ARCH%\nx86_64", &format!("%ARCH%\n{arch}")),
        )
        .unwrap();
        assert!(Command::new("/usr/bin/bsdtar")
            .arg("-cf")
            .arg(self.db.join("sync/core.db"))
            .arg("-C")
            .arg(self.root.join("repo"))
            .arg("official-2-1")
            .status()
            .unwrap()
            .success());
    }
}
#[test]
fn aur_native_locked_fixture_plan_keeps_lock_and_detects_archive_drift() {
    let f = Fixture::new();
    let env = AlpmFixture::new(&f, "", "any");
    let mut handle = env.handle();
    {
        // Unsigned synthetic fixture only. Production prepare_aur_locked always
        // requires native signatures and has no public bypass parameter.
        let locked = prepare_aur_locked_internal(
            &mut handle,
            std::slice::from_ref(&env.archive),
            &"c".repeat(64),
            true,
        )
        .unwrap();
        assert!(env.db.join("db.lck").exists());
        assert_eq!(locked.plan.packages().len(), 1);
        let q = f.queue(locked.plan.clone());
        locked.claim(&f.store, &q.operation_id, 102).unwrap();
    }
    assert!(!env.db.join("db.lck").exists());
    fs::write(&env.archive.path, b"modified archive").unwrap();
    assert!(
        prepare_aur_locked_internal(&mut handle, &[env.archive], &"c".repeat(64), true).is_err()
    );
    assert!(!env.db.join("db.lck").exists());
}
#[test]
fn aur_native_downgrade_including_epoch_is_rejected_before_claim() {
    for installed in ["3-1", "1:1-1"] {
        let f = Fixture::new();
        let env = AlpmFixture::new(&f, "", "any");
        let local = env.db.join(format!("local/aur-fixture-{installed}"));
        fs::create_dir(&local).unwrap();
        fs::write(
            local.join("desc"),
            format!("%NAME%\naur-fixture\n\n%VERSION%\n{installed}\n\n%ARCH%\nany\n"),
        )
        .unwrap();
        fs::write(local.join("files"), "%FILES%\n").unwrap();
        let before = aur_database_state_fingerprint(&env.db, "fixture").unwrap();
        let mut handle = env.handle();
        let error = prepare_aur_locked_internal(&mut handle, &[env.archive], &"c".repeat(64), true)
            .err()
            .unwrap();
        assert!(error.contains("downgrade refused"), "{error}");
        assert!(!env.db.join("db.lck").exists());
        assert_eq!(
            before,
            aur_database_state_fingerprint(&env.db, "fixture").unwrap()
        );
    }
}

#[test]
fn aur_installed_version_readback_reopens_database_and_rejects_drift() {
    let f = Fixture::new();
    let env = AlpmFixture::new(&f, "", "any");
    let handle = env.handle();
    let expected = plan();
    assert!(verify_aur_installed_versions(&handle, &expected).is_err());
    let local = env.db.join("local/aur-fixture-2-1");
    fs::create_dir(&local).unwrap();
    let desc = local.join("desc");
    fs::write(
        &desc,
        "%NAME%\naur-fixture\n\n%VERSION%\n2-1\n\n%ARCH%\nany\n",
    )
    .unwrap();
    fs::write(local.join("files"), "%FILES%\n").unwrap();
    verify_aur_installed_versions(&handle, &expected).unwrap();
    let changed = env.db.join("local/aur-fixture-3-1");
    fs::rename(&local, &changed).unwrap();
    fs::write(
        changed.join("desc"),
        "%NAME%\naur-fixture\n\n%VERSION%\n3-1\n\n%ARCH%\nany\n",
    )
    .unwrap();
    assert!(verify_aur_installed_versions(&handle, &expected)
        .unwrap_err()
        .contains("version mismatch"));
}
#[test]
fn aur_native_dependency_architecture_lock_and_official_partial_upgrade_are_rejected() {
    for (extra, arch) in [("depend = absent>=1\n", "any"), ("", "aarch64")] {
        let f = Fixture::new();
        let env = AlpmFixture::new(&f, extra, arch);
        let mut handle = env.handle();
        assert!(
            prepare_aur_locked_internal(&mut handle, &[env.archive], &"c".repeat(64), true)
                .is_err()
        );
        assert!(!env.db.join("db.lck").exists());
    }
    let f = Fixture::new();
    let env = AlpmFixture::new(&f, "", "any");
    env.official_upgrade();
    let mut handle = env.handle();
    handle.register_syncdb("core", SigLevel::NONE).unwrap();
    let error = prepare_aur_locked_internal(
        &mut handle,
        std::slice::from_ref(&env.archive),
        &"c".repeat(64),
        true,
    )
    .err()
    .unwrap();
    assert!(error.contains("official full upgrade"), "{error}");
    fs::write(env.db.join("db.lck"), "fixture lock").unwrap();
    assert!(
        prepare_aur_locked_internal(&mut handle, &[env.archive], &"c".repeat(64), true).is_err()
    );
    assert!(env.db.join("db.lck").exists());
}
#[test]
fn aur_default_signature_gate_does_not_accept_unsigned_fixture() {
    let f = Fixture::new();
    let env = AlpmFixture::new(&f, "", "any");
    let mut handle = env.handle();
    assert!(prepare_aur_locked(&mut handle, &[env.archive], &"c".repeat(64)).is_err());
    assert!(!env.db.join("db.lck").exists());
}
#[test]
fn aur_method_cannot_consume_or_cancel_an_official_preview() {
    let f = Fixture::new();
    let mut pkgs = plan().packages().to_vec();
    pkgs[0].repository = "core".into();
    let official = CanonicalPlan::from_packages(pkgs)
        .unwrap()
        .with_policy_fingerprint("c".repeat(64))
        .unwrap();
    f.store
        .create_preview(
            f.grant(f.owner.clone(), PolicyAction::Preview, 100),
            official.clone(),
            100,
        )
        .unwrap();
    assert!(f
        .store
        .consume_aur_approved_preview(
            f.grant(f.owner.clone(), PolicyAction::Install, 101),
            101,
            &official.hash()
        )
        .is_err());
    // A rejected AUR request has not consumed the official preview.
    let q = f
        .store
        .consume_approved_preview(
            f.grant(f.owner.clone(), PolicyAction::Install, 101),
            101,
            &official.hash(),
            false,
        )
        .unwrap();
    assert!(f
        .store
        .cancel_aur_before_commit(
            f.grant(f.owner.clone(), PolicyAction::Preview, 102),
            &q.operation_id,
            102
        )
        .is_err());
}
#[test]
fn aur_consumption_uses_existing_one_use_grant_and_persists_timeout() {
    let f = Fixture::new();
    let p = plan();
    f.store
        .create_preview(
            f.grant(f.owner.clone(), PolicyAction::Preview, 100),
            p.clone(),
            100,
        )
        .unwrap();
    assert!(f
        .store
        .consume_aur_approved_preview(
            f.grant(f.owner.clone(), PolicyAction::Preview, 101),
            101,
            &p.hash()
        )
        .is_err());
    let q = f
        .store
        .consume_aur_approved_preview(
            f.grant(f.owner.clone(), PolicyAction::Install, 101),
            101,
            &p.hash(),
        )
        .unwrap();
    assert!(f
        .store
        .consume_aur_approved_preview(
            f.grant(f.owner.clone(), PolicyAction::Install, 102),
            102,
            &p.hash()
        )
        .is_err());
    assert_eq!(
        f.store.claim_transaction(&q.operation_id, &q.plan, 401),
        Err(BrokerError::Expired)
    );
    assert_eq!(
        f.reopen()
            .operation_for_owner(&f.owner, &q.operation_id)
            .unwrap()
            .status,
        OperationStatus::Failed
    );
}

#[test]
fn aur_cancel_racing_commit_checkpoint_has_exactly_one_winner() {
    for _ in 0..16 {
        let f = Fixture::new();
        let q = f.queue(plan());
        f.store
            .claim_transaction(&q.operation_id, &q.plan, 102)
            .unwrap();
        let barrier = Arc::new(std::sync::Barrier::new(2));
        let store = Arc::clone(&f.store);
        let operation = q.operation_id.clone();
        let current = q.plan.clone();
        let b = barrier.clone();
        let start = std::thread::spawn(move || {
            b.wait();
            store.checkpoint_aur_commit_start(&operation, &current, "17", 103)
        });
        let store = Arc::clone(&f.store);
        let operation = q.operation_id.clone();
        let grant = f.grant(f.owner.clone(), PolicyAction::Preview, 103);
        let cancel = std::thread::spawn(move || {
            barrier.wait();
            store.cancel_aur_before_commit(grant, &operation, 103)
        });
        let started = start.join().unwrap().is_ok();
        let cancelled = cancel.join().unwrap().is_ok();
        assert_ne!(
            started, cancelled,
            "cancellation and commit start cannot both win or both fail"
        );
        let view = f
            .reopen()
            .operation_for_owner(&f.owner, &q.operation_id)
            .unwrap();
        if cancelled {
            assert_eq!(view.status, OperationStatus::Failed);
            assert_eq!(view.evidence.package_commit, CommitState::NotStarted);
        } else {
            assert_eq!(view.evidence.package_commit, CommitState::InProgress);
            f.store
                .worker_process_stopped(&q.operation_id, 104)
                .unwrap();
            assert_eq!(
                f.reopen()
                    .operation_for_owner(&f.owner, &q.operation_id)
                    .unwrap()
                    .evidence
                    .package_commit,
                CommitState::Unknown
            );
        }
    }
}

#[test]
fn aur_database_stamp_is_taken_under_lock_and_drift_aborts() {
    let f = Fixture::new();
    let env = AlpmFixture::new(&f, "", "any");
    let mut handle = env.handle();
    let mut calls = 0;
    let result = prepare_aur_locked_with_policy(
        &mut handle,
        std::slice::from_ref(&env.archive),
        || {
            assert!(
                env.db.join("db.lck").exists(),
                "fingerprint cannot be taken before lock"
            );
            calls += 1;
            if calls == 2 {
                fs::write(env.db.join("local/metadata-drift"), "changed").unwrap();
            }
            aur_database_state_fingerprint(&env.db, &"c".repeat(64))
        },
        true,
    );
    assert!(result.err().unwrap().contains("metadata or policy changed"));
    assert_eq!(calls, 2);
    assert!(!env.db.join("db.lck").exists());
}

#[test]
fn official_wrong_arch_diagnostic_returns_error_and_can_release_lock() {
    let f = Fixture::new();
    let env = AlpmFixture::new(&f, "", "any");
    env.official_upgrade_arch("aarch64");
    let mut handle = env.handle();
    handle.register_syncdb("core", SigLevel::NONE).unwrap();
    let (code, message) =
        prepare_alpm_transaction_with_policy(&mut handle, || Ok("b".repeat(64))).unwrap_err();
    assert_eq!(code, classify_alpm_error(AlpmError::PkgInvalidArch));
    assert!(message.contains("libalpm rejected"), "{message}");
    handle.trans_release().unwrap();
    assert!(!env.db.join("db.lck").exists());
    // A second attempt also returns normally, rather than corrupting the handle.
    assert!(prepare_alpm_transaction_with_policy(&mut handle, || Ok("b".repeat(64))).is_err());
    handle.trans_release().unwrap();
}

fn sealed_test_bytes(bytes: &[u8], seal: bool) -> fs::File {
    use std::io::Write;
    use std::os::fd::{AsRawFd, FromRawFd};
    let fd = unsafe {
        libc::memfd_create(
            c"aur-isolated-fixture".as_ptr(),
            libc::MFD_ALLOW_SEALING | libc::MFD_CLOEXEC,
        )
    };
    assert!(fd >= 0);
    let mut file = unsafe { fs::File::from_raw_fd(fd) };
    file.write_all(bytes).unwrap();
    if seal {
        assert_eq!(
            unsafe {
                libc::fcntl(
                    file.as_raw_fd(),
                    libc::F_ADD_SEALS,
                    libc::F_SEAL_SEAL
                        | libc::F_SEAL_WRITE
                        | libc::F_SEAL_SHRINK
                        | libc::F_SEAL_GROW,
                )
            },
            0
        );
    }
    file
}

#[test]
fn aur_signed_fd_intake_to_native_commit_persists_result_in_private_root() {
    signed_native_chain(false);
}
#[test]
fn aur_signed_native_commit_survives_post_snapshot_failure_without_retry() {
    signed_native_chain(true);
}
fn signed_native_chain(post_fails: bool) {
    // libalpm/GPGME retain process-global GPG initialization. Never test
    // different authority homes by switching handles in one process.
    let mode = std::env::var("ZETHROPOL_AUR_SIGNED_CHILD").ok();
    if mode.is_none() {
        let name = if post_fails {
            "update_service::aur_integration_tests::aur_signed_native_commit_survives_post_snapshot_failure_without_retry"
        } else {
            "update_service::aur_integration_tests::aur_signed_fd_intake_to_native_commit_persists_result_in_private_root"
        };
        for scope in ["trusted", "unknown"] {
            let result = Command::new(std::env::current_exe().unwrap())
                .args(["--exact", name, "--nocapture", "--test-threads=1"])
                .env("ZETHROPOL_AUR_SIGNED_CHILD", scope)
                .output()
                .unwrap();
            assert!(
                result.status.success(),
                "{scope}: {} {}",
                String::from_utf8_lossy(&result.stdout),
                String::from_utf8_lossy(&result.stderr)
            );
        }
        return;
    }
    use std::os::fd::AsFd;
    let f = Fixture::new();
    let mut env = AlpmFixture::new(&f, "", "any");
    let metadata = f.path.join("pkg");
    fs::write(
        metadata.join("aur-lab-payload"),
        b"signed isolated payload\n",
    )
    .unwrap();
    assert!(Command::new("/usr/bin/bsdtar")
        .arg("-cf")
        .arg(&env.archive.path)
        .arg("-C")
        .arg(&metadata)
        .args([".PKGINFO", "aur-lab-payload"])
        .status()
        .unwrap()
        .success());
    env.archive.identity.sha256 = crate::integrity::hash_file(&env.archive.path).unwrap();
    env.archive.identity.download_size = fs::metadata(&env.archive.path).unwrap().len();
    let home = f.path.join("ephemeral-lab-gnupg");
    fs::create_dir(&home).unwrap();
    fs::set_permissions(&home, fs::Permissions::from_mode(0o700)).unwrap();
    struct GpgCleanup(PathBuf);
    impl Drop for GpgCleanup {
        fn drop(&mut self) {
            let _ = Command::new("/usr/bin/gpgconf")
                .arg("--homedir")
                .arg(&self.0)
                .args(["--kill", "all"])
                .output();
        }
    }
    let _cleanup = GpgCleanup(home.clone());
    let gpg = |args: &[&str]| {
        let out = Command::new("/usr/bin/gpg")
            .arg("--homedir")
            .arg(&home)
            .args([
                "--batch",
                "--no-tty",
                "--pinentry-mode",
                "loopback",
                "--passphrase",
                "",
            ])
            .args(args)
            .output()
            .unwrap();
        assert!(
            out.status.success(),
            "{}",
            String::from_utf8_lossy(&out.stderr)
        );
        out.stdout
    };
    gpg(&[
        "--quick-generate-key",
        "Isolated AUR test <lab@invalid>",
        "ed25519",
        "sign",
        "1d",
    ]);
    gpg(&["--detach-sign", env.archive.path.to_str().unwrap()]);
    let signature = fs::read(env.archive.path.with_file_name("aur-fixture.pkg.tar.sig")).unwrap();
    let archive = sealed_test_bytes(&fs::read(&env.archive.path).unwrap(), true);
    let sig = sealed_test_bytes(&signature, true);
    let unsealed = sealed_test_bytes(b"mutable", false);
    let badsig = sealed_test_bytes(b"invalid detached signature", true);
    let changed = sealed_test_bytes(b"different archive", true);
    let oversized = sealed_test_bytes(b"", true);
    assert!(sealed_size(oversized.as_fd(), 1024).is_err());
    assert!(sealed_size(archive.as_fd(), 1).is_err());
    let parent = f.path.join("private-intake");
    fs::create_dir(&parent).unwrap();
    fs::set_permissions(&parent, fs::Permissions::from_mode(0o700)).unwrap();
    let mut handle = env.handle();
    handle.set_gpgdir(home.to_str().unwrap()).unwrap();
    // No global hook directories or host log writes in this fixture.
    handle.set_hookdirs(std::iter::empty::<&str>()).unwrap();
    handle
        .set_logfile(f.path.join("pacman.log").to_str().unwrap())
        .unwrap();
    let fingerprint = "c".repeat(64);
    let approved = CanonicalPlan::from_packages(vec![env.archive.identity.clone()])
        .unwrap()
        .with_policy_fingerprint(fingerprint.clone())
        .unwrap();
    if mode.as_deref() == Some("receipt") {
        receipt_fixture_chain(&f, &env, &home, &gpg, &archive, &sig, &parent, &mut handle);
        return;
    }
    if mode.as_deref() == Some("unknown") {
        let unknown_home = f.path.join("untrusted-lab-gnupg");
        fs::create_dir(&unknown_home).unwrap();
        let mut unknown_handle = env.handle();
        unknown_handle
            .set_gpgdir(unknown_home.to_str().unwrap())
            .unwrap();
        assert!(receive_aur_fds(
            &parent,
            &approved,
            &[(archive.as_fd(), sig.as_fd())],
            &unknown_handle
        )
        .is_err());
        assert_eq!(fs::read_dir(&parent).unwrap().count(), 0);
        drop(unknown_handle);
        return;
    }
    for pair in [
        (unsealed.as_fd(), sig.as_fd()),
        (archive.as_fd(), badsig.as_fd()),
        (changed.as_fd(), sig.as_fd()),
    ] {
        assert!(receive_aur_fds(&parent, &approved, &[pair], &handle).is_err());
        assert_eq!(fs::read_dir(&parent).unwrap().count(), 0);
    }
    let intake = receive_aur_fds(
        &parent,
        &approved,
        &[(archive.as_fd(), sig.as_fd())],
        &handle,
    )
    .unwrap();
    let locked = prepare_aur_locked(&mut handle, &intake.archives, &fingerprint).unwrap();
    assert_eq!(locked.plan, approved);
    f.store
        .create_preview(
            f.grant(f.owner.clone(), PolicyAction::Preview, 100),
            approved.clone(),
            100,
        )
        .unwrap();
    let q = f
        .store
        .consume_aur_approved_preview(
            f.grant(f.owner.clone(), PolicyAction::Install, 101),
            101,
            &approved.hash(),
        )
        .unwrap();
    // A commit cannot be reached using only a queued operation.
    assert!(f
        .store
        .aur_commit_evidence(&q.operation_id, &locked.plan)
        .is_err());
    locked.claim(&f.store, &q.operation_id, 102).unwrap();
    aur_checkpoint_after_snapshot(
        &f.store,
        &q.operation_id,
        &locked.plan,
        Ok("17".into()),
        103,
    )
    .unwrap();
    aur_commit_started_with_post(locked, &f.store, &q.operation_id, |pre| {
        assert_eq!(pre, "17");
        if post_fails {
            Err("post-snapshot fixture failure".into())
        } else {
            Ok("18".into())
        }
    })
    .unwrap();
    assert!(!env.db.join("db.lck").exists());
    let view = f
        .reopen()
        .operation_for_owner(&f.owner, &q.operation_id)
        .unwrap();
    assert_eq!(view.evidence.package_commit, CommitState::Committed);
    assert_eq!(
        view.evidence.post_snapshot,
        if post_fails { None } else { Some("18".into()) }
    );
    assert_eq!(
        fs::read(env.root.join("aur-lab-payload")).unwrap(),
        b"signed isolated payload\n"
    );
    assert_eq!(view.status, OperationStatus::CompletedWithWarnings);
    assert!(view.message.contains("installed versions Ok(())"));
    assert!(view.message.contains("No automatic retry"));
    let installed = handle.localdb().pkg("aur-fixture").unwrap();
    assert_eq!(installed.version().as_str(), "2-1");
    assert!(f
        .store
        .claim_transaction(&q.operation_id, &approved, 104)
        .is_err());
    drop(intake);
    assert_eq!(fs::read_dir(&parent).unwrap().count(), 0);
}

#[test]
fn aur_native_commit_failure_is_durable_and_not_replayable() {
    let f = Fixture::new();
    let env = AlpmFixture::new(&f, "", "any");
    let mut handle = env.handle();
    handle.set_hookdirs(std::iter::empty::<&str>()).unwrap();
    handle
        .set_logfile(f.path.join("pacman.log").to_str().unwrap())
        .unwrap();
    let locked = prepare_aur_locked_internal(
        &mut handle,
        std::slice::from_ref(&env.archive),
        &"c".repeat(64),
        true,
    )
    .unwrap();
    let q = f.queue(locked.plan.clone());
    locked.claim(&f.store, &q.operation_id, 102).unwrap();
    aur_checkpoint_after_snapshot(
        &f.store,
        &q.operation_id,
        &locked.plan,
        Ok("17".into()),
        103,
    )
    .unwrap();
    fs::remove_file(&env.archive.path).unwrap();
    assert!(
        aur_commit_started_with_post(locked, &f.store, &q.operation_id, |_| Err(
            "post-snapshot fixture failure".into()
        ))
        .is_err()
    );
    let view = f
        .reopen()
        .operation_for_owner(&f.owner, &q.operation_id)
        .unwrap();
    assert_eq!(view.status, OperationStatus::Failed);
    assert_eq!(view.evidence.package_commit, CommitState::Failed);
    assert_eq!(view.evidence.pre_snapshot, Some("17".into()));
    assert_eq!(view.evidence.post_snapshot, None);
    assert!(view.message.contains("No automatic retry"));
    assert!(f
        .store
        .claim_transaction(&q.operation_id, &q.plan, 104)
        .is_err());
    assert!(!env.db.join("db.lck").exists());
}

#[tokio::test]
async fn aur_dbus_install_is_exposed_but_closed_even_with_official_fixture_auth() {
    use std::io::{BufRead, BufReader};
    use std::process::Stdio;
    struct Bus(std::process::Child);
    impl Drop for Bus {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
    let mut bus = Bus(Command::new("/usr/bin/dbus-daemon")
        .args(["--session", "--nofork", "--nopidfile", "--print-address=1"])
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap());
    let mut address = String::new();
    BufReader::new(bus.0.stdout.take().unwrap())
        .read_line(&mut address)
        .unwrap();
    assert!(!address.trim().is_empty());
    let manager = UpdateManager {
        state: Arc::new(Mutex::new(ManagerState::default())),
        plan_refresh_lock: Mutex::new(()),
        broker: None,
        test_authentication: true,
    };
    let _service = zbus::connection::Builder::address(address.trim())
        .unwrap()
        .name("org.zethropol.Update.Test")
        .unwrap()
        .serve_at("/org/zethropol/Update", manager)
        .unwrap()
        .build()
        .await
        .unwrap();
    let client = zbus::connection::Builder::address(address.trim())
        .unwrap()
        .build()
        .await
        .unwrap();
    let proxy = zbus::Proxy::new(
        &client,
        "org.zethropol.Update.Test",
        "/org/zethropol/Update",
        "org.zethropol.Update.v1",
    )
    .await
    .unwrap();
    let report: HashMap<String, String> = proxy
        .call("ReviewAurSelection", &(vec!["aur-fixture"], vec!["2-1"]))
        .await
        .unwrap();
    assert_eq!(report["state"], "blocked");
    assert_eq!(report["installation"], "closed");
    assert_eq!(report["api_version"], "1");
    assert!(report["alpm_plan"].starts_with("not-created"));
    let bad: Result<HashMap<String, String>, _> = proxy
        .call("ReviewAurSelection", &(vec!["bad/name"], vec!["2-1"]))
        .await;
    assert!(bad.is_err());
    let reserve: Result<String, _> = proxy.call("ReserveAurBuild", &("invalid-digest",)).await;
    assert!(reserve.is_err());
    let args = (
        "a".repeat(64),
        Vec::<zbus::zvariant::OwnedFd>::new(),
        Vec::<zbus::zvariant::OwnedFd>::new(),
    );
    let denied: Result<String, zbus::Error> = proxy.call("InstallAur", &args).await;
    let error = denied.unwrap_err().to_string();
    assert!(error.contains("NotSupported"), "{error}");
    assert!(error.contains("AUR installation closed"), "{error}");
}

#[test]
fn aur_cancellation_during_snapshot_preserves_recovery_id_without_reviving_commit() {
    let f = Fixture::new();
    let q = f.queue(plan());
    f.store
        .claim_transaction(&q.operation_id, &q.plan, 102)
        .unwrap();
    f.store
        .cancel_aur_before_commit(
            f.grant(f.owner.clone(), PolicyAction::Preview, 103),
            &q.operation_id,
            103,
        )
        .unwrap();
    f.store
        .record_aur_pre_snapshot(&q.operation_id, &q.plan, "17")
        .unwrap();
    let view = f
        .reopen()
        .operation_for_owner(&f.owner, &q.operation_id)
        .unwrap();
    assert_eq!(view.status, OperationStatus::Failed);
    assert_eq!(view.evidence.package_commit, CommitState::NotStarted);
    assert_eq!(view.evidence.pre_snapshot, Some("17".into()));
    assert!(view.message.contains("cancelled"));
    assert!(f
        .store
        .record_aur_pre_snapshot(&q.operation_id, &q.plan, "18")
        .is_err());
    assert!(aur_checkpoint_after_snapshot(
        &f.store,
        &q.operation_id,
        &q.plan,
        Ok("17".into()),
        104
    )
    .is_err());
}

include!("receipt_tests.rs");

#[test]
fn public_trust_readiness_never_enables_install_or_certifies_missing_artifacts() {
    let report = aur_trust_readiness();
    assert_eq!(report["installation"], "closed");
    assert_eq!(report["system_acceptance"], "not-complete");
    assert_eq!(report["signed_artifacts"], "not-verified-by-this-query");
}

#[test]
fn deployment_policy_template_cannot_supply_production_authority() {
    let f = Fixture::new();
    let policy = f.path.join("policy.json");
    fs::write(
        &policy,
        include_str!("../../deployment/aur/policy.json.example"),
    )
    .unwrap();
    assert!(ReceiptTrust::load_owned(&policy, unsafe { libc::geteuid() }, false).is_err());
    let bus_policy = include_str!("../../deployment/dbus/org.zethropol.Update.conf");
    for method in [
        "ReviewAurSelection",
        "ReserveAurBuild",
        "PreviewAurReceipt",
        "InstallAur",
        "CancelAur",
        "GetAurOperationState",
        "GetAurReadiness",
    ] {
        assert!(bus_policy.contains(&format!("send_member=\"{method}\"")));
    }
}
