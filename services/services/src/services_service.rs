use crate::models::{ServiceInfo, OperationResult};
use std::collections::HashMap;
use std::process::Command;
use std::sync::{Arc, Mutex};
use zbus::interface;
use zbus::Connection;
use zbus::proxy;

#[proxy(
    interface = "org.freedesktop.systemd1.Manager",
    default_service = "org.freedesktop.systemd1",
    default_path = "/org/freedesktop/systemd1"
)]
trait SystemdManager {
    async fn list_units(
        &self,
    ) -> zbus::Result<Vec<(
        String,
        String,
        String,
        String,
        String,
        String,
        zbus::zvariant::OwnedObjectPath,
        u32,
        String,
        zbus::zvariant::OwnedObjectPath,
    )>>;

    async fn get_unit_file_state(&self, unit: &str) -> zbus::Result<String>;

    async fn load_unit(&self, unit: &str, mode: &str) -> zbus::Result<zbus::zvariant::OwnedObjectPath>;
}

#[derive(Debug, Clone)]
pub struct OpState {
    pub status: String,
    pub message: String,
}

#[derive(Debug)]
pub struct ServicesManager {
    operations: Arc<Mutex<HashMap<String, OpState>>>,
}

impl ServicesManager {
    pub fn new() -> Self {
        Self {
            operations: Arc::new(Mutex::new(HashMap::new())),
        }
    }

    async fn get_systemd_service(name: &str) -> Result<ServiceInfo, zbus::Error> {
        let connection = Connection::system().await?;
        let manager = SystemdManagerProxy::new(&connection).await?;
        let object_path = manager.load_unit(name, "replace").await?;

        let unit = zbus::Proxy::new(
            &connection,
            "org.freedesktop.systemd1",
            object_path,
            "org.freedesktop.systemd1.Unit",
        )
        .await?;

        let load_state: String = unit.get_property("LoadState").await?;
        let active_state: String = unit.get_property("ActiveState").await?;
        let sub_state: String = unit.get_property("SubState").await?;
        let description: String = unit.get_property("Description").await?;

        let enabled = matches!(
            manager.get_unit_file_state(name).await,
            Ok(state) if matches!(
                state.as_str(),
                "enabled" | "enabled-runtime" | "static" | "indirect" | "generated"
            )
        );

        Ok(ServiceInfo {
            name: name.to_string(),
            description,
            load_state,
            active_state,
            sub_state,
            enabled,
        })
    }

    async fn get_systemd_services() -> Result<Vec<ServiceInfo>, zbus::Error> {
        let connection = Connection::system().await?;
        let manager = SystemdManagerProxy::new(&connection).await?;
        let units = manager.list_units().await?;

        let mut services = Vec::new();

        for (
            name,
            description,
            load_state,
            active_state,
            sub_state,
            _following,
            _object_path,
            _job_id,
            _job_type,
            _job_path,
        ) in units {
            if !name.ends_with(".service") {
                continue;
            }

            let enabled = matches!(
                manager.get_unit_file_state(&name).await,
                Ok(state) if matches!(
                    state.as_str(),
                    "enabled" | "enabled-runtime" | "static" | "indirect" | "generated"
                )
            );

            services.push(ServiceInfo {
                name,
                description,
                load_state,
                active_state,
                sub_state,
                enabled,
            });
        }

        services.sort_by(|a, b| a.name.cmp(&b.name));

        Ok(services)
    }
}

#[interface(name = "org.zethropol.Services.v1")]
impl ServicesManager {
    async fn list_services(&self) -> Result<Vec<ServiceInfo>, zbus::fdo::Error> {
        Self::get_systemd_services()
            .await
            .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))
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

    async fn manage_service(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
        name: String,
        action: String,
    ) -> Result<OperationResult, zbus::fdo::Error> {
        let operation_id = uuid::Uuid::new_v4().to_string();
        let sender = header.sender().unwrap();

        let polkit_check = Command::new("pkcheck")
            .args(&["--action-id", "org.zethropol.services.modify", "--system-bus-name", &sender.to_string(), "--allow-user-interaction"])
            .status();

        if polkit_check.is_err() || !polkit_check.unwrap().success() {
            return Ok(OperationResult {
                success: false,
                code: "permission-denied".to_string(),
                message: "Polkit authentication required or denied.".to_string(),
                operation_id,
                data: HashMap::new(),
            });
        }

        let valid_actions = vec!["start", "stop", "restart", "enable", "disable"];
        if !valid_actions.contains(&action.as_str()) {
            return Ok(OperationResult {
                success: false,
                code: "invalid-argument".to_string(),
                message: "Unsupported systemd action.".to_string(),
                operation_id,
                data: HashMap::new(),
            });
        }

        {
            let mut ops = self.operations.lock().unwrap();
            ops.insert(operation_id.clone(), OpState { status: "running".to_string(), message: format!("Executing {} on {}", action, name) });
        }

        let name_clone = name.clone();
        let action_clone = action.clone();
        let ops_clone = std::sync::Arc::clone(&self.operations);
        let op_id_clone = operation_id.clone();
        let emitter = emitter.to_owned();

        tokio::spawn(async move {
            let operation_succeeded = matches!(
                Command::new("systemctl")
                    .arg(&action_clone)
                    .arg(&name_clone)
                    .status(),
                Ok(status) if status.success()
            );

            {
                let mut ops = ops_clone.lock().unwrap();
                if let Some(state) = ops.get_mut(&op_id_clone) {
                    if operation_succeeded {
                        state.status = "ok".to_string();
                        state.message = format!(
                            "Successfully executed {} on {}",
                            action_clone, name_clone
                        );
                    } else {
                        state.status = "failed".to_string();
                        state.message = format!(
                            "Failed to execute {} on {}",
                            action_clone, name_clone
                        );
                    }
                }
            }

            if operation_succeeded {
                if let Ok(service) =
                    ServicesManager::get_systemd_service(&name_clone).await
                {
                    let _ = ServicesManager::service_state_changed(
                        &emitter,
                        service,
                    )
                    .await;
                }
            }
        });

        Ok(OperationResult {
            success: true,
            code: "ok".to_string(),
            message: format!("Service operation {} queued successfully.", action),
            operation_id,
            data: HashMap::new(),
        })
    }

    #[zbus(signal)]
    async fn service_state_changed(ctxt: &zbus::object_server::SignalEmitter<'_>, service: ServiceInfo) -> zbus::Result<()>;
}