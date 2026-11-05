//! Evidence is persisted independently of worker lifecycle. A libalpm commit
//! return value cannot certify post hooks, initramfs, firmware or bootability.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum CommitState {
    NotStarted,
    InProgress,
    Committed,
    Failed,
    Unknown,
}

impl CommitState {
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            Self::NotStarted => "not-started",
            Self::InProgress => "in-progress",
            Self::Committed => "committed",
            Self::Failed => "failed-may-be-partial",
            Self::Unknown => "unknown",
        }
    }
    pub(crate) fn parse(value: &str) -> Option<Self> {
        [
            Self::NotStarted,
            Self::InProgress,
            Self::Committed,
            Self::Failed,
            Self::Unknown,
        ]
        .into_iter()
        .find(|state| state.as_str() == value)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum VerificationState {
    Unknown,
    Failed,
    Verified,
}

impl VerificationState {
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            Self::Unknown => "unknown",
            Self::Failed => "failed",
            Self::Verified => "verified",
        }
    }
    pub(crate) fn parse(value: &str) -> Option<Self> {
        [Self::Unknown, Self::Failed, Self::Verified]
            .into_iter()
            .find(|state| state.as_str() == value)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct BootVerification {
    pub kernels: VerificationState,
    pub dkms: VerificationState,
    pub initramfs: VerificationState,
    pub limine: VerificationState,
    pub new_kernel_boot: VerificationState,
    pub running_kernel: String,
    pub installed_kernels: String,
    pub updated_kernels: String,
    pub details: String,
}

impl Default for BootVerification {
    fn default() -> Self {
        Self {
            kernels: VerificationState::Unknown,
            dkms: VerificationState::Unknown,
            initramfs: VerificationState::Unknown,
            limine: VerificationState::Unknown,
            new_kernel_boot: VerificationState::Unknown,
            running_kernel: String::new(),
            installed_kernels: String::new(),
            updated_kernels: String::new(),
            details: String::new(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct ExecutionEvidence {
    pub package_commit: CommitState,
    pub hooks: VerificationState,
    pub boot_preparation: VerificationState,
    pub pre_snapshot: Option<String>,
    pub post_snapshot: Option<String>,
    pub boot: BootVerification,
    pub hook_details: String,
    pub cleanup: crate::cache_cleanup::CleanupReport,
}

impl Default for ExecutionEvidence {
    fn default() -> Self {
        Self {
            package_commit: CommitState::NotStarted,
            hooks: VerificationState::Unknown,
            boot_preparation: VerificationState::Unknown,
            pre_snapshot: None,
            post_snapshot: None,
            boot: BootVerification::default(),
            hook_details: String::new(),
            cleanup: Default::default(),
        }
    }
}

impl ExecutionEvidence {
    pub(crate) fn legacy() -> Self {
        Self {
            package_commit: CommitState::Unknown,
            ..Self::default()
        }
    }

    pub(crate) fn interrupted(&mut self) {
        if self.package_commit == CommitState::InProgress {
            self.package_commit = CommitState::Unknown;
        }
    }

    pub(crate) fn fully_verified(&self) -> bool {
        self.package_commit == CommitState::Committed
            && self.hooks == VerificationState::Verified
            && self.boot_preparation == VerificationState::Verified
            && self.boot.new_kernel_boot == VerificationState::Verified
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn commit_alone_does_not_certify_hooks_or_boot() {
        let evidence = ExecutionEvidence {
            package_commit: CommitState::Committed,
            ..Default::default()
        };
        assert!(!evidence.fully_verified());
        assert_eq!(evidence.hooks, VerificationState::Unknown);
        assert_eq!(evidence.boot_preparation, VerificationState::Unknown);
    }

    #[test]
    fn interruption_preserves_confirmed_commit_but_not_an_inflight_one() {
        let mut evidence = ExecutionEvidence {
            package_commit: CommitState::InProgress,
            ..Default::default()
        };
        evidence.interrupted();
        assert_eq!(evidence.package_commit, CommitState::Unknown);
        evidence.package_commit = CommitState::Committed;
        evidence.interrupted();
        assert_eq!(evidence.package_commit, CommitState::Committed);
        assert!(!ExecutionEvidence::legacy().fully_verified());
    }
}
