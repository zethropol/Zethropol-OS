use serde::{Deserialize, Serialize};
use std::fs;
use std::net::SocketAddr;
use std::path::{Path, PathBuf};

pub const RUNTIME_VERSION: &str = "0.1.0-stage11";
pub const RUNTIME_BUS_NAME: &str = "org.zethropol.ZciRuntime";
pub const RUNTIME_OBJECT_PATH: &str = "/org/zethropol/ZciRuntime";
#[cfg_attr(not(test), allow(dead_code))]
pub const RUNTIME_INTERFACE: &str = "org.zethropol.ZciRuntime.v1";

pub const DAEMON_BUS_NAME: &str = "org.zethropol.ZciDaemon";
pub const DAEMON_OBJECT_PATH: &str = "/org/zethropol/ZciDaemon";
pub const DAEMON_INTERFACE: &str = "org.zethropol.ZciDaemon.v1";

pub const POLICY_BUS_NAME: &str = "org.zethropol.Security";
pub const POLICY_OBJECT_PATH: &str = "/org/zethropol/Security/Policy";
pub const POLICY_INTERFACE: &str = "org.zethropol.Security.Policy.v1";

pub const POLKIT_ACTION_RUNTIME_LOAD: &str = "org.zethropol.zci.runtime.load";
pub const POLKIT_ACTION_RUNTIME_ADMIN: &str = "org.zethropol.zci.runtime.admin";

pub const DEFAULT_PKCHECK_BIN: &str = "/usr/bin/pkcheck";
pub const DEFAULT_CONFIG_FILE: &str = "/etc/zethropol/zci-runtime.json";
pub const DEFAULT_MANIFEST_FILE: &str = "/etc/zethropol/zci-models-manifest.json";

/// Shared Gemma 4 E4B loopback engine port (distinct from reserved legacy ports 8087/8088/8089).
pub const DEFAULT_ENGINE_ENDPOINT: &str = "127.0.0.1:29087";
pub const RESERVED_LEGACY_PORTS: &[u16] = &[8087, 8088, 8089];
pub const SUPPORTED_AGENTS: &[&str] = &["zci-chat", "zekans-code", "zci-command", "zci-vision"];

pub fn is_supported_agent(agent_id: &str) -> bool {
    SUPPORTED_AGENTS.contains(&agent_id)
}

/// 11.01 — Strictly validates a loopback-only TCP endpoint (`127.0.0.1:<port>` or `[::1]:<port>`).
/// Rejects wildcards (`0.0.0.0`, `::`), external IPs, hostnames, privileged ports (< 1024),
/// and reserved legacy user-webui ports (8087/8088/8089).
pub fn parse_loopback_endpoint(raw: &str) -> Result<SocketAddr, String> {
    let trimmed = raw.trim();
    if trimmed.is_empty()
        || trimmed.contains("://")
        || trimmed.contains('@')
        || trimmed.contains('/')
        || trimmed.chars().any(|c| c == '\0' || c.is_control())
    {
        return Err(format!(
            "Invalid loopback engine socket address syntax: '{trimmed}'"
        ));
    }
    let addr: SocketAddr = trimmed
        .parse()
        .map_err(|_| format!("Only numeric loopback socket addresses are permitted: '{trimmed}'"))?;
    if !addr.ip().is_loopback() {
        return Err(format!(
            "Non-loopback AI engine address rejected (external network forbidden): '{}'",
            addr.ip()
        ));
    }
    if addr.port() < 1024 {
        return Err(format!(
            "Privileged AI engine port (< 1024) rejected: {}",
            addr.port()
        ));
    }
    if RESERVED_LEGACY_PORTS.contains(&addr.port()) {
        return Err(format!(
            "Reserved legacy port ({}) cannot be bound by zci-runtime.",
            addr.port()
        ));
    }
    Ok(addr)
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RuntimeConfigFile {
    pub engine_endpoint: Option<String>,
    pub manifest_file: Option<String>,
    pub idle_sleep_timeout_secs: Option<u64>,
    pub request_timeout_ms: Option<u64>,
    pub policy_timeout_ms: Option<u64>,
    pub max_prompt_bytes: Option<usize>,
    pub require_daemon_bridge: Option<bool>,
}

#[derive(Debug, Clone)]
pub struct RuntimeConfig {
    pub config_file: PathBuf,
    pub manifest_file: PathBuf,
    pub pkcheck_bin: PathBuf,
    pub daemon_bus_name: String,
    pub daemon_object_path: String,
    pub daemon_interface: String,
    pub policy_bus_name: String,
    pub policy_object_path: String,
    pub policy_interface: String,
    pub engine_endpoint: SocketAddr,
    pub idle_sleep_timeout_secs: u64,
    pub request_timeout_ms: u64,
    pub policy_timeout_ms: u64,
    pub max_prompt_bytes: usize,
    pub require_daemon_bridge: bool,
    pub hardware_override_file: Option<PathBuf>,
}

impl Default for RuntimeConfig {
    fn default() -> Self {
        Self {
            config_file: PathBuf::from(DEFAULT_CONFIG_FILE),
            manifest_file: PathBuf::from(DEFAULT_MANIFEST_FILE),
            pkcheck_bin: PathBuf::from(DEFAULT_PKCHECK_BIN),
            daemon_bus_name: DAEMON_BUS_NAME.to_string(),
            daemon_object_path: DAEMON_OBJECT_PATH.to_string(),
            daemon_interface: DAEMON_INTERFACE.to_string(),
            policy_bus_name: POLICY_BUS_NAME.to_string(),
            policy_object_path: POLICY_OBJECT_PATH.to_string(),
            policy_interface: POLICY_INTERFACE.to_string(),
            engine_endpoint: DEFAULT_ENGINE_ENDPOINT
                .parse()
                .expect("static loopback engine endpoint"),
            idle_sleep_timeout_secs: 30,
            request_timeout_ms: 30_000,
            policy_timeout_ms: 4_000,
            max_prompt_bytes: 65_536,
            require_daemon_bridge: true,
            hardware_override_file: None,
        }
    }
}

impl RuntimeConfig {
    /// Compiled-in safety ceilings (cannot be relaxed by external config files).
    pub fn clamp_bounds(&mut self) {
        self.idle_sleep_timeout_secs = self.idle_sleep_timeout_secs.clamp(1, 600);
        self.request_timeout_ms = self.request_timeout_ms.clamp(500, 120_000);
        self.policy_timeout_ms = self.policy_timeout_ms.clamp(500, 15_000);
        self.max_prompt_bytes = self.max_prompt_bytes.clamp(512, 131_072);
    }

    pub fn reload_from_file(&mut self) -> Result<bool, String> {
        self.reload_from_path(&self.config_file.clone())
    }

    pub fn reload_from_path(&mut self, path: &Path) -> Result<bool, String> {
        if !path.exists() {
            self.clamp_bounds();
            return Ok(false);
        }
        let meta = fs::symlink_metadata(path)
            .map_err(|e| format!("Failed to stat runtime config '{}': {e}", path.display()))?;
        if meta.file_type().is_symlink() || !meta.is_file() {
            return Err("Runtime config must be a regular non-symlink file.".to_string());
        }
        if meta.len() > 65_536 {
            return Err("Runtime config exceeds 64 KiB safety limit.".to_string());
        }
        let raw = fs::read_to_string(path)
            .map_err(|e| format!("Failed to read runtime config: {e}"))?;
        let parsed: RuntimeConfigFile = serde_json::from_str(&raw)
            .map_err(|e| format!("Invalid runtime config JSON: {e}"))?;

        if let Some(ref ep) = parsed.engine_endpoint {
            self.engine_endpoint = parse_loopback_endpoint(ep)?;
        }
        if let Some(ref mf) = parsed.manifest_file {
            let p = PathBuf::from(mf);
            if !p.is_absolute() || mf.contains("..") || mf.chars().any(|c| c == '\0' || c.is_control()) {
                return Err("Manifest file path in config must be clean and absolute.".to_string());
            }
            self.manifest_file = p;
        }
        if let Some(v) = parsed.idle_sleep_timeout_secs {
            self.idle_sleep_timeout_secs = v;
        }
        if let Some(v) = parsed.request_timeout_ms {
            self.request_timeout_ms = v;
        }
        if let Some(v) = parsed.policy_timeout_ms {
            self.policy_timeout_ms = v;
        }
        if let Some(v) = parsed.max_prompt_bytes {
            self.max_prompt_bytes = v;
        }
        if let Some(v) = parsed.require_daemon_bridge {
            self.require_daemon_bridge = v;
        }

        self.clamp_bounds();
        Ok(true)
    }
}
