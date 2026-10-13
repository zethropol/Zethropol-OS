use crate::models::{UpdatePackageInfo, UpdateState, OperationResult};
use std::collections::HashMap;
use std::process::Command;
use std::sync::{Arc, Mutex};
use zbus::interface;

#[derive(Debug, Clone)]
pub struct OpState {
    pub status: String, // "pending", "running", "ok", "failed"
    pub message: String,
}

#[derive(Debug)]
pub struct UpdateManager {
    operations: Arc<Mutex<HashMap<String, OpState>>>,
}

impl UpdateManager {
    pub fn new() -> Self {
        Self {
            operations: Arc::new(Mutex::new(HashMap::new())),
        }
    }

    fn check_pacman_updates(&self) -> Vec<UpdatePackageInfo> {
        let mut list = Vec::new();
        let output = Command::new("checkupdates").output();

        if let Ok(out) = output {
            let stdout = String::from_utf8_lossy(&out.stdout);
            for line in stdout.lines() {
                let parts: Vec<&str> = line.split_whitespace().collect();
                if parts.len() >= 4 {
                    list.push(UpdatePackageInfo {
                        package_name: parts[0].to_string(),
                        current_version: parts[1].to_string(),
                        new_version: parts[3].to_string(),
                        repository: "pacman".to_string(),
                        download_size: 0,
                    });
                }
            }
        }
        list
    }
}

#[interface(name = "org.zethropol.Update.v1")]
impl UpdateManager {
    async fn get_update_state(&self) -> Result<UpdateState, zbus::fdo::Error> {
        let pending = self.check_pacman_updates();
        let count = pending.len() as u32;

        Ok(UpdateState {
            is_checking: false,
            is_updating: false,
            last_check_time: "Just now".to_string(),
            pending_updates_count: count,
        })
    }

    async fn list_pending_updates(&self) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
        Ok(self.check_pacman_updates())
    }

    async fn get_operation_state(&self, operation_id: String) -> Result<HashMap<String, String>, zbus::fdo::Error> {
        let ops = self.operations.lock().unwrap();
        let mut res = HashMap::new();
        if let Some(state) = ops.get(&operation_id) {
            res.insert("status".to_string(), state.status.clone());
            res.insert("message".to_string(), state.message.clone());
        } else {
            res.insert("status".to_string(), "not-found".to_string());
            res.insert("message".to_string(), "Operation ID does not exist.".to_string());
        }
        Ok(res)
    }

    async fn trigger_update(&self, #[zbus(header)] _header: zbus::message::Header<'_>) -> Result<OperationResult, zbus::fdo::Error> {
        Ok(OperationResult {
            success: false,
            code: "preflight_required".to_string(),
            message: "System updates are disabled until transaction planning, dependency/conflict validation, and authorization are implemented.".to_string(),
            operation_id: String::new(),
            data: HashMap::new(),
        })
    }

    #[zbus(signal)]
    async fn update_status_changed(ctxt: &zbus::object_server::SignalEmitter<'_>, new_state: UpdateState) -> zbus::Result<()>;
}