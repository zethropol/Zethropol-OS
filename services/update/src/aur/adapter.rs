// AUR-specific integration with the existing Update implementation, not a
// receipt and native integration in the canonical Update service source.

struct AurArchive {
    path: PathBuf,
    identity: UpdatePackageInfo,
}
impl AurArchive {
    // The caller is internal server code; no D-Bus method accepts these values
    // from a desktop client. Root trust/receipt intake is still an external gate.
    fn from_root_staging(path: PathBuf, identity: UpdatePackageInfo) -> Result<Self, String> {
        if unsafe { libc::geteuid() } != 0 {
            return Err("root-only verified staging required".into());
        }
        if !path.starts_with("/run/zethropol-update/aur-artifacts")
            || fs::canonicalize(&path).map_err(|e| e.to_string())? != path
        {
            return Err("server-owned AUR staging required".into());
        }
        for ancestor in path.ancestors() {
            let metadata = fs::symlink_metadata(ancestor).map_err(|e| e.to_string())?;
            if metadata.uid() != 0
                || metadata.permissions().mode() & 0o022 != 0
                || metadata.file_type().is_symlink()
            {
                return Err("unsafe archive staging ancestors".into());
            }
        }
        Self::verify_identity_fields(&path, &identity)?;
        verify_archive(&path, &identity.sha256)?;
        // The loaded archive is independently authenticated again by libalpm
        // with REQUIRED signatures in prepare_aur_locked, never by worker hash.
        Ok(Self { path, identity })
    }
    fn verify_identity_fields(path: &Path, identity: &UpdatePackageInfo) -> Result<(), String> {
        if identity.repository != "aur-attested"
            || identity.filename.is_empty()
            || path.file_name().and_then(|p| p.to_str()) != Some(identity.filename.as_str())
        {
            return Err("invalid server-attested AUR identity".into());
        }
        CanonicalPlan::from_packages(vec![identity.clone()])
            .and_then(|p| p.with_policy_fingerprint("0".repeat(64)))
            .map_err(|e| format!("{e:?}"))?;
        Ok(())
    }
}

struct LockedAur<'a> {
    handle: &'a mut Alpm,
    plan: CanonicalPlan,
}
impl Drop for LockedAur<'_> {
    fn drop(&mut self) {
        let _ = self.handle.trans_release();
    }
}
impl LockedAur<'_> {
    fn claim(
        &self,
        store: &BrokerStore,
        operation: &str,
        now: u64,
    ) -> Result<CanonicalPlan, BrokerError> {
        store.claim_transaction(operation, &self.plan, now)
    }
    // Production proof is deliberately NOT minted by fixture data. Reuse the
    // existing read-only policy/snapshot/boot checks; unknown evidence blocks.
    fn verify_recovery(&self) -> Result<(), String> {
        let hook_dirs = pacman_conf_values(&["HookDir"]).map_err(|e| e.to_string())?;
        crate::snapshot_policy::validate(self.plan.packages(), &hook_dirs)?;
        verify_snapshot_target()?;
        let report = crate::boot_verifier::verify(self.handle, self.plan.packages());
        if crate::boot_verifier::preparation_state(&report) != VerificationState::Verified {
            return Err(format!(
                "AUR boot/recovery evidence unverified: {}",
                report.details
            ));
        }
        Ok(())
    }
}

fn prepare_aur_locked<'a>(
    handle: &'a mut Alpm,
    archives: &[AurArchive],
    fingerprint: &str,
) -> Result<LockedAur<'a>, String> {
    prepare_aur_locked_internal(handle, archives, fingerprint, false)
}
fn prepare_aur_locked_internal<'a>(
    handle: &'a mut Alpm,
    archives: &[AurArchive],
    fingerprint: &str,
    fixture_unsigned: bool,
) -> Result<LockedAur<'a>, String> {
    prepare_aur_locked_with_policy(
        handle,
        archives,
        || Ok(fingerprint.into()),
        fixture_unsigned,
    )
}
fn prepare_aur_locked_with_policy<'a, F>(
    handle: &'a mut Alpm,
    archives: &[AurArchive],
    mut policy_snapshot: F,
    fixture_unsigned: bool,
) -> Result<LockedAur<'a>, String>
where
    F: FnMut() -> Result<String, String>,
{
    #[cfg(not(test))]
    if fixture_unsigned {
        return Err("fixture authority unavailable".into());
    }
    if archives.is_empty() || archives.len() > 64 {
        return Err("bounded AUR archive set required".into());
    }
    // Reuse the existing full official resolver, question policy and lock;
    // do not introduce a second approximation of official upgrade decisions.
    let official = match prepare_alpm_transaction_with_policy(handle, || {
        policy_snapshot().map_err(|e| ("aur_state_unavailable".into(), e))
    }) {
        Ok(plan) => plan,
        Err(error) => {
            let _ = handle.trans_release();
            return Err(format!("official preflight: {error:?}"));
        }
    };
    let result = (|| {
        if !official.packages().is_empty() || !official.removals().is_empty() {
            return Err("official full upgrade required first; no partial upgrade".into());
        }
        for archive in archives {
            AurArchive::verify_identity_fields(&archive.path, &archive.identity)?;
            verify_archive(&archive.path, &archive.identity.sha256)?;
            if handle
                .syncdbs()
                .iter()
                .any(|db| db.pkg(archive.identity.package_name.as_str()).is_ok())
            {
                return Err("AUR target belongs to official repository".into());
            }
            let policy = aur_required_signatures(handle);
            #[cfg(test)]
            let policy = if fixture_unsigned {
                SigLevel::NONE
            } else {
                policy
            };
            let pkg = handle
                .pkg_load(
                    archive.path.as_os_str().as_encoded_bytes().to_vec(),
                    true,
                    policy,
                )
                .map_err(|e| format!("archive/signature: {e}"))?;
            verify_loaded_archive_identity(
                pkg.name(),
                pkg.version().as_str(),
                pkg.filename(),
                &archive.path,
                &archive.identity,
            )?;
            match handle.localdb().pkg(pkg.name()) {
                Ok(installed)
                    if alpm::vercmp(pkg.version().as_str(), installed.version().as_str())
                        .is_lt() =>
                {
                    return Err(format!(
                        "AUR downgrade refused: {} {} -> {}",
                        pkg.name(),
                        installed.version(),
                        pkg.version()
                    ));
                }
                Ok(_) | Err(AlpmError::PkgNotFound) => {}
                Err(error) => return Err(format!("installed AUR metadata unavailable: {error}")),
            }
            handle
                .trans_add_pkg(pkg)
                .map_err(|e| format!("add archive: {e}"))?;
        }
        handle
            .trans_prepare()
            .map_err(|e| format!("dependency/conflict/architecture: {e}"))?;
        if !handle.trans_remove().is_empty() {
            return Err("AUR removals require a separate reviewed workflow".into());
        }
        let mut packages = Vec::new();
        for pkg in handle.trans_add() {
            if pkg.origin() != alpm::PackageFrom::File {
                return Err("official dependency transaction required first".into());
            }
            let mut identity = archives
                .iter()
                .find(|a| a.identity.package_name == pkg.name())
                .ok_or_else(|| "unreviewed archive addition".to_string())?
                .identity
                .clone();
            identity.current_version = handle
                .localdb()
                .pkg(pkg.name())
                .map(|p| p.version().as_str().to_string())
                .unwrap_or_default();
            identity.new_version = pkg.version().as_str().to_string();
            identity.download_size = pkg.size() as u64;
            packages.push(identity);
        }
        if packages.len() != archives.len() {
            return Err("selected archive silently dropped".into());
        }
        let final_fingerprint = policy_snapshot()?;
        if final_fingerprint != official.policy_fingerprint() {
            return Err("installed/repository metadata or policy changed under lock".into());
        }
        CanonicalPlan::from_packages(packages)
            .and_then(|p| p.with_policy_fingerprint(final_fingerprint))
            .map_err(|e| format!("canonical AUR plan: {e:?}"))
    })();
    match result {
        Ok(plan) => Ok(LockedAur { handle, plan }),
        Err(error) => {
            let _ = handle.trans_release();
            Err(error)
        }
    }
}

async fn aur_consume_message(
    connection: &zbus::Connection,
    header: &zbus::message::Header<'_>,
    store: &BrokerStore,
    hash: &str,
) -> Result<crate::broker_protocol::ConsumedPreview, BrokerError> {
    let now = boottime_seconds().map_err(|e| BrokerError::Io(e.to_string()))?;
    let caller =
        AuthorizedCaller::check_message(connection, header, PolicyAction::Install, now).await?;
    let now = boottime_seconds().map_err(|e| BrokerError::Io(e.to_string()))?;
    // Existing atomic one-use consume assigns/retains the server operation ID.
    let trust = ReceiptTrust::load().map_err(BrokerError::Io)?;
    store.consume_aur_receipt_preview(
        caller,
        now,
        receipt_unix_now().map_err(BrokerError::Io)?,
        hash,
        &trust.digest,
    )
}
async fn aur_cancel_message(
    connection: &zbus::Connection,
    header: &zbus::message::Header<'_>,
    store: &BrokerStore,
    operation: &str,
) -> Result<(), BrokerError> {
    let now = boottime_seconds().map_err(|e| BrokerError::Io(e.to_string()))?;
    let caller =
        AuthorizedCaller::check_message(connection, header, PolicyAction::Preview, now).await?;
    let now = boottime_seconds().map_err(|e| BrokerError::Io(e.to_string()))?;
    store.cancel_aur_before_commit(caller, operation, now)
}

// Reuse the existing mandatory snapshot boundary and durable commit evidence.
// This checkpoint never commits by itself. The executor below requires the
// same durable evidence and keeps native/recovery outcomes separate.
pub(crate) fn aur_snapshot_number_valid(number: &str) -> bool {
    valid_snapshot_number(number)
}
fn aur_checkpoint_after_snapshot(
    store: &BrokerStore,
    operation: &str,
    current: &CanonicalPlan,
    pre_snapshot: Result<String, String>,
    now: u64,
) -> Result<ExecutionEvidence, String> {
    commit_after_pre_snapshot(pre_snapshot, |number| {
        if !valid_snapshot_number(number) {
            return Err("invalid snapshot ID".into());
        }
        store
            .checkpoint_aur_commit_start(operation, current, number, now)
            .map_err(|e| format!("durable commit checkpoint: {e:?}"))
    })?
    .1
}

#[cfg(test)]
mod aur_integration_tests {
    include!("tests.rs");
}

// Keeps the actual host lock alive through comparison and the server callback.
// Never invoked by the fixture runner; no host lock/transaction is created by tests.
fn with_final_host_aur_plan<T>(
    archives: &[AurArchive],
    approved: &CanonicalPlan,
    action: impl FnOnce(LockedAur<'_>) -> Result<T, String>,
) -> Result<T, String> {
    if unsafe { libc::geteuid() } != 0 {
        return Err("host final resolver requires trusted root broker".into());
    }
    let mut handle = configured_alpm(None).map_err(|e| e.to_string())?;
    let db = PathBuf::from(handle.dbpath());
    let locked = prepare_aur_locked_with_policy(
        &mut handle,
        archives,
        || aur_host_state_fingerprint(&db),
        false,
    )?;
    if locked.plan != *approved {
        return Err("plan_changed under actual host lock; no retry".into());
    }
    if aur_host_state_fingerprint(&db)? != locked.plan.policy_fingerprint() {
        return Err("host policy changed under lock; no retry".into());
    }
    locked.verify_recovery()?;
    action(locked)
}

fn aur_begin_after_real_snapshot(
    locked: &LockedAur<'_>,
    store: &BrokerStore,
    operation: &str,
) -> Result<ExecutionEvidence, String> {
    locked.verify_recovery()?;
    let now = boottime_seconds().map_err(|e| e.to_string())?;
    locked
        .claim(store, operation, now)
        .map_err(|e| format!("AUR plan claim: {e:?}"))?;
    let result = (|| {
        let snapshot = create_pre_snapshot()?; // Existing mandatory snapshot implementation.
        store
            .record_aur_pre_snapshot(operation, &locked.plan, &snapshot)
            .map_err(|e| format!("{e:?}"))?;
        if aur_host_state_fingerprint(Path::new(locked.handle.dbpath()))?
            != locked.plan.policy_fingerprint()
        {
            return Err(format!(
                "policy changed after pre-snapshot {snapshot}; no commit or retry"
            ));
        }
        aur_checkpoint_after_snapshot(
            store,
            operation,
            &locked.plan,
            Ok(snapshot),
            boottime_seconds().map_err(|e| e.to_string())?,
        )
    })();
    if let Err(error) = &result {
        store
            .finish_worker_result(
                operation,
                OperationStatus::Failed,
                error,
                boottime_seconds().map_err(|e| e.to_string())?,
            )
            .map_err(|e| {
                format!("AUR pre-commit error {error}; durable failure write failed: {e:?}")
            })?;
    }
    result
}

fn aur_database_state_fingerprint(db: &Path, policy: &str) -> Result<String, String> {
    let values = format!("zethropol-aur-final-state-v1:{policy}");
    policy_fingerprint_with_values(&[db.join("local"), db.join("sync")], values.as_bytes())
}
fn aur_host_state_fingerprint(db: &Path) -> Result<String, String> {
    let policy = current_policy_fingerprint().map_err(|e| format!("{e:?}"))?;
    let trust = ReceiptTrust::load()?;
    aur_database_state_fingerprint(db, &format!("{policy}:aur-trust:{}", trust.digest))
}

include!("intake.rs");

// Consumes the held transaction. Invoked only by the single leased worker,
// after aur_begin_after_real_snapshot has durably won against cancellation.
// Native commit, boot verifier and post-snapshot implementation are shared
// with Update. No builder/PKGBUILD/shell command is accepted by this executor.
fn aur_commit_started(
    locked: LockedAur<'_>,
    store: &BrokerStore,
    operation: &str,
) -> Result<(), String> {
    let trust = ReceiptTrust::load()?;
    store
        .aur_receipt_binding(
            operation,
            &locked.plan,
            boottime_seconds().map_err(|e| e.to_string())?,
            receipt_unix_now()?,
            &trust.digest,
        )
        .map_err(|e| format!("receipt expired/changed before commit: {e:?}"))?;
    aur_commit_started_with_post(locked, store, operation, create_post_snapshot)
}
fn aur_commit_started_with_post(
    locked: LockedAur<'_>,
    store: &BrokerStore,
    operation: &str,
    post_snapshot: impl FnOnce(&str) -> Result<String, String>,
) -> Result<(), String> {
    let mut evidence = store
        .aur_commit_evidence(operation, &locked.plan)
        .map_err(|e| format!("commit boundary: {e:?}"))?;
    let pre = evidence
        .pre_snapshot
        .clone()
        .ok_or("missing recovery point")?;
    // All package loading and signature checks have already happened under
    // this native transaction lock; staged bytes outlive this function.
    let commit = locked.handle.trans_commit();
    record_commit_outcome(&mut evidence, commit.as_ref().err().map(|e| e.error()));
    let commit_error = commit.err().map(|e| e.to_string());
    // Persist the native result before attempting recovery checks. If this
    // write fails, leave InProgress for reconciliation; never retry commit.
    store
        .record_execution_evidence(operation, &evidence)
        .map_err(|e| format!("native outcome persistence failed: {e:?}; no retry"))?;
    // Reopen the local DB rather than trusting a potentially cached package
    // view or interpreting a native return value as installed-version proof.
    let installed_versions = if commit_error.is_none() {
        verify_aur_installed_versions(locked.handle, &locked.plan)
    } else {
        Err("version verification unavailable after failed native commit".into())
    };
    evidence.boot = crate::boot_verifier::verify(locked.handle, locked.plan.packages());
    evidence.boot_preparation = crate::boot_verifier::preparation_state(&evidence.boot);
    let release_error = locked.handle.trans_release().err().map(|e| e.to_string());
    drop(locked); // Drop is a fallback; the explicit release result is reported.
    let post = post_snapshot(&pre);
    if let Ok(number) = &post {
        evidence.post_snapshot = Some(number.clone());
    }
    store
        .record_execution_evidence(operation, &evidence)
        .map_err(|e| format!("post-commit persistence failed: {e:?}; no retry"))?;
    let status = if commit_error.is_some() || installed_versions.is_err() {
        OperationStatus::Failed
    } else {
        OperationStatus::CompletedWithWarnings
    };
    let message = format!("AUR native outcome {:?}; installed versions {installed_versions:?}; pre-snapshot {pre}; post-snapshot {post:?}; error {commit_error:?}; lock release {release_error:?}. Hook success and boot recovery remain unverified. No automatic retry.", evidence.package_commit);
    store
        .finish_worker_result(
            operation,
            status,
            &message,
            boottime_seconds().map_err(|e| e.to_string())?,
        )
        .map_err(|e| format!("terminal result persistence failed: {e:?}; no retry"))?;
    if let Some(error) = commit_error {
        return Err(error);
    }
    installed_versions?;
    Ok(())
}

fn verify_aur_installed_versions(handle: &Alpm, plan: &CanonicalPlan) -> Result<(), String> {
    let fresh = Alpm::new(handle.root(), handle.dbpath())
        .map_err(|e| format!("installed AUR database unavailable: {e}"))?;
    for expected in plan.packages() {
        let installed = fresh
            .localdb()
            .pkg(expected.package_name.as_str())
            .map_err(|e| {
                format!(
                    "installed AUR package missing: {}: {e}",
                    expected.package_name
                )
            })?;
        if installed.version().as_str() != expected.new_version {
            return Err(format!(
                "installed AUR version mismatch: {} expected {}, observed {}",
                expected.package_name,
                expected.new_version,
                installed.version()
            ));
        }
    }
    Ok(())
}

// Deliberately no runtime enable flag. Trust verification is implemented;
// real PolicyKit/snapshot/boot/recovery acceptance is a separate mandatory gate.
fn require_aur_production_authority() -> Result<(), String> {
    ReceiptTrust::load().map_err(|e| {
        format!("AUR installation closed: production receipt policy unavailable: {e}")
    })?;
    Err(
        "AUR installation closed: required PolicyKit/snapshot/boot/recovery security acceptance incomplete"
            .into(),
    )
}

fn aur_execute_received(
    store: &BrokerStore,
    consumed: &crate::broker_protocol::ConsumedPreview,
    archives: Vec<zbus::zvariant::OwnedFd>,
    signatures: Vec<zbus::zvariant::OwnedFd>,
) -> Result<(), String> {
    use std::os::fd::AsFd;
    require_aur_production_authority()?;
    let _lease = store
        .worker_lease(&consumed.operation_id)
        .map_err(|e| format!("worker lease: {e:?}"))?;
    let result = (|| {
        store
            .require_queued(&consumed.operation_id)
            .map_err(|e| format!("queued operation: {e:?}"))?;
        if unsafe { libc::geteuid() } != 0 {
            return Err("root broker required".into());
        }
        let parent = Path::new("/run/zethropol-update/aur-artifacts");
        verify_aur_staging_parent(parent)?;
        let handle = configured_alpm(None).map_err(|e| e.to_string())?;
        let fds: Vec<_> = archives
            .iter()
            .zip(&signatures)
            .map(|(a, s)| (a.as_fd(), s.as_fd()))
            .collect();
        let trust = ReceiptTrust::load()?;
        let binding = store
            .aur_receipt_binding(
                &consumed.operation_id,
                &consumed.plan,
                boottime_seconds().map_err(|e| e.to_string())?,
                receipt_unix_now()?,
                &trust.digest,
            )
            .map_err(|e| format!("receipt binding: {e:?}"))?;
        verify_receipt_signature_delivery(&consumed.plan, &binding.signatures, &fds)?;
        let intake = receive_aur_fds(parent, &consumed.plan, &fds, &handle)?;
        drop(handle);
        with_final_host_aur_plan(&intake.archives, &consumed.plan, |locked| {
            aur_begin_after_real_snapshot(&locked, store, &consumed.operation_id)?;
            aur_commit_started(locked, store, &consumed.operation_id)
        })
    })();
    if let Err(error) = &result {
        store
            .finish_worker_result(
                &consumed.operation_id,
                OperationStatus::Failed,
                &format!("{error}; no automatic retry"),
                boottime_seconds().map_err(|e| e.to_string())?,
            )
            .map_err(|e| format!("{error}; failure persistence: {e:?}; no retry"))?;
    }
    result
}

include!("receipt.rs");

fn verify_aur_staging_parent(parent: &Path) -> Result<(), String> {
    if unsafe { libc::geteuid() } != 0 {
        return Err("root staging required".into());
    }
    for ancestor in parent.ancestors() {
        let meta = fs::symlink_metadata(ancestor).map_err(|e| e.to_string())?;
        if !meta.is_dir() || meta.uid() != 0 || meta.permissions().mode() & 0o022 != 0 {
            return Err("unsafe root staging ancestors".into());
        }
    }
    Ok(())
}

pub(crate) fn aur_boottime() -> Result<u64, String> {
    boottime_seconds().map_err(|e| e.to_string())
}
fn persistent_aur_broker() -> Result<Arc<BrokerStore>, BrokerError> {
    // Initialize/reconcile once per broker process, before accepting AUR work.
    // Cache failures too: damaged state needs explicit operator investigation.
    static STORE: std::sync::OnceLock<Result<Arc<BrokerStore>, BrokerError>> =
        std::sync::OnceLock::new();
    STORE
        .get_or_init(|| BrokerStore::open_aur_persistent().map(Arc::new))
        .clone()
}
