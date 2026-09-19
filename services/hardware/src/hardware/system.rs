use std::collections::HashMap;
use std::fs;
use std::process::Command;

pub struct SystemInfo {
    pub os_name: String,
    pub os_version: String,
    pub base_system: String,
    pub architecture: String,
    pub kernel_version: String,
    pub desktop_name: String,
    pub desktop_version: String,
    pub session_type: String,
    pub hostname: String,
    pub uptime_seconds: Option<f64>,
}

pub fn read() -> SystemInfo {
    let os_release = read_os_release();

    let os_name = os_release.get("NAME").cloned().unwrap_or_else(|| "Unknown".to_string());
    let os_version = os_release.get("VERSION_ID").cloned().unwrap_or_else(|| "Unknown".to_string());
    let base_system = os_release.get("ID").cloned().unwrap_or_else(|| "Unknown".to_string());
    let architecture = command_output("uname", &["-m"]).unwrap_or_else(|| "Unknown".to_string());
    let kernel_version = command_output("uname", &["-r"]).unwrap_or_else(|| "Unknown".to_string());
    let desktop_name = std::env::var("XDG_CURRENT_DESKTOP").unwrap_or_else(|_| "Unknown".to_string());
    let desktop_version = std::env::var("KDE_SESSION_VERSION").unwrap_or_else(|_| "Unknown".to_string());
    let session_type = std::env::var("XDG_SESSION_TYPE").unwrap_or_else(|_| "Unknown".to_string());
    let hostname = command_output("hostname", &[]).unwrap_or_else(|| "Unknown".to_string());
    let uptime_seconds = fs::read_to_string("/proc/uptime").ok().and_then(|value| value.split_whitespace().next().and_then(|value| value.parse::<f64>().ok()));

    SystemInfo {
        os_name,
        os_version,
        base_system,
        architecture,
        kernel_version,
        desktop_name,
        desktop_version,
        session_type,
        hostname,
        uptime_seconds,
    }
}

fn read_os_release() -> HashMap<String, String> {
    let mut values = HashMap::new();
    let content = match fs::read_to_string("/etc/os-release") {
        Ok(content) => content,
        Err(_) => return values,
    };

    for line in content.lines() {
        let Some((key, value)) = line.split_once("=") else { continue; };
        values.insert(key.to_string(), value.trim().trim_matches('"' ).to_string());
    }

    values
}

fn command_output(program: &str, args: &[&str]) -> Option<String> {
    let output = Command::new(program).args(args).output().ok()?;
    if !output.status.success() { return None; }
    let value = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if value.is_empty() { None } else { Some(value) }
}
