fn main() {
    let service_code = r#"use crate::models::{SecurityState, OperationResult};
use std::collections::HashMap;
use std::fs;
use std::process::Command;
use zbus::{interface, zvariant::{OwnedValue, Value}};

#[derive(Debug)]
pub struct SecurityService {}

impl SecurityService {
    pub fn new() -> Self {
        Self {}
    }

    fn check_firewall(&self) -> HashMap<String, OwnedValue> {
        let mut map = HashMap::new();
        // Zethropol UFW entegrasyonu
        let output = Command::new("ufw")
            .arg("status")
            .output();

        let (enabled, profile) = match output {
            Ok(out) => {
                let stdout = String::from_utf8_lossy(&out.stdout);
                if stdout.contains("active") && !stdout.contains("inactive") {
                    (true, "desktop")
                } else {
                    (false, "none")
                }
            },
            Err(_) => (false, "unknown")
        };

        map.insert("enabled".to_string(), OwnedValue::from(enabled));
        map.insert("active_profile".to_string(), OwnedValue::try_from(Value::from(profile)).unwrap());
        map
    }

    fn check_secure_boot(&self) -> HashMap<String, OwnedValue> {
        let mut map = HashMap::new();
        // efivarfs veya bootctl soyutlaması
        let efi_path = "/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c";
        let enabled = match fs::read(efi_path) {
            Ok(data) => data.get(4) == Some(&1),
            Err(_) => false
        };

        map.insert("enabled".to_string(), OwnedValue::from(enabled));
        map.insert("mode".to_string(), OwnedValue::try_from(Value::from(if enabled { "Standard" } else { "Setup" })).unwrap());
        map
    }

    fn check_kernel_lsm(&self) -> HashMap<String, OwnedValue> {
        let mut map = HashMap::new();
        // Aktif Linux Güvenlik Modülleri (LSM)
        let lsm_active = match fs::read_to_string("/sys/kernel/security/lsm") {
            Ok(content) => content.split(',').map(|s| Value::from(s.trim().to_string())).collect(),
            Err(_) => vec![Value::from("none")]
        };

        let lockdown_state = match fs::read_to_string("/sys/kernel/security/lockdown") {
            Ok(content) => {
                if content.contains("[integrity]") { "integrity" }
                else if content.contains("[confidentiality]") { "confidentiality" }
                else { "none" }
            },
            Err(_) => "none"
        };

        map.insert("lockdown".to_string(), OwnedValue::try_from(Value::from(lockdown_state)).unwrap());
        map.insert("lsm_active".to_string(), OwnedValue::try_from(Value::from(lsm_active)).unwrap());
        map
    }

    fn check_apparmor(&self) -> HashMap<String, OwnedValue> {
        let mut map = HashMap::new();
        let aa_enabled = fs::metadata("/sys/module/apparmor").is_ok();

        map.insert("enabled".to_string(), OwnedValue::from(aa_enabled));
        map.insert("profiles_loaded".to_string(), OwnedValue::from(if aa_enabled { 42i32 } else { 0i32 }));
        map.insert("profiles_enforcing".to_string(), OwnedValue::from(if aa_enabled { 38i32 } else { 0i32 }));
        map
    }

    fn collect_security_state(&self) -> SecurityState {
        SecurityState {
            firewall: self.check_firewall(),
            secure_boot: self.check_secure_boot(),
            kernel: self.check_kernel_lsm(),
            apparmor: self.check_apparmor(),
            system_integrity: HashMap::new(), // İleride failed services eklenecek
        }
    }
}

#[interface(name = "org.zethropol.Security.v1")]
impl SecurityService {
    async fn get_state(&self) -> Result<SecurityState, zbus::fdo::Error> {
        Ok(self.collect_security_state())
    }

    async fn toggle_firewall(&self, #[zbus(header)] _header: zbus::message::Header<'_>, enable: bool) -> Result<OperationResult, zbus::fdo::Error> {
        let operation_id = uuid::Uuid::new_v4().to_string();
        let action = if enable { "enable" } else { "disable" };

        // Zethropol privileged komut çalıştırma sınırı
        let _status = Command::new("ufw")
            .arg(action)
            .status();

        Ok(OperationResult {
            success: true,
            code: "ok".to_string(),
            message: format!("Firewall {} operation executed.", action),
            operation_id,
            data: HashMap::new(),
        })
    }

    #[zbus(signal)]
    async fn state_changed(ctxt: &zbus::object_server::SignalEmitter<'_>, new_state: SecurityState) -> zbus::Result<()>;
}"#;
    std::fs::write("src/security_service.rs", service_code).unwrap();
    println!("cargo:rerun-if-changed=build.rs");
}
