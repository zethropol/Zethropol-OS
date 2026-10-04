use serde::{Serialize, Deserialize};
use std::collections::HashMap;
use zbus::zvariant::{Type, OwnedValue};

#[derive(Serialize, Deserialize, Type, Debug, Clone)]
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
