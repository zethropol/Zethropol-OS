use std::env;
use std::path::PathBuf;

pub const AGENT_VERSION: &str = "0.1.0-stage13";
pub const AGENT_BUS_NAME: &str = "org.zethropol.ZciAgent";
pub const AGENT_OBJECT_PATH: &str = "/org/zethropol/ZciAgent";
#[cfg_attr(not(test), allow(dead_code))]
pub const AGENT_INTERFACE: &str = "org.zethropol.ZciAgent.v1";

pub const DEFAULT_AGENT_PROFILE: &str = "zekans-code";
pub const SUPPORTED_AGENT_PROFILES: &[&str] = &["zekans-code", "zci-command"];

pub const RUNTIME_BUS_NAME: &str = "org.zethropol.ZciRuntime";
pub const RUNTIME_OBJECT_PATH: &str = "/org/zethropol/ZciRuntime";
pub const RUNTIME_INTERFACE: &str = "org.zethropol.ZciRuntime.v1";

pub const DAEMON_BUS_NAME: &str = "org.zethropol.ZciDaemon";
pub const DAEMON_OBJECT_PATH: &str = "/org/zethropol/ZciDaemon";
pub const DAEMON_INTERFACE: &str = "org.zethropol.ZciDaemon.v1";

pub const POLICY_BUS_NAME: &str = "org.zethropol.Security";
pub const POLICY_OBJECT_PATH: &str = "/org/zethropol/Security/Policy";
pub const POLICY_INTERFACE: &str = "org.zethropol.Security.Policy.v1";

pub const RECOVERY_BUS_NAME: &str = "org.zethropol.Recovery";
pub const RECOVERY_OBJECT_PATH: &str = "/org/zethropol/Recovery";
pub const RECOVERY_INTERFACE: &str = "org.zethropol.Recovery.v1";

pub const MAX_PLANS: usize = 64;
pub const MAX_STEPS_PER_PLAN: usize = 8;
pub const MAX_GOAL_BYTES: usize = 8_192;
pub const MAX_WRITE_FILE_BYTES: usize = 262_144;

/// Strictly non-executable structured planning prompt (never mentions protected self-mod tokens).
pub const PLANNER_SYSTEM_PROMPT: &str = "\
Sen Zethropol OS ZCI Komut Ajanı ve ZEKANS planlayıcısısın. \
Kullanıcının isteğini yalnızca izin listeli (allowlist) yapılandırılmış JSON adımlarına dönüştür. \
Ham kabuk komutu üretme. Yalnızca şu action_kind değerlerini kullan: \
read_file, project_analysis, system_diagnostics, recovery_readiness, write_workspace_file, create_workspace_file. \
Çıktı yalnızca geçerli JSON olsun: {\"summary\":\"...\",\"steps\":[{\"action_kind\":\"...\",\"target\":\"...\",\"dbus_interface\":\"\",\"method\":\"\",\"content\":\"\",\"description\":\"...\"}]}.";

#[derive(Debug, Clone)]
pub struct AgentConfig {
    pub storage_dir: PathBuf,
    pub home_dir: PathBuf,
    pub extra_workspace_roots: Vec<PathBuf>,
    pub runtime_bus_name: String,
    pub runtime_object_path: String,
    pub runtime_interface: String,
    pub daemon_bus_name: String,
    pub daemon_object_path: String,
    pub daemon_interface: String,
    pub policy_bus_name: String,
    pub policy_object_path: String,
    pub policy_interface: String,
    pub recovery_bus_name: String,
    pub recovery_object_path: String,
    pub recovery_interface: String,
    pub dbus_timeout_ms: u64,
    pub inference_timeout_ms: u64,
}

pub fn default_home_dir() -> PathBuf {
    if let Ok(home) = env::var("HOME") {
        let trimmed = home.trim();
        if !trimmed.is_empty() && trimmed.starts_with('/') && !trimmed.contains("..") {
            return PathBuf::from(trimmed);
        }
    }
    PathBuf::from("/tmp")
}

pub fn default_user_storage_dir() -> PathBuf {
    if let Ok(xdg) = env::var("XDG_DATA_HOME") {
        let trimmed = xdg.trim();
        if !trimmed.is_empty() && trimmed.starts_with('/') && !trimmed.contains("..") {
            return PathBuf::from(trimmed).join("zethropol").join("zci-agent");
        }
    }
    default_home_dir()
        .join(".local")
        .join("share")
        .join("zethropol")
        .join("zci-agent")
}

pub fn is_supported_profile(profile: &str) -> bool {
    SUPPORTED_AGENT_PROFILES.contains(&profile)
}

impl Default for AgentConfig {
    fn default() -> Self {
        Self {
            storage_dir: default_user_storage_dir(),
            home_dir: default_home_dir(),
            extra_workspace_roots: Vec::new(),
            runtime_bus_name: RUNTIME_BUS_NAME.to_string(),
            runtime_object_path: RUNTIME_OBJECT_PATH.to_string(),
            runtime_interface: RUNTIME_INTERFACE.to_string(),
            daemon_bus_name: DAEMON_BUS_NAME.to_string(),
            daemon_object_path: DAEMON_OBJECT_PATH.to_string(),
            daemon_interface: DAEMON_INTERFACE.to_string(),
            policy_bus_name: POLICY_BUS_NAME.to_string(),
            policy_object_path: POLICY_OBJECT_PATH.to_string(),
            policy_interface: POLICY_INTERFACE.to_string(),
            recovery_bus_name: RECOVERY_BUS_NAME.to_string(),
            recovery_object_path: RECOVERY_OBJECT_PATH.to_string(),
            recovery_interface: RECOVERY_INTERFACE.to_string(),
            dbus_timeout_ms: 4_000,
            inference_timeout_ms: 35_000,
        }
    }
}
