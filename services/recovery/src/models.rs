use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use zbus::zvariant::{OwnedValue, Type};

#[derive(Serialize, Deserialize, Type, Debug, Clone, PartialEq, Eq)]
pub struct RecoveryPoint {
    pub id: u32,
    pub name: String,
    pub created_at: String,
    pub description: String,
    pub is_current: bool,
}

#[derive(Serialize, Deserialize, Type, Debug, Clone)]
pub struct OperationResult {
    pub success: bool,
    pub code: String,
    pub message: String,
    pub operation_id: String,
    pub data: HashMap<String, OwnedValue>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DetectionStatus {
    InstalledAndConfigured,
    InstalledNotConfigured,
    NotInstalled,
    InvalidConfiguration,
    PermissionDenied,
    BackendUnavailable,
}

impl DetectionStatus {
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::InstalledAndConfigured => "installed_and_configured",
            Self::InstalledNotConfigured => "installed_not_configured",
            Self::NotInstalled => "not_installed",
            Self::InvalidConfiguration => "invalid_configuration",
            Self::PermissionDenied => "permission_denied",
            Self::BackendUnavailable => "backend_unavailable",
        }
    }

    pub fn result_code(&self) -> &'static str {
        match self {
            Self::InstalledAndConfigured => "ok",
            Self::InstalledNotConfigured => "not-available",
            Self::NotInstalled => "not-found",
            Self::InvalidConfiguration => "failed",
            Self::PermissionDenied => "permission-denied",
            Self::BackendUnavailable => "not-available",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SnapshotRecord {
    pub id: i32,
    pub snapshot_type: String,
    pub is_current: bool,
    pub is_pre: bool,
    pub is_post: bool,
    pub paired_snapshot_id: i32,
    pub pre_number: i32,
    pub date: String,
    pub user: String,
    pub cleanup: String,
    pub description: String,
    pub important: bool,
    pub zethropol_tag: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RecoveryEventRecord {
    pub pre_id: i32,
    pub post_id: i32,
    pub date: String,
    pub description: String,
    pub pre_description: String,
    pub snapshot_type: String,
    pub important: bool,
    pub zethropol_tag: String,
}

#[derive(Debug, Clone)]
pub struct DetectionReport {
    pub status: DetectionStatus,
    pub available: bool,
    pub permission_required: bool,
    pub snapper_installed: bool,
    pub configured: bool,
    pub filesystem: String,
    pub root_subvolume: String,
    pub snapshot_tool: String,
    pub config_name: String,
    pub available_configs: Vec<String>,
    pub snapshots_dir_exists: bool,
    pub snapshots_dir_accessible: bool,
    pub status_title: String,
    pub status_message: String,
    pub snapshot_count: i32,
    pub current_snapshot_id: i32,
    pub snapshots: Vec<SnapshotRecord>,
    pub recovery_events: Vec<RecoveryEventRecord>,
    pub recovery_points: Vec<SnapshotRecord>,
}
