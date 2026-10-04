use serde::{Serialize, Deserialize};
use std::collections::HashMap;
use zbus::zvariant::{Type, OwnedValue};

#[derive(Serialize, Deserialize, Type, Debug, Clone)]
pub struct ServiceInfo {
    pub name: String,
    pub description: String,
    pub load_state: String,
    pub active_state: String,
    pub sub_state: String,
    pub enabled: bool,
}

#[derive(Serialize, Deserialize, Type, Debug, Clone)]
pub struct OperationResult {
    pub success: bool,
    pub code: String,
    pub message: String,
    pub operation_id: String,
    pub data: HashMap<String, OwnedValue>,
}
