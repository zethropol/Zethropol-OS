use crate::config::BASE_SYSTEM_PROMPT;
use crate::crypto_store::ChatMessage;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum InputSafetyVerdict {
    Allow,
    DenyCommandExecution { pattern: String, reason: String },
    DenyPromptInjection { pattern: String, reason: String },
}

/// 12.03 — Hard fail-closed Stage 13 boundary & prompt-injection guard.
/// ZCI Chatbot (`zci-chat`) is strictly read-only local conversation; direct shell execution,
/// system state mutations, and self-modification/override injections are rejected fail-closed.
pub fn inspect_user_message_safety(message: &str) -> InputSafetyVerdict {
    let lower = message.to_ascii_lowercase();

    // 1. Self-modification / safety-lock tampering / prompt-override injection checks
    const INJECTION_PATTERNS: &[(&str, &str)] = &[
        ("ignore previous instructions", "prompt_override_en"),
        ("disregard all previous", "prompt_override_en"),
        ("önceki talimatları yoksay", "prompt_override_tr"),
        ("sistem talimatlarını geçersiz kıl", "prompt_override_tr"),
        ("rollback_execution_enabled", "recovery_lock_tamper"),
        ("repair_execution_enabled", "recovery_lock_tamper"),
        ("bypass polkit", "polkit_bypass_attempt"),
        ("disable polkit", "polkit_bypass_attempt"),
        ("agents.md", "policy_self_modification"),
        ("/etc/polkit-1", "polkit_policy_tamper"),
        ("/etc/dbus-1", "dbus_policy_tamper"),
        ("zci-policy-state.json", "policy_state_tamper"),
    ];
    for &(needle, label) in INJECTION_PATTERNS {
        if lower.contains(needle) {
            return InputSafetyVerdict::DenyPromptInjection {
                pattern: label.to_string(),
                reason: "Güvenlik engeli: Politika/kilit geçersiz kılma veya prompt-injection kalıbı tespit edildi ve reddedildi.".to_string(),
            };
        }
    }

    // 2. Direct shell command / system mutation execution boundary (Stage 13!)
    const COMMAND_EXEC_PATTERNS: &[(&str, &str)] = &[
        ("sudo ", "shell_sudo_execution"),
        ("pkexec ", "shell_pkexec_execution"),
        ("pacman -s", "package_mutation_command"),
        ("pacman -r", "package_mutation_command"),
        ("pacman -u", "package_mutation_command"),
        ("systemctl start", "systemd_state_change"),
        ("systemctl stop", "systemd_state_change"),
        ("systemctl restart", "systemd_state_change"),
        ("systemctl enable", "systemd_state_change"),
        ("systemctl disable", "systemd_state_change"),
        ("rm -rf", "destructive_fs_command"),
        ("mkfs.", "destructive_disk_command"),
        ("dd if=", "destructive_disk_command"),
        ("bash -c", "shell_invocation"),
        ("sh -c", "shell_invocation"),
        ("| sh", "pipe_to_shell"),
        ("| bash", "pipe_to_shell"),
        ("komut çalıştır:", "direct_command_request_tr"),
        ("kabuk komutu:", "direct_command_request_tr"),
        ("execute command:", "direct_command_request_en"),
        ("run shell:", "direct_command_request_en"),
    ];
    for &(needle, label) in COMMAND_EXEC_PATTERNS {
        if lower.contains(needle) {
            return InputSafetyVerdict::DenyCommandExecution {
                pattern: label.to_string(),
                reason: "ZCI Chatbot yalnızca salt-okunur yerel sohbet içindir; doğrudan kabuk komutu veya sistem değişikliği çalıştırma yetkisi kapalıdır (Aşama 13 kapsamındadır).".to_string(),
            };
        }
    }

    InputSafetyVerdict::Allow
}

/// Conservative token estimator (~3 Unicode chars/token + small message framing overhead).
pub fn estimate_tokens(text: &str) -> u32 {
    if text.is_empty() {
        return 0;
    }
    ((text.chars().count() / 3) + 4) as u32
}

#[derive(Debug, Clone)]
pub struct TrimmedPromptContext {
    pub assembled_prompt: String,
    pub kept_history_messages: usize,
    pub trimmed_history_messages: usize,
    pub estimated_tokens: u32,
}

/// 12.01 — Token-budget-aware multi-turn context trimmer aligned with `zci-runtime` `max_context_tokens`.
/// Drops oldest history messages first while preserving the base system instructions, optional
/// sealed read-only system context, and the newest sealed user turn.
pub fn assemble_and_trim_prompt(
    history: &[ChatMessage],
    sealed_system_context: Option<&str>,
    sealed_latest_user: &str,
    max_context_tokens: u32,
    reserved_output_tokens: u32,
) -> Result<TrimmedPromptContext, String> {
    let effective_budget = max_context_tokens
        .saturating_sub(reserved_output_tokens)
        .max(256);

    let mut fixed_tokens = estimate_tokens(BASE_SYSTEM_PROMPT) + estimate_tokens(sealed_latest_user) + 24;
    if let Some(ctx) = sealed_system_context {
        fixed_tokens = fixed_tokens.saturating_add(estimate_tokens(ctx) + 12);
    }

    if fixed_tokens > effective_budget {
        return Err(format!(
            "İleti ve bağlam boyutu ({fixed_tokens} token) aktif bağlam bütçesini ({effective_budget} token) aşıyor."
        ));
    }

    let history_budget = effective_budget.saturating_sub(fixed_tokens);
    let mut kept_rev: Vec<&ChatMessage> = Vec::new();
    let mut used_history_tokens = 0u32;

    for msg in history.iter().rev() {
        if msg.role != "user" && msg.role != "assistant" {
            continue;
        }
        let cost = estimate_tokens(&msg.content) + 8;
        if used_history_tokens.saturating_add(cost) <= history_budget {
            kept_rev.push(msg);
            used_history_tokens = used_history_tokens.saturating_add(cost);
        } else {
            break;
        }
    }
    kept_rev.reverse();

    let total_eligible = history
        .iter()
        .filter(|m| m.role == "user" || m.role == "assistant")
        .count();
    let kept_count = kept_rev.len();
    let trimmed_count = total_eligible.saturating_sub(kept_count);

    let mut prompt = String::new();
    prompt.push_str("[SYSTEM]\n");
    prompt.push_str(BASE_SYSTEM_PROMPT);
    prompt.push_str("\n\n");

    if let Some(ctx) = sealed_system_context {
        prompt.push_str("[READONLY_SYSTEM_CONTEXT_UNTRUSTED]\n");
        prompt.push_str(ctx);
        prompt.push_str("\n\n");
    }

    if !kept_rev.is_empty() {
        prompt.push_str("[CONVERSATION_HISTORY]\n");
        for m in kept_rev {
            let tag = if m.role == "user" { "Kullanıcı" } else { "Asistan" };
            prompt.push_str(tag);
            prompt.push_str(": ");
            prompt.push_str(&m.content);
            prompt.push('\n');
        }
        prompt.push('\n');
    }

    prompt.push_str("[LATEST_USER_TURN_UNTRUSTED]\n");
    prompt.push_str(sealed_latest_user);

    Ok(TrimmedPromptContext {
        assembled_prompt: prompt,
        kept_history_messages: kept_count,
        trimmed_history_messages: trimmed_count,
        estimated_tokens: fixed_tokens.saturating_add(used_history_tokens),
    })
}

/// Safely extracts plain display text from a sealed `WrapUntrustedData` envelope without ever
/// treating it as executable or policy-authoritative content.
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
