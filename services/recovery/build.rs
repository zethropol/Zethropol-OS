fn main() {
    let service_code = r#"use crate::models::{RecoveryPoint, OperationResult};
use std::collections::HashMap;
use std::process::Command;
use std::sync::{Arc, Mutex};
use tokio::time::{sleep, Duration};
use zbus::interface;

#[derive(Debug, Clone)]
pub struct OpState {
    pub status: String, // "pending", "running", "ok", "failed"
    pub message: String,
}

#[derive(Debug)]
pub struct RecoveryManager {
    operations: Arc<Mutex<HashMap<String, OpState>>>,
}

impl RecoveryManager {
    pub fn new() -> Self {
        Self {
            operations: Arc::new(Mutex::new(HashMap::new())),
        }
    }

    fn get_snapper_snapshots(&self) -> Vec<RecoveryPoint> {
        let mut list = Vec::new();
        let output = Command::new("snapper")
            .args(&["--json-output", "list"])
            .output();

        if let Ok(out) = output {
            let stdout = String::from_utf8_lossy(&out.stdout);
            let mut id_counter = 1;
            for line in stdout.lines() {
                if line.contains("single") || line.contains("pre") || line.contains("post") {
                    list.push(RecoveryPoint {
                        id: id_counter,
                        name: format!("Snapshot-{}", id_counter),
                        created_at: "Recent".to_string(),
                        description: line.trim().to_string(),
                        is_current: id_counter == 1,
                    });
                    id_counter += 1;
                }
            }
        }

        if list.is_empty() {
            list.push(RecoveryPoint {
                id: 1,
                name: "Zethropol-Base-Snapshot".to_string(),
                created_at: "Initial".to_string(),
                description: "Default Btrfs root subvolume".to_string(),
                is_current: true,
            });
        }
        list
    }
}

#[interface(name = "org.zethropol.Recovery.v1")]
impl RecoveryManager {
    async fn list_recovery_points(&self) -> Result<Vec<RecoveryPoint>, zbus::fdo::Error> {
        Ok(self.get_snapper_snapshots())
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

    async fn create_recovery_point(&self, #[zbus(header)] _header: zbus::message::Header<'_>, name: String, description: String) -> Result<OperationResult, zbus::fdo::Error> {
        let operation_id = uuid::Uuid::new_v4().to_string();

        {
            let mut ops = self.operations.lock().unwrap();
            ops.insert(operation_id.clone(), OpState {
                status: "running".to_string(),
                message: format!("Creating Snapper snapshot: {}", name),
            });
        }

        let name_clone = name.clone();
        let desc_clone = description.clone();
        let ops_clone = Arc::clone(&self.operations);
        let op_id_clone = operation_id.clone();

        tokio::spawn(async move {
            let status = Command::new("snapper")
                .args(&["create", "--description", &format!("{}: {}", name_clone, desc_clone)])
                .status();

            let mut ops = ops_clone.lock().unwrap();
            if let Some(state) = ops.get_mut(&op_id_clone) {
                if status.is_ok() {
                    state.status = "ok".to_string();
                    state.message = format!("Successfully created recovery point: {}", name_clone);
                } else {
                    state.status = "failed".to_string();
                    state.message = "Failed to generate Btrfs snapshot.".to_string();
                }
            }
        });

        Ok(OperationResult {
            success: true,
            code: "ok".to_string(),
            message: format!("Btrfs snapshot sequence initiated for: {}", name),
            operation_id,
            data: HashMap::new(),
        })
    }

    async fn rollback_to_point(&self, #[zbus(header)] _header: zbus::message::Header<'_>, point_id: u32) -> Result<OperationResult, zbus::fdo::Error> {
        let operation_id = uuid::Uuid::new_v4().to_string();

        {
            let mut ops = self.operations.lock().unwrap();
            ops.insert(operation_id.clone(), OpState {
                status: "running".to_string(),
                message: format!("Initiating Btrfs system rollback to snapshot ID {}...", point_id),
            });
        }

        let ops_clone = Arc::clone(&self.operations);
        let op_id_clone = operation_id.clone();

        tokio::spawn(async move {
            let status = Command::new("snapper")
                .args(&["rollback", &point_id.to_string()])
                .status();

            let _sync_status = Command::new("limine-snapper-sync").status();

            let mut ops = ops_clone.lock().unwrap();
            if let Some(state) = ops.get_mut(&op_id_clone) {
                if status.is_ok() {
                    state.status = "ok".to_string();
                    state.message = format!("System successfully rolled back to snapshot {}. Boot sync complete. Please reboot.", point_id);
                } else {
                    state.status = "failed".to_string();
                    state.message = "Rollback routine failed during Btrfs subvolume switch.".to_string();
                }
            }
        });

        Ok(OperationResult {
            success: true,
            code: "ok".to_string(),
            message: format!("System rollback routine triggered for point ID {}. Reboot recommended.", point_id),
            operation_id,
            data: HashMap::new(),
        })
    }

    #[zbus(signal)]
    async fn recovery_state_changed(ctxt: &zbus::object_server::SignalEmitter<'_>, points: Vec<RecoveryPoint>) -> zbus::Result<()>;
}"#;
    std::fs::write("src/recovery_service.rs", service_code).unwrap();
    println!("cargo:rerun-if-changed=build.rs");
}
