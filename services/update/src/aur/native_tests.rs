use super::*;
use crate::broker_protocol::aur_fixture::Fixture;
use std::os::fd::AsFd;

struct Lab {
    f: Fixture,
    paths: NativeFixturePaths,
    archive: fs::File,
    signature: fs::File,
    identity: UpdatePackageInfo,
    signing: PathBuf,
}
impl Drop for Lab {
    fn drop(&mut self) {
        for home in [&self.signing, &self.paths.domain] {
            let _ = Command::new("/usr/bin/gpgconf")
                .arg("--homedir")
                .arg(home)
                .args(["--kill", "all"])
                .output();
        }
    }
}
fn gpg(home: &Path, args: &[&str], input: Option<&[u8]>) -> Vec<u8> {
    let mut child = Command::new("/usr/bin/gpg")
        .args(["--no-options", "--homedir"])
        .arg(home)
        .args([
            "--batch",
            "--no-tty",
            "--pinentry-mode",
            "loopback",
            "--passphrase",
            "",
        ])
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    if let Some(bytes) = input {
        child.stdin.take().unwrap().write_all(bytes).unwrap();
    }
    drop(child.stdin.take());
    let result = child.wait_with_output().unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    result.stdout
}
fn child_command() -> Command {
    let mut cmd = Command::new(std::env::current_exe().unwrap());
    cmd.args([
        "--exact",
        "update_service::native_process_tests::native_fixture_child",
        "--ignored",
        "--test-threads=1",
    ]);
    cmd
}
impl Lab {
    fn new() -> Self {
        let f = Fixture::new();
        let dir = |name: &str| {
            let p = f.path.join(name);
            fs::create_dir(&p).unwrap();
            fs::set_permissions(&p, fs::Permissions::from_mode(0o700)).unwrap();
            p
        };
        let root = dir("root");
        let db = dir("db");
        fs::create_dir(db.join("local")).unwrap();
        fs::create_dir(db.join("sync")).unwrap();
        fs::write(db.join("local/ALPM_DB_VERSION"), "9\n").unwrap();
        let signing = dir("signing-private-lab");
        let domain = dir("public-native-lab");
        let staging = dir("staging");
        let metadata = dir("package");
        fs::write(metadata.join(".PKGINFO"), "pkgname = aur-fixture\npkgbase = aur-fixture\npkgver = 2-1\narch = any\nsize = 14\npkgdesc = isolated native broker fixture\n").unwrap();
        fs::write(metadata.join("native-payload"), "native fixture\n").unwrap();
        let path = f.path.join("aur-fixture.pkg.tar");
        assert!(Command::new("/usr/bin/bsdtar")
            .arg("-cf")
            .arg(&path)
            .arg("-C")
            .arg(metadata)
            .args([".PKGINFO", "native-payload"])
            .status()
            .unwrap()
            .success());
        gpg(
            &signing,
            &[
                "--quick-generate-key",
                "Ephemeral broker fixture <lab@invalid>",
                "ed25519",
                "sign",
                "1d",
            ],
            None,
        );
        gpg(&signing, &["--detach-sign", path.to_str().unwrap()], None);
        let keys =
            String::from_utf8(gpg(&signing, &["--with-colons", "--list-keys"], None)).unwrap();
        let fingerprint = keys
            .lines()
            .find(|l| l.starts_with("fpr:"))
            .unwrap()
            .split(':')
            .nth(9)
            .unwrap();
        let public = gpg(&signing, &["--export", fingerprint], None);
        fs::write(
            domain.join("gpg.conf"),
            "no-autostart\nno-auto-key-retrieve\nno-auto-key-import\nno-auto-check-trustdb\n",
        )
        .unwrap();
        gpg(&domain, &["--no-autostart", "--import"], Some(&public));
        gpg(
            &domain,
            &["--no-autostart", "--import-ownertrust"],
            Some(format!("{fingerprint}:6:\n").as_bytes()),
        );
        gpg(&domain, &["--no-autostart", "--check-trustdb"], None);
        // No signing material is copied into the native verifier domain.
        fs::write(f.path.join("archive.gpg"), &public).unwrap();
        let role = || ReceiptRoleConfig {
            keyring: "archive.gpg".into(),
            keyring_sha256: receipt_hash(&public),
            signers: vec![fingerprint.into()],
        };
        let policy = f.path.join("policy.json");
        fs::write(
            &policy,
            serde_json::to_vec(&ReceiptPolicyConfig {
                schema: 1,
                purpose: "zethropol-aur-trust".into(),
                manifest: role(),
                archive: role(),
                receipt: role(),
            })
            .unwrap(),
        )
        .unwrap();
        let raw = fs::read(&path).unwrap();
        let signature =
            seal_receipt_bytes(&fs::read(format!("{}.sig", path.display())).unwrap()).unwrap();
        let identity = UpdatePackageInfo {
            package_name: "aur-fixture".into(),
            current_version: String::new(),
            new_version: "2-1".into(),
            repository: "aur-local".into(),
            download_size: raw.len() as u64,
            filename: "aur-fixture.pkg.tar".into(),
            sha256: receipt_hash(&raw),
            repository_metadata_sha256: "b".repeat(64),
        };
        let paths = NativeFixturePaths {
            root,
            db,
            domain,
            policy,
            staging,
            store: f.path.clone(),
            fault: None,
        };
        Self {
            f,
            paths,
            archive: seal_receipt_bytes(&raw).unwrap(),
            signature,
            identity,
            signing,
        }
    }
    fn native_paths(&self) -> NativePaths {
        NativePaths {
            domain: self.paths.domain.clone(),
            trust: ReceiptTrust::load_owned(&self.paths.policy, unsafe { libc::geteuid() }, false)
                .unwrap(),
            fixture: Some(self.paths.clone()),
        }
    }
    fn proof(&self, packages: &[UpdatePackageInfo]) -> Result<String, String> {
        let mut official = Alpm::new(
            self.paths.root.to_str().unwrap(),
            self.paths.db.to_str().unwrap(),
        )
        .unwrap();
        official.set_architectures(["x86_64"].into_iter()).unwrap();
        // Official handle deliberately has NO Archive authority.
        let official_domain = self.f.path.join("empty-official-domain");
        fs::create_dir_all(&official_domain).unwrap();
        official
            .set_gpgdir(official_domain.to_str().unwrap())
            .unwrap();
        let native = self.native_paths();
        let result = official_aur_preflight(&mut official, packages, &self.paths.domain, || {
            native.state_stamp(&self.paths.db)
        });
        assert_eq!(
            official.gpgdir().map(Path::new),
            Some(official_domain.as_path())
        );
        result
    }
    fn request(&self, identity: UpdatePackageInfo, op: &str, deadline: u64) -> NativeRequest {
        let paths = self.native_paths();
        let stamp = self.proof(std::slice::from_ref(&identity)).unwrap();
        let mut r = make_native_request(
            NativeMode::Preview,
            op,
            deadline,
            "aur-fixture",
            "x86_64",
            &paths.trust,
            stamp,
            vec![identity],
            vec![receipt_hash(
                &read_receipt_fd(self.signature.as_fd(), 1024 * 1024).unwrap(),
            )],
            vec![Some("any".into())],
            None,
        )
        .unwrap();
        r.fixture = Some(self.paths.clone());
        r
    }
    fn exchange(
        &self,
        r: NativeRequest,
        lease: Option<BorrowedFd<'_>>,
    ) -> Result<CanonicalPlan, String> {
        let observer = if r.mode == NativeMode::Install {
            Some(self.f.store.as_ref())
        } else {
            None
        };
        exchange_native_observed(
            child_command(),
            r,
            &[(self.archive.as_fd(), self.signature.as_fd())],
            lease,
            observer,
        )
    }
    fn sign(&self, data: &[u8]) -> (fs::File, fs::File) {
        let path = self.f.path.join(format!("signed-{}", uuid::Uuid::new_v4()));
        fs::write(&path, data).unwrap();
        gpg(
            &self.signing,
            &["--detach-sign", path.to_str().unwrap()],
            None,
        );
        (
            seal_receipt_bytes(data).unwrap(),
            seal_receipt_bytes(&fs::read(format!("{}.sig", path.display())).unwrap()).unwrap(),
        )
    }
    fn admitted(&self) -> (crate::broker_protocol::ConsumedPreview, NativeRequest) {
        let now = aur_boottime().unwrap();
        let wall = receipt_unix_now().unwrap();
        let root = serde_json::to_vec(&RootManifest {
            schema: 1,
            purpose: "zethropol-aur-build-root".into(),
            architecture: "x86_64".into(),
            expires: wall + 300,
            packages: vec![RootPackage {
                name: "lab-tool".into(),
                version: "1-1".into(),
                sha256: "a".repeat(64),
            }],
        })
        .unwrap();
        let review = ReviewedBuild {
            source_package: "aur-fixture".into(),
            source_version: "2-1".into(),
            architecture: "x86_64".into(),
            source_plan_sha256: "d".repeat(64),
            build_root_sha256: receipt_hash(&root),
            outputs: vec![ReviewedOutput {
                name: "aur-fixture".into(),
                version: "2-1".into(),
                filename: "aur-fixture.pkg.tar".into(),
            }],
        };
        let context = self
            .f
            .store
            .reserve_aur_build(
                self.f
                    .grant(self.f.owner.clone(), PolicyAction::Preview, now),
                review,
                now,
            )
            .unwrap();
        let receipt = BuildReceipt {
            schema: 1,
            purpose: "zethropol-aur-build-receipt".into(),
            operation_id: context.operation_id.clone(),
            uid: context.uid,
            bus_name: context.bus_name.clone(),
            session_id: context.session_id.clone(),
            source_package: "aur-fixture".into(),
            source_version: "2-1".into(),
            architecture: "x86_64".into(),
            source_plan_sha256: context.review.source_plan_sha256.clone(),
            build_root_sha256: context.review.build_root_sha256.clone(),
            issued_at: wall - 1,
            expires_at: wall + 120,
            packages: vec![ReceiptOutput {
                name: "aur-fixture".into(),
                version: "2-1".into(),
                architecture: "any".into(),
                filename: self.identity.filename.clone(),
                sha256: self.identity.sha256.clone(),
                signature_sha256: receipt_hash(
                    &read_receipt_fd(self.signature.as_fd(), 1024 * 1024).unwrap(),
                ),
            }],
        };
        let (manifest, manifest_sig) = self.sign(&root);
        let (data, sig) = self.sign(&serde_json::to_vec(&receipt).unwrap());
        let paths = self.native_paths();
        let pairs = [(self.archive.as_fd(), self.signature.as_fd())];
        // Same parent receipt verifier / native exchange as production; only
        // fixed test roots, test-only snapshot evidence and fixture grants differ.
        let verified = verify_aur_receipt_with_native(
            &context,
            ReceiptFds {
                receipt: data.as_fd(),
                receipt_signature: sig.as_fd(),
                manifest: manifest.as_fd(),
                manifest_signature: manifest_sig.as_fd(),
                packages: &pairs,
            },
            &paths.trust,
            &self.paths.staging,
            wall,
            |intake, _| {
                self.exchange(
                    self.request(
                        intake.archives[0].identity.clone(),
                        &context.operation_id,
                        context.reservation_deadline_boottime,
                    ),
                    None,
                )
            },
        )
        .unwrap();
        let plan = self
            .f
            .store
            .admit_aur_receipt(
                self.f
                    .grant(self.f.owner.clone(), PolicyAction::Preview, now),
                &verified,
                now,
                wall,
            )
            .unwrap();
        assert!(self
            .f
            .store
            .consume_aur_receipt_preview(
                self.f
                    .grant(self.f.other_peer(), PolicyAction::Install, now),
                now,
                wall,
                &plan.hash(),
                &paths.trust.digest
            )
            .is_err());
        let consumed = self
            .f
            .store
            .consume_aur_receipt_preview(
                self.f
                    .grant(self.f.owner.clone(), PolicyAction::Install, now),
                now,
                wall,
                &plan.hash(),
                &paths.trust.digest,
            )
            .unwrap();
        let binding = self
            .f
            .store
            .aur_receipt_binding(
                &consumed.operation_id,
                &plan,
                now,
                wall,
                &paths.trust.digest,
            )
            .unwrap();
        let mut request = self.request(
            plan.packages()[0].clone(),
            &consumed.operation_id,
            binding.native_deadline(),
        );
        request.mode = NativeMode::Install;
        request.plan_hash = Some(plan.hash());
        request.artifacts[0].architecture = None;
        (consumed, request)
    }
}

#[test]
#[ignore = "native subprocess fixture only; never grants production authority"]
fn native_fixture_child() {
    let fd = unsafe { libc::fcntl(0, libc::F_DUPFD_CLOEXEC, 3) };
    assert!(fd >= 0);
    let input = unsafe { fs::File::from_raw_fd(fd) };
    let request: NativeRequest =
        serde_json::from_slice(&read_receipt_fd(input.as_fd(), 1024 * 1024).unwrap()).unwrap();
    let f = request.fixture.unwrap();
    for p in [&f.root, &f.db, &f.domain, &f.staging, &f.store] {
        assert!(p.starts_with(std::env::temp_dir()));
        assert_eq!(fs::canonicalize(p).unwrap(), *p);
        assert_eq!(fs::metadata(p).unwrap().uid(), unsafe { libc::geteuid() });
    }
    assert_ne!(unsafe { libc::geteuid() }, 0);
    assert_eq!(
        std::env::var("GNUPGHOME").unwrap(),
        f.domain.to_str().unwrap()
    );
    match f.fault.as_deref() {
        Some("crash") => std::process::exit(70),
        Some("sleep" | "install-timeout") => std::thread::sleep(Duration::from_secs(5)),
        _ => {}
    }
    let paths = NativePaths {
        domain: f.domain.clone(),
        trust: ReceiptTrust::load_owned(&f.policy, unsafe { libc::geteuid() }, false).unwrap(),
        fixture: Some(f),
    };
    // Return a nonzero status for a sealed error just as the release CLI does.
    if native_child_entry(paths).is_err() {
        std::process::exit(1);
    }
}

#[test]
fn broker_receipt_isolated_native_commit_readback_and_replay() {
    let lab = Lab::new();
    let (q, request) = lab.admitted();
    let lease = lab.f.store.worker_lease(&q.operation_id).unwrap();
    let plan = lab.exchange(request, Some(lease.as_fd())).unwrap();
    assert_eq!(plan, q.plan);
    assert_eq!(
        fs::read(lab.paths.root.join("native-payload")).unwrap(),
        b"native fixture\n"
    );
    let view = lab
        .f
        .reopen()
        .operation_for_owner(&lab.f.owner, &q.operation_id)
        .unwrap();
    assert_eq!(view.evidence.package_commit, CommitState::Committed);
    assert!(view.message.contains("installed versions Ok(())"));
    let mut replay = lab.request(
        q.plan.packages()[0].clone(),
        &q.operation_id,
        aur_boottime().unwrap() + 60,
    );
    replay.mode = NativeMode::Install;
    replay.plan_hash = Some(q.plan.hash());
    assert!(lab.exchange(replay, Some(lease.as_fd())).is_err());
    assert!(require_aur_production_authority().is_err());
}

#[test]
fn native_lock_handoff_rejects_state_plan_and_artifact_drift() {
    let lab = Lab::new();
    let op = uuid::Uuid::new_v4().to_string();
    let deadline = aur_boottime().unwrap() + 120;
    let before = aur_database_state_fingerprint(&lab.paths.db, "fixture").unwrap();
    for change in 0..5 {
        let mut request = lab.request(lab.identity.clone(), &op, deadline);
        match change {
            0 => request.official_stamp = "0".repeat(64),
            1 => request.artifacts[0].identity.sha256 = "0".repeat(64),
            2 => request.artifacts[0].signature_sha256 = "0".repeat(64),
            3 => request.artifacts[0].architecture = Some("x86_64".into()),
            _ => request.source_package = "wrong-pkgbase".into(),
        }
        assert!(lab.exchange(request, None).is_err(), "mutation {change}");
        assert!(!lab.paths.db.join("db.lck").exists());
        assert_eq!(
            before,
            aur_database_state_fingerprint(&lab.paths.db, "fixture").unwrap()
        );
    }
    let request = lab.request(lab.identity.clone(), &op, deadline);
    fs::write(lab.paths.db.join("local/drift"), "changed").unwrap();
    assert!(lab.exchange(request, None).is_err());
    assert!(!lab.paths.root.join("native-payload").exists());
}

#[test]
fn native_denies_mutable_unsigned_and_foreign_domain_artifacts() {
    let lab = Lab::new();
    let op = uuid::Uuid::new_v4().to_string();
    let deadline = aur_boottime().unwrap() + 120;
    let mut wrong = lab.native_paths();
    wrong.trust.archive.signers = vec!["A".repeat(40)];
    assert!(wrong.stamp().unwrap_err().contains("primaries differ"));
    let empty = seal_receipt_bytes(b"invalid signature").unwrap();
    let request = lab.request(lab.identity.clone(), &op, deadline);
    assert!(exchange_native(
        child_command(),
        request,
        &[(lab.archive.as_fd(), empty.as_fd())],
        None
    )
    .is_err());
    let fd = unsafe {
        libc::memfd_create(
            c"mutable-artifact".as_ptr(),
            libc::MFD_CLOEXEC | libc::MFD_ALLOW_SEALING,
        )
    };
    assert!(fd >= 0);
    let mut mutable = unsafe { fs::File::from_raw_fd(fd) };
    mutable.write_all(b"mutable").unwrap();
    assert!(exchange_native(
        child_command(),
        lab.request(lab.identity.clone(), &op, deadline),
        &[(mutable.as_fd(), lab.signature.as_fd())],
        None
    )
    .is_err());
}

#[test]
fn native_cancellation_and_changed_install_plan_never_commit() {
    let lab = Lab::new();
    let (q, mut request) = lab.admitted();
    let lease = lab.f.store.worker_lease(&q.operation_id).unwrap();
    request.plan_hash = Some("0".repeat(64));
    assert!(lab.exchange(request, Some(lease.as_fd())).is_err());
    let binding = lab
        .f
        .store
        .aur_receipt_binding(
            &q.operation_id,
            &q.plan,
            aur_boottime().unwrap(),
            receipt_unix_now().unwrap(),
            &lab.native_paths().trust.digest,
        )
        .unwrap();
    let mut request = lab.request(
        q.plan.packages()[0].clone(),
        &q.operation_id,
        binding.native_deadline(),
    );
    request.mode = NativeMode::Install;
    request.plan_hash = Some(q.plan.hash());
    lab.f
        .store
        .cancel_aur_before_commit(
            lab.f.grant(
                lab.f.owner.clone(),
                PolicyAction::Preview,
                aur_boottime().unwrap(),
            ),
            &q.operation_id,
            aur_boottime().unwrap(),
        )
        .unwrap();
    assert!(lab.exchange(request, Some(lease.as_fd())).is_err());
    assert!(!lab.paths.root.join("native-payload").exists());
    assert_eq!(
        lab.f
            .reopen()
            .operation_for_owner(&lab.f.owner, &q.operation_id)
            .unwrap()
            .evidence
            .package_commit,
        CommitState::NotStarted
    );
}

#[test]
fn native_crash_and_preview_timeout_are_terminal_without_payload() {
    let lab = Lab::new();
    let op = uuid::Uuid::new_v4().to_string();
    for fault in ["crash", "sleep"] {
        let mut request = lab.request(lab.identity.clone(), &op, aur_boottime().unwrap() + 1);
        request.fixture.as_mut().unwrap().fault = Some(fault.into());
        assert!(lab.exchange(request, None).is_err());
        assert!(!lab.paths.root.join("native-payload").exists());
        assert!(!lab.paths.db.join("db.lck").exists());
    }
}

#[test]
fn native_wire_rejects_aliases_expiry_and_missing_lease() {
    let lab = Lab::new();
    let mut r = lab.request(
        lab.identity.clone(),
        &uuid::Uuid::new_v4().to_string(),
        aur_boottime().unwrap() + 60,
    );
    r.response_fd = 3;
    r.artifacts[0].archive_fd = 4;
    r.artifacts[0].signature_fd = 5;
    native_request_valid(&r, aur_boottime().unwrap()).unwrap();
    r.artifacts[0].signature_fd = 4;
    assert!(native_request_valid(&r, aur_boottime().unwrap()).is_err());
    r.artifacts[0].signature_fd = 5;
    r.mode = NativeMode::Install;
    r.plan_hash = Some("a".repeat(64));
    assert!(native_request_valid(&r, aur_boottime().unwrap()).is_err());
    r.lease_fd = Some(6);
    r.deadline = aur_boottime().unwrap();
    assert!(native_request_valid(&r, aur_boottime().unwrap()).is_err());
    assert!(run_aur_native_process().is_err());
}

#[test]
fn native_signed_foreign_authority_and_unsigned_delivery_are_rejected() {
    let lab = Lab::new();
    let op = uuid::Uuid::new_v4().to_string();
    let deadline = aur_boottime().unwrap() + 120;
    let official = lab.f.path.join("official-private-lab");
    fs::create_dir(&official).unwrap();
    fs::set_permissions(&official, fs::Permissions::from_mode(0o700)).unwrap();
    gpg(
        &official,
        &[
            "--quick-generate-key",
            "Ephemeral official-domain LAB <official@invalid>",
            "ed25519",
            "sign",
            "1d",
        ],
        None,
    );
    let path = lab.f.path.join("official-signed-aur.pkg.tar");
    fs::write(
        &path,
        read_receipt_fd(lab.archive.as_fd(), 1024 * 1024).unwrap(),
    )
    .unwrap();
    gpg(&official, &["--detach-sign", path.to_str().unwrap()], None);
    let foreign =
        seal_receipt_bytes(&fs::read(format!("{}.sig", path.display())).unwrap()).unwrap();
    let before = crate::integrity::hash_file(&lab.paths.domain.join("pubring.kbx")).unwrap();
    let mut request = lab.request(lab.identity.clone(), &op, deadline);
    request.artifacts[0].signature_sha256 =
        receipt_hash(&read_receipt_fd(foreign.as_fd(), 1024 * 1024).unwrap());
    let result = exchange_native(
        child_command(),
        request,
        &[(lab.archive.as_fd(), foreign.as_fd())],
        None,
    );
    assert!(result.is_err());
    assert_eq!(
        before,
        crate::integrity::hash_file(&lab.paths.domain.join("pubring.kbx")).unwrap()
    );
    let unsigned = seal_receipt_bytes(b"").unwrap();
    assert!(exchange_native(
        child_command(),
        lab.request(lab.identity.clone(), &op, deadline),
        &[(lab.archive.as_fd(), unsigned.as_fd())],
        None
    )
    .is_err());
    assert!(!lab.paths.root.join("native-payload").exists());
    let _ = Command::new("/usr/bin/gpgconf")
        .arg("--homedir")
        .arg(official)
        .args(["--kill", "all"])
        .output();
}

#[test]
fn native_interrupted_commit_and_install_timeout_preserve_evidence() {
    for fault in ["after-checkpoint", "install-timeout"] {
        let lab = Lab::new();
        let (q, mut request) = lab.admitted();
        let lease = lab.f.store.worker_lease(&q.operation_id).unwrap();
        request.fixture.as_mut().unwrap().fault = Some(fault.into());
        assert!(lab.exchange(request, Some(lease.as_fd())).is_err());
        // Same transition used by the real broker after an interrupted child.
        lab.f
            .store
            .worker_process_stopped(&q.operation_id, aur_boottime().unwrap())
            .unwrap();
        let view = lab
            .f
            .reopen()
            .operation_for_owner(&lab.f.owner, &q.operation_id)
            .unwrap();
        assert_eq!(view.status, OperationStatus::Failed);
        if fault == "after-checkpoint" {
            assert_eq!(view.evidence.package_commit, CommitState::Unknown);
            assert_eq!(view.evidence.pre_snapshot, Some("17".into()));
            assert!(lab.f.store.require_no_uncertain_commit().is_err());
            assert!(
                lab.paths.db.join("db.lck").exists(),
                "never silently unlink a stale native pacman lock"
            );
        } else {
            assert_eq!(view.evidence.package_commit, CommitState::NotStarted);
            assert!(!lab.paths.db.join("db.lck").exists());
        }
        assert!(!lab.paths.root.join("native-payload").exists());
        assert!(lab
            .f
            .store
            .claim_transaction(&q.operation_id, &q.plan, aur_boottime().unwrap())
            .is_err());
    }
}

#[test]
fn native_official_upgrade_barrier_and_duplicate_lease_remain_authoritative() {
    let lab = Lab::new();
    let local = lab.paths.db.join("local/official-1-1");
    fs::create_dir(&local).unwrap();
    fs::write(
        local.join("desc"),
        "%NAME%\nofficial\n\n%VERSION%\n1-1\n\n%ARCH%\nx86_64\n",
    )
    .unwrap();
    fs::write(local.join("files"), "%FILES%\n").unwrap();
    let repo = lab.f.path.join("repo/official-2-1");
    fs::create_dir_all(&repo).unwrap();
    fs::write(repo.join("desc"),format!("%NAME%\nofficial\n\n%VERSION%\n2-1\n\n%ARCH%\nx86_64\n\n%FILENAME%\nofficial.pkg.tar\n\n%SHA256SUM%\n{}\n\n%CSIZE%\n1\n\n%ISIZE%\n1\n\n%DESC%\nfixture\n","a".repeat(64))).unwrap();
    assert!(Command::new("/usr/bin/bsdtar")
        .arg("-cf")
        .arg(lab.paths.db.join("sync/core.db"))
        .arg("-C")
        .arg(lab.f.path.join("repo"))
        .arg("official-2-1")
        .status()
        .unwrap()
        .success());
    let mut official = Alpm::new(
        lab.paths.root.to_str().unwrap(),
        lab.paths.db.to_str().unwrap(),
    )
    .unwrap();
    official.set_architectures(["x86_64"].into_iter()).unwrap();
    // Metadata-only fixture repo. Production uses configured native signatures.
    official.register_syncdb("core", SigLevel::NONE).unwrap();
    let official_domain = lab.f.path.join("official-domain");
    fs::create_dir(&official_domain).unwrap();
    official
        .set_gpgdir(official_domain.to_str().unwrap())
        .unwrap();
    let error = official_aur_preflight(
        &mut official,
        std::slice::from_ref(&lab.identity),
        &lab.paths.domain,
        || lab.native_paths().state_stamp(&lab.paths.db),
    )
    .unwrap_err();
    assert!(error.contains("official full upgrade required"), "{error}");
    assert!(!lab.paths.db.join("db.lck").exists());
    let op = uuid::Uuid::new_v4().to_string();
    let lease = lab.f.store.worker_lease(&op).unwrap();
    lab.f.store.verify_native_lease(&op, lease.as_fd()).unwrap();
    assert!(lab.f.store.worker_lease(&op).is_err());
    let wrong = fs::File::open(lab.f.path.join("archive.gpg")).unwrap();
    assert!(lab.f.store.verify_native_lease(&op, wrong.as_fd()).is_err());
}

#[test]
fn native_public_domain_rejects_trust_redirects_and_signing_material() {
    let lab = Lab::new();
    let config = lab.paths.domain.join("gpg.conf");
    let original = fs::read(&config).unwrap();
    for directive in [
        "keyring /etc/pacman.d/gnupg/pubring.kbx",
        "include /tmp/client-options",
        "trust-model always",
        "auto-key-retrieve",
    ] {
        fs::write(
            &config,
            format!("{}\n{directive}\n", String::from_utf8_lossy(&original)),
        )
        .unwrap();
        assert!(lab.native_paths().stamp().is_err(), "{directive}");
    }
    fs::write(&config, original).unwrap();
    let keys = lab.paths.domain.join("private-keys-v1.d");
    fs::create_dir_all(&keys).unwrap();
    fs::write(
        keys.join("forbidden-fixture"),
        b"not key material; must reject without reading",
    )
    .unwrap();
    assert!(lab
        .native_paths()
        .stamp()
        .unwrap_err()
        .contains("public material only"));
}

#[test]
fn native_official_proof_rejects_shared_domain_before_transaction() {
    let lab = Lab::new();
    let mut official = Alpm::new(
        lab.paths.root.to_str().unwrap(),
        lab.paths.db.to_str().unwrap(),
    )
    .unwrap();
    official
        .set_gpgdir(lab.paths.domain.to_str().unwrap())
        .unwrap();
    let error = official_aur_preflight(
        &mut official,
        std::slice::from_ref(&lab.identity),
        &lab.paths.domain,
        || Ok("a".repeat(64)),
    )
    .unwrap_err();
    assert!(error.contains("independent directories"));
    assert!(!lab.paths.db.join("db.lck").exists());
}

#[test]
fn native_active_precommit_cancellation_stops_the_process() {
    let lab = Lab::new();
    let (q, mut request) = lab.admitted();
    request.fixture.as_mut().unwrap().fault = Some("sleep".into());
    let lease = lab.f.store.worker_lease(&q.operation_id).unwrap();
    let store = Arc::clone(&lab.f.store);
    let operation = q.operation_id.clone();
    let grant = lab.f.grant(
        lab.f.owner.clone(),
        PolicyAction::Preview,
        aur_boottime().unwrap(),
    );
    let cancel = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(100));
        store
            .cancel_aur_before_commit(grant, &operation, aur_boottime().unwrap())
            .unwrap();
    });
    let result = lab.exchange(request, Some(lease.as_fd()));
    cancel.join().unwrap();
    assert!(result.unwrap_err().contains("cancelled before commit"));
    assert!(!lab.paths.root.join("native-payload").exists());
    let view = lab
        .f
        .reopen()
        .operation_for_owner(&lab.f.owner, &q.operation_id)
        .unwrap();
    assert_eq!(view.evidence.package_commit, CommitState::NotStarted);
}
