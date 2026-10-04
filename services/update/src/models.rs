use serde::{Serialize, Deserialize};
use std::collections::HashMap;
use zbus::zvariant::{Type, OwnedValue};

#[derive(Serialize, Deserialize, Type, Debug, Clone)]
pub struct UpdatePackageInfo {
    pub package_name: String,
    pub current_version: String,
    pub new_version: String,
    pub repository: String,
    pub download_size: u64,
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
