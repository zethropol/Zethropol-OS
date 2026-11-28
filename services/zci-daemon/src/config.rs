use serde::{Deserialize, Serialize};
use std::fs;
use std::net::SocketAddr;
use std::path::{Path, PathBuf};

pub const DAEMON_VERSION: &str = "0.1.0-stage10";
pub const DAEMON_BUS_NAME: &str = "org.zethropol.ZciDaemon";
pub const DAEMON_OBJECT_PATH: &str = "/org/zethropol/ZciDaemon";
#[cfg_attr(not(test), allow(dead_code))]
pub const DAEMON_INTERFACE: &str = "org.zethropol.ZciDaemon.v1";

pub const POLICY_BUS_NAME: &str = "org.zethropol.Security";
pub const POLICY_OBJECT_PATH: &str = "/org/zethropol/Security/Policy";
pub const POLICY_INTERFACE: &str = "org.zethropol.Security.Policy.v1";

pub const POLKIT_ACTION_DAEMON_ADMIN: &str = "org.zethropol.zci.daemon.admin";
pub const DEFAULT_PKCHECK_BIN: &str = "/usr/bin/pkcheck";
pub const DEFAULT_CONFIG_FILE: &str = "/etc/zethropol/zci-daemon.json";

pub const DEFAULT_PASSIVE_MODEL_ID: &str = "gemma-4-E4B-it-Q4_K_M";
pub const DEFAULT_PASSIVE_MODEL_PROFILE: &str = "passive-standby-hybrid-cpu-ram-gpu";
pub const DEFAULT_ZCI_CHAT_GATEWAY: &str = "127.0.0.1:19086";
pub const DEFAULT_ZEKANS_GATEWAY: &str = "127.0.0.1:29016";

pub const SUPPORTED_AGENTS: &[&str] = &["zci-chat", "zekans-code", "zci-command", "zci-vision"];

pub fn is_supported_agent(agent_id: &str) -> bool {
    SUPPORTED_AGENTS.contains(&agent_id)
}

pub fn supported_agent_list() -> Vec<String> {
    SUPPORTED_AGENTS.iter().map(|s| (*s).to_string()).collect()
}

/// Strictly validates a loopback-only TCP `127.0.0.1:<port>` (or `[::1]:<port>`) unprivileged address.
/// Rejects wildcards (`0.0.0.0`), external IPs, hostnames, and privileged ports (< 1024).
pub fn parse_loopback_gateway_addr(raw: &str) -> Result<SocketAddr, String> {
    let trimmed = raw.trim();
    let addr: SocketAddr = trimmed
        .parse()
        .map_err(|_| format!("Invalid loopback gateway socket address: '{trimmed}'"))?;
    if !addr.ip().is_loopback() {
        return Err(format!(
            "Non-loopback model gateway address rejected: '{}'",
            addr.ip()
        ));
    }
    if addr.port() < 1024 {
        return Err(format!(
            "Privileged model gateway port (< 1024) rejected: {}",
            addr.port()
        ));
    }
    Ok(addr)
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RuntimeOverridesFile {
    pub max_concurrent_global: Option<u32>,
    pub max_concurrent_per_agent: Option<u32>,
    pub rate_limit_max_requests: Option<u32>,
    pub rate_limit_window_secs: Option<u64>,
    pub action_timeout_ms: Option<u64>,
    pub session_idle_timeout_secs: Option<u64>,
    pub max_read_file_bytes: Option<u64>,
    pub model_gateway_addr: Option<String>,
}

#[derive(Debug, Clone)]
pub struct DaemonConfig {
    pub config_file: PathBuf,
    pub pkcheck_bin: PathBuf,
    pub policy_bus_name: String,
    pub policy_object_path: String,
    pub policy_interface: String,
    pub policy_timeout_ms: u64,
    pub max_concurrent_global: u32,
    pub max_concurrent_per_agent: u32,
    pub rate_limit_max_requests: u32,
    pub rate_limit_window_secs: u64,
    pub action_timeout_ms: u64,
    pub session_idle_timeout_secs: u64,
    pub max_read_file_bytes: u64,
    pub max_project_entries: usize,
    pub max_project_depth: usize,
    pub zci_chat_gateway: SocketAddr,
    pub zekans_gateway: SocketAddr,
    pub model_runtime_dir_override: Option<PathBuf>,
    pub passive_model_active_vram_estimate_mb: u64,
}

impl Default for DaemonConfig {
    fn default() -> Self {
        Self {
            config_file: PathBuf::from(DEFAULT_CONFIG_FILE),
            pkcheck_bin: PathBuf::from(DEFAULT_PKCHECK_BIN),
            policy_bus_name: POLICY_BUS_NAME.to_string(),
            policy_object_path: POLICY_OBJECT_PATH.to_string(),
            policy_interface: POLICY_INTERFACE.to_string(),
            policy_timeout_ms: 4_000,
            max_concurrent_global: 4,
            max_concurrent_per_agent: 2,
            rate_limit_max_requests: 30,
            rate_limit_window_secs: 60,
            action_timeout_ms: 8_000,
            session_idle_timeout_secs: 300,
            max_read_file_bytes: 262_144,
            max_project_entries: 200,
            max_project_depth: 3,
            zci_chat_gateway: DEFAULT_ZCI_CHAT_GATEWAY
                .parse()
                .expect("static loopback chat gateway"),
            zekans_gateway: DEFAULT_ZEKANS_GATEWAY
                .parse()
                .expect("static loopback zekans gateway"),
            model_runtime_dir_override: None,
            passive_model_active_vram_estimate_mb: 1_536,
        }
    }
}

impl DaemonConfig {
    /// Enforce hard compiled-in safety ceilings regardless of reloaded config file contents.
    pub fn clamp_bounds(&mut self) {
        self.policy_timeout_ms = self.policy_timeout_ms.clamp(500, 15_000);
        self.max_concurrent_global = self.max_concurrent_global.clamp(1, 8);
        self.max_concurrent_per_agent = self
            .max_concurrent_per_agent
            .clamp(1, self.max_concurrent_global.min(4));
        self.rate_limit_max_requests = self.rate_limit_max_requests.clamp(1, 120);
        self.rate_limit_window_secs = self.rate_limit_window_secs.clamp(5, 600);
        self.action_timeout_ms = self.action_timeout_ms.clamp(500, 30_000);
        self.session_idle_timeout_secs = self.session_idle_timeout_secs.clamp(10, 3_600);
        self.max_read_file_bytes = self.max_read_file_bytes.clamp(1_024, 524_288);
        self.max_project_entries = self.max_project_entries.clamp(10, 500);
        self.max_project_depth = self.max_project_depth.clamp(1, 5);
    }

    /// Safely apply optional bounded overrides from `self.config_file`.
    /// Never disables fail-closed Policy Engine gating, untrusted wrapping, or Polkit admin checks.
    pub fn reload_from_file(&mut self) -> Result<bool, String> {
        self.reload_from_path(&self.config_file.clone())
    }

    pub fn reload_from_path(&mut self, path: &Path) -> Result<bool, String> {
        if !path.exists() {
            self.clamp_bounds();
            return Ok(false);
        }
        let meta = fs::symlink_metadata(path)
            .map_err(|e| format!("Failed to stat daemon config '{}': {e}", path.display()))?;
        if meta.file_type().is_symlink() || !meta.is_file() {
            return Err("Daemon config must be a regular non-symlink file.".to_string());
        }
        if meta.len() > 65_536 {
            return Err("Daemon config exceeds 64 KiB safety limit.".to_string());
        }
        let raw = fs::read_to_string(path)
            .map_err(|e| format!("Failed to read daemon config: {e}"))?;
        let overrides: RuntimeOverridesFile = serde_json::from_str(&raw)
            .map_err(|e| format!("Invalid daemon config JSON: {e}"))?;

        if let Some(v) = overrides.max_concurrent_global {
            self.max_concurrent_global = v;
        }
        if let Some(v) = overrides.max_concurrent_per_agent {
            self.max_concurrent_per_agent = v;
        }
        if let Some(v) = overrides.rate_limit_max_requests {
            self.rate_limit_max_requests = v;
        }
        if let Some(v) = overrides.rate_limit_window_secs {
            self.rate_limit_window_secs = v;
        }
        if let Some(v) = overrides.action_timeout_ms {
            self.action_timeout_ms = v;
        }
        if let Some(v) = overrides.session_idle_timeout_secs {
            self.session_idle_timeout_secs = v;
        }
        if let Some(v) = overrides.max_read_file_bytes {
            self.max_read_file_bytes = v;
        }
        if let Some(ref gw) = overrides.model_gateway_addr {
            let parsed = parse_loopback_gateway_addr(gw)?;
            self.zci_chat_gateway = parsed;
            self.zekans_gateway = parsed;
        }

        self.clamp_bounds();
        Ok(true)
    }

    pub fn gateway_addr_for_agent(&self, agent_id: &str) -> SocketAddr {
        if agent_id == "zekans-code" {
            self.zekans_gateway
        } else {
            self.zci_chat_gateway
        }
    }
}
