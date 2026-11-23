use crate::models::{OperationResult, ServiceInfo};
use std::collections::HashMap;
use std::process::Command;
use std::sync::{Arc, Mutex};
use zbus::interface;
use zbus::proxy;
use zbus::Connection;

const PKCHECK_BIN: &str = "/usr/bin/pkcheck";
const SYSTEMCTL_BIN: &str = "/usr/bin/systemctl";
const SERVICES_MODIFY_ACTION: &str = "org.zethropol.services.modify";
const MAX_UNIT_NAME_LEN: usize = 256;

const VALID_ACTIONS: &[&str] = &["start", "stop", "restart", "enable", "disable"];

/// Services that are quarantined and MUST NOT be started, stopped, restarted,
/// enabled, or disabled through the Services management interface.
const QUARANTINED_SERVICES: &[&str] = &["zethropol-performance.service"];

/// Critical system/session services that MUST NOT be stopped, disabled, or restarted
/// through the Services management interface.
const PROTECTED_CRITICAL_SERVICES: &[&str] = &[
    "dbus.service",
    "dbus-broker.service",
    "systemd-journald.service",
    "systemd-logind.service",
    "systemd-udevd.service",
    "polkit.service",
    "zethropol-services.service",
    "zethropol-recovery.service",
    "display-manager.service",
    "sddm.service",
    "plasmalogin.service",
    "gdm.service",
];

pub fn validate_service_request(
    name: &str,
    action: &str,
) -> Result<(), (&'static str, &'static str)> {
    if !VALID_ACTIONS.contains(&action) {
        return Err(("invalid-argument", "Unsupported systemd action."));
    }

    if name.is_empty()
        || name.len() > MAX_UNIT_NAME_LEN
        || name.starts_with('-')
        || name.starts_with('.')
        || !name.ends_with(".service")
        || name.ends_with("@.service")
        || name.contains('/')
        || name.contains('\\')
        || name.contains("..")
        || name.contains('\0')
        || name.chars().any(|c| {
            c.is_whitespace()
                || c.is_control()
                || !(c.is_ascii_alphanumeric() || matches!(c, '_' | '-' | '.' | '@' | ':'))
        })
    {
        return Err(("invalid-unit", "Invalid systemd service unit name."));
    }

    if QUARANTINED_SERVICES.contains(&name) {
        return Err((
            "protected-service",
            "Quarantined service cannot be managed through Services.",
        ));
    }

    if PROTECTED_CRITICAL_SERVICES.contains(&name)
        && matches!(action, "stop" | "disable" | "restart")
    {
        return Err((
            "protected-service",
            "Critical system service cannot be stopped, disabled, or restarted.",
        ));
    }

    Ok(())
}

#[proxy(
    interface = "org.freedesktop.systemd1.Manager",
    default_service = "org.freedesktop.systemd1",
    default_path = "/org/freedesktop/systemd1"
)]
trait SystemdManager {
    async fn list_units(
        &self,
    ) -> zbus::Result<
        Vec<(
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
        )>,
    >;

    async fn get_unit_file_state(&self, unit: &str) -> zbus::Result<String>;

    async fn load_unit(&self, unit: &str) -> zbus::Result<zbus::zvariant::OwnedObjectPath>;
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
        let object_path = manager.load_unit(name).await?;

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
        ) in units
        {
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

    async fn get_operation_state(
        &self,
        operation_id: String,
    ) -> Result<HashMap<String, String>, zbus::fdo::Error> {
        let ops = self
            .operations
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let mut res = HashMap::new();
        if let Some(state) = ops.get(&operation_id) {
            res.insert("status".to_string(), state.status.clone());
            res.insert("message".to_string(), state.message.clone());
        } else {
            res.insert("status".to_string(), "not-found".to_string());
            res.insert(
                "message".to_string(),
                "Operation ID does not exist.".to_string(),
            );
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

        if let Err((code, message)) = validate_service_request(&name, &action) {
            return Ok(OperationResult {
                success: false,
                code: code.to_string(),
                message: message.to_string(),
                operation_id,
                data: HashMap::new(),
            });
        }

        let sender = match header.sender() {
            Some(sender) => sender.to_string(),
            None => {
                return Ok(OperationResult {
                    success: false,
                    code: "invalid-caller".to_string(),
                    message: "D-Bus caller identity is unavailable.".to_string(),
                    operation_id,
                    data: HashMap::new(),
                });
            }
        };

        let polkit_ok = tokio::task::spawn_blocking(move || {
            Command::new(PKCHECK_BIN)
                .args([
                    "--action-id",
                    SERVICES_MODIFY_ACTION,
                    "--system-bus-name",
                    &sender,
                    "--allow-user-interaction",
                ])
                .status()
                .map(|status| status.success())
                .unwrap_or(false)
        })
        .await
        .unwrap_or(false);

        if !polkit_ok {
            return Ok(OperationResult {
                success: false,
                code: "permission-denied".to_string(),
                message: "Polkit authentication required or denied.".to_string(),
                operation_id,
                data: HashMap::new(),
            });
        }

        {
            let mut ops = self
                .operations
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            ops.insert(
                operation_id.clone(),
                OpState {
                    status: "running".to_string(),
                    message: format!("Executing {} on {}", action, name),
                },
            );
        }

        let name_clone = name.clone();
        let action_clone = action.clone();
        let ops_clone = std::sync::Arc::clone(&self.operations);
        let op_id_clone = operation_id.clone();
        let emitter = emitter.to_owned();

        tokio::spawn(async move {
            let cmd_action = action_clone.clone();
            let cmd_name = name_clone.clone();
            let operation_succeeded = tokio::task::spawn_blocking(move || {
                matches!(
                    Command::new(SYSTEMCTL_BIN)
                        .env("LC_ALL", "C")
                        .args([cmd_action.as_str(), "--", cmd_name.as_str()])
                        .status(),
                    Ok(status) if status.success()
                )
            })
            .await
            .unwrap_or(false);

            {
                let mut ops = ops_clone
                    .lock()
                    .unwrap_or_else(|poisoned| poisoned.into_inner());
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
                if let Ok(service) = ServicesManager::get_systemd_service(&name_clone).await {
                    let _ = ServicesManager::service_state_changed(&emitter, service).await;
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
    async fn service_state_changed(
        ctxt: &zbus::object_server::SignalEmitter<'_>,
        service: ServiceInfo,
    ) -> zbus::Result<()>;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_flag_path_target_and_template_injection() {
        for bad in [
            "",
            "--now",
            "--root=/tmp",
            "-f.service",
            "/tmp/evil.service",
            "../evil.service",
            "reboot.target",
            "poweroff.target",
            "foo@.service",
            "cups.service;id",
            "cups.service\n",
            "cups .service",
            ".hidden.service",
        ] {
            let res = validate_service_request(bad, "start");
            assert!(res.is_err(), "expected invalid unit for {bad:?}");
        }
    }

    #[test]
    fn rejects_unsupported_actions() {
        for bad_action in ["mask", "unmask", "kill", "daemon-reexec", "edit", ""] {
            assert_eq!(
                validate_service_request("bluetooth.service", bad_action).unwrap_err().0,
                "invalid-argument"
            );
        }
    }

    #[test]
    fn blocks_quarantined_performance_service_for_all_actions() {
        for action in VALID_ACTIONS {
            assert_eq!(
                validate_service_request("zethropol-performance.service", action)
                    .unwrap_err()
                    .0,
                "protected-service"
            );
        }
    }

    #[test]
    fn protects_critical_system_services_against_stop_disable_restart() {
        for unit in [
            "dbus-broker.service",
            "systemd-journald.service",
            "systemd-logind.service",
            "polkit.service",
            "zethropol-services.service",
            "zethropol-recovery.service",
            "plasmalogin.service",
        ] {
            for action in ["stop", "disable", "restart"] {
                assert_eq!(
                    validate_service_request(unit, action).unwrap_err().0,
                    "protected-service",
                    "expected {unit} {action} to be protected"
                );
            }
            assert!(validate_service_request(unit, "start").is_ok());
        }
    }

    #[test]
    fn accepts_valid_non_critical_services_and_instances() {
        assert!(validate_service_request("bluetooth.service", "restart").is_ok());
        assert!(validate_service_request("cups.service", "stop").is_ok());
        assert!(validate_service_request("user@1000.service", "start").is_ok());
    }
}
