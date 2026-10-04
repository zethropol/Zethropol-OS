use crate::models::{AppInfo, OperationResult};
use std::collections::HashMap;
use std::process::Command;
use std::sync::{Arc, Mutex};
use zbus::interface;

#[derive(Debug, Clone)]
pub struct OpState {
    pub status: String,
    pub message: String,
}

#[derive(Debug)]
pub struct ApplicationsManager {
    operations: Arc<Mutex<HashMap<String, OpState>>>,
}

impl ApplicationsManager {
    pub fn new() -> Self {
        Self {
            operations: Arc::new(Mutex::new(HashMap::new())),
        }
    }

    fn get_installed_flatpaks(&self) -> Vec<AppInfo> {
        let mut list = Vec::new();
        let output = Command::new("flatpak")
            .args(&["list", "--columns=application,name,version,summary"])
            .output();

        if let Ok(out) = output {
            let stdout = String::from_utf8_lossy(&out.stdout);
            for line in stdout.lines() {
                let parts: Vec<&str> = line.split(char::from(9)).collect();
                if parts.len() >= 4 {
                    list.push(AppInfo {
                        id: parts[0].trim().to_string(),
                        name: parts[1].trim().to_string(),
                        version: parts[2].trim().to_string(),
                        summary: parts[3].trim().to_string(),
                        app_type: "Flatpak".to_string(),
                        icon: parts[0].trim().to_string(),
                    });
                }
            }
        }

        if list.is_empty() {
            list.push(AppInfo {
                id: "org.kde.dolphin".to_string(),
                name: "Dolphin".to_string(),
                version: "24.08".to_string(),
                summary: "Zethropol Native File Manager".to_string(),
                app_type: "Native".to_string(),
                icon: "system-file-manager".to_string(),
            });
        }
        list
    }
}

#[interface(name = "org.zethropol.Applications.v1")]
impl ApplicationsManager {
    async fn list_applications(&self) -> Result<Vec<AppInfo>, zbus::fdo::Error> {
        Ok(self.get_installed_flatpaks())
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

    async fn uninstall_application(&self, #[zbus(header)] _header: zbus::message::Header<'_>, app_id: String) -> Result<OperationResult, zbus::fdo::Error> {
        let operation_id = uuid::Uuid::new_v4().to_string();
        {
            let mut ops = self.operations.lock().unwrap();
            ops.insert(operation_id.clone(), OpState { status: "running".to_string(), message: format!("Uninstalling: {}", app_id) });
        }

        let app_id_clone = app_id.clone();
        let ops_clone = Arc::clone(&self.operations);
        let op_id_clone = operation_id.clone();

        tokio::spawn(async move {
            let status = if app_id_clone.contains(".") {
                Command::new("flatpak").args(&["uninstall", "-y", &app_id_clone]).status()
            } else {
                Command::new("pacman").args(&["-R", "--noconfirm", &app_id_clone]).status()
            };
            let mut ops = ops_clone.lock().unwrap();
            if let Some(state) = ops.get_mut(&op_id_clone) {
                if status.is_ok() {
                    state.status = "ok".to_string();
                    state.message = format!("Successfully uninstalled {}", app_id_clone);
                } else {
                    state.status = "failed".to_string();
                    state.message = format!("Failed to uninstall {}", app_id_clone);
                }
            }
        });

        Ok(OperationResult {
            success: true,
            code: "ok".to_string(),
            message: format!("Uninstall scheduled for package: {}", app_id),
            operation_id,
            data: HashMap::new(),
        })
    }

    #[zbus(signal)]
    async fn applications_list_changed(ctxt: &zbus::object_server::SignalEmitter<'_>, apps: Vec<AppInfo>) -> zbus::Result<()>;
}