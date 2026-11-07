use crate::broker_protocol::{
    AuthorizedCaller, BrokerError, BrokerStore, CanonicalPlan, OperationStatus, PolicyAction,
};
use crate::execution_evidence::{CommitState, ExecutionEvidence, VerificationState};
use crate::integrity::{policy_fingerprint_with_values, repository_database_hash, verify_archive};
use crate::models::{OperationResult, PackageRemoval, UpdatePackageInfo, UpdateState};
use alpm::{Alpm, Error as AlpmError, Event, PackageOperation, Question, SigLevel, TransFlag};
use std::collections::HashMap;
use std::fs;
use std::os::unix::fs::{symlink, MetadataExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use zbus::interface;

const PLAN_CACHE_TTL: Duration = Duration::from_secs(20);

#[derive(Debug, Clone)]
pub struct OpState {
    pub status: String, // "pending", "running", "ok", "failed"
    pub message: String,
}

#[cfg(test)]
#[derive(Debug)]
struct PreviewGrant {
    packages: Vec<UpdatePackageInfo>,
    created_at: Instant,
}

#[derive(Debug, Default)]
struct ManagerState {
    operations: HashMap<String, OpState>,
    #[cfg(test)]
    previews: HashMap<String, PreviewGrant>,
    active_operation: Option<String>,
    active_plan: Vec<UpdatePackageInfo>,
    pending_plan: Option<CachedPlan>,
}

#[derive(Debug, Clone)]
struct CachedPlan {
    packages: Vec<UpdatePackageInfo>,
    created_at: Instant,
}

pub struct UpdateManager {
    state: Arc<Mutex<ManagerState>>,
    plan_refresh_lock: Mutex<()>,
    broker: Option<Arc<BrokerStore>>,
    #[cfg(test)]
    test_authentication: bool,
}

impl UpdateManager {
    pub(crate) fn broker_store(&self) -> Arc<BrokerStore> {
        Arc::clone(self.broker.as_ref().expect("production broker"))
    }
    pub fn try_new() -> Result<Self, BrokerError> {
        let broker = Arc::new(BrokerStore::open_runtime()?);
        reconcile_worker_records(&broker)?;
        Ok(Self {
            state: Arc::new(Mutex::new(ManagerState::default())),
            plan_refresh_lock: Mutex::new(()),
            broker: Some(broker),
            #[cfg(test)]
            test_authentication: false,
        })
    }

    fn current_pending_plan(&self) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
        let _refresh_guard = self.plan_refresh_lock.lock().unwrap();
        let now = Instant::now();
        {
            let state = self.state.lock().unwrap();
            if state.active_operation.is_some() {
                return Ok(state.active_plan.clone());
            }
        }
        let paths = pacman_conf_values(&["DBPath"])?;
        if paths.len() != 1 {
            return Err(zbus::fdo::Error::Failed(
                "Pacman DBPath is ambiguous; scan refused.".into(),
            ));
        }
        ensure_scan_database_idle(Path::new(&paths[0]))?;
        cache_or_resolve_plan(&self.state, now, scan_official_update_candidates)
    }

    async fn authorize_preview_request(
        &self,
        header: &zbus::message::Header<'_>,
    ) -> Result<(), zbus::fdo::Error> {
        #[cfg(test)]
        if self.test_authentication {
            return Ok(());
        }
        let connection = zbus::Connection::system().await.map_err(|e| {
            zbus::fdo::Error::Failed(format!("Unable to connect to system bus: {e}"))
        })?;
        AuthorizedCaller::check_message(
            &connection,
            header,
            PolicyAction::Preview,
            boottime_seconds()?,
        )
        .await
        .map_err(broker_error)?;
        Ok(())
    }
}

pub(crate) fn run_worker(operation_id: &str) -> Result<(), String> {
    // systemd supplies only the broker-generated UUID. Reject all other input
    // before touching the operation store or package manager.
    if operation_id.len() != 36
        || !operation_id
            .bytes()
            .all(|b| b.is_ascii_hexdigit() || b == b'-')
    {
        return Err("Invalid opaque operation identifier".into());
    }
    let store = BrokerStore::open_runtime().map_err(|e| format!("broker store: {e:?}"))?;
    let _lease = store
        .worker_lease(operation_id)
        .map_err(|e| format!("worker already owned or unavailable: {e:?}"))?;
    store
        .require_queued(operation_id)
        .map_err(|e| format!("worker is not queued: {e:?}"))?;
    let state = Arc::new(Mutex::new(ManagerState::default()));
    let result = execute_system_upgrade(&state, operation_id, &store);
    let (status, message) = match result {
        Ok((code, message)) => (
            if code == "ok_with_warnings" {
                OperationStatus::CompletedWithWarnings
            } else if code == "ok" {
                OperationStatus::Succeeded
            } else {
                OperationStatus::Failed
            },
            format!("{code}: {message}"),
        ),
        Err((code, message)) => (OperationStatus::Failed, format!("{code}: {message}")),
    };
    store
        .finish_worker_result(
            operation_id,
            status,
            &message,
            boottime_seconds().map_err(|e| e.to_string())?,
        )
        .map_err(|e| format!("record worker result: {e:?}"))?;
    eprintln!("update-worker {operation_id}: {message}");
    if status == OperationStatus::Failed {
        Err(message)
    } else {
        Ok(())
    }
}

pub(crate) fn mark_worker_interrupted(operation_id: &str) -> Result<(), String> {
    if operation_id.len() != 36
        || !operation_id
            .bytes()
            .all(|b| b.is_ascii_hexdigit() || b == b'-')
    {
        return Err("Invalid opaque operation identifier".into());
    }
    let store = BrokerStore::open_runtime().map_err(|e| format!("broker store: {e:?}"))?;
    let _lease = match store.worker_lease(operation_id) {
        Ok(lease) => lease,
        Err(BrokerError::OperationInProgress) => return Ok(()),
        Err(error) => return Err(format!("interrupted worker lease: {error:?}")),
    };
    store
        .worker_process_stopped(operation_id, boottime_seconds().map_err(|e| e.to_string())?)
        .map_err(|e| format!("record interrupted worker: {e:?}"))
}

#[cfg(test)]
fn consume_preview_grant(
    state: &mut ManagerState,
    sender: &str,
    now: Instant,
) -> Result<Vec<UpdatePackageInfo>, String> {
    let grant = state.previews.remove(sender).ok_or_else(|| {
        "Preview the official-repository transaction again before installing.".to_string()
    })?;
    if now.duration_since(grant.created_at) > Duration::from_secs(300) {
        return Err("The transaction preview expired; preview the update again.".to_string());
    }
    Ok(grant.packages)
}

#[cfg(test)]
fn reserve_operation(
    state: &mut ManagerState,
    operation_id: &str,
    packages: Vec<UpdatePackageInfo>,
) -> Result<(), String> {
    if state.active_operation.is_some() {
        return Err("Another update transaction is already running.".to_string());
    }
    state.active_operation = Some(operation_id.to_string());
    state.active_plan = packages.clone();
    state.pending_plan = None;
    state.operations.insert(
        operation_id.to_string(),
        OpState {
            status: "running".to_string(),
            message: "Rechecking the approved transaction before creating a recovery point."
                .to_string(),
        },
    );
    Ok(())
}

#[cfg(test)]
fn plan_matches_approved(
    current_plan: &[UpdatePackageInfo],
    approved_packages: &[UpdatePackageInfo],
) -> bool {
    CanonicalPlan::from_packages(approved_packages.to_vec())
        .and_then(|approved| {
            approved
                .matches(current_plan)
                .map(|matches| matches && approved.packages().len() == approved_packages.len())
        })
        .unwrap_or(false)
}

#[interface(name = "org.zethropol.Update.v1")]
impl UpdateManager {
    // Metadata review is deliberately distinct from an authenticated ALPM plan.
    fn review_aur_selection(
        &self,
        names: Vec<String>,
        versions: Vec<String>,
    ) -> Result<HashMap<String, String>, zbus::fdo::Error> {
        aur_selection_review(&names, &versions).map_err(zbus::fdo::Error::InvalidArgs)
    }
    async fn get_aur_build_context(
        &self,
        operation_id: String,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<HashMap<String, String>, zbus::fdo::Error> {
        let caller = AuthorizedCaller::check_message(
            connection,
            &header,
            PolicyAction::Preview,
            boottime_seconds()?,
        )
        .await
        .map_err(broker_error)?;
        let store = persistent_aur_broker().map_err(broker_error)?;
        let c = store
            .receipt_context_for_operation(caller.identity(), &operation_id, boottime_seconds()?)
            .map_err(broker_error)?;
        Ok(HashMap::from([
            ("operation_id".into(), c.operation_id),
            ("uid".into(), c.uid.to_string()),
            ("bus_name".into(), c.bus_name),
            ("session_id".into(), c.session_id),
            (
                "review".into(),
                serde_json::to_string(&c.review)
                    .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?,
            ),
        ]))
    }
    async fn reserve_aur_build(
        &self,
        approved_review_sha256: String,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<String, zbus::fdo::Error> {
        let review = load_approved_aur_review(&approved_review_sha256)
            .map_err(zbus::fdo::Error::NotSupported)?;
        let context = reserve_reviewed_aur_message(connection, &header, review)
            .await
            .map_err(broker_error)?;
        Ok(context.operation_id)
    }

    // Exported by the same UpdateManager interface in normal service builds.
    // Production installation still requires its independent acceptance gate.
    async fn install_aur(
        &self,
        plan_hash: String,
        archives: Vec<zbus::zvariant::OwnedFd>,
        signatures: Vec<zbus::zvariant::OwnedFd>,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<String, zbus::fdo::Error> {
        // No caller-supplied boolean/key/receipt can open the production gate.
        require_aur_production_authority().map_err(zbus::fdo::Error::NotSupported)?;
        if archives.is_empty() || archives.len() > 64 || archives.len() != signatures.len() {
            return Err(zbus::fdo::Error::InvalidArgs(
                "exact bounded archive/signature pairs required".into(),
            ));
        }
        let store = persistent_aur_broker().map_err(broker_error)?;
        let consumed = aur_consume_message(connection, &header, &store, &plan_hash)
            .await
            .map_err(broker_error)?;
        let operation = consumed.operation_id.clone();
        // Owned D-Bus FDs remain alive until the worker finishes. No caller
        // path or command is passed across this boundary.
        tokio::task::spawn_blocking(move || {
            let result = aur_execute_received(&store, &consumed, archives, signatures);
            if let Err(error) = result {
                eprintln!("AUR {}: {error}; no automatic retry", consumed.operation_id);
            }
        });
        Ok(operation)
    }

    async fn cancel_aur(
        &self,
        operation_id: String,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<(), zbus::fdo::Error> {
        let store = persistent_aur_broker().map_err(broker_error)?;
        aur_cancel_message(connection, &header, &store, &operation_id)
            .await
            .map_err(broker_error)
    }

    // The versioned D-Bus method carries independent sealed FDs and caller context.
    #[allow(clippy::too_many_arguments)]
    #[zbus(out_args("packages", "removals", "plan_hash"))]
    async fn preview_aur_receipt(
        &self,
        operation_id: String,
        receipt: zbus::zvariant::OwnedFd,
        receipt_signature: zbus::zvariant::OwnedFd,
        manifest: zbus::zvariant::OwnedFd,
        manifest_signature: zbus::zvariant::OwnedFd,
        archives: Vec<zbus::zvariant::OwnedFd>,
        signatures: Vec<zbus::zvariant::OwnedFd>,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<(Vec<UpdatePackageInfo>, Vec<PackageRemoval>, String), zbus::fdo::Error> {
        // Verification/preview does not enable installation. InstallAur retains
        // its independent, unconditional real-system acceptance gate.
        let trust = ReceiptTrust::load().map_err(zbus::fdo::Error::NotSupported)?;
        if archives.is_empty() || archives.len() > 64 || archives.len() != signatures.len() {
            return Err(zbus::fdo::Error::InvalidArgs(
                "bounded archive/signature pairs required".into(),
            ));
        }
        let now = boottime_seconds()?;
        let caller =
            AuthorizedCaller::check_message(connection, &header, PolicyAction::Preview, now)
                .await
                .map_err(broker_error)?;
        let store = persistent_aur_broker().map_err(broker_error)?;
        let context = store
            .receipt_context_for_operation(caller.identity(), &operation_id, boottime_seconds()?)
            .map_err(broker_error)?;
        tokio::task::spawn_blocking(move || {
            use std::os::fd::AsFd;
            let parent = Path::new("/run/zethropol-update/aur-artifacts");
            verify_aur_staging_parent(parent)?;
            let fds: Vec<_> = archives
                .iter()
                .zip(&signatures)
                .map(|(a, s)| (a.as_fd(), s.as_fd()))
                .collect();
            let verified = verify_aur_receipt_isolated(
                &store,
                &context,
                ReceiptFds {
                    receipt: receipt.as_fd(),
                    receipt_signature: receipt_signature.as_fd(),
                    manifest: manifest.as_fd(),
                    manifest_signature: manifest_signature.as_fd(),
                    packages: &fds,
                },
                &trust,
                parent,
            )?;
            let plan = store
                .admit_aur_receipt(
                    caller,
                    &verified,
                    boottime_seconds().map_err(|e| e.to_string())?,
                    receipt_unix_now()?,
                )
                .map_err(|e| format!("receipt admission: {e:?}"))?;
            Ok::<_, String>((
                plan.packages().to_vec(),
                plan.removals().to_vec(),
                plan.hash(),
            ))
        })
        .await
        .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?
        .map_err(zbus::fdo::Error::Failed)
    }

    async fn get_aur_operation_state(
        &self,
        operation_id: String,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<HashMap<String, String>, zbus::fdo::Error> {
        let caller = AuthorizedCaller::check_message(
            connection,
            &header,
            PolicyAction::Preview,
            boottime_seconds()?,
        )
        .await
        .map_err(broker_error)?;
        let store = persistent_aur_broker().map_err(broker_error)?;
        let mut view = store
            .aur_history_for_owner(caller.clone(), &operation_id, boottime_seconds()?)
            .map_err(broker_error)?;
        if matches!(
            view.status,
            OperationStatus::Queued | OperationStatus::Running
        ) {
            // Original lease/grace rules also catch an in-process worker panic.
            // This never restarts a worker or retries native commit.
            store
                .reconcile_worker_state(&operation_id, false, boottime_seconds()?)
                .map_err(broker_error)?;
            view = store
                .aur_history_for_owner(caller, &operation_id, boottime_seconds()?)
                .map_err(broker_error)?;
        }
        insert_operation_view(HashMap::new(), view)
    }

    fn get_aur_readiness(&self) -> HashMap<String, String> {
        aur_trust_readiness()
    }

    async fn get_update_state(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<UpdateState, zbus::fdo::Error> {
        self.authorize_preview_request(&header).await?;
        {
            let state = self.state.lock().unwrap();
            if state.active_operation.is_some() {
                return Ok(UpdateState {
                    is_checking: false,
                    is_updating: true,
                    last_check_time: "Update in progress".to_string(),
                    pending_updates_count: state.active_plan.len() as u32,
                });
            }
        }

        let pending = self.current_pending_plan()?;
        let count = pending.len() as u32;

        Ok(UpdateState {
            is_checking: false,
            is_updating: false,
            last_check_time: "Just now".to_string(),
            pending_updates_count: count,
        })
    }

    async fn list_pending_updates(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
        self.authorize_preview_request(&header).await?;
        {
            let state = self.state.lock().unwrap();
            if state.active_operation.is_some() {
                return Ok(state.active_plan.clone());
            }
        }
        self.current_pending_plan()
    }

    async fn preview_transaction(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
        if self.state.lock().unwrap().active_operation.is_some() {
            return Err(zbus::fdo::Error::Failed(
                "An update transaction is already running.".to_string(),
            ));
        }
        #[cfg(test)]
        let packages = if self.test_authentication {
            self.current_pending_plan()?
        } else {
            store_preview(self, &header).await?
        };
        #[cfg(not(test))]
        let packages = store_preview(self, &header).await?;
        if self.state.lock().unwrap().active_operation.is_some() {
            return Err(zbus::fdo::Error::Failed(
                "An update transaction started while preparing the preview.".to_string(),
            ));
        }

        Ok(packages)
    }

    /// Replacement-aware preview. The hash and effects come from the exact
    /// canonical plan persisted for this authenticated sender.
    #[zbus(out_args("packages", "removals", "plan_hash"))]
    async fn preview_plan(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<(Vec<UpdatePackageInfo>, Vec<PackageRemoval>, String), zbus::fdo::Error> {
        let now = boottime_seconds()?;
        let connection = zbus::Connection::system()
            .await
            .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?;
        let caller =
            AuthorizedCaller::check_message(&connection, &header, PolicyAction::Preview, now)
                .await
                .map_err(broker_error)?;
        let _guard = self.plan_refresh_lock.lock().unwrap();
        let plan = resolved_transaction_preflight()?;
        let store = self
            .broker
            .as_ref()
            .ok_or_else(|| zbus::fdo::Error::Failed("Broker unavailable".into()))?;
        store
            .cleanup(now, Duration::from_secs(7 * 24 * 60 * 60))
            .map_err(broker_error)?;
        store
            .create_preview(caller, plan.clone(), now)
            .map_err(broker_error)?;
        Ok((
            plan.packages().to_vec(),
            plan.removals().to_vec(),
            plan.hash(),
        ))
    }

    async fn get_operation_state(
        &self,
        operation_id: String,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<HashMap<String, String>, zbus::fdo::Error> {
        let mut res = HashMap::new();
        let connection = zbus::Connection::system().await.map_err(|e| {
            zbus::fdo::Error::Failed(format!("Unable to connect to system bus: {e}"))
        })?;
        let caller = AuthorizedCaller::check_message(
            &connection,
            &header,
            PolicyAction::Preview,
            boottime_seconds()?,
        )
        .await
        .map_err(broker_error)?;
        let store = self.broker.as_ref().ok_or_else(|| {
            zbus::fdo::Error::Failed("Update broker store is unavailable.".into())
        })?;
        let view = match store.operation_for_owner(caller.identity(), &operation_id) {
            Ok(view) => view,
            Err(BrokerError::NotFound) => {
                res.insert("status".to_string(), "not-found".to_string());
                res.insert(
                    "message".to_string(),
                    "Operation ID does not exist.".to_string(),
                );
                return Ok(res);
            }
            Err(error) => return Err(broker_error(error)),
        };
        if matches!(
            view.status,
            OperationStatus::Queued | OperationStatus::Running
        ) {
            reconcile_worker_record(store, &view.operation_id, boottime_seconds()?)
                .map_err(broker_error)?;
            let updated = store
                .operation_for_owner(caller.identity(), &operation_id)
                .map_err(broker_error)?;
            return insert_operation_view(res, updated);
        }
        insert_operation_view(res, view)
    }

    /// Resumable, read-only post-update UI contract. No nonce or install
    /// capability is returned, and no reboot is performed by this method.
    async fn get_post_update_state(
        &self,
        operation_id: String,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<HashMap<String, String>, zbus::fdo::Error> {
        let connection = zbus::Connection::system()
            .await
            .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?;
        let caller = AuthorizedCaller::check_message(
            &connection,
            &header,
            PolicyAction::Preview,
            boottime_seconds()?,
        )
        .await
        .map_err(broker_error)?;
        let store = self
            .broker
            .as_ref()
            .ok_or_else(|| zbus::fdo::Error::Failed("Broker unavailable".into()))?;
        reconcile_worker_records(store).map_err(broker_error)?;
        let worker_busy = !store
            .unresolved_operations()
            .map_err(broker_error)?
            .is_empty();
        let requested = (!operation_id.is_empty()).then_some(operation_id.as_str());
        let mut view = match store.resumable_operation(caller.identity(), requested) {
            Ok(view) => view,
            Err(BrokerError::NotFound) => {
                return Ok(HashMap::from([
                    ("post_update_protocol".into(), "1".into()),
                    ("status".into(), "not-found".into()),
                ]))
            }
            Err(error) => return Err(broker_error(error)),
        };
        if matches!(
            view.status,
            OperationStatus::Queued | OperationStatus::Running
        ) {
            reconcile_worker_record(store, &view.operation_id, boottime_seconds()?)
                .map_err(broker_error)?;
            view = store
                .resumable_operation(caller.identity(), Some(&view.operation_id))
                .map_err(broker_error)?;
        }
        let id = view.operation_id.clone();
        let terminal = view.status.terminal();
        let (reboot, reason) = post_update_reboot_requirement(terminal, &view.evidence);
        let mut result = insert_operation_view(HashMap::new(), view)?;
        result.insert("post_update_protocol".into(), "1".into());
        result.insert("operation_id".into(), id);
        result.insert("terminal".into(), terminal.to_string());
        result.insert("worker_busy".into(), worker_busy.to_string());
        result.insert("reboot_requirement".into(), reboot.into());
        result.insert("reboot_reason".into(), reason.into());
        Ok(result)
    }

    async fn trigger_update(
        &self,
        plan_hash: String,
        approve_removals: bool,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<OperationResult, zbus::fdo::Error> {
        let now = boottime_seconds()?;
        let connection = zbus::Connection::system().await.map_err(|e| {
            zbus::fdo::Error::Failed(format!("Unable to connect to system bus: {e}"))
        })?;
        let caller =
            AuthorizedCaller::check_message(&connection, &header, PolicyAction::Install, now)
                .await
                .map_err(broker_error)?;
        let store = self.broker.as_ref().ok_or_else(|| {
            zbus::fdo::Error::Failed("Update broker store is unavailable.".into())
        })?;
        let consumed = store
            .consume_approved_preview(caller, now, &plan_hash, approve_removals)
            .map_err(broker_error)?;
        if consumed.plan.packages().is_empty() {
            store
                .worker_failed_before_claim(
                    &consumed.operation_id,
                    "No official updates remain.",
                    now,
                )
                .map_err(broker_error)?;
            return Ok(OperationResult {
                success: false,
                code: "no_updates".to_string(),
                message: "There are no official-repository updates to install.".to_string(),
                operation_id: String::new(),
                data: HashMap::new(),
            });
        }

        let operation_id = consumed.operation_id;
        let start = Command::new("/usr/bin/systemctl")
            .args([
                "start",
                &format!("zethropol-update-worker@{operation_id}.service"),
            ])
            .stdin(std::process::Stdio::null())
            .status();
        match start {
            Ok(status) if status.success() => {}
            _ => {
                store
                    .worker_start_failed(
                        &operation_id,
                        "Unable to start the fixed systemd update worker.",
                    )
                    .map_err(broker_error)?;
                return Ok(OperationResult {
                    success: false,
                    code: "worker_start_failed".into(),
                    message: "Worker start was not confirmed. Inspect operation status; no automatic retry was made.".into(),
                    operation_id,
                    data: HashMap::new(),
                });
            }
        }

        Ok(OperationResult {
            success: true,
            code: "started".to_string(),
            message: "The approved official-repository update transaction has started.".to_string(),
            operation_id,
            data: HashMap::new(),
        })
    }

    #[zbus(signal)]
    async fn update_status_changed(
        ctxt: &zbus::object_server::SignalEmitter<'_>,
        new_state: UpdateState,
    ) -> zbus::Result<()>;
}

fn insert_operation_view(
    mut res: HashMap<String, String>,
    view: crate::broker_protocol::OperationView,
) -> Result<HashMap<String, String>, zbus::fdo::Error> {
    res.insert(
        "status".to_string(),
        match view.status {
            OperationStatus::Previewed | OperationStatus::Queued => "pending",
            OperationStatus::Running => "running",
            OperationStatus::Succeeded if view.evidence.fully_verified() => "ok",
            OperationStatus::Succeeded | OperationStatus::CompletedWithWarnings => {
                "ok_with_warnings"
            }
            OperationStatus::WorkerStartFailed | OperationStatus::Failed => "failed",
        }
        .to_string(),
    );
    res.insert(
        "package_commit".into(),
        view.evidence.package_commit.as_str().into(),
    );
    res.insert("hooks".into(), view.evidence.hooks.as_str().into());
    res.insert(
        "boot_preparation".into(),
        view.evidence.boot_preparation.as_str().into(),
    );
    res.insert(
        "pre_snapshot".into(),
        view.evidence.pre_snapshot.unwrap_or_default(),
    );
    res.insert(
        "post_snapshot".into(),
        view.evidence.post_snapshot.unwrap_or_default(),
    );
    for (key, state) in [
        ("kernel_artifacts", view.evidence.boot.kernels),
        ("dkms_artifacts", view.evidence.boot.dkms),
        ("initramfs_artifacts", view.evidence.boot.initramfs),
        ("limine_artifacts", view.evidence.boot.limine),
        ("new_kernel_boot", view.evidence.boot.new_kernel_boot),
    ] {
        res.insert(key.into(), state.as_str().into());
    }
    res.insert("running_kernel".into(), view.evidence.boot.running_kernel);
    res.insert(
        "installed_kernels".into(),
        view.evidence.boot.installed_kernels,
    );
    res.insert("updated_kernels".into(), view.evidence.boot.updated_kernels);
    res.insert("boot_details".into(), view.evidence.boot.details);
    res.insert("hook_details".into(), view.evidence.hook_details);
    res.insert("cleanup_status".into(), view.evidence.cleanup.status);
    res.insert(
        "cleanup_removed_files".into(),
        view.evidence.cleanup.removed_files.to_string(),
    );
    res.insert(
        "cleanup_removed_bytes".into(),
        view.evidence.cleanup.removed_bytes.to_string(),
    );
    res.insert(
        "cleanup_reclaimed_bytes".into(),
        view.evidence.cleanup.reclaimed_bytes.to_string(),
    );
    res.insert("cleanup_details".into(), view.evidence.cleanup.details);
    res.insert("message".to_string(), view.message);
    Ok(res)
}

fn post_update_reboot_requirement(
    terminal: bool,
    evidence: &ExecutionEvidence,
) -> (&'static str, &'static str) {
    if !terminal || evidence.package_commit != CommitState::Committed {
        return (
            "unknown",
            "The package transaction has not completed with a confirmed commit.",
        );
    }
    if evidence.boot.new_kernel_boot == VerificationState::Verified {
        return (
            "not-required",
            "A separate verified observation confirms booting the updated kernel.",
        );
    }
    if evidence.boot_preparation == VerificationState::Verified
        && evidence.boot.kernels == VerificationState::Verified
        && !evidence.boot.updated_kernels.trim().is_empty()
    {
        return ("required", "The completed transaction installed verified kernel targets; booting those targets has not yet been observed.");
    }
    ("unknown", "No authoritative reboot requirement is available. Package names and cleanup results are not used to guess it.")
}

async fn store_preview(
    manager: &UpdateManager,
    header: &zbus::message::Header<'_>,
) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
    let now = boottime_seconds()?;
    let connection = zbus::Connection::system()
        .await
        .map_err(|e| zbus::fdo::Error::Failed(format!("Unable to connect to system bus: {e}")))?;
    let caller = AuthorizedCaller::check_message(&connection, header, PolicyAction::Preview, now)
        .await
        .map_err(broker_error)?;
    let plan = resolved_transaction_preflight()?;
    if !plan.removals().is_empty() {
        return Err(zbus::fdo::Error::Failed(
            "Use the replacement-aware PreviewPlan interface to inspect removals.".into(),
        ));
    }
    let store = manager
        .broker
        .as_ref()
        .ok_or_else(|| zbus::fdo::Error::Failed("Update broker store is unavailable.".into()))?;
    store
        .cleanup(now, Duration::from_secs(7 * 24 * 60 * 60))
        .map_err(broker_error)?;
    let packages = plan.packages().to_vec();
    store
        .create_preview(caller, plan, now)
        .map_err(broker_error)?;
    Ok(packages)
}

fn boottime_seconds() -> Result<u64, zbus::fdo::Error> {
    let mut value = unsafe { std::mem::zeroed::<libc::timespec>() };
    if unsafe { libc::clock_gettime(libc::CLOCK_BOOTTIME, &mut value) } != 0 {
        return Err(zbus::fdo::Error::Failed(
            "Unable to read monotonic boot time.".into(),
        ));
    }
    Ok(value.tv_sec.max(0) as u64)
}

fn broker_error(error: BrokerError) -> zbus::fdo::Error {
    zbus::fdo::Error::Failed(format!("Update broker rejected the request: {error:?}"))
}

fn systemd_worker_active(operation_id: &str) -> Result<bool, BrokerError> {
    let output = Command::new("/usr/bin/systemctl")
        .args([
            "show",
            "--property=ActiveState",
            "--value",
            &format!("zethropol-update-worker@{operation_id}.service"),
        ])
        .output()
        .map_err(|error| BrokerError::Io(format!("systemctl show failed: {error}")))?;
    if !output.status.success() {
        return Err(BrokerError::Io(format!(
            "systemctl show exited with {}: {}",
            output.status,
            String::from_utf8_lossy(&output.stderr).trim()
        )));
    }
    match String::from_utf8_lossy(&output.stdout).trim() {
        "active" | "activating" | "deactivating" | "reloading" => Ok(true),
        "inactive" | "failed" | "" => Ok(false),
        other => Err(BrokerError::Io(format!(
            "Unrecognized worker ActiveState: {other}"
        ))),
    }
}

fn reconcile_worker_record(
    store: &BrokerStore,
    operation_id: &str,
    now: u64,
) -> Result<(), BrokerError> {
    store.reconcile_worker_observation(operation_id, systemd_worker_active(operation_id), now)
}

pub(crate) fn reconcile_worker_records(store: &BrokerStore) -> Result<(), BrokerError> {
    let now = boottime_seconds().map_err(|error| BrokerError::Io(error.to_string()))?;
    for operation_id in store.unresolved_operations()? {
        reconcile_worker_record(store, &operation_id, now)?;
    }
    Ok(())
}
fn update_operation_message(state: &Arc<Mutex<ManagerState>>, operation_id: &str, message: String) {
    // Worker and D-Bus broker are separate processes: in-memory state alone
    // is not observable by GetOperationState. Do not alter status or evidence.
    let persisted = BrokerStore::open_runtime()
        .and_then(|store| store.update_worker_progress(operation_id, &message));
    if let Err(error) = persisted {
        eprintln!("update-worker {operation_id}: progress persistence failed: {error:?}");
    }
    if let Some(operation) = state.lock().unwrap().operations.get_mut(operation_id) {
        if operation.status == "running" {
            operation.message = message;
        }
    }
}

fn create_pre_snapshot() -> Result<String, String> {
    verify_snapshot_target()?;
    let output = Command::new("/usr/bin/snapper")
        .args([
            "-c",
            "root",
            "create",
            "--type",
            "pre",
            "--print-number",
            "--description",
            "Zethropol OS system update",
        ])
        .output()
        .map_err(|error| format!("recovery_snapshot_unavailable: {error}"))?;
    if !output.status.success() {
        return Err(format!(
            "recovery_snapshot_failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    let number = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if !valid_snapshot_number(&number) {
        return Err(
            "recovery_snapshot_failed: Snapper returned no valid pre-snapshot ID.".to_string(),
        );
    }
    Ok(number)
}

fn verify_snapshot_target() -> Result<(), String> {
    let output = Command::new("/usr/bin/findmnt")
        .args([
            "--noheadings",
            "--output",
            "FSTYPE,OPTIONS",
            "--target",
            "/.snapshots",
        ])
        .output()
        .map_err(|error| format!("snapshot_target_unavailable: {error}"))?;
    if !output.status.success() {
        return Err(format!(
            "snapshot_target_unavailable: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }

    let output = String::from_utf8_lossy(&output.stdout);
    let mut fields = output.split_whitespace();
    let filesystem = fields.next().unwrap_or_default();
    let options = fields.next().unwrap_or_default();
    validate_snapshot_mount(filesystem, options)
}

fn validate_snapshot_mount(filesystem: &str, options: &str) -> Result<(), String> {
    if filesystem != "btrfs" {
        return Err(format!(
            "snapshot_backend_unsupported: /.snapshots is on {filesystem}, expected btrfs."
        ));
    }
    let options: std::collections::HashSet<_> = options.split(',').collect();
    if options.contains("ro") || !options.contains("rw") {
        return Err(
            "snapshot_target_read_only: /.snapshots is not mounted read-write; refusing the update."
                .to_string(),
        );
    }
    Ok(())
}

fn commit_after_pre_snapshot<T>(
    pre_snapshot: Result<String, String>,
    commit: impl FnOnce(&str) -> T,
) -> Result<(String, T), String> {
    let number = pre_snapshot?;
    let result = commit(&number);
    Ok((number, result))
}

#[cfg(test)]
mod snapshot_preflight_tests {
    use super::*;

    #[test]
    fn read_only_snapshot_mount_blocks_commit() {
        assert!(validate_snapshot_mount("btrfs", "ro,nosuid,nodev").is_err());
        let mut committed = false;
        let result = commit_after_pre_snapshot(Err("snapshot target is read-only".into()), |_| {
            committed = true;
        });

        assert!(result.is_err());
        assert!(!committed);
    }

    #[test]
    fn writable_btrfs_snapshot_mount_is_accepted() {
        assert!(validate_snapshot_mount("btrfs", "rw,relatime,compress=zstd").is_ok());
        assert!(validate_snapshot_mount("ext4", "rw,relatime").is_err());
        assert!(validate_snapshot_mount("btrfs", "nosuid,nodev").is_err());
    }
}

fn create_post_snapshot(pre_number: &str) -> Result<String, String> {
    let output = Command::new("/usr/bin/snapper")
        .args([
            "-c",
            "root",
            "create",
            "--type",
            "post",
            "--pre-number",
            pre_number,
            "--print-number",
            "--description",
            "Zethropol OS system update result",
        ])
        .output()
        .map_err(|error| format!("{error}"))?;
    if !output.status.success() {
        return Err(String::from_utf8_lossy(&output.stderr).trim().to_string());
    }
    let number = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if !valid_snapshot_number(&number) {
        return Err("Snapper returned no valid post-snapshot ID.".to_string());
    }
    Ok(number)
}

fn classify_alpm_error(error: AlpmError) -> &'static str {
    match error {
        AlpmError::HandleLock => "package_manager_locked",
        AlpmError::Retrieve
        | AlpmError::RetrievePrepare
        | AlpmError::ServerNone
        | AlpmError::ServerBadUrl
        | AlpmError::Libcurl
        | AlpmError::ExternalDownload => "network_error",
        AlpmError::PkgInvalidSig
        | AlpmError::PkgMissingSig
        | AlpmError::SigMissing
        | AlpmError::SigInvalid
        | AlpmError::DbInvalidSig
        | AlpmError::Gpgme
        | AlpmError::MissingCapabilitySignatures => "signature_error",
        AlpmError::DiskSpace => "insufficient_disk_space",
        AlpmError::FileConflicts | AlpmError::ConflictingDeps | AlpmError::UnsatisfiedDeps => {
            "transaction_conflict"
        }
        AlpmError::TransHookFailed => "package_hook_failed",
        _ => "package_manager_error",
    }
}

fn configure_update_callbacks(
    handle: &Alpm,
    state: Arc<Mutex<ManagerState>>,
    operation_id: String,
) -> Arc<Mutex<Vec<String>>> {
    let hook_trace = Arc::new(Mutex::new(Vec::new()));
    let event_trace = Arc::clone(&hook_trace);
    let progress_state = Arc::clone(&state);
    let progress_id = operation_id.clone();
    handle.set_progress_cb((), move |progress, package, percent, _, _, _| {
        let phase = format!("{progress:?}").to_lowercase();
        update_operation_message(
            &progress_state,
            &progress_id,
            format!("{phase}: {package} ({percent}%)"),
        );
    });

    let event_state = Arc::clone(&state);
    let event_id = operation_id.clone();
    handle.set_event_cb((), move |event, _| {
        let message = match event.event() {
            Event::PackageOperationStart(operation) => match operation.operation() {
                PackageOperation::Install(package) => format!("Installing {}", package.name()),
                PackageOperation::Upgrade(package, _) => format!("Updating {}", package.name()),
                PackageOperation::Reinstall(package, _) => {
                    format!("Reinstalling {}", package.name())
                }
                PackageOperation::Downgrade(package, _) => {
                    format!("Downgrading {}", package.name())
                }
                PackageOperation::Remove(package) => format!("Removing {}", package.name()),
            },
            Event::RetrieveStart => "Downloading verified repository packages".to_string(),
            Event::IntegrityStart => "Checking package integrity and signatures".to_string(),
            Event::TransactionStart => "Applying the prepared package transaction".to_string(),
            Event::HookRunStart(hook) => {
                observe_hook_lifecycle(&event_trace, hook.name(), false);
                format!("Running package hook: {}", hook.name())
            }
            Event::HookRunDone(hook) => {
                observe_hook_lifecycle(&event_trace, hook.name(), true);
                format!(
                    "Package hook returned (exit status unavailable): {}",
                    hook.name()
                )
            }
            Event::ScriptletInfo(info) => format!("Package script: {}", info.line().trim_end()),
            _ => return,
        };
        eprintln!("update-worker {event_id}: {message}");
        update_operation_message(&event_state, &event_id, message);
    });

    let log_state = Arc::clone(&state);
    handle.set_log_cb((), move |level, message, _| {
        if !level.intersects(alpm::LogLevel::ERROR | alpm::LogLevel::WARNING) {
            return;
        }
        let message = message.trim();
        if !message.is_empty() {
            eprintln!("update-worker {operation_id}: {message}");
            update_operation_message(&log_state, &operation_id, message.to_string());
        }
    });
    hook_trace
}

fn observe_hook_lifecycle(trace: &Mutex<Vec<String>>, name: &str, returned: bool) {
    let mut trace = trace.lock().unwrap();
    if trace.len() >= 512 {
        return;
    }
    let bounded: String = name
        .chars()
        .take(256)
        .map(|character| {
            if character.is_control() {
                '?'
            } else {
                character
            }
        })
        .collect();
    trace.push(format!(
        "{}: {bounded}",
        if returned {
            "returned (exit status unavailable)"
        } else {
            "started"
        }
    ));
}

fn execute_system_upgrade(
    state: &Arc<Mutex<ManagerState>>,
    operation_id: &str,
    store: &BrokerStore,
) -> Result<(String, String), (String, String)> {
    let mut handle = configured_alpm(None)
        .map_err(|error| ("preflight_failed".to_string(), error.to_string()))?;

    update_operation_message(
        state,
        operation_id,
        "Refreshing official repository databases".to_string(),
    );
    handle.syncdbs_mut().update(false).map_err(|error| {
        (
            classify_alpm_error(error).to_string(),
            format!("Unable to refresh official package databases: {error}"),
        )
    })?;

    let hook_trace =
        configure_update_callbacks(&handle, Arc::clone(state), operation_id.to_string());
    update_operation_message(
        state,
        operation_id,
        "Rebuilding and validating the approved transaction".to_string(),
    );
    let resolved_plan = prepare_alpm_transaction(&mut handle)?;
    let approved_plan = match store.claim_transaction(
        operation_id,
        &resolved_plan,
        boottime_seconds().map_err(|error| ("clock_error".to_string(), error.to_string()))?,
    ) {
        Ok(plan) => plan,
        Err(error) => {
            let _ = handle.trans_release();
            return Err((
                if error == BrokerError::PlanChanged {
                    "plan_changed"
                } else {
                    "worker_claim_failed"
                }
                .to_string(),
                format!(
                    "Worker claim rejected while holding the pacman lock: {error:?}. {}",
                    if error == BrokerError::PlanChanged {
                        store
                            .plan_difference(operation_id, &resolved_plan)
                            .unwrap_or_else(|error| {
                                format!("Field comparison unavailable: {error:?}")
                            })
                    } else {
                        String::new()
                    }
                ),
            ));
        }
    };
    if approved_plan.packages().is_empty() {
        let _ = handle.trans_release();
        return Err((
            "no_updates".to_string(),
            "No official updates remain.".to_string(),
        ));
    }

    // Ask libalpm to download and verify every approved official package in a
    // DOWNLOAD_ONLY transaction. This transaction cannot install packages or
    // run package hooks. Reacquire the pacman lock and resolve again afterward.
    handle.trans_release().map_err(|e| {
        (
            "download_preflight_failed".into(),
            format!("Unable to release resolved transaction: {e}"),
        )
    })?;
    let download_plan = prepare_alpm_transaction_with_flags(
        &mut handle,
        TransFlag::DOWNLOAD_ONLY,
        current_policy_fingerprint,
    )?;
    if download_plan != approved_plan {
        let _ = handle.trans_release();
        return Err((
            "plan_changed".into(),
            format!(
                "Before download: {}. Create a new Preview Plan; no automatic retry was made.",
                approved_plan
                    .difference_summary(&download_plan)
                    .unwrap_or_else(|| "canonical plan mismatch".into())
            ),
        ));
    }
    handle.trans_commit().map_err(|e| {
        let _ = handle.trans_release();
        (
            "package_download_failed".into(),
            format!("libalpm download-only verification failed: {e}"),
        )
    })?;
    handle.trans_release().map_err(|e| {
        (
            "download_preflight_failed".into(),
            format!("Unable to release download transaction: {e}"),
        )
    })?;

    // Refresh and resolve from the current signed repository database. Another
    // pacman process may have run during download; the new transaction lock and
    // full canonical comparison make that a clean refusal.
    handle.syncdbs_mut().update(false).map_err(|e| {
        (
            "repository_refresh_failed".into(),
            format!("Unable to refresh repositories after download: {e}"),
        )
    })?;
    let final_plan = prepare_alpm_transaction(&mut handle)?;
    if final_plan != approved_plan {
        let _ = handle.trans_release();
        return Err((
            "plan_changed".into(),
            format!(
                "Before commit: {}. Create a new Preview Plan; no package commit was started.",
                approved_plan
                    .difference_summary(&final_plan)
                    .unwrap_or_else(|| "canonical plan mismatch".into())
            ),
        ));
    }
    verify_cached_archives(&handle, &final_plan).map_err(|e| {
        let _ = handle.trans_release();
        ("archive_verification_failed".into(), e)
    })?;
    let current_plan = final_plan.packages().to_vec();

    update_operation_message(
        state,
        operation_id,
        "Creating the required Snapper pre-update recovery point".to_string(),
    );
    update_operation_message(
        state,
        operation_id,
        "Verifying the Snapper target and creating the required pre-update snapshot".to_string(),
    );
    // Resolve snapshot ownership before creating a recovery point or committing.
    // This never edits the host hook configuration. Unsupported policies fail
    // closed, so additional snap-pac recovery guarantees cannot be lost.
    let hook_dirs = pacman_conf_values(&["HookDir"])
        .map_err(|error| ("snapshot_policy_unavailable".into(), error.to_string()))?;
    if let Err(error) = crate::snapshot_policy::validate(&current_plan, &hook_dirs) {
        let _ = handle.trans_release();
        return Err(("snapshot_policy_unsupported".into(), error));
    }
    let mut evidence = ExecutionEvidence::default();
    let (pre_snapshot, commit) = match commit_after_pre_snapshot(create_pre_snapshot(), |number| {
        evidence.pre_snapshot = Some(number.to_string());
        verify_cached_archives(&handle, &approved_plan).map_err(|error| {
            format!(
                "Pre-snapshot {number}; package content or policy changed before commit: {error}"
            )
        })?;
        evidence.package_commit = CommitState::InProgress;
        // No package commit if its start cannot be recorded durably. A crash
        // after this checkpoint leaves an unknown/possibly partial outcome.
        store
            .record_execution_evidence(operation_id, &evidence)
            .map_err(|error| {
                format!("Pre-snapshot {number}; unable to record commit start: {error:?}")
            })?;
        update_operation_message(
            state,
            operation_id,
            format!("Applying {} repository package updates", current_plan.len()),
        );
        Ok::<_, String>(handle.trans_commit())
    }) {
        Ok((number, Ok(commit))) => (number, commit),
        Ok((_, Err(error))) => {
            let _ = handle.trans_release();
            return Err(("result_record_failed".into(), error));
        }
        Err(error) => {
            let _ = handle.trans_release();
            return Err(("recovery_snapshot_failed".into(), error));
        }
    };
    evidence.hook_details = format!(
        "libalpm {}: hook lifecycle events are not exit-status evidence.\n{}",
        alpm::version(),
        hook_trace.lock().unwrap().join("\n")
    );
    record_commit_outcome(
        &mut evidence,
        commit.as_ref().err().map(|error| error.error()),
    );
    // Do not infer post-hook/boot success from this return value. Post hooks
    // may fail without failing libalpm commit. They remain explicitly unknown.
    let checkpoint_error = store
        .record_execution_evidence(operation_id, &evidence)
        .err();
    // Inspect only local artifacts while this transaction still owns the
    // pacman lock. These checks never invoke package scripts, builds or boot.
    evidence.boot = crate::boot_verifier::verify(&handle, &current_plan);
    evidence.boot_preparation = crate::boot_verifier::preparation_state(&evidence.boot);
    eprintln!("update-worker {operation_id}: {}", evidence.boot.details);
    let artifact_checkpoint_error = store
        .record_execution_evidence(operation_id, &evidence)
        .err();
    let release = handle.trans_release();

    let commit_error = commit.err().map(|error| {
        let code = classify_alpm_error(error.error()).to_string();
        (code, error.to_string())
    });
    update_operation_message(
        state,
        operation_id,
        "Recording the post-transaction recovery point".to_string(),
    );
    let post_snapshot = create_post_snapshot(&pre_snapshot);
    if let Ok(number) = &post_snapshot {
        evidence.post_snapshot = Some(number.clone());
    }
    let final_checkpoint_error = store
        .record_execution_evidence(operation_id, &evidence)
        .err();
    if let Some(error) = final_checkpoint_error
        .or(artifact_checkpoint_error)
        .or(checkpoint_error)
    {
        return Err(("result_record_failed".into(), format!("Package outcome {:?}; unable to persist execution evidence: {error:?}. Do not retry automatically.", evidence.package_commit)));
    }

    let release_error = release
        .err()
        .map(|error| format!("Unable to release the package transaction lock: {error}"));

    if let Some((code, detail)) = commit_error {
        let recovery = match post_snapshot {
            Ok(number) => format!(" Pre-snapshot {pre_snapshot}; post-snapshot {number}. No automatic rollback was attempted."),
            Err(snapshot_error) => format!(" Pre-snapshot {pre_snapshot} remains; post-snapshot failed: {snapshot_error}. No automatic rollback was attempted."),
        };
        let release = release_error
            .map(|error| format!(" {error}."))
            .unwrap_or_default();
        return Err((code, format!("{detail}.{recovery}{release}")));
    }

    if let Some(error) = release_error {
        return Err(("package_manager_error".to_string(), format!(
            "{error}. Pre-snapshot {pre_snapshot}; post-snapshot {}. The package transaction may have completed; inspect pacman logs before retrying.",
            post_snapshot.as_ref().map(String::as_str).unwrap_or("failed")
        )));
    }

    // Only reached after successful commit, durable post-check checkpoints,
    // post-snapshot attempt and successful transaction lock release. The
    // cleanup gate still rejects every unverified/uncertain postcondition.
    evidence.cleanup = crate::cache_cleanup::after_success(&evidence, || {
        configured_alpm(None).map_err(|error| error.to_string())
    });
    let cleanup_checkpoint = store.record_execution_evidence(operation_id, &evidence);
    let cleanup_summary = format!(
        " Cache cleanup: {}; removed files: {}; removed bytes: {}; observed free-space increase: {} bytes. {}{}",
        evidence.cleanup.status, evidence.cleanup.removed_files, evidence.cleanup.removed_bytes,
        evidence.cleanup.reclaimed_bytes, evidence.cleanup.details,
        cleanup_checkpoint.err().map(|e| format!(" Cleanup result persistence warning: {e:?}.")).unwrap_or_default()
    );
    eprintln!("update-worker {operation_id}: {cleanup_summary}");

    match post_snapshot {
        Ok(number) => Ok(("ok_with_warnings".to_string(), format!(
            "Package commit completed. Hook success and a successful new kernel boot are unverified; see the separate artifact checks. Snapper pre/post snapshots: {pre_snapshot}/{number}. This does not guarantee boot recovery.{cleanup_summary}"
        ))),
        Err(error) => Ok(("ok_with_warnings".to_string(), format!(
            "Package commit completed, but the post-snapshot failed: {error}. Hook success and boot preparation are unverified. Pre-snapshot {pre_snapshot} remains. Recovery is not guaranteed.{cleanup_summary}"
        ))),
    }
}

fn record_commit_outcome(evidence: &mut ExecutionEvidence, error: Option<AlpmError>) {
    evidence.package_commit = if error.is_none() {
        CommitState::Committed
    } else {
        CommitState::Failed
    };
    // This explicit libalpm failure is evidence of a hook failure. An Ok
    // commit is NOT evidence of hook success; neither are generic error logs.
    if error == Some(AlpmError::TransHookFailed) {
        evidence.hooks = VerificationState::Failed;
    }
}

fn valid_snapshot_number(value: &str) -> bool {
    value
        .parse::<u32>()
        .is_ok_and(|number| number > 0 && number.to_string() == value)
}

#[cfg(test)]
fn finish_operation(
    state: &Arc<Mutex<ManagerState>>,
    operation_id: &str,
    result: Result<(String, String), (String, String)>,
) {
    let mut state = state.lock().unwrap();
    if let Some(operation) = state.operations.get_mut(operation_id) {
        match result {
            Ok((status, message)) => {
                operation.status = status;
                operation.message = message;
            }
            Err((code, message)) => {
                operation.status = "failed".to_string();
                operation.message = format!("{code}: {message}");
            }
        }
    }
    if state.active_operation.as_deref() == Some(operation_id) {
        state.active_operation = None;
        state.active_plan.clear();
    }
}

fn pacman_conf_values(args: &[&str]) -> Result<Vec<String>, zbus::fdo::Error> {
    let output = Command::new("/usr/bin/pacman-conf")
        .args(args)
        .output()
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!("Unable to read pacman configuration: {error}"))
        })?;

    if !output.status.success() || !output.stderr.is_empty() {
        return Err(zbus::fdo::Error::Failed(format!(
            "Unable to verify pacman configuration for {:?}: {}",
            args,
            String::from_utf8_lossy(&output.stderr).trim()
        )));
    }

    Ok(String::from_utf8_lossy(&output.stdout)
        .lines()
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_string)
        .collect())
}

fn parse_pacman_siglevel(
    values: &[String],
    fallback: alpm::SigLevel,
) -> Result<alpm::SigLevel, zbus::fdo::Error> {
    use alpm::SigLevel;

    if values.is_empty() {
        return Ok(fallback);
    }

    let mut flags = SigLevel::NONE;
    for value in values {
        match value.as_str() {
            "PackageRequired" => {
                flags.remove(SigLevel::PACKAGE_OPTIONAL);
                flags.insert(SigLevel::PACKAGE);
            }
            "PackageOptional" => {
                flags.insert(SigLevel::PACKAGE | SigLevel::PACKAGE_OPTIONAL);
            }
            "PackageMarginalOK" => flags.insert(SigLevel::PACKAGE_MARGINAL_OK),
            "PackageUnknownOK" => flags.insert(SigLevel::PACKAGE_UNKNOWN_OK),
            "DatabaseRequired" => {
                flags.remove(SigLevel::DATABASE_OPTIONAL);
                flags.insert(SigLevel::DATABASE);
            }
            "DatabaseOptional" => {
                flags.insert(SigLevel::DATABASE | SigLevel::DATABASE_OPTIONAL);
            }
            "DatabaseMarginalOK" => flags.insert(SigLevel::DATABASE_MARGINAL_OK),
            "DatabaseUnknownOK" => flags.insert(SigLevel::DATABASE_UNKNOWN_OK),
            "PackageTrustedOnly" => {
                flags.remove(SigLevel::PACKAGE_MARGINAL_OK | SigLevel::PACKAGE_UNKNOWN_OK);
            }
            "DatabaseTrustedOnly" => {
                flags.remove(SigLevel::DATABASE_MARGINAL_OK | SigLevel::DATABASE_UNKNOWN_OK);
            }
            other => {
                return Err(zbus::fdo::Error::Failed(format!(
                    "Unsupported pacman SigLevel value: {other}"
                )))
            }
        }
    }
    Ok(flags)
}

struct TemporaryPlanDatabase {
    path: PathBuf,
}

impl TemporaryPlanDatabase {
    fn new(system_db_path: &str) -> Result<Self, zbus::fdo::Error> {
        let system_local =
            fs::canonicalize(Path::new(system_db_path).join("local")).map_err(|error| {
                zbus::fdo::Error::Failed(format!(
                    "The local pacman package database is unavailable for preview: {error}"
                ))
            })?;

        let path =
            std::env::temp_dir().join(format!("zethropol-update-plan-{}", uuid::Uuid::new_v4()));
        fs::create_dir(&path).map_err(|error| {
            zbus::fdo::Error::Failed(format!("Unable to create a private plan database: {error}"))
        })?;
        fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).map_err(|error| {
            let _ = fs::remove_dir_all(&path);
            zbus::fdo::Error::Failed(format!("Unable to secure the plan database: {error}"))
        })?;
        fs::create_dir(path.join("sync")).map_err(|error| {
            let _ = fs::remove_dir_all(&path);
            zbus::fdo::Error::Failed(format!("Unable to create temporary sync database: {error}"))
        })?;
        symlink(system_local, path.join("local")).map_err(|error| {
            let _ = fs::remove_dir_all(&path);
            zbus::fdo::Error::Failed(format!("Unable to read the local pacman database: {error}"))
        })?;

        Ok(Self { path })
    }

    fn path(&self) -> Result<&str, zbus::fdo::Error> {
        self.path.to_str().ok_or_else(|| {
            zbus::fdo::Error::Failed(
                "Temporary package database path is not valid UTF-8.".to_string(),
            )
        })
    }
}

impl Drop for TemporaryPlanDatabase {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

fn configured_alpm(db_path_override: Option<&str>) -> Result<Alpm, zbus::fdo::Error> {
    configured_alpm_domain(db_path_override, false)
}

fn configured_alpm_domain(
    db_path_override: Option<&str>,
    archive_domain: bool,
) -> Result<Alpm, zbus::fdo::Error> {
    let root_dirs = pacman_conf_values(&["RootDir"])?;
    let db_paths = pacman_conf_values(&["DBPath"])?;
    let gpg_dirs = pacman_conf_values(&["GPGDir"])?;
    let architectures = pacman_conf_values(&["Architecture"])?;
    let cache_dirs = pacman_conf_values(&["CacheDir"])?;
    let repositories = pacman_conf_values(&["--repo-list"])?;
    let transfer_commands = pacman_conf_values(&["XferCommand"])?;

    if root_dirs.len() != 1
        || root_dirs[0] != "/"
        || db_paths.len() != 1
        || gpg_dirs.len() != 1
        || architectures.is_empty()
        || cache_dirs.is_empty()
        || repositories.is_empty()
    {
        return Err(zbus::fdo::Error::Failed(
            "Pacman configuration is incomplete; transaction planning and execution refused."
                .to_string(),
        ));
    }
    if !transfer_commands.is_empty() {
        return Err(zbus::fdo::Error::Failed(
            "Custom XferCommand is not supported by the libalpm update executor; refusing to use a different download path.".to_string(),
        ));
    }

    let db_path = db_path_override.unwrap_or(&db_paths[0]);
    let mut handle = Alpm::new(root_dirs[0].clone(), db_path.to_string()).map_err(|error| {
        zbus::fdo::Error::Failed(format!("Unable to initialize libalpm: {error}"))
    })?;

    let architecture_refs: Vec<&str> = architectures.iter().map(String::as_str).collect();
    handle
        .set_architectures(architecture_refs.into_iter())
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!(
                "Unable to configure libalpm architectures: {error}"
            ))
        })?;

    let cache_refs: Vec<&str> = cache_dirs.iter().map(String::as_str).collect();
    handle
        .set_cachedirs(cache_refs.into_iter())
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!(
                "Unable to configure libalpm cache directories: {error}"
            ))
        })?;

    handle
        .set_gpgdir(if archive_domain {
            AUR_NATIVE_DOMAIN.to_string()
        } else {
            gpg_dirs[0].clone()
        })
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!(
                "Unable to configure libalpm GPG directory: {error}"
            ))
        })?;

    let default_siglevel = parse_pacman_siglevel(
        &pacman_conf_values(&["SigLevel"])?,
        SigLevel::PACKAGE | SigLevel::DATABASE_OPTIONAL,
    )?;
    handle
        .set_default_siglevel(default_siglevel)
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!(
                "Unable to configure libalpm signature policy: {error}"
            ))
        })?;

    let local_siglevel = parse_pacman_siglevel(
        &pacman_conf_values(&["LocalFileSigLevel"])?,
        default_siglevel,
    )?;
    handle
        .set_local_file_siglevel(local_siglevel)
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!(
                "Unable to configure local-file signature policy: {error}"
            ))
        })?;

    let remote_values = pacman_conf_values(&["RemoteFileSigLevel"])?;
    if !remote_values.is_empty() {
        let remote_siglevel = parse_pacman_siglevel(&remote_values, default_siglevel)?;
        handle
            .set_remote_file_siglevel(remote_siglevel)
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!(
                    "Unable to configure remote-file signature policy: {error}"
                ))
            })?;
    }
    let hook_dirs = pacman_conf_values(&["HookDir"])?;
    if !hook_dirs.is_empty() {
        handle
            .set_hookdirs(hook_dirs.iter().map(String::as_str))
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!(
                    "Unable to configure libalpm hook directories: {error}"
                ))
            })?;
    }

    let no_upgrade = pacman_conf_values(&["NoUpgrade"])?;
    if !no_upgrade.is_empty() {
        handle
            .set_noupgrades(no_upgrade.iter().map(String::as_str))
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!("Unable to configure NoUpgrade rules: {error}"))
            })?;
    }

    let no_extract = pacman_conf_values(&["NoExtract"])?;
    if !no_extract.is_empty() {
        handle
            .set_noextracts(no_extract.iter().map(String::as_str))
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!("Unable to configure NoExtract rules: {error}"))
            })?;
    }

    let check_space = pacman_conf_values(&["CheckSpace"])?;
    handle.set_check_space(check_space.iter().any(|value| {
        value.eq_ignore_ascii_case("true") || value.eq_ignore_ascii_case("yes") || value == "1"
    }));

    let parallel_downloads = pacman_conf_values(&["ParallelDownloads"])?;
    if !parallel_downloads.is_empty() {
        let count = parallel_downloads[0].parse::<u32>().map_err(|_| {
            zbus::fdo::Error::Failed(
                "Invalid ParallelDownloads value in pacman configuration.".to_string(),
            )
        })?;
        handle.set_parallel_downloads(count);
    }

    if db_path_override.is_none() {
        let log_files = pacman_conf_values(&["LogFile"])?;
        if log_files.len() == 1 {
            handle.set_logfile(log_files[0].clone()).map_err(|error| {
                zbus::fdo::Error::Failed(format!("Unable to configure libalpm logfile: {error}"))
            })?;
        } else if log_files.len() > 1 {
            return Err(zbus::fdo::Error::Failed(
                "Multiple LogFile values found; transaction preview refused.".to_string(),
            ));
        }
    }

    let ignored_packages = pacman_conf_values(&["IgnorePkg"])?;
    if !ignored_packages.is_empty() {
        let refs: Vec<&str> = ignored_packages.iter().map(String::as_str).collect();
        handle.set_ignorepkgs(refs.into_iter()).map_err(|error| {
            zbus::fdo::Error::Failed(format!("Unable to configure ignored packages: {error}"))
        })?;
    }

    let ignored_groups = pacman_conf_values(&["IgnoreGroup"])?;
    if !ignored_groups.is_empty() {
        let refs: Vec<&str> = ignored_groups.iter().map(String::as_str).collect();
        handle.set_ignoregroups(refs.into_iter()).map_err(|error| {
            zbus::fdo::Error::Failed(format!("Unable to configure ignored groups: {error}"))
        })?;
    }

    if archive_domain {
        handle
            .set_default_siglevel(SigLevel::PACKAGE)
            .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?;
        handle
            .set_local_file_siglevel(SigLevel::PACKAGE)
            .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?;
        return Ok(handle); // No official repositories in the Archive native domain.
    }
    for repository in repositories {
        let servers = pacman_conf_values(&["--repo", repository.as_str(), "Server"])?;
        if servers.is_empty() {
            return Err(zbus::fdo::Error::Failed(format!(
                "Repository {repository} has no verified server configuration."
            )));
        }

        let repo_siglevel = parse_pacman_siglevel(
            &pacman_conf_values(&["--repo", repository.as_str(), "SigLevel"])?,
            default_siglevel,
        )?;
        let usage_values = pacman_conf_values(&["--repo", repository.as_str(), "Usage"])?;
        let usage = if usage_values.is_empty() {
            alpm::Usage::ALL
        } else {
            let mut usage = alpm::Usage::NONE;
            for value in usage_values
                .iter()
                .flat_map(|value| value.split_whitespace())
            {
                match value {
                    "All" => usage = alpm::Usage::ALL,
                    "Sync" => usage.insert(alpm::Usage::SYNC),
                    "Search" => usage.insert(alpm::Usage::SEARCH),
                    "Install" => usage.insert(alpm::Usage::INSTALL),
                    "Upgrade" => usage.insert(alpm::Usage::UPGRADE),
                    other => {
                        return Err(zbus::fdo::Error::Failed(format!(
                            "Unsupported Usage value for repository {repository}: {other}"
                        )))
                    }
                }
            }
            usage
        };

        let db = handle
            .register_syncdb_mut(repository.clone(), repo_siglevel)
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!(
                    "Unable to register repository {repository}: {error}"
                ))
            })?;
        db.set_usage(usage).map_err(|error| {
            zbus::fdo::Error::Failed(format!(
                "Unable to configure Usage for repository {repository}: {error}"
            ))
        })?;
        for server in servers {
            db.add_server(server).map_err(|error| {
                zbus::fdo::Error::Failed(format!(
                    "Unable to configure repository {repository}: {error}"
                ))
            })?;
        }
    }

    Ok(handle)
}

fn prepare_alpm_transaction(handle: &mut Alpm) -> Result<CanonicalPlan, (String, String)> {
    prepare_alpm_transaction_with_policy(handle, current_policy_fingerprint)
}

fn current_policy_fingerprint() -> Result<String, (String, String)> {
    let hooks = pacman_conf_values(&["HookDir"])
        .map_err(|e| ("policy_unavailable".into(), e.to_string()))?;
    let gpg_dirs = pacman_conf_values(&["GPGDir"])
        .map_err(|e| ("policy_unavailable".into(), e.to_string()))?;
    let mut roots = vec![
        PathBuf::from("/etc/pacman.conf"),
        PathBuf::from("/etc/pacman.d"),
    ];
    roots.extend(hooks.into_iter().map(PathBuf::from));
    roots.extend(gpg_dirs.into_iter().map(PathBuf::from));
    let config = Command::new("/usr/bin/pacman-conf")
        .arg("--verbose")
        .output()
        .map_err(|e| {
            (
                "policy_unavailable".into(),
                format!("Cannot obtain effective pacman configuration: {e}"),
            )
        })?;
    if !config.status.success() {
        return Err((
            "policy_unavailable".into(),
            "pacman-conf could not resolve effective package policy".into(),
        ));
    }
    policy_fingerprint_with_values(&roots, &config.stdout)
        .map_err(|e| ("policy_unavailable".into(), e))
}

fn verify_cached_archives(handle: &Alpm, plan: &CanonicalPlan) -> Result<(), String> {
    if plan.policy_fingerprint().len() != 64 {
        return Err("approved package-policy fingerprint is missing".into());
    }
    let current_policy = current_policy_fingerprint().map_err(|e| e.1)?;
    if current_policy != plan.policy_fingerprint() {
        return Err("pacman configuration or local ALPM hook content changed after preview".into());
    }
    let sync_dir = Path::new(handle.dbpath()).join("sync");
    let mut repo_hashes = HashMap::<String, String>::new();
    let cache_dirs: Vec<PathBuf> = handle.cachedirs().iter().map(PathBuf::from).collect();
    for package in plan.packages() {
        if package.filename.is_empty()
            || package.sha256.len() != 64
            || package.repository_metadata_sha256.len() != 64
        {
            return Err(format!(
                "approved content identity is incomplete for {}",
                package.package_name
            ));
        }
        let repo_hash = if let Some(value) = repo_hashes.get(&package.repository) {
            value.clone()
        } else {
            let value = repository_database_hash(&sync_dir, &package.repository)?;
            repo_hashes.insert(package.repository.clone(), value.clone());
            value
        };
        if repo_hash != package.repository_metadata_sha256 {
            return Err(format!(
                "signed repository metadata changed for {}",
                package.repository
            ));
        }
        let mut found = false;
        for cache in &cache_dirs {
            let path = cache.join(&package.filename);
            match fs::symlink_metadata(&path) {
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => continue,
                Err(error) => {
                    return Err(format!(
                        "cannot inspect cached archive {}: {error}",
                        path.display()
                    ))
                }
                Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_file() => {
                    return Err(format!(
                        "cached archive is not a regular file: {}",
                        path.display()
                    ))
                }
                Ok(metadata) => {
                    let cache_meta = fs::symlink_metadata(cache).map_err(|e| {
                        format!("cannot inspect cache directory {}: {e}", cache.display())
                    })?;
                    if !cache_meta.is_dir()
                        || cache_meta.uid() != 0
                        || cache_meta.permissions().mode() & 0o022 != 0
                        || metadata.uid() != 0
                        || metadata.permissions().mode() & 0o022 != 0
                    {
                        return Err(format!(
                            "package cache/archive is writable by an unprivileged user: {}",
                            path.display()
                        ));
                    }
                    found = true;
                    if package.download_size != 0 && metadata.len() != package.download_size {
                        return Err(format!(
                            "archive size differs from signed metadata for {}",
                            package.package_name
                        ));
                    }
                    verify_archive(&path, &package.sha256)?;
                    let signature_policy = handle
                        .syncdbs()
                        .iter()
                        .find(|db| db.name() == package.repository)
                        .ok_or_else(|| {
                            format!(
                                "approved repository {} is no longer registered",
                                package.repository
                            )
                        })?
                        .siglevel()
                        & (SigLevel::PACKAGE
                            | SigLevel::PACKAGE_OPTIONAL
                            | SigLevel::PACKAGE_MARGINAL_OK
                            | SigLevel::PACKAGE_UNKNOWN_OK
                            | SigLevel::USE_DEFAULT);
                    let loaded = handle
                        .pkg_load(
                            path.as_os_str().as_encoded_bytes().to_vec(),
                            true,
                            signature_policy,
                        )
                        .map_err(|error| {
                            format!(
                                "libalpm archive/signature verification failed for {}: {error}",
                                package.package_name
                            )
                        })?;
                    let archive = loaded.pkg();
                    verify_loaded_archive_identity(
                        archive.name(),
                        archive.version().as_str(),
                        archive.filename(),
                        &path,
                        package,
                    )?;
                }
            }
        }
        if !found {
            return Err(format!(
                "approved package archive was not downloaded: {}",
                package.filename
            ));
        }
    }
    Ok(())
}

fn verify_loaded_archive_identity(
    name: &str,
    version: &str,
    loaded_filename: Option<&str>,
    path: &Path,
    expected: &UpdatePackageInfo,
) -> Result<(), String> {
    // alpm_pkg_load preserves the complete filename passed to it; sync-db
    // metadata contains a basename. Compare each against its own contract.
    if name != expected.package_name
        || version != expected.new_version
        || loaded_filename.map(Path::new) != Some(path)
        || path.file_name().and_then(|name| name.to_str()) != Some(expected.filename.as_str())
    {
        return Err(format!(
            "archive identity differs from approved metadata for {}",
            expected.package_name
        ));
    }
    Ok(())
}

fn prepare_alpm_transaction_with_policy<F>(
    handle: &mut Alpm,
    policy_snapshot: F,
) -> Result<CanonicalPlan, (String, String)>
where
    F: FnOnce() -> Result<String, (String, String)>,
{
    prepare_alpm_transaction_with_flags(handle, TransFlag::NONE, policy_snapshot)
}

fn prepare_alpm_transaction_with_flags<F>(
    handle: &mut Alpm,
    flags: TransFlag,
    policy_snapshot: F,
) -> Result<CanonicalPlan, (String, String)>
where
    F: FnOnce() -> Result<String, (String, String)>,
{
    // Answers only resolve metadata here. No commit is possible before the
    // complete plan is authenticated, explicitly approved and re-compared.
    let decisions = Arc::new(Mutex::new((Vec::<PackageRemoval>::new(), None::<String>)));
    handle.set_question_cb(Arc::clone(&decisions), |mut question, decisions| {
        let mut decisions = decisions.lock().unwrap();
        match question.question() {
            Question::Replace(replace) => {
                let old = replace.oldpkg();
                let new = replace.newpkg();
                if crate::broker_protocol::critical_removal(old.name()) || old.files().files().iter().any(|f| {
                    std::str::from_utf8(f.name()).map(critical_removal_file).unwrap_or(true)
                }) {
                    decisions.1 = Some(format!("critical_removal: {}", old.name()));
                    replace.set_replace(false);
                } else {
                    decisions.0.push(PackageRemoval {
                        package_name: old.name().into(), current_version: old.version().as_str().into(),
                        reason: "replacement".into(), replacement: new.name().into(),
                    });
                    replace.set_replace(true);
                }
            }
            Question::Conflict(conflict) => {
                let c = conflict.conflict();
                decisions.1 = Some(format!("conflict_removal: {} conflicts with {} ({}); conflict-driven removals are unsupported", c.package1().name(), c.package2().name(), c.reason()));
                question.set_answer(false);
            }
            Question::RemovePkgs(remove) => {
                let names = remove.packages().iter().map(|p| p.name().to_string()).collect::<Vec<_>>().join(", ");
                decisions.1 = Some(format!("unresolved_dependencies: {names}; dropping unresolved transaction targets is forbidden (this libalpm question is not evidence of installed package removal)"));
                question.set_answer(false);
            }
            Question::SelectProvider(mut provider) => {
                decisions.1 = Some("ambiguous_provider: explicit provider selection is unsupported".into());
                provider.set_index(-1);
            },
            _ => question.set_answer(false),
        }
    });
    handle.trans_init(flags).map_err(|error| {
        (
            classify_alpm_error(error).to_string(),
            format!("Unable to initialize the package transaction: {error}"),
        )
    })?;

    let upgrade = handle.sync_sysupgrade(false);
    if let Some(reason) = decisions.lock().unwrap().1.clone() {
        return Err(("unsafe_removal".into(), reason));
    }
    upgrade.map_err(|error| {
        (
            classify_alpm_error(error).to_string(),
            format!("Unable to plan system upgrade: {error}"),
        )
    })?;
    let prepared = handle.trans_prepare();
    if let Some(reason) = decisions.lock().unwrap().1.clone() {
        return Err(("unsafe_removal".into(), reason));
    }
    prepared.map_err(|error| {
        (
            classify_alpm_error(error.error()).to_string(),
            // alpm 5.0.2 PrepareError::Debug dereferences PkgInvalidArch
            // strings as Package pointers. Display formats only the errno;
            // never inspect the binding's incorrectly typed diagnostic list.
            format!("libalpm rejected the update transaction: {error}"),
        )
    })?;

    let removals = verified_removal_edges(
        handle
            .trans_remove()
            .iter()
            .map(|p| (p.name().to_string(), p.version().as_str().to_string()))
            .collect(),
        decisions.lock().unwrap().0.clone(),
    )?;

    let mut planned = Vec::new();
    let mut repository_hashes: HashMap<String, String> = HashMap::new();
    let sync_dir = Path::new(handle.dbpath()).join("sync");
    for package in handle.trans_add().iter() {
        if package.origin() != alpm::PackageFrom::SyncDb {
            return Err((
                "preflight_failed".to_string(),
                format!(
                    "Unexpected package origin for {}; transaction refused.",
                    package.name()
                ),
            ));
        }
        let repository = package.db().ok_or_else(|| {
            (
                "preflight_failed".to_string(),
                format!(
                    "No repository identity is available for {}; transaction refused.",
                    package.name()
                ),
            )
        })?;
        repository.is_valid().map_err(|error| (
            "repository_signature_invalid".into(),
            format!("Repository metadata for {} is not valid under the configured pacman signature policy: {error}", repository.name()),
        ))?;
        let current_version = match handle.localdb().pkg(package.name()) {
            Ok(installed) => installed.version().as_str().to_string(),
            Err(AlpmError::PkgNotFound) => "not installed (new dependency)".to_string(),
            Err(error) => {
                return Err((
                    "preflight_failed".to_string(),
                    format!(
                        "Unable to read installed version for {}: {error}",
                        package.name()
                    ),
                ));
            }
        };
        // Canonical identity uses the signed archive size, not libalpm's
        // remaining transfer size (which becomes zero when the cache fills).
        let download_size = u64::try_from(package.size()).map_err(|_| {
            (
                "preflight_failed".to_string(),
                format!("Invalid signed archive size for {}.", package.name()),
            )
        })?;
        let filename = package
            .filename()
            .ok_or_else(|| {
                (
                    "integrity_metadata_missing".to_string(),
                    format!("No trusted archive filename for {}.", package.name()),
                )
            })?
            .to_string();
        if Path::new(&filename).file_name().and_then(|n| n.to_str()) != Some(filename.as_str()) {
            return Err((
                "integrity_metadata_invalid".into(),
                format!("Unsafe repository filename for {}.", package.name()),
            ));
        }
        let sha256 = package
            .sha256sum()
            .ok_or_else(|| {
                (
                    "integrity_metadata_missing".to_string(),
                    format!(
                        "No trusted repository SHA-256 for {}; refusing unverifiable content.",
                        package.name()
                    ),
                )
            })?
            .to_ascii_lowercase();
        if sha256.len() != 64 || !sha256.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err((
                "integrity_metadata_invalid".into(),
                format!("Invalid trusted SHA-256 for {}.", package.name()),
            ));
        }
        let repository_metadata_sha256 =
            if let Some(value) = repository_hashes.get(repository.name()) {
                value.clone()
            } else {
                let value = repository_database_hash(&sync_dir, repository.name())
                    .map_err(|error| ("repository_metadata_unavailable".into(), error))?;
                repository_hashes.insert(repository.name().to_string(), value.clone());
                value
            };
        planned.push(UpdatePackageInfo {
            package_name: package.name().to_string(),
            current_version,
            new_version: package.version().as_str().to_string(),
            repository: repository.name().to_string(),
            download_size,
            filename,
            sha256,
            repository_metadata_sha256,
        });
    }
    let policy = policy_snapshot()?;
    CanonicalPlan::from_transaction(planned, removals)
        .and_then(|plan| plan.with_policy_fingerprint(policy))
        .map_err(|e| {
            (
                "unsafe_removal".into(),
                format!("Ambiguous or unsafe replacement plan: {e:?}"),
            )
        })
}

fn critical_removal_file(path: &str) -> bool {
    path.starts_with("boot/")
        || path.starts_with("efi/")
        || path.starts_with("usr/lib/modules/")
        || path.starts_with("usr/share/limine/")
        || path.starts_with("usr/lib/systemd/system/zethropol-update")
}

fn verified_removal_edges(
    actual: Vec<(String, String)>,
    mut edges: Vec<PackageRemoval>,
) -> Result<Vec<PackageRemoval>, (String, String)> {
    // Deduplicate repeated identical questions, but never choose between two
    // different replacements for the same installed package.
    edges.sort();
    edges.dedup();
    let mut expected: Vec<_> = edges
        .iter()
        .map(|r| (r.package_name.clone(), r.current_version.clone()))
        .collect();
    let mut actual = actual;
    expected.sort();
    actual.sort();
    if expected != actual {
        return Err((
            "ambiguous_removal".into(),
            "libalpm removal set differs from its explicit replacement decisions".into(),
        ));
    }
    Ok(edges)
}

// Informational candidates only: never initialize a transaction against the
// installed database. PreviewPlan independently resolves and authenticates all
// effects (including replacements/dependencies) under the real pacman lock.
fn ensure_scan_database_idle(db_path: &Path) -> Result<(), zbus::fdo::Error> {
    match fs::symlink_metadata(db_path.join("db.lck")) {
        Ok(_) => Err(zbus::fdo::Error::Failed(
            "package_manager_busy: The package database is locked. Wait for the package operation to finish, then check again. The lock has not been changed.".into(),
        )),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(zbus::fdo::Error::Failed(format!(
            "package_database_unavailable: Cannot inspect the package database lock: {error}"
        ))),
    }
}

fn scan_database_fingerprint(db_path: &Path) -> Result<String, zbus::fdo::Error> {
    ensure_scan_database_idle(db_path)?;
    policy_fingerprint_with_values(&[db_path.join("local")], &[])
        .map_err(|error| zbus::fdo::Error::Failed(format!(
            "package_database_changed: Cannot read a stable installed database; retry after package operations finish: {error}"
        )))
}

fn list_upgrade_candidates(handle: &Alpm) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
    let mut repositories = Vec::new();
    for db in handle.syncdbs().iter() {
        let usage = db.usage().map_err(|error| {
            zbus::fdo::Error::Failed(format!("Unable to read repository usage: {error}"))
        })?;
        if usage.contains(alpm::Usage::UPGRADE) {
            repositories.push(db);
        }
    }
    let mut packages = Vec::new();
    for installed in handle.localdb().pkgs().iter() {
        if installed.should_ignore() {
            continue;
        }
        if let Some(candidate) = installed.sync_new_version(repositories.iter().copied()) {
            if candidate.should_ignore() {
                continue;
            }
            packages.push(UpdatePackageInfo {
                package_name: candidate.name().into(),
                current_version: installed.version().as_str().into(),
                new_version: candidate.version().as_str().into(),
                repository: candidate.db().unwrap().name().into(),
                download_size: u64::try_from(candidate.size()).map_err(|_| {
                    zbus::fdo::Error::Failed("Invalid repository package size".into())
                })?,
                filename: candidate.filename().unwrap_or_default().into(),
                sha256: candidate.sha256sum().unwrap_or_default().into(),
                // Listing is not a CanonicalPlan or an installation grant.
                repository_metadata_sha256: String::new(),
            });
        }
    }
    Ok(packages)
}

fn scan_official_update_candidates() -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
    let paths = pacman_conf_values(&["DBPath"])?;
    if paths.len() != 1 {
        return Err(zbus::fdo::Error::Failed(
            "Pacman DBPath is ambiguous; scan refused.".into(),
        ));
    }
    let db_path = Path::new(&paths[0]);
    ensure_scan_database_idle(db_path)?;
    let temporary_db = TemporaryPlanDatabase::new(&paths[0])?;
    let mut handle = configured_alpm(Some(temporary_db.path()?))?;
    handle.syncdbs_mut().update(false).map_err(|error| {
        zbus::fdo::Error::Failed(format!(
            "{}: Unable to refresh official package databases: {error}",
            classify_alpm_error(error)
        ))
    })?;
    let before = scan_database_fingerprint(db_path)?;
    let packages = list_upgrade_candidates(&handle)?;
    let after = scan_database_fingerprint(db_path)?;
    ensure_scan_database_idle(db_path)?;
    if before != after {
        return Err(zbus::fdo::Error::Failed("package_database_changed: Installed packages changed during the scan. Wait for package operations to finish and retry.".into()));
    }
    Ok(packages)
}

fn resolved_transaction_preflight() -> Result<CanonicalPlan, zbus::fdo::Error> {
    let db_paths = pacman_conf_values(&["DBPath"])?;
    if db_paths.len() != 1 {
        return Err(zbus::fdo::Error::Failed(
            "Pacman DBPath is ambiguous; transaction planning refused.".to_string(),
        ));
    }

    // Refresh only a private sync database. Its local/ symlink reads the
    // installed package database but it never changes /var/lib/pacman/sync.
    let temporary_db = TemporaryPlanDatabase::new(&db_paths[0])?;
    let mut planning_handle = configured_alpm(Some(temporary_db.path()?))?;
    planning_handle
        .syncdbs_mut()
        .update(false)
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!(
                "{}: Unable to refresh official package databases: {error}",
                classify_alpm_error(error)
            ))
        })?;

    // Take pacman's real lock while the temporary handle reads local/ and
    // resolves dependencies, so a concurrent package transaction cannot alter
    // the installed database halfway through the preview.
    let mut system_lock = configured_alpm(None)?;
    system_lock.trans_init(TransFlag::NONE).map_err(|error| {
        zbus::fdo::Error::Failed(format!(
            "{}: Unable to lock the installed package database for preview: {error}",
            classify_alpm_error(error)
        ))
    })?;

    let plan_result = prepare_alpm_transaction(&mut planning_handle);
    let plan_release = planning_handle.trans_release();
    let lock_release = system_lock.trans_release();
    match (plan_result, plan_release, lock_release) {
        (Ok(plan), Ok(()), Ok(())) => Ok(plan),
        (Err((code, message)), _, _) => Err(zbus::fdo::Error::Failed(format!("{code}: {message}"))),
        (Ok(_), Err(error), _) => Err(zbus::fdo::Error::Failed(format!(
            "Unable to release temporary libalpm preview transaction: {error}"
        ))),
        (Ok(_), Ok(()), Err(error)) => Err(zbus::fdo::Error::Failed(format!(
            "Unable to release pacman database lock after preview: {error}"
        ))),
    }
}

fn cached_plan_is_fresh(plan: &CachedPlan, now: Instant) -> bool {
    now.duration_since(plan.created_at) <= PLAN_CACHE_TTL
}

fn cache_or_resolve_plan<F>(
    state: &Arc<Mutex<ManagerState>>,
    now: Instant,
    resolve: F,
) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error>
where
    F: FnOnce() -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error>,
{
    if let Some(plan) = state.lock().unwrap().pending_plan.as_ref() {
        if cached_plan_is_fresh(plan, now) {
            return Ok(plan.packages.clone());
        }
    }

    let packages = resolve()?;
    let mut state = state.lock().unwrap();
    if state.active_operation.is_some() {
        return Ok(state.active_plan.clone());
    }
    state.pending_plan = Some(CachedPlan {
        packages: packages.clone(),
        created_at: Instant::now().max(now),
    });
    Ok(packages)
}

#[cfg(test)]
mod plan_cache_tests {
    use super::*;
    use std::io::{BufRead, BufReader};
    use std::process::{Child, Stdio};

    struct PrivateBus(Child);

    impl Drop for PrivateBus {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }

    fn packages(count: usize) -> Vec<UpdatePackageInfo> {
        (0..count)
            .map(|index| UpdatePackageInfo {
                package_name: format!("pkg-{index}"),
                current_version: "1.0-1".to_string(),
                new_version: "1.1-1".to_string(),
                repository: "core".to_string(),
                download_size: 1024,
                filename: format!("pkg-{index}.pkg.tar.zst"),
                sha256: "a".repeat(64),
                repository_metadata_sha256: "b".repeat(64),
            })
            .collect()
    }

    #[test]
    #[ignore = "explicit host scan: downloads repository metadata to a private temporary database only"]
    fn host_fresh_read_only_scan() {
        let packages = scan_official_update_candidates().unwrap();
        println!(
            "Fresh read-only host scan: {} upgrade candidates",
            packages.len()
        );
    }

    #[test]
    #[ignore = "explicit read-only host check using existing sync metadata; no refresh or transaction"]
    fn host_cached_read_only_scan() {
        let paths = pacman_conf_values(&["DBPath"]).unwrap();
        let path = Path::new(&paths[0]);
        let before = scan_database_fingerprint(path).unwrap();
        let handle = configured_alpm(None).unwrap();
        let packages = list_upgrade_candidates(&handle).unwrap();
        assert_eq!(before, scan_database_fingerprint(path).unwrap());
        ensure_scan_database_idle(path).unwrap();
        println!("Read-only host scan: {} upgrade candidates", packages.len());
    }

    #[test]
    fn read_only_scan_lists_versions_without_transaction_or_system_lock() {
        let temp =
            std::env::temp_dir().join(format!("zethropol-scan-test-{}", uuid::Uuid::new_v4()));
        fs::create_dir_all(temp.join("local/demo-1-1")).unwrap();
        fs::create_dir_all(temp.join("sync")).unwrap();
        fs::create_dir_all(temp.join("repo/demo-2-1")).unwrap();
        fs::write(temp.join("local/ALPM_DB_VERSION"), "9\n").unwrap();
        fs::write(
            temp.join("local/demo-1-1/desc"),
            "%NAME%\ndemo\n\n%VERSION%\n1-1\n\n%ARCH%\nx86_64\n",
        )
        .unwrap();
        fs::write(temp.join("repo/demo-2-1/desc"), "%NAME%\ndemo\n\n%VERSION%\n2-1\n\n%ARCH%\nx86_64\n\n%FILENAME%\ndemo.pkg.tar.zst\n\n%CSIZE%\n42\n").unwrap();
        assert!(Command::new("/usr/bin/bsdtar")
            .arg("-cf")
            .arg(temp.join("sync/core.db"))
            .arg("-C")
            .arg(temp.join("repo"))
            .arg("demo-2-1")
            .status()
            .unwrap()
            .success());
        let mut handle = Alpm::new("/", temp.to_str().unwrap()).unwrap();
        handle.register_syncdb("core", SigLevel::NONE).unwrap();
        let before = scan_database_fingerprint(&temp).unwrap();
        let packages = list_upgrade_candidates(&handle).unwrap();
        assert_eq!(packages.len(), 1);
        assert_eq!(packages[0].current_version, "1-1");
        assert_eq!(packages[0].new_version, "2-1");
        assert_eq!(packages[0].download_size, 42);
        handle.set_ignorepkgs(["demo"].into_iter()).unwrap();
        assert!(list_upgrade_candidates(&handle).unwrap().is_empty());
        handle.set_ignorepkgs(std::iter::empty::<&str>()).unwrap();
        handle
            .syncdbs()
            .iter()
            .next()
            .unwrap()
            .set_usage(alpm::Usage::SYNC)
            .unwrap();
        assert!(list_upgrade_candidates(&handle).unwrap().is_empty());
        assert!(!temp.join("db.lck").exists());
        // An active lock is refused and preserved, even though listing itself
        // requires no transaction. No installation grant is created.
        fs::write(temp.join("db.lck"), "test-owner").unwrap();
        assert!(scan_database_fingerprint(&temp)
            .unwrap_err()
            .to_string()
            .contains("package_manager_busy"));
        assert_eq!(
            fs::read_to_string(temp.join("db.lck")).unwrap(),
            "test-owner"
        );
        // Cleanup only this test-owned lock in the isolated fixture.
        fs::remove_file(temp.join("db.lck")).unwrap();
        fs::write(temp.join("local/demo-1-1/desc"), "changed").unwrap();
        assert_ne!(before, scan_database_fingerprint(&temp).unwrap());
        drop(handle);
        fs::remove_dir_all(temp).unwrap();
    }

    #[test]
    fn pending_list_and_preview_share_one_nine_package_snapshot() {
        let state = Arc::new(Mutex::new(ManagerState::default()));
        let now = Instant::now();
        let pending = cache_or_resolve_plan(&state, now, || Ok(packages(9))).unwrap();
        // Simulate the old split source: a stale local database would have made
        // the preview empty. A fresh cached plan must be reused for the preview.
        let preview = cache_or_resolve_plan(&state, Instant::now(), || Ok(Vec::new())).unwrap();

        assert_eq!(pending.len(), 9);
        assert_eq!(preview, pending);
        assert_eq!(preview.len(), 9);
    }

    #[test]
    fn stale_pending_snapshot_is_resolved_again() {
        let state = Arc::new(Mutex::new(ManagerState::default()));
        let now = Instant::now();
        let first = cache_or_resolve_plan(&state, now, || Ok(packages(9))).unwrap();
        state.lock().unwrap().pending_plan = Some(CachedPlan {
            packages: first.clone(),
            created_at: now - PLAN_CACHE_TTL - Duration::from_secs(1),
        });
        let refreshed = cache_or_resolve_plan(&state, Instant::now(), || Ok(packages(2))).unwrap();

        assert_eq!(first.len(), 9);
        assert_eq!(refreshed.len(), 2);
    }

    #[test]
    fn planning_database_is_private_and_uses_read_only_installed_metadata() {
        let system_db =
            std::env::temp_dir().join(format!("zethropol-update-test-db-{}", uuid::Uuid::new_v4()));
        fs::create_dir_all(system_db.join("local")).unwrap();

        let temporary = TemporaryPlanDatabase::new(system_db.to_str().unwrap()).unwrap();
        let permissions = fs::metadata(&temporary.path).unwrap().permissions();
        assert_eq!(permissions.mode() & 0o777, 0o700);
        assert_eq!(
            fs::read_link(temporary.path.join("local")).unwrap(),
            fs::canonicalize(system_db.join("local")).unwrap()
        );
        let temporary_path = temporary.path.clone();
        drop(temporary);
        assert!(!temporary_path.exists());

        fs::remove_dir_all(system_db).unwrap();
    }

    /// Uses the installed libalpm resolver with synthetic metadata only. All
    /// locks/databases are under /tmp; no archive download or commit is run.
    fn fixture_resolution(
        old_name: &str,
        extra_metadata: &str,
        dependency: &str,
    ) -> Result<CanonicalPlan, (String, String)> {
        let temp = std::env::temp_dir().join(format!(
            "zethropol-replacement-fixture-{}",
            uuid::Uuid::new_v4()
        ));
        struct Cleanup(PathBuf);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                let _ = fs::remove_dir_all(&self.0);
            }
        }
        fs::create_dir_all(temp.join("db/local")).unwrap();
        let _cleanup = Cleanup(temp.clone());
        fs::create_dir_all(temp.join("root")).unwrap();
        fs::create_dir_all(temp.join("db/sync")).unwrap();
        fs::write(temp.join("db/local/ALPM_DB_VERSION"), "9\n").unwrap();
        let installed = temp.join(format!("db/local/{old_name}-1-1"));
        fs::create_dir_all(&installed).unwrap();
        fs::write(installed.join("desc"), format!("%NAME%\n{old_name}\n\n%VERSION%\n1-1\n\n%ARCH%\nx86_64\n\n%DESC%\nfixture\n\n%SIZE%\n1\n\n%REASON%\n0\n")).unwrap();
        fs::write(
            installed.join("files"),
            if old_name == "custom-kernel" {
                "%FILES%\nusr/lib/modules/custom/vmlinuz\n"
            } else {
                "%FILES%\n"
            },
        )
        .unwrap();
        if extra_metadata.contains("other-demo") {
            let other = temp.join("db/local/other-demo-1-1");
            fs::create_dir_all(&other).unwrap();
            fs::write(
                other.join("desc"),
                "%NAME%\nother-demo\n\n%VERSION%\n1-1\n\n%ARCH%\nx86_64\n",
            )
            .unwrap();
            fs::write(other.join("files"), "%FILES%\n").unwrap();
        }
        let incoming = temp.join("repo/new-demo-2-1");
        fs::create_dir_all(&incoming).unwrap();
        fs::write(incoming.join("desc"), format!("%NAME%\nnew-demo\n\n%VERSION%\n2-1\n\n%BASE%\nnew-demo\n\n%FILENAME%\nnew-demo-2-1-x86_64.pkg.tar.zst\n\n%SHA256SUM%\n{}\n\n%ARCH%\nx86_64\n\n%CSIZE%\n1\n\n%ISIZE%\n1\n\n%DESC%\nfixture\n\n%REPLACES%\n{old_name}\n\n{extra_metadata}{dependency}", "a".repeat(64))).unwrap();
        assert!(Command::new("/usr/bin/bsdtar")
            .arg("-cf")
            .arg(temp.join("db/sync/core.db"))
            .arg("-C")
            .arg(temp.join("repo"))
            .arg("new-demo-2-1")
            .status()
            .unwrap()
            .success());
        let mut handle = Alpm::new(
            temp.join("root").to_str().unwrap(),
            temp.join("db").to_str().unwrap(),
        )
        .unwrap();
        handle
            .set_architectures(["x86_64"].iter().copied())
            .unwrap();
        handle.register_syncdb("core", SigLevel::NONE).unwrap();
        let result = prepare_alpm_transaction_with_policy(&mut handle, || Ok("b".repeat(64)));
        let _ = handle.trans_release();
        result
    }

    #[test]
    fn canonical_plan_is_unchanged_by_partial_or_completed_cache_download() {
        let temp = std::env::temp_dir().join(format!(
            "zethropol-plan-cache-size-{}",
            uuid::Uuid::new_v4()
        ));
        struct Cleanup(PathBuf);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                let _ = fs::remove_dir_all(&self.0);
            }
        }
        let _cleanup = Cleanup(temp.clone());
        for path in [
            "root",
            "db/local/demo-1-1",
            "db/sync",
            "repo/demo-2-1",
            "cache",
        ] {
            fs::create_dir_all(temp.join(path)).unwrap();
        }
        fs::write(temp.join("db/local/ALPM_DB_VERSION"), "9\n").unwrap();
        fs::write(
            temp.join("db/local/demo-1-1/desc"),
            "%NAME%\ndemo\n\n%VERSION%\n1-1\n\n%ARCH%\nx86_64\n",
        )
        .unwrap();
        fs::write(temp.join("db/local/demo-1-1/files"), "%FILES%\n").unwrap();
        fs::write(temp.join("repo/demo-2-1/desc"),format!("%NAME%\ndemo\n\n%VERSION%\n2-1\n\n%ARCH%\nx86_64\n\n%FILENAME%\ndemo-2-1-x86_64.pkg.tar.zst\n\n%CSIZE%\n100\n\n%ISIZE%\n1\n\n%SHA256SUM%\n{}\n", "a".repeat(64))).unwrap();
        assert!(Command::new("/usr/bin/bsdtar")
            .arg("-cf")
            .arg(temp.join("db/sync/core.db"))
            .arg("-C")
            .arg(temp.join("repo"))
            .arg("demo-2-1")
            .status()
            .unwrap()
            .success());
        let resolve = || {
            let mut handle = Alpm::new(
                temp.join("root").to_str().unwrap(),
                temp.join("db").to_str().unwrap(),
            )
            .unwrap();
            handle.set_architectures(["x86_64"].into_iter()).unwrap();
            handle
                .set_cachedirs([temp.join("cache").to_str().unwrap()].into_iter())
                .unwrap();
            // Synthetic metadata only. No network, package commit or host DB.
            handle.register_syncdb("core", SigLevel::NONE).unwrap();
            let plan =
                prepare_alpm_transaction_with_policy(&mut handle, || Ok("b".repeat(64))).unwrap();
            let transfer = handle.trans_add().iter().next().unwrap().download_size();
            handle.trans_release().unwrap();
            (plan, transfer)
        };
        let (before, transfer_before) = resolve();
        assert_eq!(transfer_before, 100);
        fs::write(
            temp.join("cache/demo-2-1-x86_64.pkg.tar.zst.part"),
            vec![0u8; 30],
        )
        .unwrap();
        let (partial, transfer_partial) = resolve();
        assert_eq!(transfer_partial, 70);
        // Existence is enough for libalpm's transfer estimator; this dummy
        // archive is NEVER committed and still fails the real checksum guard.
        let cache = temp.join("cache/demo-2-1-x86_64.pkg.tar.zst");
        fs::write(&cache, vec![0u8; 100]).unwrap();
        let (after, transfer_after) = resolve();
        assert_eq!(transfer_after, 0);
        assert_eq!(before, partial);
        assert_eq!(before, after);
        assert_eq!(before.hash(), after.hash());
        assert_eq!(after.packages()[0].download_size, 100);
        assert!(verify_archive(&cache, &"a".repeat(64)).is_err());
        let mut buggy_fields = after.packages().to_vec();
        buggy_fields[0].download_size = transfer_after as u64;
        let buggy = CanonicalPlan::from_packages(buggy_fields)
            .unwrap()
            .with_policy_fingerprint("b".repeat(64))
            .unwrap();
        assert_ne!(before, buggy);
        assert!(before
            .difference_summary(&buggy)
            .unwrap()
            .contains("archive_size"));
    }

    #[test]
    fn actual_libalpm_replacement_is_a_versioned_directed_removal() {
        let plan = fixture_resolution(
            "old-demo",
            "%CONFLICTS%\nold-demo\n\n%PROVIDES%\nold-demo=1-1\n\n",
            "",
        )
        .unwrap();
        assert_eq!(plan.packages().len(), 1);
        assert_eq!(plan.packages()[0].package_name, "new-demo");
        assert_eq!(plan.packages()[0].sha256, "a".repeat(64));
        assert_eq!(plan.policy_fingerprint(), "b".repeat(64));
        assert_eq!(plan.packages()[0].repository_metadata_sha256.len(), 64);
        assert_eq!(
            plan.removals(),
            &[PackageRemoval {
                package_name: "old-demo".into(),
                current_version: "1-1".into(),
                reason: "replacement".into(),
                replacement: "new-demo".into()
            }]
        );
    }

    #[test]
    fn actual_libalpm_critical_replacement_and_unresolved_dependency_are_blocked() {
        assert!(fixture_resolution("custom-kernel", "", "")
            .unwrap_err()
            .1
            .contains("critical_removal"));
        assert!(fixture_resolution("pacman", "", "")
            .unwrap_err()
            .1
            .contains("critical_removal"));
        assert!(fixture_resolution(
            "old-demo",
            "",
            "%DEPENDS%\nnonexistent-fixture-dependency\n\n"
        )
        .is_err());
    }

    #[test]
    fn actual_libalpm_unrelated_conflict_is_not_accepted_as_replacement() {
        let error = fixture_resolution("old-demo", "%CONFLICTS%\nother-demo\n\n", "").unwrap_err();
        assert!(error.1.contains("conflict_removal"), "{error:?}");
        assert!(critical_removal_file("usr/lib/modules/custom/vmlinuz"));
        assert!(critical_removal_file("boot/EFI/custom/loader.efi"));
        assert!(!critical_removal_file("usr/share/doc/demo/readme"));
    }

    #[test]
    fn undeclared_or_ambiguous_removal_edges_are_rejected() {
        let edge = PackageRemoval {
            package_name: "old".into(),
            current_version: "1".into(),
            reason: "replacement".into(),
            replacement: "new".into(),
        };
        assert!(verified_removal_edges(vec![("old".into(), "1".into())], vec![]).is_err());
        assert!(verified_removal_edges(vec![], vec![edge.clone()]).is_err());
        let second = PackageRemoval {
            replacement: "another".into(),
            ..edge.clone()
        };
        assert!(
            verified_removal_edges(vec![("old".into(), "1".into())], vec![edge, second]).is_err()
        );
    }

    #[tokio::test]
    async fn private_dbus_exposes_the_same_nine_package_plan_to_state_list_and_preview() {
        let child = Command::new("dbus-daemon")
            .args(["--session", "--nofork", "--nopidfile", "--print-address=1"])
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let mut bus = PrivateBus(child);
        let stdout = bus.0.stdout.take().unwrap();
        let mut address = String::new();
        BufReader::new(stdout).read_line(&mut address).unwrap();
        assert!(
            !address.trim().is_empty(),
            "private D-Bus did not publish an address"
        );

        let pending = packages(9);
        let manager = UpdateManager {
            state: Arc::new(Mutex::new(ManagerState {
                pending_plan: Some(CachedPlan {
                    packages: pending.clone(),
                    created_at: Instant::now(),
                }),
                ..ManagerState::default()
            })),
            plan_refresh_lock: Mutex::new(()),
            broker: None,
            test_authentication: true,
        };
        let service = zbus::connection::Builder::address(address.trim())
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

        let introspection = zbus::Proxy::new(
            &client,
            "org.zethropol.Update.Test",
            "/org/zethropol/Update",
            "org.freedesktop.DBus.Introspectable",
        )
        .await
        .unwrap();
        let xml: String = introspection.call("Introspect", &()).await.unwrap();
        let plan_method = xml
            .split("<method name=\"PreviewPlan\">")
            .nth(1)
            .unwrap()
            .split("</method>")
            .next()
            .unwrap();
        assert!(plan_method.contains("type=\"a(sssstsss)\""));
        assert!(plan_method.contains("type=\"a(ssss)\""));
        assert_eq!(plan_method.matches("direction=\"out\"").count(), 3);
        let trigger = xml
            .split("<method name=\"TriggerUpdate\">")
            .nth(1)
            .unwrap()
            .split("</method>")
            .next()
            .unwrap();
        assert_eq!(trigger.matches("direction=\"in\"").count(), 2);
        let state: UpdateState = proxy.call("GetUpdateState", &()).await.unwrap();
        let listed: Vec<UpdatePackageInfo> = proxy.call("ListPendingUpdates", &()).await.unwrap();
        let previewed: Vec<UpdatePackageInfo> =
            proxy.call("PreviewTransaction", &()).await.unwrap();
        assert_eq!(state.pending_updates_count, 9);
        assert_eq!(listed, pending);
        assert_eq!(previewed, listed);

        drop(proxy);
        drop(client);
        drop(service);
        drop(bus);
    }
}

#[cfg(test)]
mod transaction_tests {
    use super::*;

    fn package() -> UpdatePackageInfo {
        UpdatePackageInfo {
            package_name: "sample".to_string(),
            current_version: "1.0-1".to_string(),
            new_version: "1.1-1".to_string(),
            repository: "core".to_string(),
            download_size: 1234,
            filename: "sample.pkg.tar.zst".into(),
            sha256: "a".repeat(64),
            repository_metadata_sha256: "b".repeat(64),
        }
    }

    #[test]
    fn loaded_archive_uses_full_path_while_approved_metadata_uses_basename() {
        let temp = std::env::temp_dir().join(format!(
            "zethropol-archive-identity-{}",
            uuid::Uuid::new_v4()
        ));
        fs::create_dir_all(temp.join("db/local")).unwrap();
        fs::create_dir_all(temp.join("payload")).unwrap();
        fs::write(
            temp.join("payload/.PKGINFO"),
            "pkgname = sample\npkgver = 1.1-1\npkgdesc = test\narch = any\nsize = 1\n",
        )
        .unwrap();
        let expected = package();
        let path = temp.join(&expected.filename);
        assert!(Command::new("/usr/bin/bsdtar")
            .arg("-cf")
            .arg(&path)
            .arg("-C")
            .arg(temp.join("payload"))
            .arg(".PKGINFO")
            .status()
            .unwrap()
            .success());
        let handle = Alpm::new("/", temp.join("db").to_str().unwrap()).unwrap();
        // Unsigned synthetic fixture only; production signature/hash checks
        // still run before this identity check and are unchanged.
        let loaded = handle
            .pkg_load(
                path.as_os_str().as_encoded_bytes().to_vec(),
                true,
                SigLevel::NONE,
            )
            .unwrap();
        let archive = loaded.pkg();
        assert_eq!(archive.filename(), path.to_str());
        assert_ne!(archive.filename(), Some(expected.filename.as_str()));
        assert!(verify_loaded_archive_identity(
            archive.name(),
            archive.version().as_str(),
            archive.filename(),
            &path,
            &expected
        )
        .is_ok());
        for (name, version, filename) in [
            ("other", "1.1-1", path.to_str()),
            ("sample", "2-1", path.to_str()),
            ("sample", "1.1-1", None),
            ("sample", "1.1-1", Some("/different/path")),
        ] {
            assert!(
                verify_loaded_archive_identity(name, version, filename, &path, &expected).is_err()
            );
        }
        assert!(verify_loaded_archive_identity(
            "sample",
            "1.1-1",
            path.to_str(),
            &path,
            &UpdatePackageInfo {
                filename: "different.pkg.tar.zst".into(),
                ..expected
            }
        )
        .is_err());
        drop(loaded);
        drop(handle);
        fs::remove_dir_all(temp).unwrap();
    }

    #[test]
    fn post_update_reboot_requirement_never_uses_cleanup_or_running_state_as_proof() {
        let mut evidence = ExecutionEvidence::default();
        assert_eq!(post_update_reboot_requirement(true, &evidence).0, "unknown");
        evidence.package_commit = CommitState::Committed;
        evidence.cleanup.status = "completed".into();
        assert_eq!(post_update_reboot_requirement(true, &evidence).0, "unknown");
        evidence.boot.updated_kernels = "6.20.1-test".into();
        evidence.boot.kernels = VerificationState::Verified;
        evidence.boot_preparation = VerificationState::Verified;
        assert_eq!(
            post_update_reboot_requirement(false, &evidence).0,
            "unknown"
        );
        assert_eq!(
            post_update_reboot_requirement(true, &evidence).0,
            "required"
        );
        evidence.cleanup.status = "skipped-unverified".into();
        assert_eq!(
            post_update_reboot_requirement(true, &evidence).0,
            "required"
        );
        evidence.boot.new_kernel_boot = VerificationState::Verified;
        assert_eq!(
            post_update_reboot_requirement(true, &evidence).0,
            "not-required"
        );
    }

    #[test]
    fn preview_grant_is_single_use_and_expires() {
        let now = Instant::now();
        let mut state = ManagerState::default();
        state.previews.insert(
            ":1.42".to_string(),
            PreviewGrant {
                packages: vec![package()],
                created_at: now,
            },
        );

        assert_eq!(
            consume_preview_grant(&mut state, ":1.42", now).unwrap(),
            vec![package()]
        );
        assert!(consume_preview_grant(&mut state, ":1.42", now).is_err());

        state.previews.insert(
            ":1.43".to_string(),
            PreviewGrant {
                packages: vec![package()],
                created_at: now - Duration::from_secs(301),
            },
        );
        assert!(consume_preview_grant(&mut state, ":1.43", now)
            .unwrap_err()
            .contains("expired"));
    }

    #[test]
    fn update_plan_must_match_the_approved_official_plan_exactly() {
        let current = vec![package()];
        assert!(plan_matches_approved(&current, &[package()]));

        let other = UpdatePackageInfo {
            package_name: "other".to_string(),
            current_version: "1.0-1".to_string(),
            new_version: "2.0-1".to_string(),
            repository: "extra".to_string(),
            download_size: 200,
            filename: String::new(),
            sha256: String::new(),
            repository_metadata_sha256: String::new(),
        };
        let reversed_current = vec![other.clone(), package()];
        assert!(plan_matches_approved(
            &reversed_current,
            &[other, package()]
        ));

        let mut changed_version = package();
        changed_version.new_version = "1.2-1".to_string();
        assert!(!plan_matches_approved(
            std::slice::from_ref(&changed_version),
            &[package()]
        ));
        let mut changed_repo = package();
        changed_repo.repository = "third-party".to_string();
        assert!(!plan_matches_approved(
            std::slice::from_ref(&changed_repo),
            &[package()]
        ));
        let mut changed_installed_version = package();
        changed_installed_version.current_version = "0.9-1".to_string();
        assert!(!plan_matches_approved(
            std::slice::from_ref(&changed_installed_version),
            &[package()]
        ));
        let mut changed_download_size = package();
        changed_download_size.download_size += 1;
        assert!(!plan_matches_approved(
            std::slice::from_ref(&changed_download_size),
            &[package()]
        ));
    }

    #[test]
    fn only_one_update_operation_can_be_reserved() {
        let mut state = ManagerState::default();
        reserve_operation(&mut state, "op-1", vec![package()]).unwrap();
        assert!(reserve_operation(&mut state, "op-2", vec![package()]).is_err());
        assert_eq!(state.active_operation.as_deref(), Some("op-1"));
    }

    #[test]
    fn finishing_an_operation_releases_the_single_flight_guard() {
        let state = Arc::new(Mutex::new(ManagerState::default()));
        reserve_operation(&mut state.lock().unwrap(), "op-1", vec![package()]).unwrap();

        finish_operation(
            &state,
            "op-1",
            Ok((
                "ok_with_warnings".to_string(),
                "post-snapshot failed".to_string(),
            )),
        );

        let mut manager_state = state.lock().unwrap();
        assert_eq!(manager_state.operations["op-1"].status, "ok_with_warnings");
        assert!(manager_state.active_operation.is_none());
        assert!(manager_state.active_plan.is_empty());
        reserve_operation(&mut manager_state, "op-2", vec![package()]).unwrap();
    }

    #[test]
    fn alpm_failures_are_reported_by_safety_category() {
        assert_eq!(
            classify_alpm_error(AlpmError::HandleLock),
            "package_manager_locked"
        );
        assert_eq!(classify_alpm_error(AlpmError::Retrieve), "network_error");
        assert_eq!(
            classify_alpm_error(AlpmError::PkgInvalidSig),
            "signature_error"
        );
        assert_eq!(
            classify_alpm_error(AlpmError::DiskSpace),
            "insufficient_disk_space"
        );
        assert_eq!(
            classify_alpm_error(AlpmError::FileConflicts),
            "transaction_conflict"
        );
    }
}

#[cfg(test)]
mod execution_boundary_tests {
    use super::*;
    use crate::broker_protocol::OperationView;

    #[test]
    fn hook_return_event_is_not_exit_status_or_success() {
        let trace = Mutex::new(Vec::new());
        observe_hook_lifecycle(&trace, "fixture.hook", false);
        observe_hook_lifecycle(&trace, "fixture.hook", true);
        assert_eq!(
            trace.lock().unwrap().as_slice(),
            [
                "started: fixture.hook",
                "returned (exit status unavailable): fixture.hook"
            ]
        );
        assert_eq!(
            ExecutionEvidence::default().hooks,
            VerificationState::Unknown
        );
    }

    #[test]
    fn verified_local_artifacts_do_not_certify_a_new_kernel_boot() {
        let evidence = ExecutionEvidence {
            package_commit: CommitState::Committed,
            hooks: VerificationState::Verified,
            boot_preparation: VerificationState::Verified,
            boot: crate::execution_evidence::BootVerification {
                kernels: VerificationState::Verified,
                dkms: VerificationState::Verified,
                initramfs: VerificationState::Verified,
                limine: VerificationState::Verified,
                running_kernel: "old".into(),
                installed_kernels: "new".into(),
                ..Default::default()
            },
            ..Default::default()
        };
        let reply = insert_operation_view(
            HashMap::new(),
            OperationView {
                operation_id: "fixture".into(),
                status: OperationStatus::Succeeded,
                message: "finished".into(),
                evidence,
            },
        )
        .unwrap();
        assert_eq!(reply["status"], "ok_with_warnings");
        assert_eq!(reply["kernel_artifacts"], "verified");
        assert_eq!(reply["new_kernel_boot"], "unknown");
        assert_eq!(reply["running_kernel"], "old");
        assert_eq!(reply["installed_kernels"], "new");
    }

    #[test]
    fn only_an_explicit_hook_error_is_failure_evidence() {
        let mut evidence = ExecutionEvidence::default();
        record_commit_outcome(&mut evidence, Some(AlpmError::TransHookFailed));
        assert_eq!(evidence.hooks, VerificationState::Failed);
        assert_eq!(evidence.package_commit, CommitState::Failed);
        let mut other = ExecutionEvidence::default();
        record_commit_outcome(&mut other, Some(AlpmError::Retrieve));
        assert_eq!(other.hooks, VerificationState::Unknown);
        let mut successful_commit = ExecutionEvidence::default();
        record_commit_outcome(&mut successful_commit, None);
        assert_eq!(successful_commit.package_commit, CommitState::Committed);
        assert_eq!(successful_commit.hooks, VerificationState::Unknown);
        assert_eq!(
            successful_commit.boot_preparation,
            VerificationState::Unknown
        );
    }

    #[test]
    fn snapshot_ids_must_be_positive_canonical_numbers() {
        assert!(valid_snapshot_number("301"));
        for number in ["0", "-1", "+1", "01", "snapshot 301", "4294967296"] {
            assert!(!valid_snapshot_number(number), "{number}");
        }
    }

    #[test]
    fn unobserved_hooks_and_boot_are_warnings_on_the_wire() {
        for status in [
            OperationStatus::Succeeded,
            OperationStatus::CompletedWithWarnings,
        ] {
            let view = OperationView {
                operation_id: "fixture".into(),
                status,
                message: "finished".into(),
                evidence: ExecutionEvidence {
                    package_commit: CommitState::Committed,
                    ..Default::default()
                },
            };
            let reply = insert_operation_view(HashMap::new(), view).unwrap();
            assert_eq!(reply["status"], "ok_with_warnings");
            assert_eq!(reply["package_commit"], "committed");
            assert_eq!(reply["hooks"], "unknown");
            assert_eq!(reply["boot_preparation"], "unknown");
        }
    }

    #[test]
    fn worker_profile_is_scoped_and_does_not_own_shared_record_lifetime() {
        let unit = include_str!("../deployment/systemd/zethropol-update-worker@.service");
        for setting in [
            "ProtectSystem=no",
            "ReadWritePaths=\n",
            "Environment=SNAP_PAC_SKIP=1",
            "NoNewPrivileges=yes",
            "Restart=no",
            "ProtectHome=yes",
            "ProtectControlGroups=yes",
            "ProtectKernelTunables=no",
            "RestrictSUIDSGID=no",
        ] {
            assert!(unit.contains(setting), "{setting}");
        }
        assert!(!unit.contains("RuntimeDirectory="));
        assert!(
            !include_str!("../deployment/systemd/zethropol-update.service")
                .contains("SNAP_PAC_SKIP")
        );
    }
}
include!("aur/adapter.rs");
