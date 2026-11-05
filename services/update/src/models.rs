use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use zbus::zvariant::{OwnedValue, Type};

#[derive(Serialize, Deserialize, Type, Debug, Clone, PartialEq, Eq)]
pub struct UpdatePackageInfo {
    pub package_name: String,
    pub current_version: String,
    pub new_version: String,
    pub repository: String,
    /// Signed compressed archive size (legacy wire name); never remaining transfer bytes.
    pub download_size: u64,
    /// Basename from the signed sync database.
    pub filename: String,
    /// SHA-256 recorded in the trusted sync database. No checksum fallback.
    pub sha256: String,
    /// Digest of the exact repository database and signature material used.
    pub repository_metadata_sha256: String,
}

#[derive(Serialize, Deserialize, Type, Debug, Clone)]
pub struct UpdateState {
    pub is_checking: bool,
    pub is_updating: bool,
    pub last_check_time: String,
    pub pending_updates_count: u32,
}

#[derive(Serialize, Deserialize, Type, Debug, Clone)]
pub struct OperationResult {
    pub success: bool,
    pub code: String,
    pub message: String,
    pub operation_id: String,
    pub data: HashMap<String, OwnedValue>,
}

/// A directed, version-bound removal edge computed by libalpm, never supplied
/// as transaction authority by the client.
#[derive(Serialize, Deserialize, Type, Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct PackageRemoval {
    pub package_name: String,
    pub current_version: String,
    pub reason: String,
    pub replacement: String,
}
