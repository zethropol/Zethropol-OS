//! Pure synthetic evidence policy, deliberately disconnected from production.
//!
//! All assertions (including provenance and replay detection) are inputs, NOT
//! authenticated by this module. No result grants authority or proves host boot.
//! Times are seconds on the observed boot's monotonic clock, never wall time.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    Verified,
    Unverified,
    Failed,
    Unknown,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Check {
    Passed,
    Failed,
    NotVerified,
    Missing,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Commit {
    Committed,
    NotStarted,
    InProgress,
    Unknown,
    FailedMayBePartial,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Kernel {
    pub release: String,
    /// Exact content identity, not a release string or disk path.
    pub sha256: String,
}

#[derive(Clone, Debug)]
pub struct Expected {
    pub operation_id: String,
    pub origin_boot_id: String,
    pub kernel: Kernel,
    pub entry_id: String,
    /// Opaque root filesystem/subvolume identity, not a mount path.
    pub root_id: String,
}

#[derive(Clone, Debug)]
pub struct Evidence {
    pub operation_id: Option<String>,
    pub commit: Commit,
    pub boot_id: Option<String>,
    pub boot_id_after_checks: Option<String>,
    pub kernel: Option<Kernel>,
    pub entry_id: Option<String>,
    pub root_id: Option<String>,
    /// Passed means an upstream trusted producer established provenance.
    pub source: Check,
    pub envelope_integrity: Check,
    /// Passed must be established by a future external freshness authority.
    pub replay_check: Check,
    pub observed_at: Option<u64>,
    /// Covers actual executed loader/kernel/initramfs, not on-disk equality.
    pub execution_binding: Check,
    pub kernel_integrity: Check,
    pub initramfs_integrity: Check,
    /// Aggregate of a declared nonempty policy, or explicit verified no-driver scope.
    pub required_drivers: Check,
    /// Aggregate of all checks in a declared versioned required-health policy.
    pub critical_health: Check,
    /// Passed means recovery/rollback has been explicitly ruled out.
    pub normal_root: Check,
}

#[derive(Clone, Debug)]
pub struct Context {
    pub current_boot_id: String,
    pub now: u64,
    /// Inclusive maximum age. Zero is invalid, never an unlimited lifetime.
    pub max_age: u64,
}

macro_rules! reasons {
    ($($name:ident => $code:literal),+ $(,)?) => {
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub enum Reason { $($name),+ }
        impl Reason {
            pub fn code(self) -> &'static str {
                match self { $(Self::$name => $code),+ }
            }
        }
    };
}
reasons! {
    Complete => "boot_evidence_complete",
    InvalidExpectation => "invalid_expectation",
    MissingBinding => "missing_binding",
    OperationMismatch => "operation_mismatch",
    InvalidObservation => "invalid_observation",
    BootMismatch => "boot_mismatch",
    BootChangedDuringChecks => "boot_changed_during_checks",
    SameBoot => "same_boot",
    CommitNotStarted => "commit_not_started",
    CommitInterrupted => "commit_interrupted",
    CommitUnknown => "commit_unknown",
    CommitPartial => "commit_may_be_partial",
    KernelMismatch => "kernel_mismatch",
    EntryMismatch => "entry_mismatch",
    RootMismatch => "root_mismatch",
    Source => "source_not_trusted",
    Envelope => "envelope_integrity",
    Replay => "replay_check",
    MissingTime => "missing_observation_time",
    InvalidTime => "invalid_time_context",
    Future => "future_observation",
    Stale => "stale_observation",
    Execution => "execution_binding",
    Kernel => "kernel_integrity",
    Initramfs => "initramfs_integrity",
    Drivers => "required_drivers",
    Health => "critical_health",
    Recovery => "recovery_or_rollback"
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Decision {
    pub state: State,
    /// Fixed codes only: never copy identifiers, paths, diagnostics or tokens.
    pub reasons: Vec<Reason>,
}

fn uuid(value: &str) -> bool {
    uuid::Uuid::parse_str(value).is_ok_and(|id| !id.is_nil() && id.to_string() == value)
}
fn kernel_valid(kernel: &Kernel) -> bool {
    !kernel.release.is_empty()
        && kernel.release.len() <= 256
        && !kernel.release.chars().any(char::is_control)
        && kernel.sha256.len() == 64
        && kernel
            .sha256
            .bytes()
            .all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase())
}
fn opaque(value: &str) -> bool {
    !value.is_empty() && value.len() <= 256 && !value.chars().any(char::is_control)
}
fn reject(state: State, reason: Reason) -> Decision {
    Decision {
        state,
        reasons: vec![reason],
    }
}

/// Structural/provenance failures take precedence over content conclusions.
/// Failed checks outrank missing checks, which outrank explicit nonverification.
/// Compatibility, hook success, cleanup and reboot permission are outside scope.
pub fn evaluate(expected: &Expected, evidence: &Evidence, context: &Context) -> Decision {
    use Reason::*;
    use State::*;
    if !uuid(&expected.operation_id)
        || !uuid(&expected.origin_boot_id)
        || !uuid(&context.current_boot_id)
        || !kernel_valid(&expected.kernel)
        || !opaque(&expected.entry_id)
        || !opaque(&expected.root_id)
    {
        return reject(Unknown, InvalidExpectation);
    }
    // Untrusted or corrupt input cannot establish even a reliable failure cause.
    for (check, reason) in [
        (evidence.source, Source),
        (evidence.envelope_integrity, Envelope),
        (evidence.replay_check, Replay),
    ] {
        if check != Check::Passed {
            return reject(
                match check {
                    Check::Failed => Failed,
                    Check::NotVerified => Unverified,
                    _ => Unknown,
                },
                reason,
            );
        }
    }
    let (Some(operation), Some(boot), Some(after), Some(kernel), Some(entry), Some(root)) = (
        &evidence.operation_id,
        &evidence.boot_id,
        &evidence.boot_id_after_checks,
        &evidence.kernel,
        &evidence.entry_id,
        &evidence.root_id,
    ) else {
        return reject(Unknown, MissingBinding);
    };
    if !uuid(operation)
        || !uuid(boot)
        || !uuid(after)
        || !kernel_valid(kernel)
        || !opaque(entry)
        || !opaque(root)
    {
        return reject(Failed, InvalidObservation);
    }
    if operation != &expected.operation_id {
        return reject(Failed, OperationMismatch);
    }
    if boot != after {
        return reject(Failed, BootChangedDuringChecks);
    }
    if boot != &context.current_boot_id {
        return reject(Failed, BootMismatch);
    }
    if context.max_age == 0 {
        return reject(Unknown, InvalidTime);
    }
    let Some(time) = evidence.observed_at else {
        return reject(Unknown, MissingTime);
    };
    let Some(age) = context.now.checked_sub(time) else {
        return reject(Failed, Future);
    };
    if age > context.max_age {
        return reject(Unverified, Stale);
    }
    match evidence.commit {
        Commit::Committed => {}
        Commit::NotStarted => return reject(Unverified, CommitNotStarted),
        Commit::InProgress => return reject(Unknown, CommitInterrupted),
        Commit::Unknown => return reject(Unknown, CommitUnknown),
        Commit::FailedMayBePartial => return reject(Failed, CommitPartial),
    }
    if boot == &expected.origin_boot_id {
        return reject(Unverified, SameBoot);
    }
    if kernel != &expected.kernel {
        return reject(Failed, KernelMismatch);
    }
    if entry != &expected.entry_id {
        return reject(Failed, EntryMismatch);
    }
    if root != &expected.root_id {
        return reject(Failed, RootMismatch);
    }
    let checks = [
        (evidence.execution_binding, Execution),
        (evidence.kernel_integrity, Kernel),
        (evidence.initramfs_integrity, Initramfs),
        (evidence.required_drivers, Drivers),
        (evidence.critical_health, Health),
        (evidence.normal_root, Recovery),
    ];
    let mut state = Verified;
    let mut reasons = Vec::new();
    for (check, reason) in checks {
        match check {
            Check::Passed => continue,
            Check::Failed => state = Failed,
            Check::Missing if state != Failed => state = Unknown,
            Check::NotVerified if state == Verified => state = Unverified,
            _ => {}
        }
        reasons.push(reason);
    }
    if reasons.is_empty() {
        reasons.push(Complete);
    }
    Decision { state, reasons }
}
