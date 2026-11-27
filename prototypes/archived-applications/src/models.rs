use serde::{Serialize, Deserialize};
use std::collections::HashMap;
use zbus::zvariant::{Type, OwnedValue};

#[derive(Serialize, Deserialize, Type, Debug, Clone)]
pub struct AppInfo {
    pub id: String,
    pub name: String,
    pub version: String,
    pub summary: String,
    pub app_type: String, // Flatpak, Native, System
    pub icon: String,
}

#[derive(Serialize, Deserialize, Type, Debug, Clone)]
pub struct OperationResult {
    pub success: bool,
    pub code: String,
    pub message: String,
    pub operation_id: String,
    pub data: HashMap<String, OwnedValue>,
}
