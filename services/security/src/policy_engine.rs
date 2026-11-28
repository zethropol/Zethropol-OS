use crate::audit_log::{AuditEvent, AuditEventInput, AuditLogger, AuditQueryResult};
use crate::crypto_util::now_unix_secs;
use crate::policy_state::{AgentPenaltyState, PolicyStateStore};
use crate::untrusted::{inspect_and_wrap_untrusted, UntrustedInspection};
use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;
use std::fs;
use std::path::{Component, Path, PathBuf};

pub const DEFAULT_STATE_FILE: &str = "/var/lib/zethropol-security/zci-policy-state.json";
pub const DEFAULT_AUDIT_LOG_FILE: &str = "/var/log/zethropol-security/zci-audit.jsonl";
pub const POLICY_ENGINE_VERSION: &str = "0.1.0-stage08";

/// 08.01 — Deterministic Action Classification.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum ActionClass {
    ReadOnly,
    UserSpaceSafe,
    SystemStateChange,
    Privileged,
    SelfModificationDenied,
}

impl ActionClass {
    pub fn as_str(&self) -> &'static str {
        match self {
            ActionClass::ReadOnly => "ReadOnly",
            ActionClass::UserSpaceSafe => "UserSpaceSafe",
            ActionClass::SystemStateChange => "SystemStateChange",
            ActionClass::Privileged => "Privileged",
            ActionClass::SelfModificationDenied => "SelfModificationDenied",
        }
    }
}

#[derive(Debug, Clone)]
pub struct PolicyConfig {
    pub state_file: PathBuf,
    pub audit_log_file: PathBuf,
    /// Optional explicit user home override for deterministic tests.
    /// In production (None), home directory is resolved solely from `/etc/passwd` by verified D-Bus caller UID.
    pub home_override: Option<PathBuf>,
    /// Additional explicit workspace roots allowed for `zekans-code` (used in tests or explicit workspace setup).
    pub extra_workspace_roots: Vec<PathBuf>,
    pub emit_audit_stderr: bool,
}

impl Default for PolicyConfig {
    fn default() -> Self {
        Self {
            state_file: PathBuf::from(DEFAULT_STATE_FILE),
            audit_log_file: PathBuf::from(DEFAULT_AUDIT_LOG_FILE),
            home_override: None,
            extra_workspace_roots: Vec::new(),
            emit_audit_stderr: true,
        }
    }
}

#[derive(Debug, Clone)]
pub struct ActionRequest {
    pub agent_id: String,
    pub operation: String,
    pub target: String,
    pub dbus_interface: String,
    pub untrusted_payload: String,
    pub caller_bus_name: String,
    pub caller_uid: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PolicyDecision {
    pub allowed: bool,
    pub decision: String,
    pub action_class: ActionClass,
    pub requires_polkit: bool,
    pub polkit_action: String,
    pub rule_id: String,
    pub code: String,
    pub reason: String,
    pub penalty_delta: u32,
    pub penalty_score: u32,
    pub penalty_level: u8,
    pub lockout_active: bool,
    pub hard_locked: bool,
    pub cooldown_remaining_secs: u64,
    pub untrusted_flags: Vec<String>,
    pub audit_event_id: String,
}

#[derive(Debug, Clone)]
struct AgentAllowlist {
    agent_id: &'static str,
    allowed_operations: &'static [&'static str],
    allowed_dbus_calls: &'static [(&'static str, &'static str)],
    allow_workspace_read: bool,
    allow_workspace_write: bool,
    read_subpaths_under_home: &'static [&'static str],
    write_subpaths_under_home: &'static [&'static str],
    read_tmp_prefixes: &'static [&'static str],
    write_tmp_prefixes: &'static [&'static str],
}

/// 08.02 & 08.03 — Immutable compiled-in least-privilege agent allowlists.
/// Unknown agents and unlisted operations/interfaces/paths are denied by default.
const AGENT_PROFILES: &[AgentAllowlist] = &[
    AgentAllowlist {
        agent_id: "zci-chat",
        allowed_operations: &["read_context", "inspect_untrusted", "dbus_call", "health_check"],
        allowed_dbus_calls: &[
            ("org.zethropol.Hardware.v1", "GetTelemetry"),
            ("org.zethropol.Hardware.v1", "GetState"),
            ("org.zethropol.Monitoring.v1", "GetState"),
            ("org.zethropol.Security.v1", "GetState"),
            ("org.zethropol.Security.Policy.v1", "GetPolicySummary"),
            ("org.zethropol.Security.Policy.v1", "GetAgentStatus"),
            ("org.zethropol.Security.Policy.v1", "WrapUntrustedData"),
        ],
        allow_workspace_read: false,
        allow_workspace_write: false,
        read_subpaths_under_home: &[".local/share/zci/chat-context", ".local/share/zci/models"],
        write_subpaths_under_home: &[],
        read_tmp_prefixes: &["/tmp/zci-chat"],
        write_tmp_prefixes: &[],
    },
    AgentAllowlist {
        agent_id: "zekans-code",
        allowed_operations: &[
            "read_file",
            "list_dir",
            "write_file",
            "create_file",
            "run_tests",
            "inspect_untrusted",
            "dbus_call",
            "health_check",
        ],
        allowed_dbus_calls: &[
            ("org.zethropol.Hardware.v1", "GetTelemetry"),
            ("org.zethropol.Diagnostics.v1", "GetState"),
            ("org.zethropol.Security.Policy.v1", "GetPolicySummary"),
            ("org.zethropol.Security.Policy.v1", "GetAgentStatus"),
            ("org.zethropol.Security.Policy.v1", "WrapUntrustedData"),
        ],
        allow_workspace_read: true,
        allow_workspace_write: true,
        read_subpaths_under_home: &["Zethropol-OS", "Projects", ".local/share/zci/agent"],
        write_subpaths_under_home: &["Zethropol-OS", "Projects"],
        read_tmp_prefixes: &["/tmp/zekans-work"],
        write_tmp_prefixes: &["/tmp/zekans-work"],
    },
    AgentAllowlist {
        agent_id: "zci-command",
        allowed_operations: &["read_status", "dbus_call", "inspect_untrusted", "health_check"],
        allowed_dbus_calls: &[
            ("org.zethropol.Hardware.v1", "GetTelemetry"),
            ("org.zethropol.Hardware.v1", "GetState"),
            ("org.zethropol.Monitoring.v1", "GetState"),
            ("org.zethropol.Performance.v1", "GetState"),
            ("org.zethropol.Performance.v1", "GetProfileState"),
            ("org.zethropol.Performance.v1", "GetGamingReadiness"),
            ("org.zethropol.Power.v1", "GetState"),
            ("org.zethropol.Diagnostics.v1", "GetState"),
            ("org.zethropol.Services.v1", "ListServices"),
            ("org.zethropol.Services.v1", "GetState"),
            ("org.zethropol.Security.v1", "GetState"),
            ("org.zethropol.Security.Policy.v1", "GetPolicySummary"),
            ("org.zethropol.Security.Policy.v1", "GetAgentStatus"),
            ("org.zethropol.Security.Policy.v1", "WrapUntrustedData"),
        ],
        allow_workspace_read: false,
        allow_workspace_write: false,
        read_subpaths_under_home: &[".local/share/zci/command-cache"],
        write_subpaths_under_home: &[],
        read_tmp_prefixes: &["/tmp/zci-command"],
        write_tmp_prefixes: &["/tmp/zci-command"],
    },
    AgentAllowlist {
        agent_id: "zci-vision",
        allowed_operations: &[
            "read_image",
            "write_output",
            "run_workflow_template",
            "inspect_untrusted",
            "dbus_call",
            "health_check",
        ],
        allowed_dbus_calls: &[
            ("org.zethropol.Hardware.v1", "GetTelemetry"),
            ("org.zethropol.Performance.v1", "GetState"),
            ("org.zethropol.Performance.v1", "GetGamingReadiness"),
            ("org.zethropol.Security.Policy.v1", "GetPolicySummary"),
            ("org.zethropol.Security.Policy.v1", "GetAgentStatus"),
            ("org.zethropol.Security.Policy.v1", "WrapUntrustedData"),
        ],
        allow_workspace_read: false,
        allow_workspace_write: false,
        read_subpaths_under_home: &[".local/share/zci/zepica", "Pictures"],
        write_subpaths_under_home: &[".local/share/zci/zepica/outputs"],
        read_tmp_prefixes: &["/tmp/zci-vision"],
        write_tmp_prefixes: &["/tmp/zci-vision"],
    },
];

pub fn supported_agent_ids() -> Vec<String> {
    AGENT_PROFILES
        .iter()
        .map(|p| p.agent_id.to_string())
        .collect()
}

fn find_agent_profile(agent_id: &str) -> Option<&'static AgentAllowlist> {
    AGENT_PROFILES.iter().find(|p| p.agent_id == agent_id)
}

/// Resolve user home directory purely from `/etc/passwd` by numeric UID (never from request input).
pub fn resolve_home_for_uid(uid: u32) -> Option<PathBuf> {
    let passwd = fs::read_to_string("/etc/passwd").ok()?;
    for line in passwd.lines() {
        let fields: Vec<&str> = line.split(':').collect();
        if fields.len() >= 6 {
            if let Ok(entry_uid) = fields[2].parse::<u32>() {
                if entry_uid == uid && fields[5].starts_with('/') {
                    return Some(PathBuf::from(fields[5]));
                }
            }
        }
    }
    None
}

fn has_control_or_nul(s: &str) -> bool {
    s.chars().any(|c| c == '\0' || c.is_control())
}

fn is_valid_agent_id_syntax(agent_id: &str) -> bool {
    !agent_id.is_empty()
        && agent_id.len() <= 64
        && agent_id
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-' || b == b'_')
}

fn is_mutating_operation(op: &str) -> bool {
    matches!(
        op,
        "write_file"
            | "create_file"
            | "delete_file"
            | "remove_file"
            | "rename_file"
            | "chmod"
            | "chown"
            | "symlink"
            | "patch_file"
            | "edit_file"
            | "append_file"
            | "truncate_file"
            | "write_output"
            | "modify_policy"
            | "disable_policy"
            | "disable_self_mod_lock"
            | "reset_own_lockout"
            | "modify_polkit"
            | "modify_dbus_policy"
            | "modify_systemd_unit"
            | "modify_recovery_locks"
            | "replace_security_binary"
            | "exec_command"
            | "shell_exec"
            | "install_package"
            | "remove_package"
            | "toggle_firewall"
            | "set_profile"
            | "start_service"
            | "stop_service"
            | "restart_service"
            | "rollback"
            | "repair"
    )
}

fn is_file_operation(op: &str) -> bool {
    matches!(
        op,
        "read_context"
            | "read_file"
            | "list_dir"
            | "write_file"
            | "create_file"
            | "delete_file"
            | "remove_file"
            | "rename_file"
            | "chmod"
            | "chown"
            | "symlink"
            | "patch_file"
            | "edit_file"
            | "append_file"
            | "truncate_file"
            | "read_image"
            | "write_output"
            | "read_status"
            | "run_tests"
            | "run_workflow_template"
    )
}

#[derive(Debug, Clone)]
enum NormalizedPath {
    Clean { lexical: PathBuf, canonical: PathBuf },
    TraversalRejected(&'static str),
}

fn normalize_and_resolve_path(raw: &str, home_dir: Option<&Path>) -> NormalizedPath {
    if raw.is_empty() || raw.len() > 2048 || has_control_or_nul(raw) {
        return NormalizedPath::TraversalRejected("PATH_INVALID_SYNTAX");
    }
    let lower = raw.to_ascii_lowercase();
    if raw.contains('\\')
        || lower.contains("%2e")
        || lower.contains("%2f")
        || lower.contains("%5c")
        || lower.contains("%00")
    {
        return NormalizedPath::TraversalRejected("PATH_ENCODED_OR_BACKSLASH_TRAVERSAL");
    }

    let expanded = if raw == "~" || raw.starts_with("~/") {
        let Some(home) = home_dir else {
            return NormalizedPath::TraversalRejected("PATH_HOME_UNRESOLVED");
        };
        if raw == "~" {
            home.to_path_buf()
        } else {
            home.join(&raw[2..])
        }
    } else {
        PathBuf::from(raw)
    };

    if !expanded.is_absolute() {
        return NormalizedPath::TraversalRejected("PATH_MUST_BE_ABSOLUTE");
    }

    // Strict lexical check: reject ANY `..` parent component immediately!
    let mut lexical = PathBuf::new();
    for comp in expanded.components() {
        match comp {
            Component::RootDir => lexical.push("/"),
            Component::Normal(seg) => lexical.push(seg),
            Component::CurDir => {}
            Component::ParentDir | Component::Prefix(_) => {
                return NormalizedPath::TraversalRejected("PATH_TRAVERSAL_PARENT_DIR");
            }
        }
    }

    // Resolve symlinks on target or nearest existing ancestor to defeat symlink escapes.
    let canonical = resolve_existing_ancestor_canonical(&lexical);
    match canonical {
        Ok(canon) => NormalizedPath::Clean { lexical, canonical: canon },
        Err(reason) => NormalizedPath::TraversalRejected(reason),
    }
}

fn resolve_existing_ancestor_canonical(path: &Path) -> Result<PathBuf, &'static str> {
    if let Ok(meta) = fs::symlink_metadata(path) {
        if meta.file_type().is_symlink() {
            // Symlink MUST resolve cleanly; dangling or unreadable symlinks fail closed!
            return fs::canonicalize(path).map_err(|_| "PATH_UNRESOLVABLE_SYMLINK");
        }
        return fs::canonicalize(path).or_else(|_| Ok(path.to_path_buf()));
    }

    // Walk up to find the nearest existing ancestor; resolve symlinks there and re-join tail.
    let mut tail: Vec<std::ffi::OsString> = Vec::new();
    let mut cursor = path;
    while let Some(parent) = cursor.parent() {
        if let Some(name) = cursor.file_name() {
            tail.push(name.to_os_string());
        }
        if parent.exists() {
            let mut resolved = fs::canonicalize(parent).unwrap_or_else(|_| parent.to_path_buf());
            for seg in tail.into_iter().rev() {
                resolved.push(seg);
            }
            return Ok(resolved);
        }
        cursor = parent;
    }

    Ok(path.to_path_buf())
}

/// 08.05 — Hardcoded Engine-Level Self-Modification Lock.
/// Evaluated BEFORE all allowlists and cannot be disabled at runtime.
fn check_self_modification_lock(
    operation: &str,
    raw_target: &str,
    paths: &[&Path],
    dbus_interface: &str,
    untrusted_self_mod: bool,
) -> Option<(&'static str, &'static str)> {
    // 1. Explicit self-modification operations are always unconditionally denied.
    if matches!(
        operation,
        "modify_policy"
            | "disable_policy"
            | "disable_self_mod_lock"
            | "reset_own_lockout"
            | "modify_polkit"
            | "modify_dbus_policy"
            | "modify_systemd_unit"
            | "modify_recovery_locks"
            | "replace_security_binary"
    ) {
        return Some((
            "SELF_MOD_LOCK_OPERATION",
            "Self-modification lock forbids altering ZCI policy, Polkit, D-Bus, systemd, recovery locks, or security binaries.",
        ));
    }

    // 2. Attempting to invoke admin lockout reset or policy mutation via EvaluateAction.
    if dbus_interface.eq_ignore_ascii_case("org.zethropol.Security.Policy.v1")
        && raw_target.eq_ignore_ascii_case("ResetAgentLockout")
    {
        return Some((
            "SELF_MOD_LOCK_ADMIN_RESET",
            "Self-modification lock forbids ZCI agents from invoking ResetAgentLockout.",
        ));
    }

    // 3. Direct access (even read!) to live persistent security state/audit storage is blocked at path level.
    let check_path_str = |s: &str| -> Option<(&'static str, &'static str)> {
        let lower = s.to_ascii_lowercase();
        if lower.starts_with("/var/lib/zethropol-security")
            || lower.starts_with("/var/log/zethropol-security")
            || lower.contains("zci-policy-state.json")
            || lower.contains("zci-audit.jsonl")
        {
            return Some((
                "SELF_MOD_LOCK_SECURITY_STATE",
                "Self-modification lock protects persistent policy state and audit logs from direct filesystem access.",
            ));
        }

        // For any mutating/modifying operation, block all protected policies, locks, binaries, and AGENTS.md.
        if is_mutating_operation(operation) || operation == "dbus_call" {
            if lower.ends_with("/agents.md") || lower == "agents.md" {
                return Some((
                    "SELF_MOD_LOCK_AGENTS_MD",
                    "Self-modification lock forbids modifying AGENTS.md instructions.",
                ));
            }
            if lower.contains("services/recovery/src/recovery_service.rs")
                || lower.contains("rollback_execution_enabled")
                || lower.contains("repair_execution_enabled")
                || lower.contains("services/recovery/deployment/")
            {
                return Some((
                    "SELF_MOD_LOCK_RECOVERY_LOCKS",
                    "Self-modification lock protects recovery_service.rs safety locks (rollback/repair=false).",
                ));
            }
            if lower.starts_with("/etc/polkit-1")
                || lower.starts_with("/usr/share/polkit-1")
                || lower.contains("/deployment/polkit/")
                || lower.ends_with(".policy")
            {
                return Some((
                    "SELF_MOD_LOCK_POLKIT",
                    "Self-modification lock forbids modifying PolicyKit policies or rules.",
                ));
            }
            if lower.starts_with("/etc/dbus-1")
                || lower.starts_with("/usr/share/dbus-1")
                || lower.contains("/deployment/dbus/")
            {
                return Some((
                    "SELF_MOD_LOCK_DBUS_POLICY",
                    "Self-modification lock forbids modifying D-Bus policy or activation configs.",
                ));
            }
            if lower.starts_with("/etc/systemd")
                || lower.starts_with("/usr/lib/systemd")
                || lower.starts_with("/lib/systemd")
                || lower.contains("/deployment/systemd/")
            {
                return Some((
                    "SELF_MOD_LOCK_SYSTEMD",
                    "Self-modification lock forbids modifying systemd service unit files.",
                ));
            }
            if lower.starts_with("/etc/zethropol")
                || lower.starts_with("/usr/lib/zethropol")
                || lower.starts_with("/usr/bin/zethropol-")
                || lower.contains("/services/security/")
                || lower.ends_with("/services/security")
                || lower.contains("/packages/zethropol-security/")
                || lower.contains("/.local/share/zci/policy")
                || lower.contains("/.config/zethropol/policy")
            {
                return Some((
                    "SELF_MOD_LOCK_ENGINE_FILES",
                    "Self-modification lock forbids altering Zethropol security/ZCI policy binaries or sources.",
                ));
            }
            if lower.contains("/.git/hooks")
                || lower.ends_with("/.git/config")
                || lower.ends_with("/.git/info/exclude")
            {
                return Some((
                    "SELF_MOD_LOCK_GIT_CONTROL",
                    "Self-modification lock forbids altering Git hooks, config, or local exclude files.",
                ));
            }
        }
        None
    };

    if let Some(hit) = check_path_str(raw_target) {
        return Some(hit);
    }
    for p in paths {
        if let Some(s) = p.to_str() {
            if let Some(hit) = check_path_str(s) {
                return Some(hit);
            }
        }
    }

    if untrusted_self_mod && is_mutating_operation(operation) {
        return Some((
            "SELF_MOD_LOCK_PAYLOAD_DIRECTIVE",
            "Self-modification lock blocked mutating action containing protected policy/lock identifiers.",
        ));
    }

    None
}

/// Sensitive OS and user-credential paths denied across all ZCI agents (both read & write).
fn is_sensitive_path_denied(path: &Path) -> bool {
    let Some(s) = path.to_str() else {
        return true;
    };
    let lower = s.to_ascii_lowercase();
    const SENSITIVE_PREFIXES: &[&str] = &[
        "/etc/shadow",
        "/etc/gshadow",
        "/etc/sudoers",
        "/etc/pacman.conf",
        "/etc/polkit-1",
        "/boot",
        "/root",
        "/proc/1/",
        "/sys/firmware",
        "/dev/",
    ];
    if SENSITIVE_PREFIXES.iter().any(|p| lower == *p || lower.starts_with(p)) {
        return true;
    }

    const SENSITIVE_FRAGMENTS: &[&str] = &[
        "/.ssh/",
        "/.gnupg/",
        "/.pki/",
        "/.aws/",
        "/.kube/",
        "/.local/share/keyrings/",
        "/.config/autostart/",
        "/.config/fish/",
        "/.netrc",
        "/.git-credentials",
        "/.bashrc",
        "/.zshrc",
        "/.profile",
        "/.bash_profile",
    ];
    if SENSITIVE_FRAGMENTS.iter().any(|f| lower.contains(f))
        || lower.ends_with("/.ssh")
        || lower.ends_with("/.gnupg")
        || lower.ends_with("/.env")
        || lower.ends_with(".pem")
        || lower.ends_with(".key")
    {
        return true;
    }
    false
}

/// 08.01 — Deterministic classification of requested action into the 5 policy tiers.
fn classify_action(
    operation: &str,
    target_paths: &[&Path],
    raw_target: &str,
    dbus_interface: &str,
) -> (ActionClass, bool, &'static str) {
    // 1. Privileged operations (explicit administrative / Polkit boundary).
    if matches!(
        operation,
        "toggle_firewall"
            | "install_package"
            | "remove_package"
            | "remove_orphans"
            | "rollback"
            | "repair"
            | "create_recovery_point"
            | "delete_recovery_point"
            | "start_service"
            | "stop_service"
            | "restart_service"
            | "enable_service"
            | "disable_service"
            | "exec_command"
            | "shell_exec"
            | "mount_fs"
    ) {
        return (ActionClass::Privileged, true, "org.zethropol.security.modify");
    }

    if operation == "dbus_call" {
        let iface = dbus_interface.trim();
        let method = raw_target.trim();
        if matches!(
            method,
            "ToggleFirewall"
                | "ResetAgentLockout"
                | "StartService"
                | "StopService"
                | "RestartService"
                | "EnableService"
                | "DisableService"
                | "CreateRecoveryPoint"
                | "DeleteRecoveryPoint"
                | "RemoveApplication"
        ) {
            let polkit_action = if iface == "org.zethropol.Security.v1" {
                "org.zethropol.security.modify"
            } else if iface == "org.zethropol.Security.Policy.v1" {
                "org.zethropol.security.policy.admin"
            } else {
                "org.zethropol.security.modify"
            };
            return (ActionClass::Privileged, true, polkit_action);
        }
        if method == "SetProfile" || method.starts_with("Set") || method.starts_with("Apply") {
            return (
                ActionClass::SystemStateChange,
                true,
                "org.zethropol.performance.modify",
            );
        }
        if matches!(
            method,
            "GetState"
                | "GetTelemetry"
                | "GetProfileState"
                | "GetGamingReadiness"
                | "ListServices"
                | "GetPolicySummary"
                | "GetAgentStatus"
                | "WrapUntrustedData"
                | "CheckCapabilities"
        ) {
            return (ActionClass::ReadOnly, false, "");
        }
        // Unknown D-Bus method defaults conservatively to SystemStateChange (default-deny).
        return (ActionClass::SystemStateChange, true, "");
    }

    // 2. System state changes (modifying system paths or performance profiles).
    if operation == "set_profile" {
        return (
            ActionClass::SystemStateChange,
            true,
            "org.zethropol.performance.modify",
        );
    }

    if is_mutating_operation(operation) {
        for p in target_paths {
            if let Some(s) = p.to_str() {
                if s.starts_with("/etc")
                    || s.starts_with("/usr")
                    || s.starts_with("/var")
                    || s.starts_with("/sys")
                    || s.starts_with("/boot")
                    || s.starts_with("/opt")
                    || s.starts_with("/lib")
                {
                    return (ActionClass::SystemStateChange, true, "");
                }
            }
        }
        return (ActionClass::UserSpaceSafe, false, "");
    }

    // 3. Read-only and bounded user-space checks.
    if matches!(
        operation,
        "read_context"
            | "read_file"
            | "list_dir"
            | "read_image"
            | "read_status"
            | "inspect_untrusted"
            | "health_check"
    ) {
        return (ActionClass::ReadOnly, false, "");
    }

    if matches!(operation, "run_tests" | "run_workflow_template") {
        return (ActionClass::UserSpaceSafe, false, "");
    }

    // Default conservative classification for unrecognized operations.
    (ActionClass::SystemStateChange, true, "")
}

fn path_within_any_root(candidate: &Path, roots: &[PathBuf]) -> bool {
    roots.iter().any(|root| {
        let resolved_root = fs::canonicalize(root).unwrap_or_else(|_| root.clone());
        candidate.starts_with(root) || candidate.starts_with(&resolved_root)
    })
}

pub struct PolicyEngine {
    config: PolicyConfig,
    state: PolicyStateStore,
    audit: AuditLogger,
}

impl PolicyEngine {
    pub fn with_config(config: PolicyConfig) -> Self {
        let state = PolicyStateStore::load(config.state_file.clone());
        let audit = AuditLogger::open(config.audit_log_file.clone(), config.emit_audit_stderr);
        Self {
            config,
            state,
            audit,
        }
    }

    pub fn config(&self) -> &PolicyConfig {
        &self.config
    }

    pub fn state_corrupted(&self) -> bool {
        self.state.is_corrupted()
    }

    pub fn audit_total_events(&self) -> u64 {
        self.audit.total_events()
    }

    pub fn locked_agent_count(&self) -> usize {
        self.state.locked_agent_count(now_unix_secs())
    }

    pub fn get_agent_state(&self, agent_id: &str) -> AgentPenaltyState {
        self.state.get_agent(agent_id)
    }

    pub fn get_audit_events(&self, limit: u32) -> AuditQueryResult {
        self.audit.query_recent(limit)
    }

    fn effective_home(&self, caller_uid: u32) -> Option<PathBuf> {
        if let Some(ref override_home) = self.config.home_override {
            return Some(override_home.clone());
        }
        resolve_home_for_uid(caller_uid)
    }

    fn allowed_roots_for_agent(
        &self,
        profile: &AgentAllowlist,
        caller_uid: u32,
        write: bool,
    ) -> Vec<PathBuf> {
        let mut roots = Vec::new();
        if let Some(home) = self.effective_home(caller_uid) {
            let subpaths = if write {
                profile.write_subpaths_under_home
            } else {
                profile.read_subpaths_under_home
            };
            for sub in subpaths {
                roots.push(home.join(sub));
            }
        }
        let tmp_prefixes = if write {
            profile.write_tmp_prefixes
        } else {
            profile.read_tmp_prefixes
        };
        for prefix in tmp_prefixes {
            roots.push(PathBuf::from(prefix));
        }
        if (write && profile.allow_workspace_write) || (!write && profile.allow_workspace_read) {
            for extra in &self.config.extra_workspace_roots {
                roots.push(extra.clone());
            }
        }
        roots
    }

    pub fn wrap_untrusted_data(
        &mut self,
        agent_id: &str,
        source_kind: &str,
        raw_content: &str,
        caller_bus_name: &str,
        caller_uid: u32,
    ) -> UntrustedInspection {
        let inspection = inspect_and_wrap_untrusted(source_kind, raw_content);
        let agent_key = if is_valid_agent_id_syntax(agent_id) {
            agent_id
        } else {
            "unknown-agent"
        };

        let (decision, rule_id, reason) = if inspection.injection_detected {
            (
                "flagged",
                if inspection.self_modification_injection {
                    "UNTRUSTED_SELF_MOD_TAGGED"
                } else {
                    "UNTRUSTED_INJECTION_TAGGED"
                },
                "Untrusted payload isolated as non-executable data; suspicious injection patterns detected.",
            )
        } else {
            (
                "wrapped",
                "UNTRUSTED_DATA_ISOLATED",
                "Untrusted payload isolated as strictly non-executable data.",
            )
        };

        let cur = self.state.get_agent(agent_key);
        let _ = self.audit.append(AuditEventInput {
            agent_id: agent_key.to_string(),
            caller_bus_name: caller_bus_name.to_string(),
            caller_uid,
            operation: "wrap_untrusted_data".to_string(),
            target: format!("source={} sha256={}", inspection.source_kind, inspection.content_sha256),
            dbus_interface: "org.zethropol.Security.Policy.v1".to_string(),
            action_class: ActionClass::ReadOnly.as_str().to_string(),
            decision: decision.to_string(),
            rule_id: rule_id.to_string(),
            reason: reason.to_string(),
            penalty_delta: 0,
            penalty_score_after: cur.strike_score,
            penalty_level_after: cur.penalty_level,
            cooldown_until_unix: cur.cooldown_until_unix,
            hard_locked: cur.hard_locked,
            untrusted_flags: inspection.detected_patterns.clone(),
        });

        inspection
    }

    pub fn evaluate_action(&mut self, req: &ActionRequest) -> PolicyDecision {
        self.evaluate_action_at(req, now_unix_secs())
    }

    pub fn evaluate_action_at(&mut self, req: &ActionRequest, now_unix: u64) -> PolicyDecision {
        let agent_key = if is_valid_agent_id_syntax(&req.agent_id) {
            req.agent_id.clone()
        } else {
            "malformed-agent".to_string()
        };

        // 0. Fail-closed if persistent policy state file was corrupted or tampered with.
        if self.state.is_corrupted() {
            let detail = self
                .state
                .corruption_detail()
                .unwrap_or("Persistent policy state corrupted.")
                .to_string();
            return self.finalize_decision(
                req,
                &agent_key,
                false,
                "locked_out",
                ActionClass::SelfModificationDenied,
                true,
                "org.zethropol.security.policy.admin",
                "STATE_CORRUPTED_FAIL_CLOSED",
                "policy-state-corrupted",
                &detail,
                0,
                Vec::new(),
                now_unix,
                true,
            );
        }

        // 1. Validate basic request syntax (reject control/NUL injection in fields).
        if has_control_or_nul(&req.agent_id)
            || has_control_or_nul(&req.operation)
            || has_control_or_nul(&req.target)
            || has_control_or_nul(&req.dbus_interface)
            || req.operation.trim().is_empty()
            || req.operation.len() > 64
        {
            return self.finalize_decision(
                req,
                &agent_key,
                false,
                "deny",
                ActionClass::SystemStateChange,
                false,
                "",
                "INVALID_REQUEST_SYNTAX",
                "invalid-request",
                "Malformed control character, NUL byte, or invalid operation syntax rejected.",
                2,
                Vec::new(),
                now_unix,
                false,
            );
        }

        // 2. Inspect untrusted payload (08.04) first to capture injection flags.
        let untrusted_inspection = if !req.untrusted_payload.is_empty() {
            Some(inspect_and_wrap_untrusted("external_message", &req.untrusted_payload))
        } else {
            None
        };
        let untrusted_flags = untrusted_inspection
            .as_ref()
            .map(|u| u.detected_patterns.clone())
            .unwrap_or_default();
        let untrusted_self_mod = untrusted_inspection
            .as_ref()
            .map(|u| u.self_modification_injection)
            .unwrap_or(false);

        // Normalize target path early when applicable so self-mod lock inspects canonicalized paths too.
        let home_dir = self.effective_home(req.caller_uid);
        let normalized_path = if is_file_operation(&req.operation) {
            Some(normalize_and_resolve_path(&req.target, home_dir.as_deref()))
        } else {
            None
        };

        let mut candidate_paths: Vec<&Path> = Vec::new();
        if let Some(NormalizedPath::Clean { lexical, canonical }) = &normalized_path {
            candidate_paths.push(lexical.as_path());
            candidate_paths.push(canonical.as_path());
        }

        // 3. 08.05 Hardcoded Engine-Level Self-Modification Lock (evaluated BEFORE allowlists & lockout!).
        if let Some((rule_id, reason)) = check_self_modification_lock(
            &req.operation,
            &req.target,
            &candidate_paths,
            &req.dbus_interface,
            untrusted_self_mod,
        ) {
            return self.finalize_decision(
                req,
                &agent_key,
                false,
                "deny",
                ActionClass::SelfModificationDenied,
                false,
                "",
                rule_id,
                "self-modification-denied",
                reason,
                3,
                untrusted_flags,
                now_unix,
                true,
            );
        }

        // 4. 08.06 Check active cooldown / persistent lockout BEFORE evaluating allowlists.
        let lockout = self.state.check_lockout(&agent_key, now_unix);
        if lockout.locked {
            let reason = if lockout.hard_locked {
                "Agent is under persistent hard lockout; Polkit admin reset is required."
            } else {
                "Agent is currently under abuse penalty cooldown; request denied."
            };
            // Continued attempts while locked out add +1 strike and extend cooldown.
            return self.finalize_decision(
                req,
                &agent_key,
                false,
                "locked_out",
                ActionClass::ReadOnly,
                lockout.hard_locked,
                if lockout.hard_locked {
                    "org.zethropol.security.policy.admin"
                } else {
                    ""
                },
                if lockout.hard_locked {
                    "ABUSE_HARD_LOCKOUT_ACTIVE"
                } else {
                    "ABUSE_COOLDOWN_ACTIVE"
                },
                "agent-locked-out",
                reason,
                1,
                untrusted_flags,
                now_unix,
                false,
            );
        }

        // 5. 08.04 Block actions carrying detected prompt/policy/command injection in untrusted payload.
        if let Some(ref insp) = untrusted_inspection {
            if insp.injection_detected {
                let (cls, delta, rule_id) = if insp.self_modification_injection {
                    (
                        ActionClass::SelfModificationDenied,
                        3,
                        "UNTRUSTED_SELF_MOD_INJECTION_BLOCKED",
                    )
                } else {
                    (
                        ActionClass::SystemStateChange,
                        2,
                        "UNTRUSTED_INJECTION_BLOCKED",
                    )
                };
                return self.finalize_decision(
                    req,
                    &agent_key,
                    false,
                    "deny",
                    cls,
                    false,
                    "",
                    rule_id,
                    "untrusted-injection-blocked",
                    "Untrusted data payload triggered injection/authority-escalation guard and cannot drive an action.",
                    delta,
                    untrusted_flags,
                    now_unix,
                    insp.self_modification_injection,
                );
            }
        }

        // 6. 08.01 Deterministic action classification.
        let (action_class, requires_polkit, polkit_action) = classify_action(
            &req.operation,
            &candidate_paths,
            &req.target,
            &req.dbus_interface,
        );

        // 7. 08.02 Default-deny unknown agents.
        let Some(profile) = find_agent_profile(&req.agent_id) else {
            return self.finalize_decision(
                req,
                &agent_key,
                false,
                "deny",
                action_class,
                requires_polkit,
                polkit_action,
                "DEFAULT_DENY_UNKNOWN_AGENT",
                "unknown-agent",
                "Unknown ZCI agent identity denied by default-deny policy.",
                2,
                untrusted_flags,
                now_unix,
                false,
            );
        };

        // 8. Privileged and SystemStateChange actions are unconditionally denied for ZCI agents.
        if action_class == ActionClass::Privileged {
            return self.finalize_decision(
                req,
                &agent_key,
                false,
                "deny",
                ActionClass::Privileged,
                true,
                polkit_action,
                "DENY_PRIVILEGED_FOR_ZCI",
                "privileged-action-denied",
                "ZCI agents have no direct privilege grant; privileged operations require separate Polkit-authorized service flow.",
                2,
                untrusted_flags,
                now_unix,
                false,
            );
        }

        if action_class == ActionClass::SystemStateChange {
            return self.finalize_decision(
                req,
                &agent_key,
                false,
                "deny",
                ActionClass::SystemStateChange,
                requires_polkit,
                polkit_action,
                "DENY_SYSTEM_STATE_CHANGE_FOR_ZCI",
                "system-state-change-denied",
                "ZCI agents cannot mutate system-wide state under default-deny policy.",
                2,
                untrusted_flags,
                now_unix,
                false,
            );
        }

        // 9. 08.03 Per-agent operation allowlist check.
        if !profile.allowed_operations.contains(&req.operation.as_str()) {
            return self.finalize_decision(
                req,
                &agent_key,
                false,
                "deny",
                action_class,
                false,
                "",
                "DENY_OPERATION_NOT_ALLOWLISTED",
                "operation-not-allowed",
                "Requested operation is not in this ZCI agent's explicit allowlist.",
                1,
                untrusted_flags,
                now_unix,
                false,
            );
        }

        // 10. 08.03 Per-agent D-Bus interface & method allowlist check.
        if req.operation == "dbus_call" {
            let iface = req.dbus_interface.trim();
            let method = req.target.trim();
            let allowed = profile
                .allowed_dbus_calls
                .iter()
                .any(|(i, m)| *i == iface && *m == method);
            if !allowed {
                return self.finalize_decision(
                    req,
                    &agent_key,
                    false,
                    "deny",
                    action_class,
                    false,
                    "",
                    "DENY_DBUS_NOT_ALLOWLISTED",
                    "dbus-not-allowed",
                    "Requested D-Bus interface/method is not in this ZCI agent's read-only allowlist.",
                    1,
                    untrusted_flags,
                    now_unix,
                    false,
                );
            }
            return self.finalize_decision(
                req,
                &agent_key,
                true,
                "allow",
                ActionClass::ReadOnly,
                false,
                "",
                "ALLOW_DBUS_READONLY",
                "ok",
                "Allowed read-only D-Bus call matching agent allowlist.",
                0,
                untrusted_flags,
                now_unix,
                false,
            );
        }

        // 11. 08.02 Per-agent filesystem path allowlist + traversal + sensitive-path checks.
        if let Some(norm) = normalized_path {
            let (lexical, canonical) = match norm {
                NormalizedPath::Clean { lexical, canonical } => (lexical, canonical),
                NormalizedPath::TraversalRejected(rule_id) => {
                    return self.finalize_decision(
                        req,
                        &agent_key,
                        false,
                        "deny",
                        ActionClass::SystemStateChange,
                        false,
                        "",
                        rule_id,
                        "path-traversal-blocked",
                        "Path traversal, encoded separator, relative path, or unresolvable symlink blocked.",
                        2,
                        untrusted_flags,
                        now_unix,
                        false,
                    );
                }
            };

            if is_sensitive_path_denied(&lexical) || is_sensitive_path_denied(&canonical) {
                return self.finalize_decision(
                    req,
                    &agent_key,
                    false,
                    "deny",
                    ActionClass::Privileged,
                    false,
                    "",
                    "DENY_SENSITIVE_PATH",
                    "sensitive-path-denied",
                    "Access to credentials, shell startup files, or sensitive system paths is denied.",
                    2,
                    untrusted_flags,
                    now_unix,
                    false,
                );
            }

            let write_op = is_mutating_operation(&req.operation);
            let allowed_roots = self.allowed_roots_for_agent(profile, req.caller_uid, write_op);
            if allowed_roots.is_empty()
                || !path_within_any_root(&lexical, &allowed_roots)
                || !path_within_any_root(&canonical, &allowed_roots)
            {
                return self.finalize_decision(
                    req,
                    &agent_key,
                    false,
                    "deny",
                    action_class,
                    false,
                    "",
                    if write_op {
                        "DENY_WRITE_PATH_OUTSIDE_ALLOWLIST"
                    } else {
                        "DENY_READ_PATH_OUTSIDE_ALLOWLIST"
                    },
                    "path-not-allowed",
                    "Canonical target path is outside this ZCI agent's allowed filesystem boundaries.",
                    1,
                    untrusted_flags,
                    now_unix,
                    false,
                );
            }

            return self.finalize_decision(
                req,
                &agent_key,
                true,
                "allow",
                action_class,
                false,
                "",
                if write_op {
                    "ALLOW_USERSPACE_WRITE"
                } else {
                    "ALLOW_USERSPACE_READ"
                },
                "ok",
                "Allowed bounded user-space file action within agent allowlist.",
                0,
                untrusted_flags,
                now_unix,
                false,
            );
        }

        // 12. Remaining explicitly allowlisted non-file read-only operations (`health_check`, `inspect_untrusted`).
        self.finalize_decision(
            req,
            &agent_key,
            true,
            "allow",
            action_class,
            false,
            "",
            "ALLOW_EXPLICIT_SAFE_OPERATION",
            "ok",
            "Allowed explicitly allowlisted non-privileged operation.",
            0,
            untrusted_flags,
            now_unix,
            false,
        )
    }

    /// Administrative reset after Polkit `org.zethropol.security.policy.admin` authorization.
    pub fn admin_reset_lockout(
        &mut self,
        agent_id: &str,
        reason: &str,
        caller_bus_name: &str,
        caller_uid: u32,
    ) -> Result<AuditEvent, String> {
        let trimmed = agent_id.trim();
        if trimmed != "*" && !is_valid_agent_id_syntax(trimmed) {
            return Err("Invalid agent_id for administrative lockout reset.".to_string());
        }
        self.state.admin_reset_agent(trimmed)?;
        let audit_ev = self.audit.append(AuditEventInput {
            agent_id: trimmed.to_string(),
            caller_bus_name: caller_bus_name.to_string(),
            caller_uid,
            operation: "admin_reset_lockout".to_string(),
            target: trimmed.to_string(),
            dbus_interface: "org.zethropol.Security.Policy.v1".to_string(),
            action_class: ActionClass::Privileged.as_str().to_string(),
            decision: "admin_reset".to_string(),
            rule_id: "ADMIN_POLKIT_LOCKOUT_RESET".to_string(),
            reason: if reason.trim().is_empty() {
                "Administrator reset agent penalty state via Polkit auth_admin.".to_string()
            } else {
                reason.trim().to_string()
            },
            penalty_delta: 0,
            penalty_score_after: 0,
            penalty_level_after: 0,
            cooldown_until_unix: 0,
            hard_locked: false,
            untrusted_flags: Vec::new(),
        })?;
        Ok(audit_ev)
    }

    pub fn describe_agent_allowlist(&self, agent_id: &str) -> Option<(Vec<String>, Vec<String>)> {
        let profile = find_agent_profile(agent_id)?;
        let ops = profile
            .allowed_operations
            .iter()
            .map(|s| (*s).to_string())
            .collect();
        let dbus_set: BTreeSet<String> = profile
            .allowed_dbus_calls
            .iter()
            .map(|(i, m)| format!("{i}.{m}"))
            .collect();
        Some((ops, dbus_set.into_iter().collect()))
    }

    #[allow(clippy::too_many_arguments)]
    fn finalize_decision(
        &mut self,
        req: &ActionRequest,
        agent_key: &str,
        allowed: bool,
        decision: &str,
        action_class: ActionClass,
        requires_polkit: bool,
        polkit_action: &str,
        rule_id: &str,
        code: &str,
        reason: &str,
        penalty_delta: u32,
        untrusted_flags: Vec<String>,
        now_unix: u64,
        is_self_mod: bool,
    ) -> PolicyDecision {
        let mut state_after = self.state.get_agent(agent_key);
        let mut actual_delta = 0u32;

        if !allowed && penalty_delta > 0 && !self.state.is_corrupted() {
            match self
                .state
                .record_violation(agent_key, penalty_delta, rule_id, is_self_mod, now_unix)
            {
                Ok(updated) => {
                    state_after = updated;
                    actual_delta = penalty_delta;
                }
                Err(_) => {
                    // State persistence failure fails closed without granting access.
                }
            }
        }

        let lockout = self.state.check_lockout(agent_key, now_unix);
        let audit_event_id = self
            .audit
            .append(AuditEventInput {
                agent_id: agent_key.to_string(),
                caller_bus_name: req.caller_bus_name.clone(),
                caller_uid: req.caller_uid,
                operation: req.operation.clone(),
                target: req.target.clone(),
                dbus_interface: req.dbus_interface.clone(),
                action_class: action_class.as_str().to_string(),
                decision: decision.to_string(),
                rule_id: rule_id.to_string(),
                reason: reason.to_string(),
                penalty_delta: actual_delta,
                penalty_score_after: state_after.strike_score,
                penalty_level_after: state_after.penalty_level,
                cooldown_until_unix: state_after.cooldown_until_unix,
                hard_locked: state_after.hard_locked || self.state.is_corrupted(),
                untrusted_flags: untrusted_flags.clone(),
            })
            .map(|ev| ev.event_id)
            .unwrap_or_default();

        PolicyDecision {
            allowed,
            decision: decision.to_string(),
            action_class,
            requires_polkit,
            polkit_action: polkit_action.to_string(),
            rule_id: rule_id.to_string(),
            code: code.to_string(),
            reason: reason.to_string(),
            penalty_delta: actual_delta,
            penalty_score: state_after.strike_score,
            penalty_level: lockout.penalty_level,
            lockout_active: lockout.locked,
            hard_locked: lockout.hard_locked,
            cooldown_remaining_secs: lockout.remaining_secs,
            untrusted_flags,
            audit_event_id,
        }
    }
}
