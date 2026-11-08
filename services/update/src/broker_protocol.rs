// D-Bus identity, authorization, and persistent broker transaction store.

// inherited module dead_code allowance

use crate::execution_evidence::{
    BootVerification, CommitState, ExecutionEvidence, VerificationState,
};
use crate::models::{PackageRemoval, UpdatePackageInfo};
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;
use uuid::Uuid;

const RECORD_VERSION: &str = "ZETHROPOL-UPDATE-BROKER-6";
const V5_RECORD_VERSION: &str = "ZETHROPOL-UPDATE-BROKER-5";
const V4_RECORD_VERSION: &str = "ZETHROPOL-UPDATE-BROKER-4";
const V3_RECORD_VERSION: &str = "ZETHROPOL-UPDATE-BROKER-3";
const PREVIOUS_RECORD_VERSION: &str = "ZETHROPOL-UPDATE-BROKER-2";
const LEGACY_RECORD_VERSION: &str = "ZETHROPOL-UPDATE-BROKER-1";
const PREVIEW_TTL_SECS: u64 = 300;
const AUTHORIZATION_MAX_AGE_SECS: u64 = 60;
const MAX_RECORD_BYTES: u64 = 8 * 1024 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct VerifiedIdentity {
    bus_name: String,
    uid: u32,
    session_id: String,
}

impl VerifiedIdentity {
    /// Resolve identity from the actual message header and trusted system
    /// services. No method accepts a caller-supplied sender name or UID.
    pub(crate) async fn from_header(
        connection: &zbus::Connection,
        header: &zbus::message::Header<'_>,
    ) -> Result<Self, BrokerError> {
        let bus_name = header
            .sender()
            .ok_or(BrokerError::InvalidIdentity)?
            .to_string();
        Self::from_sender(connection, &bus_name).await
    }

    // Production callers can reach this only through the received header.
    async fn from_sender(
        connection: &zbus::Connection,
        bus_name: &str,
    ) -> Result<Self, BrokerError> {
        if !bus_name.starts_with(':') {
            return Err(BrokerError::InvalidIdentity);
        }
        let bus = zbus::Proxy::new(
            connection,
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
        )
        .await
        .map_err(|error| BrokerError::IdentityLookup(error.to_string()))?;
        let uid: u32 = bus
            .call("GetConnectionUnixUser", &(bus_name,))
            .await
            .map_err(|error| BrokerError::IdentityLookup(error.to_string()))?;
        let pid: u32 = bus
            .call("GetConnectionUnixProcessID", &(bus_name,))
            .await
            .map_err(|error| BrokerError::IdentityLookup(error.to_string()))?;

        let session_id = crate::session_identity::resolve(connection, uid, pid).await?;
        let current_uid: u32 = bus
            .call("GetConnectionUnixUser", &(bus_name,))
            .await
            .map_err(|error| BrokerError::IdentityLookup(error.to_string()))?;
        let current_pid: u32 = bus
            .call("GetConnectionUnixProcessID", &(bus_name,))
            .await
            .map_err(|error| BrokerError::IdentityLookup(error.to_string()))?;
        if current_uid != uid || current_pid != pid {
            return Err(BrokerError::InvalidIdentity);
        }
        Ok(Self {
            bus_name: bus_name.to_string(),
            uid,
            session_id,
        })
    }

    pub(crate) fn bus_name(&self) -> &str {
        &self.bus_name
    }

    #[cfg(test)]
    fn test_identity(bus_name: String, uid: u32, session_id: String) -> Result<Self, BrokerError> {
        if !bus_name.starts_with(':') || session_id.is_empty() {
            return Err(BrokerError::InvalidIdentity);
        }
        Ok(Self {
            bus_name,
            uid,
            session_id,
        })
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum PolicyAction {
    Preview,
    Install,
}

impl PolicyAction {
    pub(crate) fn id(self) -> &'static str {
        match self {
            Self::Preview => "org.zethropol.update.preview",
            Self::Install => "org.zethropol.update.install",
        }
    }
}

/// This value is created only after PolicyKit accepted the action for the
/// verified sender from the received D-Bus header. Tests use a mock constructor
/// and therefore do not establish live authority behavior.
#[derive(Debug, Clone)]
pub(crate) struct AuthorizedCaller {
    identity: VerifiedIdentity,
    action: PolicyAction,
    decided_at: u64,
}

impl AuthorizedCaller {
    pub(crate) fn identity(&self) -> &VerifiedIdentity {
        &self.identity
    }
    pub(crate) async fn check_message(
        connection: &zbus::Connection,
        header: &zbus::message::Header<'_>,
        action: PolicyAction,
        decided_at: u64,
    ) -> Result<Self, BrokerError> {
        let identity = VerifiedIdentity::from_header(connection, header).await?;
        let result = tokio::process::Command::new("/usr/bin/pkcheck")
            .args([
                "--action-id",
                action.id(),
                "--system-bus-name",
                identity.bus_name.as_str(),
                "--allow-user-interaction",
            ])
            .stdin(std::process::Stdio::null())
            .status()
            .await
            .map_err(|error| BrokerError::AuthorizationUnavailable(error.to_string()))?;
        if !result.success() {
            return Err(BrokerError::AuthorizationDenied);
        }
        // Authorization can involve user interaction. Bind the decision to
        // the same live sender and logind session after it completes too.
        let current = VerifiedIdentity::from_header(connection, header).await?;
        if current != identity {
            return Err(BrokerError::AuthorizationDenied);
        }
        Ok(Self {
            identity,
            action,
            decided_at,
        })
    }

    #[cfg(test)]
    fn test_authorized(identity: VerifiedIdentity, action: PolicyAction, decided_at: u64) -> Self {
        Self {
            identity,
            action,
            decided_at,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct CanonicalPlan(Vec<UpdatePackageInfo>, Vec<PackageRemoval>, String);

impl CanonicalPlan {
    pub(crate) fn from_packages(mut packages: Vec<UpdatePackageInfo>) -> Result<Self, BrokerError> {
        let mut names = HashSet::new();
        for package in &mut packages {
            if package.package_name.trim().is_empty()
                || package.new_version.trim().is_empty()
                || package.repository.trim().is_empty()
                || !names.insert(package.package_name.clone())
                || (!package.sha256.is_empty() && !valid_sha256(&package.sha256))
                || (!package.repository_metadata_sha256.is_empty()
                    && !valid_sha256(&package.repository_metadata_sha256))
                || (!package.filename.is_empty()
                    && (package.filename.contains('/')
                        || package.filename.contains('\\')
                        || package.filename == "."
                        || package.filename == ".."))
            {
                return Err(BrokerError::InvalidPlan);
            }
            package.sha256.make_ascii_lowercase();
            package.repository_metadata_sha256.make_ascii_lowercase();
        }
        packages.sort_by(|left, right| {
            (
                &left.package_name,
                &left.repository,
                &left.new_version,
                &left.current_version,
                left.download_size,
            )
                .cmp(&(
                    &right.package_name,
                    &right.repository,
                    &right.new_version,
                    &right.current_version,
                    right.download_size,
                ))
        });
        Ok(Self(packages, Vec::new(), String::new()))
    }

    pub(crate) fn from_transaction(
        packages: Vec<UpdatePackageInfo>,
        mut removals: Vec<PackageRemoval>,
    ) -> Result<Self, BrokerError> {
        let mut plan = Self::from_packages(packages)?;
        removals.sort();
        let mut seen = HashSet::new();
        for removal in &removals {
            if removal.package_name.trim().is_empty()
                || removal.reason != "replacement"
                || removal.current_version.is_empty()
                || critical_removal(&removal.package_name)
                || !seen.insert(&removal.package_name)
                || removal.package_name == removal.replacement
                || !plan.0.iter().any(|p| p.package_name == removal.replacement)
                || plan
                    .0
                    .iter()
                    .any(|p| p.package_name == removal.package_name)
            {
                return Err(BrokerError::InvalidPlan);
            }
        }
        plan.1 = removals;
        Ok(plan)
    }

    pub(crate) fn with_policy_fingerprint(
        mut self,
        fingerprint: String,
    ) -> Result<Self, BrokerError> {
        if fingerprint.len() != 64 || !fingerprint.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(BrokerError::InvalidPlan);
        }
        if self.0.iter().any(|package| {
            package.filename.is_empty()
                || !valid_sha256(&package.sha256)
                || !valid_sha256(&package.repository_metadata_sha256)
        }) {
            return Err(BrokerError::InvalidPlan);
        }
        self.2 = fingerprint.to_ascii_lowercase();
        Ok(self)
    }

    pub(crate) fn policy_fingerprint(&self) -> &str {
        &self.2
    }

    pub(crate) fn removals(&self) -> &[PackageRemoval] {
        &self.1
    }

    /// Length-prefixed, domain-separated SHA-256; includes installed versions,
    /// repositories, signed archive sizes, removal reasons and replacement edges.
    pub(crate) fn hash(&self) -> String {
        let mut digest = Sha256::new();
        fn field(d: &mut Sha256, value: &str) {
            d.update((value.len() as u64).to_be_bytes());
            d.update(value.as_bytes());
        }
        field(&mut digest, "zethropol-transaction-plan-v2");
        field(&mut digest, &self.2);
        digest.update((self.0.len() as u64).to_be_bytes());
        for p in &self.0 {
            for value in [
                &p.package_name,
                &p.current_version,
                &p.new_version,
                &p.repository,
            ] {
                field(&mut digest, value);
            }
            digest.update(p.download_size.to_be_bytes());
            for value in [&p.filename, &p.sha256, &p.repository_metadata_sha256] {
                field(&mut digest, value);
            }
        }
        digest.update((self.1.len() as u64).to_be_bytes());
        for r in &self.1 {
            for value in [
                &r.package_name,
                &r.current_version,
                &r.reason,
                &r.replacement,
            ] {
                field(&mut digest, value);
            }
        }
        format!("{:x}", digest.finalize())
    }

    /// Diagnostics only: authorization still uses complete structural equality
    /// and the full hash. No token, timestamp or bus identity is disclosed.
    pub(crate) fn difference_summary(&self, current: &Self) -> Option<String> {
        if self == current {
            return None;
        }
        let mut differences = Vec::new();
        let mut total = 0usize;
        let mut record = |field: &str, before: &str, after: &str| {
            if before == after {
                return;
            }
            total += 1;
            if differences.len() < 24 {
                let safe = |value: &str| {
                    value
                        .chars()
                        .flat_map(char::escape_default)
                        .take(256)
                        .collect::<String>()
                };
                differences.push(format!(
                    "{}: {} -> {}",
                    safe(field),
                    safe(before),
                    safe(after)
                ));
            }
        };
        record(
            "policy_fingerprint (pacman config/hooks/keyring)",
            &self.2,
            &current.2,
        );
        let old: std::collections::BTreeMap<_, _> = self
            .0
            .iter()
            .map(|p| (p.package_name.as_str(), p))
            .collect();
        let new: std::collections::BTreeMap<_, _> = current
            .0
            .iter()
            .map(|p| (p.package_name.as_str(), p))
            .collect();
        for (name, before) in &old {
            let Some(after) = new.get(name) else {
                record("removed transaction target", name, "absent");
                continue;
            };
            for (label, a, b) in [
                (
                    "current_version",
                    before.current_version.as_str(),
                    after.current_version.as_str(),
                ),
                (
                    "new_version",
                    before.new_version.as_str(),
                    after.new_version.as_str(),
                ),
                (
                    "repository",
                    before.repository.as_str(),
                    after.repository.as_str(),
                ),
                (
                    "filename",
                    before.filename.as_str(),
                    after.filename.as_str(),
                ),
                ("sha256", before.sha256.as_str(), after.sha256.as_str()),
                (
                    "repository_metadata_sha256",
                    before.repository_metadata_sha256.as_str(),
                    after.repository_metadata_sha256.as_str(),
                ),
            ] {
                record(&format!("{name}.{label}"), a, b);
            }
            record(
                &format!("{name}.archive_size (download_size wire field)"),
                &before.download_size.to_string(),
                &after.download_size.to_string(),
            );
        }
        for name in new.keys().filter(|name| !old.contains_key(*name)) {
            record("added transaction target", "absent", name);
        }
        if self.1 != current.1 {
            record(
                "removal/replacement edges",
                &format!("{:?}", self.1),
                &format!("{:?}", current.1),
            );
        }
        if total > differences.len() {
            differences.push(format!("{} more changed fields", total - differences.len()));
        }
        Some(format!(
            "Changed canonical fields: {}. Approved plan SHA-256: {}; current: {}",
            differences.join("; "),
            self.hash(),
            current.hash()
        ))
    }

    pub(crate) fn packages(&self) -> &[UpdatePackageInfo] {
        &self.0
    }

    pub(crate) fn matches(&self, candidate: &[UpdatePackageInfo]) -> Result<bool, BrokerError> {
        Ok(self == &Self::from_packages(candidate.to_vec())?)
    }
}

fn valid_sha256(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|byte| byte.is_ascii_hexdigit())
}

/// Conservative removal policy; upgrades of these packages are still allowed.
/// Unsupported boot/recovery migrations require a separate reviewed workflow.
pub(crate) fn critical_removal(name: &str) -> bool {
    const NAMES: &[&str] = &[
        "linux",
        "systemd",
        "glibc",
        "filesystem",
        "pacman",
        "bash",
        "coreutils",
        "gcc-libs",
        "openssl",
        "libarchive",
        "libalpm",
        "kmod",
        "util-linux",
        "sudo",
        "polkit",
        "dbus",
        "mkinitcpio",
        "dracut",
        "limine",
        "grub",
        "efibootmgr",
        "btrfs-progs",
        "snapper",
        "snap-pac",
        "cryptsetup",
        "lvm2",
        "intel-ucode",
        "amd-ucode",
        "zethropol-update",
    ];
    NAMES
        .iter()
        .any(|n| name == *n || name.starts_with(&format!("{n}-")))
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum OperationStatus {
    Previewed,
    Queued,
    Running,
    WorkerStartFailed,
    Succeeded,
    CompletedWithWarnings,
    Failed,
}

impl OperationStatus {
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            Self::Previewed => "previewed",
            Self::Queued => "queued",
            Self::Running => "running",
            Self::WorkerStartFailed => "worker-start-failed",
            Self::Succeeded => "succeeded",
            Self::CompletedWithWarnings => "completed-with-warnings",
            Self::Failed => "failed",
        }
    }

    fn parse(value: &str) -> Result<Self, BrokerError> {
        match value {
            "previewed" => Ok(Self::Previewed),
            "queued" => Ok(Self::Queued),
            "running" => Ok(Self::Running),
            "worker-start-failed" => Ok(Self::WorkerStartFailed),
            "succeeded" => Ok(Self::Succeeded),
            "completed-with-warnings" => Ok(Self::CompletedWithWarnings),
            "failed" => Ok(Self::Failed),
            _ => Err(BrokerError::CorruptRecord),
        }
    }

    pub(crate) fn terminal(self) -> bool {
        matches!(
            self,
            Self::Succeeded | Self::CompletedWithWarnings | Self::Failed
        )
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum BrokerError {
    InvalidIdentity,
    IdentityLookup(String),
    AuthorizationUnavailable(String),
    AuthorizationDenied,
    InvalidPlan,
    InvalidToken,
    Expired,
    NoPreview,
    IdentityMismatch,
    OperationInProgress,
    PlanChanged,
    InvalidTransition,
    NotFound,
    CorruptRecord,
    UnsafeStore,
    Io(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct OperationView {
    pub operation_id: String,
    pub status: OperationStatus,
    pub message: String,
    pub evidence: ExecutionEvidence,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct ConsumedPreview {
    pub operation_id: String,
    pub plan: CanonicalPlan,
}

#[derive(Debug, Clone)]
struct Record {
    operation_id: String,
    owner: VerifiedIdentity,
    preview_authorized_at: u64,
    install_authorized_at: Option<u64>,
    finished_at: Option<u64>,
    token: String,
    created_at: u64,
    expires_at: u64,
    plan: CanonicalPlan,
    removals_approved: bool,
    status: OperationStatus,
    message: String,
    evidence: ExecutionEvidence,
}

pub(crate) trait TokenSource: Send + Sync {
    fn next_token(&self) -> Result<String, BrokerError>;
}

struct UuidTokenSource;

impl TokenSource for UuidTokenSource {
    fn next_token(&self) -> Result<String, BrokerError> {
        Ok(Uuid::new_v4().to_string())
    }
}

/// Root-only record store. Production callers must use `open_runtime`, which
/// pins the directory to `/run/zethropol-update` and requires effective UID 0.
/// Tests use a private temporary directory with the test process as owner.
pub(crate) struct BrokerStore {
    root: PathBuf,
    expected_owner: u32,
    tokens: Arc<dyn TokenSource>,
    aur_durable: bool,
}

impl BrokerStore {
    /// Held across preflight, pacman work and result publication. A duplicate
    /// process must exit without changing the original worker's record.
    pub(crate) fn worker_lease(&self, operation_id: &str) -> Result<File, BrokerError> {
        let id = valid_uuid(operation_id.to_string())?;
        verify_directory(&self.root, self.expected_owner)?;
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(self.root.join(format!("worker-{id}.lock")))
            .map_err(io_error)?;
        let metadata = file.metadata().map_err(io_error)?;
        if !metadata.is_file()
            || metadata.uid() != self.expected_owner
            || metadata.permissions().mode() & 0o777 != 0o600
        {
            return Err(BrokerError::UnsafeStore);
        }
        if unsafe {
            libc::flock(
                std::os::fd::AsRawFd::as_raw_fd(&file),
                libc::LOCK_EX | libc::LOCK_NB,
            )
        } != 0
        {
            let error = std::io::Error::last_os_error();
            return Err(if error.kind() == std::io::ErrorKind::WouldBlock {
                BrokerError::OperationInProgress
            } else {
                io_error(error)
            });
        }
        Ok(file)
    }

    /// Validate the inherited open description; never reopen or unlock the
    /// parent's lease during the broker-to-native exec handoff.
    pub(crate) fn verify_native_lease(
        &self,
        operation_id: &str,
        fd: std::os::fd::BorrowedFd<'_>,
    ) -> Result<(), BrokerError> {
        use std::os::fd::AsRawFd;
        let id = valid_uuid(operation_id.to_string())?;
        verify_directory(&self.root, self.expected_owner)?;
        let file = File::from(fd.try_clone_to_owned().map_err(io_error)?);
        let a = file.metadata().map_err(io_error)?;
        let b =
            fs::symlink_metadata(self.root.join(format!("worker-{id}.lock"))).map_err(io_error)?;
        if !a.is_file()
            || !b.is_file()
            || a.uid() != self.expected_owner
            || a.permissions().mode() & 0o777 != 0o600
            || a.dev() != b.dev()
            || a.ino() != b.ino()
            || unsafe { libc::flock(fd.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0
        {
            return Err(BrokerError::OperationInProgress);
        }
        Ok(())
    }

    pub(crate) fn native_cancelled_before_commit(
        &self,
        operation: &str,
    ) -> Result<bool, BrokerError> {
        self.with_lock(|| {
            let record = self.read_record(&self.operation_path(operation)?)?;
            Ok(record.status == OperationStatus::Failed
                && record.evidence.package_commit == CommitState::NotStarted)
        })
    }

    pub(crate) fn require_queued(&self, operation_id: &str) -> Result<(), BrokerError> {
        self.with_lock(|| {
            let record = self.read_record(&self.operation_path(operation_id)?)?;
            if record.status == OperationStatus::Queued {
                Ok(())
            } else {
                Err(BrokerError::InvalidTransition)
            }
        })
    }

    pub(crate) fn open_runtime() -> Result<Self, BrokerError> {
        let uid = unsafe { libc::geteuid() };
        if uid != 0 {
            return Err(BrokerError::UnsafeStore);
        }
        Self::open_with(
            PathBuf::from("/run/zethropol-update"),
            0,
            Arc::new(UuidTokenSource),
        )
    }

    fn open_with(
        root: PathBuf,
        expected_owner: u32,
        tokens: Arc<dyn TokenSource>,
    ) -> Result<Self, BrokerError> {
        match fs::symlink_metadata(&root) {
            Ok(_) => {}
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                let mut builder = fs::DirBuilder::new();
                builder.mode(0o700);
                builder.create(&root).map_err(io_error)?;
            }
            Err(error) => return Err(io_error(error)),
        }
        verify_directory(&root, expected_owner)?;
        let store = Self {
            root,
            expected_owner,
            tokens,
            aur_durable: false,
        };
        store.with_lock(|| Ok(()))?;
        Ok(store)
    }

    pub(crate) fn create_preview(
        &self,
        authorization: AuthorizedCaller,
        plan: CanonicalPlan,
        now: u64,
    ) -> Result<(), BrokerError> {
        if authorization.action != PolicyAction::Preview
            || authorization.decided_at > now
            || now.saturating_sub(authorization.decided_at) > AUTHORIZATION_MAX_AGE_SECS
        {
            return Err(BrokerError::AuthorizationDenied);
        }
        if plan.packages().len() > 100_000
            || plan.removals().len() > 100_000
            || plan.policy_fingerprint().len() != 64
        {
            return Err(BrokerError::InvalidPlan);
        }
        let token = valid_uuid(self.tokens.next_token()?)?;
        let operation_id = valid_uuid(self.tokens.next_token()?)?;
        if self.aur_is_persistent()? && self.aur_nonce_retained(&operation_id)? {
            return Err(BrokerError::InvalidToken);
        }
        let record = Record {
            operation_id,
            owner: authorization.identity.clone(),
            preview_authorized_at: authorization.decided_at,
            install_authorized_at: None,
            finished_at: None,
            token,
            created_at: now,
            expires_at: now.saturating_add(PREVIEW_TTL_SECS),
            plan,
            removals_approved: false,
            status: OperationStatus::Previewed,
            message: "Preview is ready; it expires in five minutes.".to_string(),
            evidence: ExecutionEvidence::default(),
        };
        self.with_lock(|| {
            let path = self.preview_path(&authorization.identity);
            self.aur_preserve_replaced_preview(&path)?;
            self.write_record(&path, &record)
        })
    }

    pub(crate) async fn operation_for_message(
        &self,
        connection: &zbus::Connection,
        header: &zbus::message::Header<'_>,
        operation_id: &str,
    ) -> Result<OperationView, BrokerError> {
        let identity = VerifiedIdentity::from_header(connection, header).await?;
        self.operation_for_owner(&identity, operation_id)
    }

    /// Atomically renames a preview into its operation record while holding an
    /// inter-process flock. A crash after rename consumes the preview safely;
    /// the resulting `Previewed` record is inert and cannot start a worker.
    #[cfg(test)]
    pub(crate) fn consume_preview(
        &self,
        authorization: AuthorizedCaller,
        now: u64,
    ) -> Result<ConsumedPreview, BrokerError> {
        self.consume_preview_checked(authorization, now, None)
    }

    pub(crate) fn consume_approved_preview(
        &self,
        authorization: AuthorizedCaller,
        now: u64,
        hash: &str,
        approve_removals: bool,
    ) -> Result<ConsumedPreview, BrokerError> {
        self.consume_preview_checked(authorization, now, Some((hash, approve_removals)))
    }

    fn consume_preview_checked(
        &self,
        authorization: AuthorizedCaller,
        now: u64,
        approval: Option<(&str, bool)>,
    ) -> Result<ConsumedPreview, BrokerError> {
        let owner = &authorization.identity;
        if authorization.action != PolicyAction::Install
            || authorization.decided_at > now
            || now.saturating_sub(authorization.decided_at) > AUTHORIZATION_MAX_AGE_SECS
        {
            return Err(BrokerError::AuthorizationDenied);
        }
        self.with_lock(|| {
            let preview_path = self.preview_path(owner);
            let mut record = match self.read_record(&preview_path) {
                Err(BrokerError::NotFound) => return Err(BrokerError::NoPreview),
                result => result?,
            };
            ensure_owner(&record, owner)?;
            if now >= record.expires_at {
                self.aur_expire_preview(&preview_path)?;
                return Err(BrokerError::Expired);
            }
            if record.status != OperationStatus::Previewed || !is_uuid(&record.token) {
                return Err(BrokerError::CorruptRecord);
            }
            match approval {
                Some((hash, approved))
                    if hash == record.plan.hash()
                        && (record.plan.removals().is_empty() || approved) => {}
                None if record.plan.removals().is_empty() => {}
                _ => return Err(BrokerError::AuthorizationDenied),
            }
            if self.has_active_operation()? {
                return Err(BrokerError::OperationInProgress);
            }
            let operation_path = self.operation_path(&record.operation_id)?;
            if operation_path.exists() {
                return Err(BrokerError::UnsafeStore);
            }
            fs::rename(&preview_path, &operation_path).map_err(io_error)?;
            record.install_authorized_at = Some(authorization.decided_at);
            record.removals_approved = !record.plan.removals().is_empty();
            record.finished_at = None;
            record.status = OperationStatus::Queued;
            record.message = format!(
                "Authorized plan {} queued; explicit removal consent: {}",
                record.plan.hash(),
                !record.plan.removals().is_empty()
            );
            self.write_record(&operation_path, &record)?;
            Ok(ConsumedPreview {
                operation_id: record.operation_id,
                plan: record.plan,
            })
        })
    }

    pub(crate) fn worker_start_failed(
        &self,
        operation_id: &str,
        message: &str,
    ) -> Result<(), BrokerError> {
        self.with_lock(|| {
            let path = self.operation_path(operation_id)?;
            let mut record = self.read_record(&path)?;
            if record.status != OperationStatus::Queued {
                // systemctl may report failure after ExecStart/ExecStopPost
                // has already advanced the record. Never overwrite that result.
                return Ok(());
            }
            record.status = OperationStatus::WorkerStartFailed;
            record.message = message.to_string();
            self.write_record(&path, &record)
        })
    }

    pub(crate) fn worker_failed_before_claim(
        &self,
        operation_id: &str,
        message: &str,
        now: u64,
    ) -> Result<(), BrokerError> {
        self.with_lock(|| {
            let path = self.operation_path(operation_id)?;
            let mut record = self.read_record(&path)?;
            if record.status != OperationStatus::Queued {
                return Err(BrokerError::InvalidTransition);
            }
            record.status = OperationStatus::Failed;
            record.finished_at = Some(now);
            record.message = message.to_string();
            self.write_record(&path, &record)
        })
    }

    /// Retry is limited to a worker that never claimed the queued record. The
    /// same operation ID makes duplicate systemd starts idempotent at claim.
    pub(crate) fn retry_worker_start(
        &self,
        owner: &VerifiedIdentity,
        operation_id: &str,
    ) -> Result<(), BrokerError> {
        self.with_lock(|| {
            let path = self.operation_path(operation_id)?;
            let mut record = self.read_record(&path)?;
            ensure_owner(&record, owner)?;
            if record.status != OperationStatus::WorkerStartFailed {
                return Err(BrokerError::InvalidTransition);
            }
            record.status = OperationStatus::Queued;
            record.message = "Worker start retry queued.".to_string();
            self.write_record(&path, &record)
        })
    }

    /// Compare and transition are one locked operation. Only one duplicate
    /// worker can claim Queued; a second start cannot execute the plan.
    pub(crate) fn claim_worker(
        &self,
        operation_id: &str,
        current_plan: &[UpdatePackageInfo],
        now: u64,
    ) -> Result<CanonicalPlan, BrokerError> {
        #[cfg(not(test))]
        {
            self.claim_transaction(
                operation_id,
                &CanonicalPlan::from_packages(current_plan.to_vec())?,
                now,
            )
        }
        #[cfg(test)]
        {
            // Legacy unit fixtures predate content identities. Preserve their
            // package-field drift checks while copying only opaque fixture
            // identities from the persisted plan; production never uses this API.
            let approved =
                self.with_lock(|| Ok(self.read_record(&self.operation_path(operation_id)?)?.plan))?;
            if !approved.removals().is_empty() {
                return Err(BrokerError::PlanChanged);
            }
            let mut candidate = CanonicalPlan::from_packages(current_plan.to_vec())?;
            for package in &mut candidate.0 {
                if let Some(old) = approved
                    .packages()
                    .iter()
                    .find(|p| p.package_name == package.package_name)
                {
                    package.filename = old.filename.clone();
                    package.sha256 = old.sha256.clone();
                    package.repository_metadata_sha256 = old.repository_metadata_sha256.clone();
                }
            }
            candidate.2 = approved.policy_fingerprint().to_string();
            self.claim_transaction(operation_id, &candidate, now)
        }
    }

    pub(crate) fn claim_transaction(
        &self,
        operation_id: &str,
        current_plan: &CanonicalPlan,
        now: u64,
    ) -> Result<CanonicalPlan, BrokerError> {
        self.with_lock(|| {
            let path = self.operation_path(operation_id)?;
            let mut record = self.read_record(&path)?;
            if record.status != OperationStatus::Queued {
                return Err(BrokerError::InvalidTransition);
            }
            if now >= record.expires_at {
                record.status = OperationStatus::Failed;
                record.finished_at = Some(now);
                record.message =
                    "The queued preview expired before its worker started.".to_string();
                self.write_record(&path, &record)?;
                return Err(BrokerError::Expired);
            }
            if &record.plan != current_plan {
                record.status = OperationStatus::Failed;
                record.finished_at = Some(now);
                record.message = format!(
                    "plan_changed: {}. Create a new Preview Plan.",
                    record
                        .plan
                        .difference_summary(current_plan)
                        .unwrap_or_else(|| "canonical plan mismatch".into())
                );
                self.write_record(&path, &record)?;
                return Err(BrokerError::PlanChanged);
            }
            record.status = OperationStatus::Running;
            record.message = "Update worker started.".to_string();
            let plan = record.plan.clone();
            self.write_record(&path, &record)?;
            Ok(plan)
        })
    }

    /// Compare only public plan identities; never return a preview token.
    pub(crate) fn plan_difference(
        &self,
        operation_id: &str,
        current: &CanonicalPlan,
    ) -> Result<String, BrokerError> {
        self.with_lock(|| {
            let record = self.read_record(&self.operation_path(operation_id)?)?;
            Ok(record
                .plan
                .difference_summary(current)
                .unwrap_or_else(|| "No canonical fields changed".into()))
        })
    }

    /// Persist worker messages without changing transaction status/evidence.
    pub(crate) fn update_worker_progress(
        &self,
        operation_id: &str,
        message: &str,
    ) -> Result<(), BrokerError> {
        self.with_lock(|| {
            let path = self.operation_path(operation_id)?;
            let mut record = self.read_record(&path)?;
            if !matches!(
                record.status,
                OperationStatus::Queued | OperationStatus::Running
            ) {
                return Err(BrokerError::InvalidTransition);
            }
            record.message = message.chars().take(4096).collect();
            self.write_record(&path, &record)
        })
    }

    pub(crate) fn update_worker_status(
        &self,
        operation_id: &str,
        status: OperationStatus,
        message: &str,
        now: u64,
    ) -> Result<(), BrokerError> {
        self.with_lock(|| {
            let path = self.operation_path(operation_id)?;
            let mut record = self.read_record(&path)?;
            if record.status != OperationStatus::Running
                || !matches!(
                    status,
                    OperationStatus::Running
                        | OperationStatus::Succeeded
                        | OperationStatus::CompletedWithWarnings
                        | OperationStatus::Failed
                )
            {
                return Err(BrokerError::InvalidTransition);
            }
            record.status = status;
            record.finished_at = status.terminal().then_some(now);
            record.message = message.to_string();
            self.write_record(&path, &record)
        })
    }

    pub(crate) fn finish_worker_result(
        &self,
        operation_id: &str,
        status: OperationStatus,
        message: &str,
        now: u64,
    ) -> Result<(), BrokerError> {
        self.with_lock(|| {
            let path = self.operation_path(operation_id)?;
            let mut record = self.read_record(&path)?;
            if record.status.terminal() {
                return Ok(());
            }
            if !matches!(
                record.status,
                OperationStatus::Queued | OperationStatus::Running
            ) || !matches!(
                status,
                OperationStatus::Succeeded
                    | OperationStatus::CompletedWithWarnings
                    | OperationStatus::Failed
            ) || (matches!(
                status,
                OperationStatus::Succeeded | OperationStatus::CompletedWithWarnings
            ) && record.status != OperationStatus::Running)
            {
                return Err(BrokerError::InvalidTransition);
            }
            record.status = status;
            record.finished_at = Some(now);
            record.message = message.to_string();
            if status == OperationStatus::Failed {
                record.evidence.interrupted();
            }
            self.write_record(&path, &record)
        })
    }

    /// Durable checkpoints bracket commit. Only the worker's running operation
    /// can publish evidence; interruption must never turn an inflight commit
    /// into a claim that no files changed.
    pub(crate) fn record_execution_evidence(
        &self,
        operation_id: &str,
        evidence: &ExecutionEvidence,
    ) -> Result<(), BrokerError> {
        self.with_lock(|| {
            let path = self.operation_path(operation_id)?;
            let mut record = self.read_record(&path)?;
            if record.status != OperationStatus::Running {
                return Err(BrokerError::InvalidTransition);
            }
            record.evidence = evidence.clone();
            self.write_record(&path, &record)
        })
    }

    /// Called by systemd ExecStopPost. It turns an interrupted queued or
    /// running worker into a terminal failure and never makes the operation
    /// runnable again. A normal completed worker is an idempotent no-op.
    pub(crate) fn worker_process_stopped(
        &self,
        operation_id: &str,
        now: u64,
    ) -> Result<(), BrokerError> {
        self.with_lock(|| {
            let path = self.operation_path(operation_id)?;
            let mut record = self.read_record(&path)?;
            match record.status {
                OperationStatus::Queued | OperationStatus::Running => {
                    record.status = OperationStatus::Failed;
                    record.finished_at = Some(now);
                    record.evidence.interrupted();
                    record.message = "Worker stopped before recording completion. The package transaction may have partially or fully applied; inspect pacman logs before any manual recovery. No automatic retry was made.".to_string();
                    self.write_record(&path, &record)
                }
                OperationStatus::Failed
                | OperationStatus::Succeeded
                | OperationStatus::CompletedWithWarnings
                | OperationStatus::WorkerStartFailed => Ok(()),
                OperationStatus::Previewed => Err(BrokerError::InvalidTransition),
            }
        })
    }

    pub(crate) fn reconcile_worker_observation(
        &self,
        operation_id: &str,
        observation: Result<bool, BrokerError>,
        now: u64,
    ) -> Result<(), BrokerError> {
        let active = match observation {
            Ok(active) => active,
            Err(error) => {
                // A worker owns its lease from before preflight through result
                // publication. Missing systemd access must not strand a dead
                // worker; the lease and queue handoff grace still apply.
                eprintln!("Worker systemd inspection unavailable for {operation_id}: {error:?}; checking worker lease");
                false
            }
        };
        self.reconcile_worker_state(operation_id, active, now)
    }

    pub(crate) fn reconcile_worker_state(
        &self,
        operation_id: &str,
        worker_active: bool,
        now: u64,
    ) -> Result<(), BrokerError> {
        if worker_active {
            return Ok(());
        }
        let _lease = match self.worker_lease(operation_id) {
            Ok(lease) => lease,
            Err(BrokerError::OperationInProgress) => return Ok(()),
            Err(error) => return Err(error),
        };
        // Allow the broker's queue-to-systemd-start handoff to complete. A
        // broker crash in that window is failed by a later periodic sweep.
        let starting = self.with_lock(|| {
            let record = self.read_record(&self.operation_path(operation_id)?)?;
            Ok(record.status == OperationStatus::Queued
                && now.saturating_sub(record.install_authorized_at.unwrap_or(record.created_at))
                    < 60)
        })?;
        if starting {
            return Ok(());
        }
        self.worker_process_stopped(operation_id, now)
    }

    pub(crate) fn unresolved_operations(&self) -> Result<Vec<String>, BrokerError> {
        self.with_lock(|| {
            let mut operation_ids = Vec::new();
            for entry in fs::read_dir(&self.root).map_err(io_error)? {
                let entry = entry.map_err(io_error)?;
                if !entry
                    .file_name()
                    .to_string_lossy()
                    .starts_with("operation-")
                {
                    continue;
                }
                let record = self.read_record(&entry.path())?;
                if matches!(
                    record.status,
                    OperationStatus::Queued | OperationStatus::Running
                ) {
                    operation_ids.push(record.operation_id);
                }
            }
            Ok(operation_ids)
        })
    }

    pub(crate) fn operation_for_owner(
        &self,
        owner: &VerifiedIdentity,
        operation_id: &str,
    ) -> Result<OperationView, BrokerError> {
        self.with_lock(|| {
            let record = self.read_record(&self.operation_path(operation_id)?)?;
            ensure_owner(&record, owner)?;
            Ok(OperationView {
                operation_id: record.operation_id,
                status: record.status,
                message: record.message,
                evidence: record.evidence,
            })
        })
    }

    /// Read-only resume scope: active authenticated user/session may inspect
    /// its previous connection's operations. Preview tokens and installation
    /// authorization remain strictly bound to the original D-Bus identity.
    pub(crate) fn resumable_operation(
        &self,
        owner: &VerifiedIdentity,
        operation_id: Option<&str>,
    ) -> Result<OperationView, BrokerError> {
        self.with_lock(|| {
            let mut latest: Option<Record> = None;
            for entry in fs::read_dir(&self.root).map_err(io_error)? {
                let entry = entry.map_err(io_error)?;
                let name = entry.file_name().to_string_lossy().into_owned();
                if !name.starts_with("operation-") || !name.ends_with(".record") {
                    continue;
                }
                if let Some(id) = operation_id {
                    if name != format!("operation-{}.record", valid_uuid(id.to_string())?) {
                        continue;
                    }
                }
                let record = self.read_record(&entry.path())?;
                if record.owner.uid != owner.uid || record.owner.session_id != owner.session_id {
                    if operation_id.is_some() {
                        return Err(BrokerError::IdentityMismatch);
                    }
                    continue;
                }
                if latest.as_ref().is_none_or(|previous| {
                    (record.created_at, &record.operation_id)
                        > (previous.created_at, &previous.operation_id)
                }) {
                    latest = Some(record);
                }
            }
            let record = latest.ok_or(BrokerError::NotFound)?;
            Ok(OperationView {
                operation_id: record.operation_id,
                status: record.status,
                message: record.message,
                evidence: record.evidence,
            })
        })
    }

    pub(crate) fn cleanup(
        &self,
        now: u64,
        terminal_retention: Duration,
    ) -> Result<usize, BrokerError> {
        self.with_lock(|| {
            if self.aur_is_persistent()? {
                return Err(BrokerError::InvalidTransition);
            }
            let mut removed = 0;
            for entry in fs::read_dir(&self.root).map_err(io_error)? {
                let entry = entry.map_err(io_error)?;
                let name = entry.file_name();
                let name = name.to_string_lossy();
                if name.starts_with("preview-") {
                    match self.read_record(&entry.path()) {
                        Ok(record) if now >= record.expires_at => {
                            remove_owned_record(&entry.path(), self.expected_owner)?;
                            removed += 1;
                        }
                        Ok(_) => {}
                        Err(error) => return Err(error),
                    }
                } else if name.starts_with("operation-") {
                    let mut record = self.read_record(&entry.path())?;
                    // A queued worker may still be refreshing repositories.
                    // Preview expiry is not evidence that it stopped; only
                    // reconciliation under the worker lease may fail it.
                    if record.status == OperationStatus::WorkerStartFailed
                        && now >= record.expires_at
                    {
                        record.status = OperationStatus::Failed;
                        record.finished_at = Some(now);
                        record.message =
                            "The preview expired before the worker completed its start transition."
                                .to_string();
                        self.write_record(&entry.path(), &record)?;
                        continue;
                    }
                    let old_terminal = record.status.terminal()
                        && record.finished_at.is_some_and(|finished| {
                            now.saturating_sub(finished) >= terminal_retention.as_secs()
                        });
                    let expired_orphan =
                        record.status == OperationStatus::Previewed && now >= record.expires_at;
                    if old_terminal || expired_orphan {
                        remove_owned_record(&entry.path(), self.expected_owner)?;
                        removed += 1;
                    }
                }
            }
            Ok(removed)
        })
    }

    fn preview_path(&self, owner: &VerifiedIdentity) -> PathBuf {
        self.root.join(format!(
            "preview-{}.record",
            encode_hex(owner.bus_name.as_bytes())
        ))
    }

    fn has_active_operation(&self) -> Result<bool, BrokerError> {
        for entry in fs::read_dir(&self.root).map_err(io_error)? {
            let entry = entry.map_err(io_error)?;
            if !entry
                .file_name()
                .to_string_lossy()
                .starts_with("operation-")
            {
                continue;
            }
            let record = self.read_record(&entry.path())?;
            if matches!(
                record.status,
                OperationStatus::Queued | OperationStatus::Running
            ) {
                return Ok(true);
            }
        }
        Ok(false)
    }

    fn operation_path(&self, id: &str) -> Result<PathBuf, BrokerError> {
        let id = valid_uuid(id.to_string())?;
        Ok(self.root.join(format!("operation-{id}.record")))
    }

    fn read_record(&self, path: &Path) -> Result<Record, BrokerError> {
        let mut options = OpenOptions::new();
        options
            .read(true)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC);
        let file = options.open(path).map_err(|error| {
            if error.kind() == std::io::ErrorKind::NotFound {
                BrokerError::NotFound
            } else if error.raw_os_error() == Some(libc::ELOOP) {
                BrokerError::UnsafeStore
            } else {
                io_error(error)
            }
        })?;
        let metadata = file.metadata().map_err(io_error)?;
        if !metadata.is_file()
            || metadata.uid() != self.expected_owner
            || metadata.permissions().mode() & 0o777 != 0o600
            || metadata.len() > MAX_RECORD_BYTES
        {
            return Err(BrokerError::UnsafeStore);
        }
        let mut bytes = Vec::with_capacity(metadata.len() as usize);
        file.take(MAX_RECORD_BYTES + 1)
            .read_to_end(&mut bytes)
            .map_err(io_error)?;
        if bytes.len() as u64 > MAX_RECORD_BYTES {
            return Err(BrokerError::CorruptRecord);
        }
        let bytes = self.aur_unpack_bytes(&bytes)?;
        let record = decode_record(&bytes)?;
        let name = path
            .file_name()
            .and_then(|value| value.to_str())
            .ok_or(BrokerError::CorruptRecord)?;
        let expected_operation = format!("operation-{}.record", record.operation_id);
        let expected_preview = format!(
            "preview-{}.record",
            encode_hex(record.owner.bus_name.as_bytes())
        );
        if (name.starts_with("operation-") && name != expected_operation)
            || (name.starts_with("preview-") && name != expected_preview)
            || (!name.starts_with("operation-") && !name.starts_with("preview-"))
        {
            return Err(BrokerError::CorruptRecord);
        }
        Ok(record)
    }

    fn write_record(&self, path: &Path, record: &Record) -> Result<(), BrokerError> {
        let bytes = self.aur_pack_bytes(&encode_record(record))?;
        let temp = self.root.join(format!("tmp-{}.record", Uuid::new_v4()));
        let mut options = OpenOptions::new();
        options
            .write(true)
            .create_new(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC);
        let mut file = options.open(&temp).map_err(io_error)?;
        file.set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(io_error)?;
        if let Err(error) = file.write_all(&bytes).and_then(|_| file.sync_all()) {
            let _ = fs::remove_file(&temp);
            return Err(io_error(error));
        }
        #[cfg(test)]
        aur_persistent_crash_point(self, "before-record-rename");
        fs::rename(&temp, path).map_err(|error| {
            let _ = fs::remove_file(&temp);
            io_error(error)
        })?;
        {
            #[cfg(test)]
            aur_persistent_crash_point(self, "after-record-rename");
        }
        File::open(&self.root)
            .and_then(|directory| directory.sync_all())
            .map_err(io_error)
    }

    fn with_lock<T>(
        &self,
        operation: impl FnOnce() -> Result<T, BrokerError>,
    ) -> Result<T, BrokerError> {
        verify_directory(&self.root, self.expected_owner)?;
        self.aur_check_persistent_lock()?;
        let mut options = OpenOptions::new();
        options
            .read(true)
            .write(true)
            .create(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC);
        let lock = options.open(self.root.join(".lock")).map_err(io_error)?;
        lock.set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(io_error)?;
        let metadata = lock.metadata().map_err(io_error)?;
        if !metadata.is_file()
            || metadata.uid() != self.expected_owner
            || metadata.permissions().mode() & 0o777 != 0o600
        {
            return Err(BrokerError::UnsafeStore);
        }
        // SAFETY: `lock` is an open file descriptor held until after the
        // operation closure returns. flock is process-safe and automatically
        // released if the process exits.
        if unsafe { libc::flock(std::os::fd::AsRawFd::as_raw_fd(&lock), libc::LOCK_EX) } != 0 {
            return Err(io_error(std::io::Error::last_os_error()));
        }
        let result = operation();
        let unlock = unsafe { libc::flock(std::os::fd::AsRawFd::as_raw_fd(&lock), libc::LOCK_UN) };
        if unlock != 0 && result.is_ok() {
            return Err(io_error(std::io::Error::last_os_error()));
        }
        result
    }
}

fn verify_directory(path: &Path, expected_owner: u32) -> Result<(), BrokerError> {
    let metadata = fs::symlink_metadata(path).map_err(io_error)?;
    if !metadata.file_type().is_dir()
        || metadata.uid() != expected_owner
        || metadata.permissions().mode() & 0o777 != 0o700
    {
        return Err(BrokerError::UnsafeStore);
    }
    Ok(())
}

fn ensure_owner(record: &Record, identity: &VerifiedIdentity) -> Result<(), BrokerError> {
    if record.owner == *identity {
        Ok(())
    } else {
        Err(BrokerError::IdentityMismatch)
    }
}

fn remove_owned_record(path: &Path, expected_owner: u32) -> Result<(), BrokerError> {
    let metadata = fs::symlink_metadata(path).map_err(io_error)?;
    if !metadata.file_type().is_file()
        || metadata.uid() != expected_owner
        || metadata.permissions().mode() & 0o777 != 0o600
    {
        return Err(BrokerError::UnsafeStore);
    }
    fs::remove_file(path).map_err(io_error)
}

fn valid_uuid(value: String) -> Result<String, BrokerError> {
    let parsed = Uuid::parse_str(&value).map_err(|_| BrokerError::InvalidToken)?;
    let canonical = parsed.to_string();
    if canonical != value.to_ascii_lowercase() {
        return Err(BrokerError::InvalidToken);
    }
    Ok(canonical)
}

fn is_uuid(value: &str) -> bool {
    Uuid::parse_str(value).is_ok_and(|parsed| parsed.to_string() == value)
}

fn encode_record(record: &Record) -> Vec<u8> {
    let mut lines = vec![
        RECORD_VERSION.to_string(),
        record.operation_id.clone(),
        encode_hex(record.owner.bus_name.as_bytes()),
        record.owner.uid.to_string(),
        encode_hex(record.owner.session_id.as_bytes()),
        record.preview_authorized_at.to_string(),
        record
            .install_authorized_at
            .map(|value| value.to_string())
            .unwrap_or_else(|| "-".to_string()),
        record
            .finished_at
            .map(|value| value.to_string())
            .unwrap_or_else(|| "-".to_string()),
        record.token.clone(),
        record.created_at.to_string(),
        record.expires_at.to_string(),
        record.status.as_str().to_string(),
        encode_hex(record.message.as_bytes()),
        record.evidence.package_commit.as_str().to_string(),
        record.evidence.hooks.as_str().to_string(),
        record.evidence.boot_preparation.as_str().to_string(),
        record
            .evidence
            .pre_snapshot
            .clone()
            .unwrap_or_else(|| "-".into()),
        record
            .evidence
            .post_snapshot
            .clone()
            .unwrap_or_else(|| "-".into()),
        record.evidence.boot.kernels.as_str().into(),
        record.evidence.boot.dkms.as_str().into(),
        record.evidence.boot.initramfs.as_str().into(),
        record.evidence.boot.limine.as_str().into(),
        record.evidence.boot.new_kernel_boot.as_str().into(),
        encode_hex(record.evidence.boot.running_kernel.as_bytes()),
        encode_hex(record.evidence.boot.installed_kernels.as_bytes()),
        encode_hex(record.evidence.boot.updated_kernels.as_bytes()),
        encode_hex(record.evidence.boot.details.as_bytes()),
        encode_hex(record.evidence.hook_details.as_bytes()),
        record.plan.0.len().to_string(),
    ];
    for package in &record.plan.0 {
        lines.push(encode_hex(package.package_name.as_bytes()));
        lines.push(encode_hex(package.current_version.as_bytes()));
        lines.push(encode_hex(package.new_version.as_bytes()));
        lines.push(encode_hex(package.repository.as_bytes()));
        lines.push(package.download_size.to_string());
    }
    lines.push(record.plan.1.len().to_string());
    for r in &record.plan.1 {
        for value in [
            &r.package_name,
            &r.current_version,
            &r.reason,
            &r.replacement,
        ] {
            lines.push(encode_hex(value.as_bytes()));
        }
    }
    lines.push(if record.removals_approved { "1" } else { "0" }.into());
    lines.push(encode_hex(record.plan.policy_fingerprint().as_bytes()));
    lines.push(record.plan.packages().len().to_string());
    for package in record.plan.packages() {
        for value in [
            &package.filename,
            &package.sha256,
            &package.repository_metadata_sha256,
        ] {
            lines.push(encode_hex(value.as_bytes()));
        }
    }
    lines.extend([
        record.evidence.cleanup.status.clone(),
        record.evidence.cleanup.removed_files.to_string(),
        record.evidence.cleanup.removed_bytes.to_string(),
        record.evidence.cleanup.reclaimed_bytes.to_string(),
        encode_hex(record.evidence.cleanup.details.as_bytes()),
    ]);
    lines.push(String::new());
    lines.join("\n").into_bytes()
}

fn decode_record(bytes: &[u8]) -> Result<Record, BrokerError> {
    let text = std::str::from_utf8(bytes).map_err(|_| BrokerError::CorruptRecord)?;
    let mut lines = text.lines();
    let version = match lines.next() {
        Some(RECORD_VERSION) => 6,
        Some(V5_RECORD_VERSION) => 5,
        Some(V4_RECORD_VERSION) => 4,
        Some(V3_RECORD_VERSION) => 3,
        Some(PREVIOUS_RECORD_VERSION) => 2,
        Some(LEGACY_RECORD_VERSION) => 1,
        _ => return Err(BrokerError::CorruptRecord),
    };
    let operation_id = valid_uuid(next(&mut lines)?.to_string())?;
    let bus_name = decode_string(next(&mut lines)?)?;
    let uid = next(&mut lines)?
        .parse()
        .map_err(|_| BrokerError::CorruptRecord)?;
    let session_id = decode_string(next(&mut lines)?)?;
    let owner = VerifiedIdentity {
        bus_name,
        uid,
        session_id,
    };
    if !owner.bus_name.starts_with(':') || owner.session_id.is_empty() {
        return Err(BrokerError::CorruptRecord);
    }
    let preview_authorized_at: u64 = next(&mut lines)?
        .parse()
        .map_err(|_| BrokerError::CorruptRecord)?;
    let install_authorized_at = match next(&mut lines)? {
        "-" => None,
        value => Some(value.parse().map_err(|_| BrokerError::CorruptRecord)?),
    };
    let finished_at = match next(&mut lines)? {
        "-" => None,
        value => Some(value.parse().map_err(|_| BrokerError::CorruptRecord)?),
    };
    let token = valid_uuid(next(&mut lines)?.to_string())?;
    let created_at: u64 = next(&mut lines)?
        .parse()
        .map_err(|_| BrokerError::CorruptRecord)?;
    let expires_at: u64 = next(&mut lines)?
        .parse()
        .map_err(|_| BrokerError::CorruptRecord)?;
    if expires_at != created_at.saturating_add(PREVIEW_TTL_SECS) {
        return Err(BrokerError::CorruptRecord);
    }
    let status = OperationStatus::parse(next(&mut lines)?)?;
    if preview_authorized_at > created_at
        || match (status, install_authorized_at) {
            (OperationStatus::Previewed, None) => false,
            (OperationStatus::Previewed, Some(_)) => true,
            (_, Some(authorized_at)) => authorized_at < created_at || authorized_at >= expires_at,
            (OperationStatus::Failed, None) => false,
            (_, None) => true,
        }
        || status.terminal() != finished_at.is_some()
        || finished_at.is_some_and(|finished| finished < created_at)
    {
        return Err(BrokerError::CorruptRecord);
    }
    let message = decode_string(next(&mut lines)?)?;
    let mut evidence = if version == 1 {
        // Old "succeeded" records never observed post-hook or boot success.
        ExecutionEvidence::legacy()
    } else {
        ExecutionEvidence {
            package_commit: CommitState::parse(next(&mut lines)?)
                .ok_or(BrokerError::CorruptRecord)?,
            hooks: VerificationState::parse(next(&mut lines)?).ok_or(BrokerError::CorruptRecord)?,
            boot_preparation: VerificationState::parse(next(&mut lines)?)
                .ok_or(BrokerError::CorruptRecord)?,
            pre_snapshot: parse_snapshot_id(next(&mut lines)?)?,
            post_snapshot: parse_snapshot_id(next(&mut lines)?)?,
            boot: BootVerification::default(),
            hook_details: String::new(),
            cleanup: Default::default(),
        }
    };
    if version >= 3 {
        evidence.boot = BootVerification {
            kernels: VerificationState::parse(next(&mut lines)?)
                .ok_or(BrokerError::CorruptRecord)?,
            dkms: VerificationState::parse(next(&mut lines)?).ok_or(BrokerError::CorruptRecord)?,
            initramfs: VerificationState::parse(next(&mut lines)?)
                .ok_or(BrokerError::CorruptRecord)?,
            limine: VerificationState::parse(next(&mut lines)?)
                .ok_or(BrokerError::CorruptRecord)?,
            new_kernel_boot: VerificationState::parse(next(&mut lines)?)
                .ok_or(BrokerError::CorruptRecord)?,
            running_kernel: decode_string(next(&mut lines)?)?,
            installed_kernels: decode_string(next(&mut lines)?)?,
            updated_kernels: decode_string(next(&mut lines)?)?,
            details: decode_string(next(&mut lines)?)?,
        };
    }
    if version >= 3 {
        evidence.hook_details = decode_string(next(&mut lines)?)?;
    }
    let count: usize = next(&mut lines)?
        .parse()
        .map_err(|_| BrokerError::CorruptRecord)?;
    if count > 100_000 {
        return Err(BrokerError::CorruptRecord);
    }
    let mut packages = Vec::with_capacity(count);
    for _ in 0..count {
        packages.push(UpdatePackageInfo {
            package_name: decode_string(next(&mut lines)?)?,
            current_version: decode_string(next(&mut lines)?)?,
            new_version: decode_string(next(&mut lines)?)?,
            repository: decode_string(next(&mut lines)?)?,
            download_size: next(&mut lines)?
                .parse()
                .map_err(|_| BrokerError::CorruptRecord)?,
            filename: String::new(),
            sha256: String::new(),
            repository_metadata_sha256: String::new(),
        });
    }
    let mut removals = Vec::new();
    if version >= 4 {
        let count: usize = next(&mut lines)?
            .parse()
            .map_err(|_| BrokerError::CorruptRecord)?;
        if count > 100_000 {
            return Err(BrokerError::CorruptRecord);
        }
        for _ in 0..count {
            removals.push(PackageRemoval {
                package_name: decode_string(next(&mut lines)?)?,
                current_version: decode_string(next(&mut lines)?)?,
                reason: decode_string(next(&mut lines)?)?,
                replacement: decode_string(next(&mut lines)?)?,
            });
        }
    }
    let removals_approved = if version >= 4 {
        match next(&mut lines)? {
            "1" => true,
            "0" => false,
            _ => return Err(BrokerError::CorruptRecord),
        }
    } else {
        false
    };
    if (removals_approved && (removals.is_empty() || status == OperationStatus::Previewed))
        || (!removals.is_empty() && status != OperationStatus::Previewed && !removals_approved)
    {
        return Err(BrokerError::CorruptRecord);
    }
    let policy_fingerprint = if version >= 5 {
        decode_string(next(&mut lines)?)?
    } else {
        String::new()
    };
    if version >= 5 {
        let count: usize = next(&mut lines)?
            .parse()
            .map_err(|_| BrokerError::CorruptRecord)?;
        if count != packages.len() {
            return Err(BrokerError::CorruptRecord);
        }
        for package in &mut packages {
            package.filename = decode_string(next(&mut lines)?)?;
            package.sha256 = decode_string(next(&mut lines)?)?;
            package.repository_metadata_sha256 = decode_string(next(&mut lines)?)?;
        }
    }
    let mut plan = CanonicalPlan::from_transaction(packages, removals)?;
    if !policy_fingerprint.is_empty() {
        plan = plan.with_policy_fingerprint(policy_fingerprint)?;
    } else if version >= 5 {
        return Err(BrokerError::CorruptRecord);
    }
    if version >= 6 {
        let status = next(&mut lines)?.to_string();
        if !["not-run", "skipped-unverified", "completed", "warnings"].contains(&status.as_str()) {
            return Err(BrokerError::CorruptRecord);
        }
        evidence.cleanup = crate::cache_cleanup::CleanupReport {
            status,
            removed_files: next(&mut lines)?
                .parse()
                .map_err(|_| BrokerError::CorruptRecord)?,
            removed_bytes: next(&mut lines)?
                .parse()
                .map_err(|_| BrokerError::CorruptRecord)?,
            reclaimed_bytes: next(&mut lines)?
                .parse()
                .map_err(|_| BrokerError::CorruptRecord)?,
            details: decode_string(next(&mut lines)?)?,
        };
    }
    if lines.next().is_some() {
        return Err(BrokerError::CorruptRecord);
    }
    if install_authorized_at.is_none()
        && status == OperationStatus::Failed
        && (evidence.package_commit != CommitState::NotStarted
            || !plan.removals().is_empty()
            || plan.packages().is_empty()
            || plan.packages().iter().any(|p| {
                !matches!(p.repository.as_str(), "aur-local" | "aur-build-reservation")
            }))
    {
        return Err(BrokerError::CorruptRecord);
    }
    let plan = plan;
    Ok(Record {
        operation_id,
        owner,
        preview_authorized_at,
        install_authorized_at,
        finished_at,
        token,
        created_at,
        expires_at,
        plan,
        removals_approved,
        status,
        message,
        evidence,
    })
}

fn parse_snapshot_id(value: &str) -> Result<Option<String>, BrokerError> {
    if value == "-" {
        return Ok(None);
    }
    if value
        .parse::<u32>()
        .is_ok_and(|id| id > 0 && id.to_string() == value)
    {
        Ok(Some(value.to_string()))
    } else {
        Err(BrokerError::CorruptRecord)
    }
}

fn next<'a>(lines: &mut impl Iterator<Item = &'a str>) -> Result<&'a str, BrokerError> {
    lines.next().ok_or(BrokerError::CorruptRecord)
}

fn encode_hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut output = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        output.push(HEX[(byte >> 4) as usize] as char);
        output.push(HEX[(byte & 0x0f) as usize] as char);
    }
    output
}

fn decode_string(value: &str) -> Result<String, BrokerError> {
    let (pairs, remainder) = value.as_bytes().as_chunks::<2>();
    if !remainder.is_empty() {
        return Err(BrokerError::CorruptRecord);
    }
    let mut bytes = Vec::with_capacity(value.len() / 2);
    for pair in pairs {
        let high = hex_nibble(pair[0]).ok_or(BrokerError::CorruptRecord)?;
        let low = hex_nibble(pair[1]).ok_or(BrokerError::CorruptRecord)?;
        bytes.push((high << 4) | low);
    }
    String::from_utf8(bytes).map_err(|_| BrokerError::CorruptRecord)
}

fn hex_nibble(value: u8) -> Option<u8> {
    match value {
        b'0'..=b'9' => Some(value - b'0'),
        b'a'..=b'f' => Some(value - b'a' + 10),
        b'A'..=b'F' => Some(value - b'A' + 10),
        _ => None,
    }
}

fn io_error(error: std::io::Error) -> BrokerError {
    BrokerError::Io(error.to_string())
}

// Shared with the isolated native worker; unchanged uncertain-outcome gate.
impl BrokerStore {
    pub(crate) fn require_no_uncertain_commit(&self) -> Result<(), BrokerError> {
        self.with_lock(|| {
            for entry in fs::read_dir(&self.root).map_err(io_error)? {
                let entry = entry.map_err(io_error)?;
                if !entry
                    .file_name()
                    .to_string_lossy()
                    .starts_with("operation-")
                {
                    continue;
                }
                let record = self.read_record(&entry.path())?;
                if matches!(
                    record.evidence.package_commit,
                    CommitState::Unknown | CommitState::InProgress | CommitState::Failed
                ) {
                    return Err(BrokerError::OperationInProgress);
                }
            }
            Ok(())
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::Barrier;
    use std::thread;

    fn strip_v5_integrity_extension(lines: &mut Vec<String>) {
        lines.truncate(lines.len() - 5); // Remove v6 cleanup extension first.
        let packages: usize = lines[28].parse().unwrap();
        lines.truncate(lines.len() - 2 - packages * 3);
    }

    struct TestTokens(AtomicUsize);

    impl TokenSource for TestTokens {
        fn next_token(&self) -> Result<String, BrokerError> {
            let next = self.0.fetch_add(1, Ordering::SeqCst) + 1;
            Ok(format!("00000000-0000-4000-8000-{next:012x}"))
        }
    }

    struct TempStore(PathBuf);

    impl TempStore {
        fn new() -> (Self, Arc<BrokerStore>) {
            let path = std::env::temp_dir().join(format!("update-broker-test-{}", Uuid::new_v4()));
            let owner = unsafe { libc::geteuid() };
            let store = BrokerStore::open_with(
                path.clone(),
                owner,
                Arc::new(TestTokens(AtomicUsize::new(0))),
            )
            .unwrap();
            (Self(path), Arc::new(store))
        }
    }

    impl Drop for TempStore {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn identity(uid: u32, session: &str) -> VerifiedIdentity {
        VerifiedIdentity::test_identity(format!(":1.{}", uid + 7), uid, session.to_string())
            .unwrap()
    }

    fn identity_for_bus(bus_name: &str, uid: u32, session: &str) -> VerifiedIdentity {
        VerifiedIdentity::test_identity(bus_name.to_string(), uid, session.to_string()).unwrap()
    }

    fn plan() -> CanonicalPlan {
        CanonicalPlan::from_packages(vec![UpdatePackageInfo {
            package_name: "demo".to_string(),
            current_version: "1.0-1".to_string(),
            new_version: "1.1-1".to_string(),
            repository: "core".to_string(),
            download_size: 4096,
            filename: "demo.pkg.tar.zst".into(),
            sha256: "a".repeat(64),
            repository_metadata_sha256: "c".repeat(64),
        }])
        .unwrap()
        .with_policy_fingerprint("b".repeat(64))
        .unwrap()
    }

    fn replacement_plan() -> CanonicalPlan {
        CanonicalPlan::from_transaction(
            plan().packages().to_vec(),
            vec![PackageRemoval {
                package_name: "old-demo".into(),
                current_version: "1-1".into(),
                reason: "replacement".into(),
                replacement: "demo".into(),
            }],
        )
        .unwrap()
        .with_policy_fingerprint("b".repeat(64))
        .unwrap()
    }

    #[test]
    fn progress_is_visible_after_reopen_and_never_rewrites_terminal_results() {
        let (temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        let plan = plan();
        store
            .create_preview(
                AuthorizedCaller::test_authorized(owner.clone(), PolicyAction::Preview, 1000),
                plan.clone(),
                1000,
            )
            .unwrap();
        let consumed = store
            .consume_approved_preview(
                AuthorizedCaller::test_authorized(owner.clone(), PolicyAction::Install, 1001),
                1001,
                &plan.hash(),
                false,
            )
            .unwrap();
        store
            .update_worker_progress(&consumed.operation_id, "Refreshing repositories")
            .unwrap();
        let reopened = BrokerStore::open_with(
            temp.0.clone(),
            unsafe { libc::geteuid() },
            Arc::new(UuidTokenSource),
        )
        .unwrap();
        let queued = reopened
            .operation_for_owner(&owner, &consumed.operation_id)
            .unwrap();
        assert_eq!(queued.status, OperationStatus::Queued);
        assert_eq!(queued.message, "Refreshing repositories");
        store
            .claim_transaction(&consumed.operation_id, &plan, 1002)
            .unwrap();
        store
            .update_worker_progress(&consumed.operation_id, "upgrading demo (50%)")
            .unwrap();
        assert_eq!(
            reopened
                .operation_for_owner(&owner, &consumed.operation_id)
                .unwrap()
                .message,
            "upgrading demo (50%)"
        );
        store
            .finish_worker_result(
                &consumed.operation_id,
                OperationStatus::Failed,
                "signature failed",
                1003,
            )
            .unwrap();
        assert_eq!(
            store.update_worker_progress(&consumed.operation_id, "late event"),
            Err(BrokerError::InvalidTransition)
        );
        assert_eq!(
            reopened
                .operation_for_owner(&owner, &consumed.operation_id)
                .unwrap()
                .message,
            "signature failed"
        );
    }

    #[test]
    fn replacement_requires_exact_hash_explicit_consent_and_install_authority() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        let plan = replacement_plan();
        store
            .create_preview(
                AuthorizedCaller::test_authorized(owner.clone(), PolicyAction::Preview, 1000),
                plan.clone(),
                1000,
            )
            .unwrap();
        for (action, hash, approve) in [
            (PolicyAction::Install, plan.hash(), false),
            (PolicyAction::Install, "wrong-plan".into(), true),
            (PolicyAction::Preview, plan.hash(), true),
        ] {
            assert_eq!(
                store.consume_approved_preview(
                    AuthorizedCaller::test_authorized(owner.clone(), action, 1001),
                    1001,
                    &hash,
                    approve
                ),
                Err(BrokerError::AuthorizationDenied)
            );
        }
        let other = identity(1001, "session-a");
        assert_eq!(
            store.consume_approved_preview(
                AuthorizedCaller::test_authorized(other, PolicyAction::Install, 1001),
                1001,
                &plan.hash(),
                true
            ),
            Err(BrokerError::NoPreview)
        );
        let authorize =
            || AuthorizedCaller::test_authorized(owner.clone(), PolicyAction::Install, 1001);
        let consumed = store
            .consume_approved_preview(authorize(), 1001, &plan.hash(), true)
            .unwrap();
        assert_eq!(
            store.consume_approved_preview(authorize(), 1001, &plan.hash(), true),
            Err(BrokerError::NoPreview)
        );
        let path = store.operation_path(&consumed.operation_id).unwrap();
        let approved_record = store.read_record(&path).unwrap();
        assert!(approved_record.removals_approved);
        let mut invalid_record = approved_record.clone();
        invalid_record.removals_approved = false;
        assert_eq!(
            decode_record(&encode_record(&invalid_record)).unwrap_err(),
            BrokerError::CorruptRecord
        );
        assert_eq!(
            store
                .claim_transaction(&consumed.operation_id, &plan, 1002)
                .unwrap(),
            plan
        );
        assert_eq!(
            store.claim_transaction(&consumed.operation_id, &plan, 1003),
            Err(BrokerError::InvalidTransition)
        );
    }

    #[test]
    fn replacement_removal_drift_is_rejected_before_worker_claim() {
        for mutation in 0..5 {
            let (_temp, store) = TempStore::new();
            let owner = identity(1000, "session-a");
            let base = replacement_plan();
            let mut additions = base.packages().to_vec();
            additions.push(UpdatePackageInfo {
                package_name: "alternate".into(),
                ..additions[0].clone()
            });
            let approved = CanonicalPlan::from_transaction(additions, base.removals().to_vec())
                .unwrap()
                .with_policy_fingerprint(base.policy_fingerprint().to_string())
                .unwrap();
            store
                .create_preview(
                    AuthorizedCaller::test_authorized(owner.clone(), PolicyAction::Preview, 1000),
                    approved.clone(),
                    1000,
                )
                .unwrap();
            let consumed = store
                .consume_approved_preview(
                    AuthorizedCaller::test_authorized(owner, PolicyAction::Install, 1001),
                    1001,
                    &approved.hash(),
                    true,
                )
                .unwrap();
            let mut removals = approved.removals().to_vec();
            match mutation {
                0 => removals.clear(),
                1 => removals[0].current_version = "2-1".into(),
                2 => removals[0].package_name = "different-demo".into(),
                4 => removals[0].replacement = "alternate".into(),
                _ => removals.push(PackageRemoval {
                    package_name: "old-alt".into(),
                    current_version: "1-1".into(),
                    reason: "replacement".into(),
                    replacement: "alternate".into(),
                }),
            }
            let current =
                CanonicalPlan::from_transaction(approved.packages().to_vec(), removals).unwrap();
            assert_ne!(current.hash(), approved.hash());
            assert_eq!(
                store.claim_transaction(&consumed.operation_id, &current, 1002),
                Err(BrokerError::PlanChanged)
            );
        }
    }

    #[test]
    fn critical_ambiguous_conflict_and_dependency_removals_are_not_authorizable() {
        let base = replacement_plan();
        for name in [
            "pacman",
            "linux-cachyos",
            "systemd",
            "limine",
            "snapper",
            "glibc",
        ] {
            let mut r = base.removals().to_vec();
            r[0].package_name = name.into();
            assert_eq!(
                CanonicalPlan::from_transaction(base.packages().to_vec(), r),
                Err(BrokerError::InvalidPlan)
            );
        }
        for reason in ["conflict", "dependency", "unknown"] {
            let mut r = base.removals().to_vec();
            r[0].reason = reason.into();
            assert_eq!(
                CanonicalPlan::from_transaction(base.packages().to_vec(), r),
                Err(BrokerError::InvalidPlan)
            );
        }
        let mut r = base.removals().to_vec();
        r.push(r[0].clone());
        assert_eq!(
            CanonicalPlan::from_transaction(base.packages().to_vec(), r),
            Err(BrokerError::InvalidPlan)
        );
        let mut r = base.removals().to_vec();
        r[0].replacement = "absent".into();
        assert_eq!(
            CanonicalPlan::from_transaction(base.packages().to_vec(), r),
            Err(BrokerError::InvalidPlan)
        );
    }

    #[test]
    fn canonical_hash_covers_removal_edges_and_survives_record_roundtrip() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        let replacement = replacement_plan();
        assert_ne!(replacement.hash(), plan().hash());
        store
            .create_preview(
                AuthorizedCaller::test_authorized(owner.clone(), PolicyAction::Preview, 1000),
                replacement.clone(),
                1000,
            )
            .unwrap();
        assert_eq!(
            store.read_record(&store.preview_path(&owner)).unwrap().plan,
            replacement
        );
        let mut r = replacement.removals().to_vec();
        r[0].current_version.push('x');
        assert_ne!(
            CanonicalPlan::from_transaction(replacement.packages().to_vec(), r)
                .unwrap()
                .hash(),
            replacement.hash()
        );
    }

    #[test]
    fn archive_repository_and_policy_identity_are_bound_to_the_one_use_plan() {
        let owner = identity(1000, "session-a");
        let original = plan();
        for field in 0..4 {
            let (_temp, store) = TempStore::new();
            store
                .create_preview(
                    AuthorizedCaller::test_authorized(owner.clone(), PolicyAction::Preview, 1000),
                    original.clone(),
                    1000,
                )
                .unwrap();
            let consumed = store
                .consume_approved_preview(
                    AuthorizedCaller::test_authorized(owner.clone(), PolicyAction::Install, 1001),
                    1001,
                    &original.hash(),
                    false,
                )
                .unwrap();
            let mut packages = original.packages().to_vec();
            let mut policy = original.policy_fingerprint().to_string();
            match field {
                0 => packages[0].filename = "replaced.pkg.tar.zst".into(),
                1 => packages[0].sha256.replace_range(..1, "d"),
                2 => packages[0]
                    .repository_metadata_sha256
                    .replace_range(..1, "e"),
                _ => policy.replace_range(..1, "f"),
            }
            let changed = CanonicalPlan::from_packages(packages)
                .unwrap()
                .with_policy_fingerprint(policy)
                .unwrap();
            assert_ne!(changed.hash(), original.hash());
            assert_eq!(
                store.claim_transaction(&consumed.operation_id, &changed, 1002),
                Err(BrokerError::PlanChanged)
            );
        }
    }

    #[test]
    fn reconnect_can_only_read_same_user_session_and_cannot_consume_old_token() {
        let (_temp, store) = TempStore::new();
        let owner = identity_for_bus(":1.10", 1000, "session-a");
        make_preview(&store, &owner);
        let op = consume(&store, &owner, 1001).unwrap();
        let reconnected = identity_for_bus(":1.20", 1000, "session-a");
        assert_eq!(
            store
                .resumable_operation(&reconnected, None)
                .unwrap()
                .operation_id,
            op.operation_id
        );
        assert!(store
            .operation_for_owner(&reconnected, &op.operation_id)
            .is_err());
        assert!(consume(&store, &reconnected, 1002).is_err());
        for other in [identity(1001, "session-a"), identity(1000, "session-b")] {
            assert!(store
                .resumable_operation(&other, Some(&op.operation_id))
                .is_err());
            assert!(matches!(
                store.resumable_operation(&other, None),
                Err(BrokerError::NotFound)
            ));
        }
    }

    #[test]
    fn plan_difference_identifies_real_security_changes_without_weakening_equality() {
        let approved = plan();
        for (index, label) in [
            "new_version",
            "current_version",
            "repository",
            "filename",
            "archive_size",
            "sha256",
            "repository_metadata_sha256",
        ]
        .iter()
        .enumerate()
        {
            let mut packages = approved.packages().to_vec();
            let package = &mut packages[0];
            match index {
                0 => package.new_version = "2-1".into(),
                1 => package.current_version = "0-1".into(),
                2 => package.repository = "extra".into(),
                3 => package.filename = "different.pkg.tar.zst".into(),
                4 => package.download_size += 1,
                5 => package.sha256 = "d".repeat(64),
                _ => package.repository_metadata_sha256 = "e".repeat(64),
            }
            let changed = CanonicalPlan::from_packages(packages)
                .unwrap()
                .with_policy_fingerprint(approved.policy_fingerprint().into())
                .unwrap();
            assert_ne!(approved.hash(), changed.hash());
            let summary = approved.difference_summary(&changed).unwrap();
            assert!(summary.contains(label), "{summary}");
            assert!(summary.contains("demo."));
            let (_temp, store) = TempStore::new();
            let owner = identity(1000, "session-a");
            make_preview(&store, &owner);
            let operation = consume(&store, &owner, 1001).unwrap();
            assert_eq!(
                store.claim_transaction(&operation.operation_id, &changed, 1002),
                Err(BrokerError::PlanChanged)
            );
            let view = store
                .operation_for_owner(&owner, &operation.operation_id)
                .unwrap();
            assert!(view.message.contains(label));
            assert_eq!(view.evidence.package_commit, CommitState::NotStarted);
        }
        let changed = approved
            .clone()
            .with_policy_fingerprint("f".repeat(64))
            .unwrap();
        assert!(approved
            .difference_summary(&changed)
            .unwrap()
            .contains("policy_fingerprint"));
        assert_ne!(approved.hash(), changed.hash());
        assert!(approved.difference_summary(&approved).is_none());
    }

    #[test]
    fn version_five_without_cleanup_stays_readable_and_does_not_invent_results() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let bytes = fs::read_to_string(store.preview_path(&owner)).unwrap();
        let mut lines: Vec<_> = bytes.lines().map(str::to_owned).collect();
        lines[0] = V5_RECORD_VERSION.into();
        lines.truncate(lines.len() - 5);
        let decoded = decode_record((lines.join("\n") + "\n").as_bytes()).unwrap();
        assert_eq!(
            decoded.evidence.cleanup,
            crate::cache_cleanup::CleanupReport::default()
        );
        assert_eq!(decoded.plan, plan());
    }

    #[test]
    fn version_three_is_readable_as_add_only_without_fabricating_consent() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let bytes = fs::read_to_string(store.preview_path(&owner)).unwrap();
        let mut lines: Vec<_> = bytes.lines().map(str::to_string).collect();
        lines[0] = V3_RECORD_VERSION.into();
        strip_v5_integrity_extension(&mut lines);
        lines.pop();
        lines.pop();
        let decoded = decode_record(lines.join("\n").as_bytes()).unwrap();
        assert!(decoded.plan.removals().is_empty());
        assert_eq!(decoded.plan.packages()[0].package_name, "demo");
        assert!(decoded.plan.packages()[0].sha256.is_empty());
        assert!(decoded.plan.policy_fingerprint().is_empty());
    }

    fn make_preview(store: &BrokerStore, owner: &VerifiedIdentity) {
        let authorization =
            AuthorizedCaller::test_authorized(owner.clone(), PolicyAction::Preview, 1000);
        store.create_preview(authorization, plan(), 1000).unwrap();
    }

    fn consume(
        store: &BrokerStore,
        owner: &VerifiedIdentity,
        now: u64,
    ) -> Result<ConsumedPreview, BrokerError> {
        let authorization =
            AuthorizedCaller::test_authorized(owner.clone(), PolicyAction::Install, now);
        store.consume_preview(authorization, now)
    }

    #[test]
    fn execution_evidence_survives_reopen_and_normal_stop() {
        let (temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let op = consume(&store, &owner, 1001).unwrap();
        assert_eq!(
            store.record_execution_evidence(&op.operation_id, &ExecutionEvidence::default()),
            Err(BrokerError::InvalidTransition)
        );
        store
            .claim_worker(&op.operation_id, plan().packages(), 1002)
            .unwrap();
        let evidence = ExecutionEvidence {
            package_commit: CommitState::Committed,
            pre_snapshot: Some("301".into()),
            post_snapshot: Some("302".into()),
            boot: BootVerification {
                kernels: VerificationState::Verified,
                dkms: VerificationState::Failed,
                running_kernel: "old-running".into(),
                installed_kernels: "new-installed".into(),
                updated_kernels: "new-installed".into(),
                details: "DKMS mismatch\nBoot is not tested".into(),
                ..Default::default()
            },
            hook_details: "started: fixture\nreturned (exit status unavailable): fixture".into(),
            cleanup: crate::cache_cleanup::CleanupReport {
                status: "warnings".into(),
                removed_files: 3,
                removed_bytes: 4000,
                reclaimed_bytes: 2048,
                details: "One busy archive retained".into(),
            },
            ..Default::default()
        };
        store
            .record_execution_evidence(&op.operation_id, &evidence)
            .unwrap();
        store
            .finish_worker_result(
                &op.operation_id,
                OperationStatus::CompletedWithWarnings,
                "unverified hooks and boot",
                1003,
            )
            .unwrap();
        drop(store);
        let reopened = BrokerStore::open_with(
            temp.0.clone(),
            unsafe { libc::geteuid() },
            Arc::new(TestTokens(AtomicUsize::new(10))),
        )
        .unwrap();
        reopened
            .worker_process_stopped(&op.operation_id, 1004)
            .unwrap();
        let view = reopened
            .operation_for_owner(&owner, &op.operation_id)
            .unwrap();
        assert_eq!(view.status, OperationStatus::CompletedWithWarnings);
        assert_eq!(view.evidence, evidence);
        assert!(!view.evidence.fully_verified());
        assert_eq!(
            reopened.record_execution_evidence(&op.operation_id, &ExecutionEvidence::default()),
            Err(BrokerError::InvalidTransition)
        );
    }

    #[test]
    fn interrupted_commit_is_unknown_and_never_replayable() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let op = consume(&store, &owner, 1001).unwrap();
        store
            .claim_worker(&op.operation_id, plan().packages(), 1002)
            .unwrap();
        store
            .record_execution_evidence(
                &op.operation_id,
                &ExecutionEvidence {
                    package_commit: CommitState::InProgress,
                    pre_snapshot: Some("301".into()),
                    ..Default::default()
                },
            )
            .unwrap();
        store
            .worker_process_stopped(&op.operation_id, 1003)
            .unwrap();
        let view = store.operation_for_owner(&owner, &op.operation_id).unwrap();
        assert_eq!(view.evidence.package_commit, CommitState::Unknown);
        assert_eq!(view.evidence.pre_snapshot.as_deref(), Some("301"));
        assert_eq!(view.status, OperationStatus::Failed);
        assert!(store
            .claim_worker(&op.operation_id, plan().packages(), 1004)
            .is_err());
    }

    #[test]
    fn legacy_success_does_not_invent_execution_evidence() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let op = consume(&store, &owner, 1001).unwrap();
        store
            .claim_worker(&op.operation_id, plan().packages(), 1002)
            .unwrap();
        store
            .finish_worker_result(
                &op.operation_id,
                OperationStatus::Succeeded,
                "old success",
                1003,
            )
            .unwrap();
        let path = store.operation_path(&op.operation_id).unwrap();
        let mut lines: Vec<_> = fs::read_to_string(&path)
            .unwrap()
            .lines()
            .map(str::to_string)
            .collect();
        strip_v5_integrity_extension(&mut lines);
        lines[0] = LEGACY_RECORD_VERSION.into();
        lines.pop();
        lines.pop();
        lines.drain(13..28);
        fs::write(&path, lines.join("\n")).unwrap();
        let view = store.operation_for_owner(&owner, &op.operation_id).unwrap();
        assert_eq!(view.evidence, ExecutionEvidence::legacy());
        assert!(!view.evidence.fully_verified());
    }

    #[test]
    fn format_two_preserves_commit_but_has_no_boot_artifact_proof() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let path = store.preview_path(&owner);
        let mut lines: Vec<_> = fs::read_to_string(&path)
            .unwrap()
            .lines()
            .map(str::to_string)
            .collect();
        strip_v5_integrity_extension(&mut lines);
        lines[0] = PREVIOUS_RECORD_VERSION.into();
        lines.pop();
        lines.pop();
        lines[13] = "committed".into();
        lines.drain(18..28);
        let record = decode_record(lines.join("\n").as_bytes()).unwrap();
        assert_eq!(record.evidence.package_commit, CommitState::Committed);
        assert_eq!(record.evidence.boot, BootVerification::default());
        assert!(!record.evidence.fully_verified());
    }

    #[test]
    fn corrupt_execution_enum_and_snapshot_are_rejected() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let path = store.preview_path(&owner);
        let original = fs::read_to_string(&path).unwrap();
        for (line, value) in [
            (13, "success-without-proof"),
            (14, "ok"),
            (16, "not-a-snapshot"),
            (18, "invented-kernel-proof"),
            (22, "reboot-success-without-proof"),
        ] {
            let mut lines: Vec<_> = original.lines().map(str::to_string).collect();
            lines[line] = value.into();
            assert_eq!(
                decode_record(lines.join("\n").as_bytes()).unwrap_err(),
                BrokerError::CorruptRecord
            );
        }
    }

    #[test]
    fn canonical_plan_sorts_and_rejects_duplicate_package_names() {
        let one = UpdatePackageInfo {
            package_name: "a".into(),
            current_version: "1".into(),
            new_version: "2".into(),
            repository: "core".into(),
            download_size: 1,
            filename: String::new(),
            sha256: String::new(),
            repository_metadata_sha256: String::new(),
        };
        let two = UpdatePackageInfo {
            package_name: "b".into(),
            ..one.clone()
        };
        let canonical = CanonicalPlan::from_packages(vec![two.clone(), one.clone()]).unwrap();
        assert_eq!(canonical.packages()[0].package_name, "a");
        assert!(CanonicalPlan::from_packages(vec![one.clone(), one]).is_err());
    }

    #[test]
    fn concurrent_consumers_can_claim_a_preview_only_once() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let barrier = Arc::new(Barrier::new(3));
        let mut threads = Vec::new();
        for _ in 0..2 {
            let store = Arc::clone(&store);
            let owner = owner.clone();
            let barrier = Arc::clone(&barrier);
            threads.push(thread::spawn(move || {
                barrier.wait();
                consume(&store, &owner, 1001)
            }));
        }
        barrier.wait();
        let outcomes: Vec<_> = threads
            .into_iter()
            .map(|thread| thread.join().unwrap())
            .collect();
        assert_eq!(outcomes.iter().filter(|result| result.is_ok()).count(), 1);
        assert_eq!(
            outcomes
                .iter()
                .filter(|result| **result == Err(BrokerError::NoPreview))
                .count(),
            1
        );
    }

    #[test]
    fn a_second_operation_is_rejected_while_another_is_queued_or_running() {
        let (_temp, store) = TempStore::new();
        let first = identity(1000, "session-a");
        let second = identity(1001, "session-b");
        make_preview(&store, &first);
        make_preview(&store, &second);
        consume(&store, &first, 1001).unwrap();
        assert_eq!(
            consume(&store, &second, 1001),
            Err(BrokerError::OperationInProgress)
        );
    }

    #[test]
    fn consumed_token_cannot_be_replayed() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let operation = consume(&store, &owner, 1001).unwrap();
        assert_eq!(consume(&store, &owner, 1002), Err(BrokerError::NoPreview));
        assert_eq!(
            store
                .operation_for_owner(&owner, &operation.operation_id)
                .unwrap()
                .status,
            OperationStatus::Queued
        );
    }

    #[test]
    fn wrong_or_stale_mock_policy_decision_cannot_advance_the_record() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        let denied = AuthorizedCaller::test_authorized(owner.clone(), PolicyAction::Install, 1000);
        assert_eq!(
            store.create_preview(denied, plan(), 1000),
            Err(BrokerError::AuthorizationDenied)
        );
        make_preview(&store, &owner);
        let stale = AuthorizedCaller::test_authorized(owner.clone(), PolicyAction::Install, 900);
        assert_eq!(
            store.consume_preview(stale, 1001),
            Err(BrokerError::AuthorizationDenied)
        );
        let operation = consume(&store, &owner, 1001).unwrap();
        assert_eq!(
            store
                .operation_for_owner(&owner, &operation.operation_id)
                .unwrap()
                .status,
            OperationStatus::Queued
        );
    }

    #[test]
    fn records_survive_broker_process_reopen_within_the_same_boot() {
        let (temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        drop(store);
        let reopened = BrokerStore::open_with(
            temp.0.clone(),
            unsafe { libc::geteuid() },
            Arc::new(TestTokens(AtomicUsize::new(10))),
        )
        .unwrap();
        let operation = consume(&reopened, &owner, 1001).unwrap();
        assert_eq!(operation.plan, plan());
        assert_eq!(
            reopened
                .operation_for_owner(&owner, &operation.operation_id)
                .unwrap()
                .status,
            OperationStatus::Queued
        );
    }

    #[test]
    fn expired_preview_is_removed_and_rejected() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        assert_eq!(consume(&store, &owner, 1300), Err(BrokerError::Expired));
        assert_eq!(consume(&store, &owner, 1301), Err(BrokerError::NoPreview));
    }

    #[test]
    fn uid_or_session_mismatch_cannot_consume_or_read_operation() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let sender = owner.bus_name.clone();
        assert_eq!(
            consume(&store, &identity_for_bus(&sender, 1001, "session-a"), 1001),
            Err(BrokerError::IdentityMismatch)
        );
        assert_eq!(
            consume(&store, &identity_for_bus(&sender, 1000, "session-b"), 1001),
            Err(BrokerError::IdentityMismatch)
        );
        let operation = consume(&store, &owner, 1001).unwrap();
        assert_eq!(
            store.operation_for_owner(&identity(1001, "session-a"), &operation.operation_id),
            Err(BrokerError::IdentityMismatch)
        );
    }

    #[test]
    fn changed_plan_fails_worker_claim_without_running_it() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let operation = consume(&store, &owner, 1001).unwrap();
        let mut changed = plan().packages().to_vec();
        changed[0].new_version = "1.2-1".into();
        assert_eq!(
            store.claim_worker(&operation.operation_id, &changed, 1002),
            Err(BrokerError::PlanChanged)
        );
        assert_eq!(
            store
                .operation_for_owner(&owner, &operation.operation_id)
                .unwrap()
                .status,
            OperationStatus::Failed
        );
    }

    #[test]
    fn malformed_records_and_unsafe_permissions_are_rejected() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let path = store.preview_path(&owner);
        fs::write(&path, b"broken").unwrap();
        assert_eq!(
            consume(&store, &owner, 1001),
            Err(BrokerError::CorruptRecord)
        );

        make_preview(&store, &owner);
        fs::set_permissions(&path, fs::Permissions::from_mode(0o644)).unwrap();
        assert_eq!(consume(&store, &owner, 1001), Err(BrokerError::UnsafeStore));

        make_preview(&store, &owner);
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        let backup = path.with_extension("record.saved");
        fs::rename(&path, &backup).unwrap();
        std::os::unix::fs::symlink(&backup, &path).unwrap();
        assert_eq!(consume(&store, &owner, 1001), Err(BrokerError::UnsafeStore));
    }

    #[test]
    fn only_owner_can_read_status_and_worker_claim_is_single_use() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let operation = consume(&store, &owner, 1001).unwrap();
        assert!(store
            .operation_for_owner(&identity(2000, "session-z"), &operation.operation_id)
            .is_err());
        let actual = operation.plan.packages().to_vec();
        assert!(store
            .claim_worker(&operation.operation_id, &actual, 1002)
            .is_ok());
        assert_eq!(
            store.claim_worker(&operation.operation_id, &actual, 1002),
            Err(BrokerError::InvalidTransition)
        );
    }

    #[test]
    fn broker_reopen_keeps_running_record_and_interruption_is_terminal() {
        let (temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let operation = consume(&store, &owner, 1001).unwrap();
        store
            .claim_worker(&operation.operation_id, operation.plan.packages(), 1002)
            .unwrap();

        drop(store);
        let reopened = BrokerStore::open_with(
            temp.0.clone(),
            unsafe { libc::geteuid() },
            Arc::new(TestTokens(AtomicUsize::new(20))),
        )
        .unwrap();
        assert_eq!(
            reopened
                .operation_for_owner(&owner, &operation.operation_id)
                .unwrap()
                .status,
            OperationStatus::Running
        );
        reopened
            .worker_process_stopped(&operation.operation_id, 1003)
            .unwrap();
        assert_eq!(
            reopened
                .operation_for_owner(&owner, &operation.operation_id)
                .unwrap()
                .status,
            OperationStatus::Failed
        );
        assert_eq!(
            reopened.claim_worker(&operation.operation_id, operation.plan.packages(), 1004),
            Err(BrokerError::InvalidTransition)
        );
    }

    #[test]
    fn queued_worker_interruption_and_normal_stop_hook_are_safe() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let operation = consume(&store, &owner, 1001).unwrap();

        store
            .worker_process_stopped(&operation.operation_id, 1002)
            .unwrap();
        assert_eq!(
            store
                .operation_for_owner(&owner, &operation.operation_id)
                .unwrap()
                .status,
            OperationStatus::Failed
        );
        // systemd calls ExecStopPost after successful and failed exits. A
        // terminal result must remain unchanged and the call must be safe.
        store
            .update_worker_status(
                &operation.operation_id,
                OperationStatus::Failed,
                "transaction failed",
                1003,
            )
            .unwrap_err();
        store
            .worker_process_stopped(&operation.operation_id, 1004)
            .unwrap();
        assert_eq!(
            store
                .operation_for_owner(&owner, &operation.operation_id)
                .unwrap()
                .message,
            "Worker stopped before recording completion. The package transaction may have partially or fully applied; inspect pacman logs before any manual recovery. No automatic retry was made."
        );

        make_preview(&store, &owner);
        let completed = consume(&store, &owner, 1005).unwrap();
        store
            .claim_worker(&completed.operation_id, completed.plan.packages(), 1006)
            .unwrap();
        store
            .finish_worker_result(
                &completed.operation_id,
                OperationStatus::Succeeded,
                "done",
                1007,
            )
            .unwrap();
        store
            .worker_process_stopped(&completed.operation_id, 1008)
            .unwrap();
        assert_eq!(
            store
                .operation_for_owner(&owner, &completed.operation_id)
                .unwrap()
                .status,
            OperationStatus::Succeeded
        );
    }

    #[test]
    fn duplicate_workers_cannot_claim_one_queued_operation() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let operation = consume(&store, &owner, 1001).unwrap();
        let plan = operation.plan.packages().to_vec();
        let barrier = Arc::new(Barrier::new(3));
        let mut threads = Vec::new();
        for _ in 0..2 {
            let store = Arc::clone(&store);
            let barrier = Arc::clone(&barrier);
            let id = operation.operation_id.clone();
            let plan = plan.clone();
            threads.push(thread::spawn(move || {
                barrier.wait();
                store.claim_worker(&id, &plan, 1002)
            }));
        }
        barrier.wait();
        let results: Vec<_> = threads
            .into_iter()
            .map(|thread| thread.join().unwrap())
            .collect();
        assert_eq!(results.iter().filter(|result| result.is_ok()).count(), 1);
        assert_eq!(
            results
                .iter()
                .filter(|result| **result == Err(BrokerError::InvalidTransition))
                .count(),
            1
        );
    }

    #[test]
    fn worker_start_failure_can_retry_same_id_but_not_replay_worker_claim() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let operation = consume(&store, &owner, 1001).unwrap();
        store
            .worker_start_failed(&operation.operation_id, "systemd unavailable")
            .unwrap();
        assert_eq!(
            store
                .operation_for_owner(&owner, &operation.operation_id)
                .unwrap()
                .status,
            OperationStatus::WorkerStartFailed
        );
        store
            .retry_worker_start(&owner, &operation.operation_id)
            .unwrap();
        store
            .claim_worker(&operation.operation_id, operation.plan.packages(), 1002)
            .unwrap();
        assert_eq!(
            store.retry_worker_start(&owner, &operation.operation_id),
            Err(BrokerError::InvalidTransition)
        );
    }

    #[test]
    fn duplicate_worker_lease_cannot_publish_failure_or_touch_plan() {
        // Concurrent tests spawn processes. fork briefly inherits other
        // threads' open descriptions before CLOEXEC, so dropping a File in
        // this process alone need not release its flock immediately. Exercise
        // the exact same assertions in a single-test process; do not add
        // retries, sleeps or weaker production lease semantics.
        if std::env::var_os("ZETHROPOL_LEASE_ISOLATED_CHILD").is_none() {
            let result = std::process::Command::new(std::env::current_exe().unwrap())
                .args([
                    "--exact",
                    "broker_protocol::tests::duplicate_worker_lease_cannot_publish_failure_or_touch_plan",
                    "--test-threads=1",
                ])
                .env("ZETHROPOL_LEASE_ISOLATED_CHILD", "1")
                .output()
                .unwrap();
            assert!(
                result.status.success(),
                "{} {}",
                String::from_utf8_lossy(&result.stdout),
                String::from_utf8_lossy(&result.stderr)
            );
            return;
        }
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let op = consume(&store, &owner, 1001).unwrap();
        let lease = store.worker_lease(&op.operation_id).unwrap();
        store
            .claim_worker(&op.operation_id, op.plan.packages(), 1002)
            .unwrap();
        assert!(matches!(
            store.worker_lease(&op.operation_id),
            Err(BrokerError::OperationInProgress)
        ));
        assert_eq!(
            store
                .operation_for_owner(&owner, &op.operation_id)
                .unwrap()
                .status,
            OperationStatus::Running
        );
        drop(lease);
        let _lease = store.worker_lease(&op.operation_id).unwrap();
        assert_eq!(
            store.require_queued(&op.operation_id),
            Err(BrokerError::InvalidTransition)
        );
    }

    #[test]
    fn broker_restart_reconciles_live_and_crashed_workers_without_retry() {
        let (temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let op = consume(&store, &owner, 1001).unwrap();
        let lease = store.worker_lease(&op.operation_id).unwrap();
        store
            .claim_worker(&op.operation_id, op.plan.packages(), 1002)
            .unwrap();
        drop(store);
        let reopened = BrokerStore::open_with(
            temp.0.clone(),
            unsafe { libc::geteuid() },
            Arc::new(TestTokens(AtomicUsize::new(20))),
        )
        .unwrap();
        reopened
            .reconcile_worker_state(&op.operation_id, true, 1003)
            .unwrap();
        // Even a stale inactive systemd observation cannot fail a live lease.
        reopened
            .reconcile_worker_state(&op.operation_id, false, 1003)
            .unwrap();
        assert_eq!(
            reopened
                .operation_for_owner(&owner, &op.operation_id)
                .unwrap()
                .status,
            OperationStatus::Running
        );
        drop(lease);
        reopened
            .reconcile_worker_state(&op.operation_id, false, 1004)
            .unwrap();
        assert_eq!(
            reopened
                .operation_for_owner(&owner, &op.operation_id)
                .unwrap()
                .status,
            OperationStatus::Failed
        );
        assert_eq!(
            reopened.require_queued(&op.operation_id),
            Err(BrokerError::InvalidTransition)
        );
        assert_eq!(
            consume(&reopened, &owner, 1005),
            Err(BrokerError::NoPreview)
        );
    }

    #[test]
    fn late_start_failure_does_not_overwrite_claim_or_completion() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let op = consume(&store, &owner, 1001).unwrap();
        store
            .claim_worker(&op.operation_id, op.plan.packages(), 1002)
            .unwrap();
        store
            .worker_start_failed(&op.operation_id, "late systemctl error")
            .unwrap();
        assert_eq!(
            store
                .operation_for_owner(&owner, &op.operation_id)
                .unwrap()
                .status,
            OperationStatus::Running
        );
        store
            .finish_worker_result(&op.operation_id, OperationStatus::Succeeded, "done", 1003)
            .unwrap();
        store
            .worker_start_failed(&op.operation_id, "late systemctl error")
            .unwrap();
        assert_eq!(
            store
                .operation_for_owner(&owner, &op.operation_id)
                .unwrap()
                .status,
            OperationStatus::Succeeded
        );
    }

    #[test]
    fn every_approved_plan_field_is_checked_before_claim() {
        for field in 0..5 {
            let (_temp, store) = TempStore::new();
            let owner = identity(1000, "session-a");
            make_preview(&store, &owner);
            let op = consume(&store, &owner, 1001).unwrap();
            let mut changed = op.plan.packages().to_vec();
            match field {
                0 => changed[0].package_name.push_str("-changed"),
                1 => changed[0].repository.push_str("-changed"),
                2 => changed[0].current_version.push_str("-changed"),
                3 => changed[0].new_version.push_str("-changed"),
                _ => changed[0].download_size += 1,
            }
            assert_eq!(
                store.claim_worker(&op.operation_id, &changed, 1002),
                Err(BrokerError::PlanChanged)
            );
            assert_eq!(
                store.require_queued(&op.operation_id),
                Err(BrokerError::InvalidTransition)
            );
        }
    }

    #[test]
    fn pacman_lock_collision_fails_before_claim_without_replay() {
        let (temp, store) = TempStore::new();
        let db = temp.0.join("pacman");
        fs::create_dir(&db).unwrap();
        let mut first = alpm::Alpm::new("/", db.to_str().unwrap()).unwrap();
        let second = alpm::Alpm::new("/", db.to_str().unwrap()).unwrap();
        first.trans_init(alpm::TransFlag::NONE).unwrap();
        assert_eq!(
            second.trans_init(alpm::TransFlag::NONE),
            Err(alpm::Error::HandleLock)
        );
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let op = consume(&store, &owner, 1001).unwrap();
        let _lease = store.worker_lease(&op.operation_id).unwrap();
        store
            .finish_worker_result(
                &op.operation_id,
                OperationStatus::Failed,
                "package_manager_locked",
                1002,
            )
            .unwrap();
        first.trans_release().unwrap();
        assert_eq!(
            store.require_queued(&op.operation_id),
            Err(BrokerError::InvalidTransition)
        );
        assert_eq!(
            store.claim_worker(&op.operation_id, op.plan.packages(), 1003),
            Err(BrokerError::InvalidTransition)
        );
    }

    #[test]
    fn abandoned_queue_is_failed_after_start_handoff_grace() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let op = consume(&store, &owner, 1001).unwrap();
        store
            .reconcile_worker_state(&op.operation_id, false, 1002)
            .unwrap();
        assert!(store.require_queued(&op.operation_id).is_ok());
        store
            .reconcile_worker_state(&op.operation_id, false, 1061)
            .unwrap();
        assert_eq!(
            store
                .operation_for_owner(&owner, &op.operation_id)
                .unwrap()
                .status,
            OperationStatus::Failed
        );
    }

    #[test]
    fn killed_worker_process_releases_lease_and_reconciliation_is_terminal() {
        let (temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        let op = consume(&store, &owner, 1001).unwrap();
        store
            .claim_worker(&op.operation_id, op.plan.packages(), 1002)
            .unwrap();
        // Only a private test lock is used. No systemd or real package work.
        let path = temp.0.join(format!("worker-{}.lock", op.operation_id));
        drop(store.worker_lease(&op.operation_id).unwrap());
        let status = std::process::Command::new("/usr/bin/flock")
            .arg("--nonblock")
            .arg(path)
            .args(["/bin/sh", "-c", "kill -KILL $$"])
            .status()
            .unwrap();
        assert_eq!(status.code(), Some(128 + libc::SIGKILL));
        store
            .reconcile_worker_state(&op.operation_id, false, 1003)
            .unwrap();
        assert_eq!(
            store
                .operation_for_owner(&owner, &op.operation_id)
                .unwrap()
                .status,
            OperationStatus::Failed
        );
        assert_eq!(
            store.require_queued(&op.operation_id),
            Err(BrokerError::InvalidTransition)
        );
    }

    #[test]
    fn preview_expiry_cannot_fail_a_live_queued_worker() {
        for observation in [
            Ok(false),
            Err(BrokerError::Io("system bus unavailable".into())),
        ] {
            let (_temp, store) = TempStore::new();
            let owner = identity(1000, "session-a");
            make_preview(&store, &owner);
            let op = consume(&store, &owner, 1001).unwrap();
            let lease = store.worker_lease(&op.operation_id).unwrap();
            assert_eq!(store.cleanup(1400, Duration::ZERO).unwrap(), 0);
            store
                .reconcile_worker_observation(&op.operation_id, observation.clone(), 1400)
                .unwrap();
            assert!(store.require_queued(&op.operation_id).is_ok());
            drop(lease);
            // Parallel process-spawning tests can briefly inherit CLOEXEC file
            // descriptors between fork and exec. Reconciliation conservatively
            // defers until that last lease reference closes, just as in production.
            let deadline = std::time::Instant::now() + Duration::from_secs(2);
            loop {
                store
                    .reconcile_worker_observation(&op.operation_id, observation.clone(), 1401)
                    .unwrap();
                if store
                    .operation_for_owner(&owner, &op.operation_id)
                    .unwrap()
                    .status
                    == OperationStatus::Failed
                {
                    break;
                }
                assert!(
                    std::time::Instant::now() < deadline,
                    "worker lease did not close"
                );
                thread::sleep(Duration::from_millis(5));
            }
            assert_eq!(
                store
                    .operation_for_owner(&owner, &op.operation_id)
                    .unwrap()
                    .status,
                OperationStatus::Failed
            );
            assert_eq!(
                store.claim_worker(&op.operation_id, op.plan.packages(), 1402),
                Err(BrokerError::InvalidTransition)
            );
        }
    }

    #[test]
    fn cleanup_removes_expired_previews_and_old_terminal_records_only() {
        let (_temp, store) = TempStore::new();
        let owner = identity(1000, "session-a");
        make_preview(&store, &owner);
        assert_eq!(store.cleanup(1300, Duration::from_secs(0)).unwrap(), 1);

        make_preview(&store, &owner);
        let operation = consume(&store, &owner, 1001).unwrap();
        store
            .claim_worker(&operation.operation_id, operation.plan.packages(), 1002)
            .unwrap();
        store
            .update_worker_status(
                &operation.operation_id,
                OperationStatus::Succeeded,
                "done",
                1100,
            )
            .unwrap();
        assert_eq!(store.cleanup(1400, Duration::from_secs(100)).unwrap(), 1);
    }
}

#[cfg(test)]
mod live_identity_tests {
    use super::*;

    /// Read-only checks against existing KDE peers. Does not acquire the
    /// broker's bus name, start a service, or request install authorization.
    #[tokio::test]
    #[ignore = "requires a real KDE system bus, logind, and installed preview policy"]
    async fn live_kde_identity_and_preview_policy() {
        let connection = zbus::Connection::system().await.unwrap();
        let bus = zbus::Proxy::new(
            &connection,
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
        )
        .await
        .unwrap();
        let names: Vec<String> = bus.call("ListNames", &()).await.unwrap();
        let mut checked = 0;
        for name in names.into_iter().filter(|name| name.starts_with(':')) {
            let Ok(pid) = bus
                .call::<_, _, u32>("GetConnectionUnixProcessID", &(name.as_str(),))
                .await
            else {
                continue;
            };
            let Ok(comm) = std::fs::read_to_string(format!("/proc/{pid}/comm")) else {
                continue;
            };
            if !matches!(
                comm.trim(),
                "kded6" | "plasmashell" | "polkit-kde-auth" | "zethropol-contr"
            ) {
                continue;
            }
            let identity = VerifiedIdentity::from_sender(&connection, &name)
                .await
                .unwrap();
            assert_eq!(identity.uid, unsafe { libc::geteuid() });
            assert!(!identity.session_id.is_empty());
            let status = std::process::Command::new("/usr/bin/pkcheck")
                .args([
                    "--action-id",
                    PolicyAction::Preview.id(),
                    "--system-bus-name",
                    &name,
                ])
                .stdin(std::process::Stdio::null())
                .status()
                .unwrap();
            assert!(
                status.success(),
                "noninteractive preview policy denied {name}: {status}"
            );
            eprintln!(
                "verified KDE peer {name} pid={pid} uid={} session={}",
                identity.uid, identity.session_id
            );
            checked += 1;
        }
        assert!(checked > 0, "no existing KDE peers to verify");
        let peer = zbus::Connection::system().await.unwrap();
        let name = peer.unique_name().unwrap().to_string();
        drop(peer);
        // Once the bus has observed disconnection, no identity can be reused.
        for _ in 0..100 {
            if VerifiedIdentity::from_sender(&connection, &name)
                .await
                .is_err()
            {
                return;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        panic!("disconnected bus identity remained resolvable");
    }
}

#[cfg(test)]
#[path = "worker_integration_tests.rs"]
mod worker_integration_tests;
include!("aur/broker.rs");

include!("aur/persistent.rs");

#[cfg(test)]
include!("aur/fixture_access.rs");
