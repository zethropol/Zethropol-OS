use crate::models::{OperationResult, SecurityState};
use std::collections::HashMap;
use std::fs;
use std::process::Command;
use zbus::{
    interface,
    zvariant::{OwnedValue, Value},
};

const PKCHECK_BIN: &str = "/usr/bin/pkcheck";
const UFW_BIN: &str = "/usr/bin/ufw";
const SYSTEMCTL_BIN: &str = "/usr/bin/systemctl";
const SECURITY_MODIFY_ACTION: &str = "org.zethropol.security.modify";

pub fn parse_ufw_status(stdout: &str) -> (bool, &'static str) {
    let lower = stdout.to_ascii_lowercase();
    if lower.contains("status: active")
        || (lower.contains("active") && !lower.contains("inactive"))
    {
        (true, "ufw")
    } else if lower.contains("inactive") {
        (false, "none")
    } else {
        (false, "unknown")
    }
}

pub fn parse_apparmor_profiles(content: &str) -> (i32, i32) {
    let mut loaded: i32 = 0;
    let mut enforcing: i32 = 0;
    for line in content.lines() {
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        loaded = loaded.saturating_add(1);
        if trimmed.ends_with("(enforce)") {
            enforcing = enforcing.saturating_add(1);
        }
    }
    (loaded, enforcing)
}

#[derive(Debug)]
pub struct SecurityService {}

impl SecurityService {
    pub fn new() -> Self {
        Self {}
    }

    fn check_firewall(&self) -> HashMap<String, OwnedValue> {
        let mut map = HashMap::new();
        let output = Command::new(UFW_BIN)
            .env("LC_ALL", "C")
            .arg("status")
            .output();

        let (enabled, profile) = match output {
            Ok(out) if out.status.success() => {
                let stdout = String::from_utf8_lossy(&out.stdout);
                parse_ufw_status(&stdout)
            }
            _ => {
                // Fallback read-only check for firewalld if ufw is unavailable.
                let fw = Command::new(SYSTEMCTL_BIN)
                    .env("LC_ALL", "C")
                    .args(["is-active", "firewalld.service"])
                    .output();
                match fw {
                    Ok(out) if String::from_utf8_lossy(&out.stdout).trim() == "active" => {
                        (true, "firewalld")
                    }
                    _ => (false, "unknown"),
                }
            }
        };

        map.insert("enabled".to_string(), OwnedValue::from(enabled));
        if let Ok(val) = OwnedValue::try_from(Value::from(profile)) {
            map.insert("active_profile".to_string(), val);
        }
        map
    }

    fn check_secure_boot(&self) -> HashMap<String, OwnedValue> {
        let mut map = HashMap::new();
        let efi_path = "/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c";
        let (enabled, mode) = match fs::read(efi_path) {
            Ok(data) if data.get(4) == Some(&1) => (true, "Standard"),
            Ok(_) => (false, "Disabled"),
            Err(_) => (false, "Unknown"),
        };

        map.insert("enabled".to_string(), OwnedValue::from(enabled));
        if let Ok(val) = OwnedValue::try_from(Value::from(mode)) {
            map.insert("mode".to_string(), val);
        }
        map
    }

    fn check_kernel_lsm(&self) -> HashMap<String, OwnedValue> {
        let mut map = HashMap::new();
        let lsm_active: Vec<Value<'_>> = match fs::read_to_string("/sys/kernel/security/lsm") {
            Ok(content) => content
                .split(',')
                .map(|s| s.trim())
                .filter(|s| !s.is_empty())
                .map(|s| Value::from(s.to_string()))
                .collect(),
            Err(_) => vec![Value::from("none")],
        };

        let lockdown_state = match fs::read_to_string("/sys/kernel/security/lockdown") {
            Ok(content) => {
                if content.contains("[integrity]") {
                    "integrity"
                } else if content.contains("[confidentiality]") {
                    "confidentiality"
                } else {
                    "none"
                }
            }
            Err(_) => "none",
        };

        if let Ok(val) = OwnedValue::try_from(Value::from(lockdown_state)) {
            map.insert("lockdown".to_string(), val);
        }
        if let Ok(val) = OwnedValue::try_from(Value::from(lsm_active)) {
            map.insert("lsm_active".to_string(), val);
        }
        map
    }

    fn check_apparmor(&self) -> HashMap<String, OwnedValue> {
        let mut map = HashMap::new();
        let aa_enabled = fs::metadata("/sys/module/apparmor").is_ok();
        let (loaded, enforcing) = if aa_enabled {
            fs::read_to_string("/sys/kernel/security/apparmor/profiles")
                .map(|content| parse_apparmor_profiles(&content))
                .unwrap_or((0, 0))
        } else {
            (0, 0)
        };

        map.insert("enabled".to_string(), OwnedValue::from(aa_enabled));
        map.insert("profiles_loaded".to_string(), OwnedValue::from(loaded));
        map.insert(
            "profiles_enforcing".to_string(),
            OwnedValue::from(enforcing),
        );
        map
    }

    fn collect_security_state(&self) -> SecurityState {
        SecurityState {
            firewall: self.check_firewall(),
            secure_boot: self.check_secure_boot(),
            kernel: self.check_kernel_lsm(),
            apparmor: self.check_apparmor(),
            system_integrity: HashMap::new(),
        }
    }
}

#[interface(name = "org.zethropol.Security.v1")]
impl SecurityService {
    async fn get_state(&self) -> Result<SecurityState, zbus::fdo::Error> {
        Ok(self.collect_security_state())
    }

    async fn toggle_firewall(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
        enable: bool,
    ) -> Result<OperationResult, zbus::fdo::Error> {
        let operation_id = uuid::Uuid::new_v4().to_string();
        let action = if enable { "enable" } else { "disable" };

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
                    SECURITY_MODIFY_ACTION,
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

        let ufw_result = tokio::task::spawn_blocking(move || {
            let mut cmd = Command::new(UFW_BIN);
            cmd.env("LC_ALL", "C");
            if enable {
                cmd.args(["--force", "enable"]);
            } else {
                cmd.arg("disable");
            }
            cmd.status()
        })
        .await;

        match ufw_result {
            Ok(Ok(status)) if status.success() => {
                let updated = self.collect_security_state();
                let _ = Self::state_changed(&emitter, updated).await;
                Ok(OperationResult {
                    success: true,
                    code: "ok".to_string(),
                    message: format!("Firewall {} operation completed.", action),
                    operation_id,
                    data: HashMap::new(),
                })
            }
            Ok(Ok(_)) => Ok(OperationResult {
                success: false,
                code: "operation-failed".to_string(),
                message: format!("Firewall {} operation failed.", action),
                operation_id,
                data: HashMap::new(),
            }),
            Ok(Err(error)) => Ok(OperationResult {
                success: false,
                code: "execution-failed".to_string(),
                message: format!("Failed to execute ufw: {}", error),
                operation_id,
                data: HashMap::new(),
            }),
            Err(error) => Ok(OperationResult {
                success: false,
                code: "execution-failed".to_string(),
                message: format!("Internal execution error: {}", error),
                operation_id,
                data: HashMap::new(),
            }),
        }
    }

    #[zbus(signal)]
    async fn state_changed(
        ctxt: &zbus::object_server::SignalEmitter<'_>,
        new_state: SecurityState,
    ) -> zbus::Result<()>;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_ufw_status_classifies_active_and_inactive() {
        assert_eq!(parse_ufw_status("Status: active\n"), (true, "ufw"));
        assert_eq!(parse_ufw_status("Status: inactive\n"), (false, "none"));
        assert_eq!(parse_ufw_status("ERROR: need root"), (false, "unknown"));
    }

    #[test]
    fn parse_apparmor_profiles_does_not_fabricate_counts() {
        assert_eq!(parse_apparmor_profiles(""), (0, 0));
        let sample = "usr.bin.firefox (enforce)\nusr.bin.curl (complain)\nusr.sbin.cupsd (enforce)\n";
        assert_eq!(parse_apparmor_profiles(sample), (3, 2));
    }

    #[test]
    fn polkit_action_and_absolute_paths_are_pinned() {
        assert_eq!(SECURITY_MODIFY_ACTION, "org.zethropol.security.modify");
        assert!(PKCHECK_BIN.starts_with('/'));
        assert!(UFW_BIN.starts_with('/'));
    }
}
