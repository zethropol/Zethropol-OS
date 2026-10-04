use serde::{Serialize, Deserialize};
use std::collections::HashMap;
use zbus::zvariant::{Type, OwnedValue};

#[derive(Serialize, Deserialize, Type, Debug, Clone)]
pub struct SecurityState {
    pub firewall: HashMap<String, OwnedValue>,
    pub secure_boot: HashMap<String, OwnedValue>,
    pub kernel: HashMap<String, OwnedValue>,
    pub apparmor: HashMap<String, OwnedValue>,
    pub system_integrity: HashMap<String, OwnedValue>,
}

#[derive(Serialize, Deserialize, Type, Debug, Clone)]
pub struct OperationResult {
    pub success: bool,
    pub code: String,
    pub message: String,
    pub operation_id: String,
    pub data: HashMap<String, OwnedValue>,
}
