use crate::config::{MAX_STEPS_PER_PLAN, MAX_WRITE_FILE_BYTES, PLANNER_SYSTEM_PROMPT};
use serde::{Deserialize, Serialize};
use std::path::{Component, Path, PathBuf};

/// Risk classification presented to user & enforced at execution boundary (13.02 & 13.05).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum RiskLevel {
    ReadOnly,
    UserSpaceSafe,
    ApprovalRequired,
}

impl RiskLevel {
    pub fn as_str(&self) -> &'static str {
        match self {
            RiskLevel::ReadOnly => "ReadOnly",
            RiskLevel::UserSpaceSafe => "UserSpaceSafe",
            RiskLevel::ApprovalRequired => "ApprovalRequired",
        }
    }
}

/// Strictly allowlisted structured actions (ZERO raw shell strings anywhere).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PlanStep {
    pub step_id: String,
    pub index: u32,
    pub action_kind: String,
    pub operation: String,
    pub agent_id: String,
    pub target: String,
    pub dbus_interface: String,
    pub method: String,
    pub content: String,
    pub description: String,
    pub risk_level: String,
    pub requires_approval: bool,
    pub approval_status: String,
    pub approval_note: String,
    pub execution_status: String,
    pub policy_rule_id: String,
    pub policy_action_class: String,
    pub output_preview: String,
    pub executable: bool,
    pub policy_authority: String,
    pub audit_hash: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SnapperPreflightReport {
    pub checked: bool,
    pub recovery_service_reachable: bool,
    pub recovery_status: String,
    pub snapshot_count: u64,
    pub rollback_execution_enabled: bool,
    pub repair_execution_enabled: bool,
    pub summary: String,
}

impl Default for SnapperPreflightReport {
    fn default() -> Self {
        Self {
            checked: false,
            recovery_service_reachable: false,
            recovery_status: "not-checked".to_string(),
            snapshot_count: 0,
            rollback_execution_enabled: false,
            repair_execution_enabled: false,
            summary: String::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AgentPlan {
    pub plan_id: String,
    pub goal: String,
    pub summary: String,
    pub agent_profile: String,
    pub workspace_dir: String,
    pub status: String,
    pub created_at_unix: u64,
    pub updated_at_unix: u64,
    pub planner_source: String,
    pub input_envelope_id: String,
    pub output_envelope_id: String,
    pub executable: bool,
    pub policy_authority: String,
    pub snapper_preflight: SnapperPreflightReport,
    pub steps: Vec<PlanStep>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SafetyVerdict {
    Allow,
    DenyShellInjection { pattern: String, reason: String },
    DenyProtectedPath { pattern: String, reason: String },
    DenyPolicyTamper { pattern: String, reason: String },
}

pub const ALLOWED_DIAGNOSTIC_PAIRS: &[(&str, &str)] = &[
    ("org.zethropol.Hardware.v1", "GetTelemetry"),
    ("org.zethropol.Hardware.v1", "GetState"),
    ("org.zethropol.Monitoring.v1", "GetState"),
    ("org.zethropol.Diagnostics.v1", "GetState"),
    ("org.zethropol.Performance.v1", "GetState"),
    ("org.zethropol.Performance.v1", "GetProfileState"),
    ("org.zethropol.Performance.v1", "GetGamingReadiness"),
    ("org.zethropol.Power.v1", "GetState"),
    ("org.zethropol.Services.v1", "ListServices"),
    ("org.zethropol.Services.v1", "GetState"),
    ("org.zethropol.Security.v1", "GetState"),
    ("org.zethropol.Security.Policy.v1", "GetPolicySummary"),
];

pub fn is_allowed_diagnostic_pair(iface: &str, method: &str) -> bool {
    ALLOWED_DIAGNOSTIC_PAIRS
        .iter()
        .any(|&(i, m)| i == iface.trim() && m == method.trim())
}

/// 13.01 & 13.02 — Fail-closed safety gate for natural-language goals and step targets.
pub fn inspect_text_safety(text: &str) -> SafetyVerdict {
    let lower = text.to_ascii_lowercase();

    // 1. Recovery lock / policy tamper / prompt injection
    const POLICY_TAMPER_NEEDLES: &[(&str, &str)] = &[
        ("rollback_execution_enabled", "recovery_lock_tamper"),
        ("repair_execution_enabled", "recovery_lock_tamper"),
        ("self_modification_lock", "self_mod_lock_tamper"),
        ("ignore previous instructions", "prompt_override_en"),
        ("disregard all previous", "prompt_override_en"),
        ("önceki talimatları yoksay", "prompt_override_tr"),
        ("bypass polkit", "polkit_bypass"),
        ("disable polkit", "polkit_bypass"),
        ("disable policy", "policy_bypass"),
        ("zci-policy-state.json", "policy_state_tamper"),
        ("zci-audit.jsonl", "audit_log_tamper"),
        ("agents.md", "agent_policy_file"),
        ("recovery_service.rs", "recovery_lock_source"),
    ];
    for &(needle, label) in POLICY_TAMPER_NEEDLES {
        if lower.contains(needle) {
            return SafetyVerdict::DenyPolicyTamper {
                pattern: label.to_string(),
                reason: "Güvenlik engeli: Politika veya kurtarma kilidi değişikliği / enjeksiyon kalıbı fail-closed reddedildi.".to_string(),
            };
        }
    }

    // 2. Raw shell command execution / metacharacter injection (ZERO sh -c / eval!)
    const SHELL_NEEDLES: &[(&str, &str)] = &[
        ("sh -c", "raw_shell_sh_c"),
        ("bash -c", "raw_shell_bash_c"),
        ("zsh -c", "raw_shell_zsh_c"),
        ("fish -c", "raw_shell_fish_c"),
        ("eval ", "raw_shell_eval"),
        ("exec ", "raw_shell_exec"),
        ("$(", "command_substitution"),
        ("`", "backtick_substitution"),
        ("| sh", "pipe_to_shell"),
        ("| bash", "pipe_to_shell"),
        ("; rm ", "chained_rm_command"),
        ("&& rm ", "chained_rm_command"),
        ("rm -rf", "destructive_rm"),
        ("sudo ", "privileged_sudo"),
        ("pkexec ", "privileged_pkexec"),
        ("mkfs.", "destructive_mkfs"),
        ("dd if=", "destructive_dd"),
        ("chmod 777", "unsafe_chmod"),
        ("chown root", "unsafe_chown"),
        ("pacman -s", "raw_pacman_mutation"),
        ("pacman -r", "raw_pacman_mutation"),
        ("pacman -u", "raw_pacman_mutation"),
        ("systemctl stop", "raw_systemd_mutation"),
        ("systemctl disable", "raw_systemd_mutation"),
        ("curl ", "external_network_curl"),
        ("wget ", "external_network_wget"),
    ];
    for &(needle, label) in SHELL_NEEDLES {
        if lower.contains(needle) {
            return SafetyVerdict::DenyShellInjection {
                pattern: label.to_string(),
                reason: "Güvenlik engeli: Ham kabuk komutu, enjeksiyon veya ayrıcalık yükseltme kalıbı yasaktır; yalnızca yapılandırılmış allowlist eylemleri kullanılabilir.".to_string(),
            };
        }
    }

    // 3. Protected system paths (13.02)
    const PROTECTED_PATH_NEEDLES: &[(&str, &str)] = &[
        ("/boot", "protected_boot_dir"),
        ("/etc/zethropol", "protected_etc_zethropol"),
        ("/etc/polkit-1", "protected_polkit_dir"),
        ("/etc/dbus-1", "protected_dbus_dir"),
        ("/etc/shadow", "protected_shadow"),
        ("/etc/gshadow", "protected_gshadow"),
        ("/etc/sudoers", "protected_sudoers"),
        ("/usr/", "protected_usr_dir"),
        ("/sys/", "protected_sys_dir"),
        ("/proc/", "protected_proc_dir"),
        ("/dev/", "protected_dev_dir"),
        ("/root", "protected_root_dir"),
        ("/var/lib/zethropol-security", "protected_security_state"),
        ("/var/log/zethropol-security", "protected_security_audit"),
        ("/.ssh", "protected_ssh_secrets"),
        ("/.gnupg", "protected_gnupg_secrets"),
    ];
    for &(needle, label) in PROTECTED_PATH_NEEDLES {
        if lower.contains(needle) {
            return SafetyVerdict::DenyProtectedPath {
                pattern: label.to_string(),
                reason: format!(
                    "Güvenlik engeli: Korumalı sistem/gizli dizine ({needle}) erişim veya değişiklik fail-closed reddedildi."
                ),
            };
        }
    }

    SafetyVerdict::Allow
}

pub fn is_protected_path(path: &Path) -> bool {
    let Some(s) = path.to_str() else {
        return true;
    };
    let lower = s.to_ascii_lowercase();
    if lower == "/usr" || lower == "/sys" || lower == "/proc" || lower == "/dev" || lower == "/boot" {
        return true;
    }
    const PREFIXES: &[&str] = &[
        "/boot/",
        "/etc/zethropol",
        "/etc/polkit-1",
        "/etc/dbus-1",
        "/etc/shadow",
        "/etc/gshadow",
        "/etc/sudoers",
        "/usr/",
        "/sys/",
        "/proc/",
        "/dev/",
        "/root",
        "/var/lib/zethropol-security",
        "/var/log/zethropol-security",
    ];
    if PREFIXES.iter().any(|p| lower == *p || lower.starts_with(p)) {
        return true;
    }
    const FRAGMENTS: &[&str] = &[
        "/.ssh",
        "/.gnupg",
        "/.pki/",
        "/.aws/",
        "/.kube/",
        "/.local/share/keyrings",
        "/.netrc",
        "/.git-credentials",
        "zci-policy-state.json",
        "zci-audit.jsonl",
        "services/recovery/src/recovery_service.rs",
    ];
    FRAGMENTS.iter().any(|f| lower.contains(f))
        || lower.ends_with("/agents.md")
        || lower.ends_with("/.env")
        || lower.ends_with(".pem")
        || lower.ends_with(".key")
}

/// Validates and lexically normalizes an absolute target path with zero traversal or protected prefixes.
pub fn validate_target_path(raw: &str) -> Result<PathBuf, String> {
    let trimmed = raw.trim();
    if trimmed.is_empty()
        || trimmed.len() > 2048
        || trimmed.chars().any(|c| c == '\0' || c.is_control())
    {
        return Err("Geçersiz veya kontrol karakteri içeren hedef yol reddedildi.".to_string());
    }
    let lower = trimmed.to_ascii_lowercase();
    if trimmed.contains('\\')
        || lower.contains("%2e")
        || lower.contains("%2f")
        || lower.contains("%5c")
        || lower.contains("%00")
    {
        return Err("Kodlanmış yol geçişi veya ters eğik çizgi reddedildi.".to_string());
    }
    let path = PathBuf::from(trimmed);
    if !path.is_absolute() {
        return Err("Hedef yol mutlak (absolute) olmalıdır.".to_string());
    }
    let mut normalized = PathBuf::new();
    for comp in path.components() {
        match comp {
            Component::RootDir => normalized.push("/"),
            Component::Normal(seg) => normalized.push(seg),
            Component::CurDir => {}
            Component::ParentDir | Component::Prefix(_) => {
                return Err("Üst dizin geçişi ('..') içeren hedef yol reddedildi.".to_string());
            }
        }
    }
    if is_protected_path(&normalized) {
        return Err(format!(
            "Korumalı sistem dizini fail-closed reddedildi: {}",
            normalized.display()
        ));
    }
    Ok(normalized)
}

/// Verifies a write/create target stays strictly within allowlisted user workspace roots
/// (`~/Zethropol-OS`, `~/Projects`, `/tmp/zekans-work`, or explicit test workspace roots).
pub fn validate_writable_workspace_path(
    raw: &str,
    home_dir: &Path,
    extra_roots: &[PathBuf],
) -> Result<PathBuf, String> {
    let normalized = validate_target_path(raw)?;
    let mut allowed_roots = vec![
        home_dir.join("Zethropol-OS"),
        home_dir.join("Projects"),
        PathBuf::from("/tmp/zekans-work"),
    ];
    for r in extra_roots {
        allowed_roots.push(r.clone());
    }
    let inside_root = allowed_roots.iter().any(|root| normalized.starts_with(root));
    if !inside_root {
        return Err(format!(
            "Yazma hedefi izinli çalışma alanı dışındadır (fail-closed): {}",
            normalized.display()
        ));
    }
    Ok(normalized)
}

pub fn extract_display_text_from_envelope(wrapped_payload: &str) -> String {
    const MARKERS: &[(&str, &str)] = &[
        ("<<<ZCI_UNTRUSTED_DATA", "<<<END_ZCI_UNTRUSTED_DATA"),
        ("<<<BEGIN_UNTRUSTED_DATA", "<<<END_UNTRUSTED_DATA"),
        ("<<<ZETHROPOL_UNTRUSTED_DATA", "<<<END_ZETHROPOL_UNTRUSTED_DATA"),
    ];
    for &(open_marker, close_marker) in MARKERS {
        if let Some(start) = wrapped_payload.find(open_marker) {
            if let Some(after_open_nl) = wrapped_payload[start..].find('\n') {
                let body_start = start + after_open_nl + 1;
                if let Some(end_rel) = wrapped_payload[body_start..].rfind(close_marker) {
                    return wrapped_payload[body_start..body_start + end_rel]
                        .trim()
                        .to_string();
                }
            }
        }
    }
    wrapped_payload.trim().to_string()
}

pub fn build_planning_prompt(
    safe_goal: &str,
    agent_profile: &str,
    workspace_dir: &str,
) -> String {
    format!(
        "[SYSTEM]\n{PLANNER_SYSTEM_PROMPT}\n\n[AGENT_PROFILE]\n{agent_profile}\n\n[WORKSPACE_DIR]\n{workspace_dir}\n\n[USER_GOAL_UNTRUSTED]\n{safe_goal}"
    )
}

#[derive(Debug, Deserialize)]
struct RawModelPlan {
    summary: Option<String>,
    steps: Vec<RawModelStep>,
}

#[derive(Debug, Deserialize)]
struct RawModelStep {
    action_kind: String,
    target: Option<String>,
    dbus_interface: Option<String>,
    method: Option<String>,
    content: Option<String>,
    description: Option<String>,
}

/// Validates and converts raw allowlisted step descriptors (from sealed model output or
/// deterministic safe standby planner) into policy-classified `PlanStep` objects.
pub fn parse_and_validate_model_plan(
    raw_text: &str,
    agent_profile: &str,
    workspace_dir: &str,
    home_dir: &Path,
    extra_roots: &[PathBuf],
) -> Result<(String, Vec<PlanStep>), String> {
    let trimmed = raw_text.trim();
    let json_slice = if let (Some(start), Some(end)) = (trimmed.find('{'), trimmed.rfind('}')) {
        if end >= start {
            &trimmed[start..=end]
        } else {
            trimmed
        }
    } else {
        return Err("Model planı yapılandırılmış JSON içermiyor.".to_string());
    };

    let parsed: RawModelPlan = serde_json::from_str(json_slice)
        .map_err(|e| format!("Model plan JSON ayrıştırma hatası: {e}"))?;
    if parsed.steps.is_empty() {
        return Err("Plan en az bir izin listeli adım içermelidir.".to_string());
    }
    if parsed.steps.len() > MAX_STEPS_PER_PLAN {
        return Err(format!(
            "Plan adım sayısı azami sınırı ({MAX_STEPS_PER_PLAN}) aşıyor."
        ));
    }

    let mut validated = Vec::with_capacity(parsed.steps.len());
    for (idx, raw) in parsed.steps.into_iter().enumerate() {
        let step = validate_and_build_step(
            idx as u32 + 1,
            &raw.action_kind,
            raw.target.as_deref().unwrap_or(""),
            raw.dbus_interface.as_deref().unwrap_or(""),
            raw.method.as_deref().unwrap_or(""),
            raw.content.as_deref().unwrap_or(""),
            raw.description.as_deref().unwrap_or(""),
            agent_profile,
            workspace_dir,
            home_dir,
            extra_roots,
        )?;
        validated.push(step);
    }

    let summary = parsed
        .summary
        .map(|s| s.trim().chars().take(240).collect::<String>())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "Yapılandırılmış ZCI Komut Ajanı eylem planı hazırlandı.".to_string());

    Ok((summary, validated))
}

#[allow(clippy::too_many_arguments)]
pub fn validate_and_build_step(
    index: u32,
    action_kind: &str,
    raw_target: &str,
    raw_iface: &str,
    raw_method: &str,
    content: &str,
    description: &str,
    _agent_profile: &str,
    workspace_dir: &str,
    home_dir: &Path,
    extra_roots: &[PathBuf],
) -> Result<PlanStep, String> {
    let kind = action_kind.trim();
    if let SafetyVerdict::DenyShellInjection { reason, .. }
    | SafetyVerdict::DenyProtectedPath { reason, .. }
    | SafetyVerdict::DenyPolicyTamper { reason, .. } = inspect_text_safety(raw_target)
    {
        return Err(reason);
    }
    if let SafetyVerdict::DenyShellInjection { reason, .. }
    | SafetyVerdict::DenyPolicyTamper { reason, .. } = inspect_text_safety(kind)
    {
        return Err(reason);
    }

    let resolved_target = |t: &str| -> String {
        let tt = t.trim();
        if tt.is_empty() {
            workspace_dir.to_string()
        } else if tt.starts_with('/') {
            tt.to_string()
        } else if !workspace_dir.is_empty() {
            PathBuf::from(workspace_dir)
                .join(tt)
                .display()
                .to_string()
        } else {
            tt.to_string()
        }
    };

    let (operation, exec_agent, target, dbus_interface, method, risk, requires_approval, default_desc) =
        match kind {
            "read_file" => {
                let candidate = resolved_target(raw_target);
                let clean = validate_target_path(&candidate)?;
                (
                    "read_file",
                    "zekans-code",
                    clean.display().to_string(),
                    String::new(),
                    String::new(),
                    RiskLevel::ReadOnly,
                    false,
                    "İzinli dosya salt-okunur okunacak ve WrapUntrustedData ile mühürlenecek",
                )
            }
            "project_analysis" => {
                let candidate = resolved_target(raw_target);
                let clean = validate_target_path(&candidate)?;
                (
                    "list_dir",
                    "zekans-code",
                    clean.display().to_string(),
                    String::new(),
                    String::new(),
                    RiskLevel::ReadOnly,
                    false,
                    "Çalışma alanı salt-okunur proje analizi ile taranacak",
                )
            }
            "system_diagnostics" => {
                let iface = if raw_iface.trim().is_empty() {
                    "org.zethropol.Hardware.v1"
                } else {
                    raw_iface.trim()
                };
                let meth = if raw_method.trim().is_empty() {
                    if !raw_target.trim().is_empty() && !raw_target.contains('/') {
                        raw_target.trim()
                    } else {
                        "GetTelemetry"
                    }
                } else {
                    raw_method.trim()
                };
                if !is_allowed_diagnostic_pair(iface, meth) {
                    return Err(format!(
                        "İzin listesi dışı D-Bus tanılama hedefi reddedildi: {iface}.{meth}"
                    ));
                }
                (
                    "dbus_call",
                    "zci-command",
                    meth.to_string(),
                    iface.to_string(),
                    meth.to_string(),
                    RiskLevel::ReadOnly,
                    false,
                    "Zethropol salt-okunur sistem tanılama verisi alınacak",
                )
            }
            "recovery_readiness" => (
                "recovery_preflight",
                "zci-command",
                "org.zethropol.Recovery.v1.GetState".to_string(),
                "org.zethropol.Recovery.v1".to_string(),
                "GetState".to_string(),
                RiskLevel::ReadOnly,
                false,
                "Snapper ve kurtarma kilidi (rollback/repair=false) ön-kontrolü yapılacak",
            ),
            "write_workspace_file" | "create_workspace_file" => {
                if content.len() > MAX_WRITE_FILE_BYTES {
                    return Err("Yazılacak içerik 256 KiB güvenlik sınırını aşıyor.".to_string());
                }
                if content.contains('\0') {
                    return Err("NUL bayt içeren ikili dosya yazma isteği reddedildi.".to_string());
                }
                let candidate = resolved_target(raw_target);
                let clean = validate_writable_workspace_path(&candidate, home_dir, extra_roots)?;
                let op = if kind == "create_workspace_file" {
                    "create_file"
                } else {
                    "write_file"
                };
                (
                    op,
                    "zekans-code",
                    clean.display().to_string(),
                    String::new(),
                    String::new(),
                    RiskLevel::ApprovalRequired,
                    true,
                    "Çalışma alanındaki dosyaya yazma (kullanıcı onayı + Snapper ön-kontrol + zci-daemon preflight zorunlu)",
                )
            }
            other => {
                return Err(format!(
                    "İzin listesi dışı veya ham kabuk eylemi reddedildi (default-deny): '{other}'"
                ));
            }
        };

    let desc_clean = description.trim();
    let final_desc = if desc_clean.is_empty() {
        default_desc.to_string()
    } else {
        desc_clean.chars().take(200).collect()
    };

    Ok(PlanStep {
        step_id: format!("step-{index}"),
        index,
        action_kind: kind.to_string(),
        operation: operation.to_string(),
        agent_id: exec_agent.to_string(),
        target,
        dbus_interface,
        method,
        content: content.to_string(),
        description: final_desc,
        risk_level: risk.as_str().to_string(),
        requires_approval,
        approval_status: if requires_approval {
            "pending".to_string()
        } else {
            "auto-readonly".to_string()
        },
        approval_note: String::new(),
        execution_status: "planned".to_string(),
        policy_rule_id: String::new(),
        policy_action_class: String::new(),
        output_preview: String::new(),
        executable: false,
        policy_authority: "none".to_string(),
        audit_hash: String::new(),
    })
}

/// Deterministic safe fallback planner used when `zci-runtime` is in `Standby` or returns plain text.
/// Produces ONLY allowlisted, validated structured steps — NEVER raw shell commands.
pub fn build_deterministic_safe_plan(
    goal: &str,
    agent_profile: &str,
    workspace_dir: &str,
    home_dir: &Path,
    extra_roots: &[PathBuf],
) -> Result<(String, Vec<PlanStep>), String> {
    let lower = goal.to_ascii_lowercase();
    let mut steps: Vec<PlanStep> = Vec::new();
    let mut next_idx = 1u32;

    // Detect an explicit absolute path mentioned in the user goal (if safe & non-protected)
    let extracted_abs_path = goal
        .split_whitespace()
        .map(|tok| tok.trim_matches(|c: char| matches!(c, '"' | '\'' | ',' | ';' | ')' | '(')))
        .find(|tok| tok.starts_with('/') && tok.len() > 2);

    let wants_write = lower.contains("oluştur")
        || lower.contains("yaz")
        || lower.contains("kaydet")
        || lower.contains("create file")
        || lower.contains("write file");

    if wants_write {
        let target_path = if let Some(p) = extracted_abs_path {
            p.to_string()
        } else if !workspace_dir.is_empty() {
            PathBuf::from(workspace_dir)
                .join("zci-agent-note.txt")
                .display()
                .to_string()
        } else {
            "/tmp/zekans-work/zci-agent-note.txt".to_string()
        };
        steps.push(validate_and_build_step(
            next_idx,
            "recovery_readiness",
            "",
            "",
            "",
            "",
            "Yazma adımı öncesi Snapper ve kurtarma kilidi hazırlık kontrolü",
            agent_profile,
            workspace_dir,
            home_dir,
            extra_roots,
        )?);
        next_idx += 1;

        steps.push(validate_and_build_step(
            next_idx,
            "create_workspace_file",
            &target_path,
            "",
            "",
            "ZCI Komut Ajanı tarafından onaylı çalışma alanı çıktısı.\n",
            "Kullanıcı onaylı çalışma alanı dosyası oluşturma adımı",
            agent_profile,
            workspace_dir,
            home_dir,
            extra_roots,
        )?);
        return Ok((
            "Çalışma alanı yazma planı (Snapper ön-kontrol + kullanıcı onayı gerektirir)".to_string(),
            steps,
        ));
    }

    if lower.contains("snapper") || lower.contains("kurtarma") || lower.contains("recovery") {
        steps.push(validate_and_build_step(
            next_idx,
            "recovery_readiness",
            "",
            "",
            "",
            "",
            "Snapper anlık görüntü ve kurtarma kilitleri salt-okunur kontrolü",
            agent_profile,
            workspace_dir,
            home_dir,
            extra_roots,
        )?);
        next_idx += 1;
    }

    if lower.contains("proje") || lower.contains("workspace") || lower.contains("analiz") || lower.contains("dizin") {
        if !workspace_dir.is_empty() {
            steps.push(validate_and_build_step(
                next_idx,
                "project_analysis",
                workspace_dir,
                "",
                "",
                "",
                "Çalışma alanı dosya yapısı salt-okunur proje analizi",
                agent_profile,
                workspace_dir,
                home_dir,
                extra_roots,
            )?);
            next_idx += 1;
        }
    }

    if let Some(p) = extracted_abs_path {
        if Path::new(p).is_file() || lower.contains("oku") || lower.contains("read") {
            steps.push(validate_and_build_step(
                next_idx,
                "read_file",
                p,
                "",
                "",
                "",
                "Belirtilen dosyanın salt-okunur içerik denetimi",
                agent_profile,
                workspace_dir,
                home_dir,
                extra_roots,
            )?);
            next_idx += 1;
        }
    }

    if steps.is_empty()
        || lower.contains("sistem")
        || lower.contains("donanım")
        || lower.contains("durum")
        || lower.contains("status")
        || lower.contains("servis")
    {
        steps.push(validate_and_build_step(
            next_idx,
            "system_diagnostics",
            "GetTelemetry",
            "org.zethropol.Hardware.v1",
            "GetTelemetry",
            "",
            "Donanım ve sistem telemetrisi salt-okunur sorgusu",
            agent_profile,
            workspace_dir,
            home_dir,
            extra_roots,
        )?);
        next_idx += 1;
        steps.push(validate_and_build_step(
            next_idx,
            "recovery_readiness",
            "",
            "",
            "",
            "",
            "Snapper kurtarma hazırlığı ve kilit doğrulaması",
            agent_profile,
            workspace_dir,
            home_dir,
            extra_roots,
        )?);
    }

    Ok((
        "Salt-okunur ZCI Komut Ajanı doğrulama planı hazırlandı.".to_string(),
        steps,
    ))
}
