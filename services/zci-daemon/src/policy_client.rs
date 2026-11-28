use crate::config::DaemonConfig;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use zbus::proxy::CacheProperties;
use zbus::zvariant::OwnedValue;

pub fn now_unix_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// Hard defense-in-depth invariant (10.02):
/// `zci-daemon` will NEVER execute or approve any action outside `ReadOnly` and `UserSpaceSafe`,
/// even if an upstream response unexpectedly claims `allowed=true`.
pub fn is_gateway_permitted_action_class(action_class: &str) -> bool {
    matches!(action_class, "ReadOnly" | "UserSpaceSafe")
}

#[derive(Debug, Clone)]
pub struct PolicyEvalReply {
    pub allowed: bool,
    pub decision: String,
    pub action_class: String,
    pub requires_polkit: bool,
    pub polkit_action: String,
    pub rule_id: String,
    pub code: String,
    pub reason: String,
    pub penalty_score: u32,
    pub lockout_active: bool,
    pub hard_locked: bool,
    pub cooldown_remaining_secs: u64,
    pub untrusted_flags: Vec<String>,
    pub audit_event_id: String,
}

#[derive(Debug, Clone)]
pub struct WrappedDataReply {
    pub trust_level: String,
    pub executable: bool,
    pub policy_authority: String,
    pub source_kind: String,
    pub content_sha256: String,
    pub byte_length: u64,
    pub oversized: bool,
    pub injection_detected: bool,
    pub self_modification_injection: bool,
    pub detected_patterns: Vec<String>,
    pub envelope_id: String,
    pub wrapped_payload: String,
    pub code: String,
}

#[derive(Debug, Clone)]
pub struct AgentPolicyStatusReply {
    pub known_agent: bool,
    pub lockout_active: bool,
    pub hard_locked: bool,
    pub cooldown_remaining_secs: u64,
    pub strike_score: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PolicyHealthSnapshot {
    pub reachable: bool,
    pub engine_version: String,
    pub default_deny: bool,
    pub self_modification_lock: bool,
    pub recovery_locks_protected: bool,
    pub state_corrupted: bool,
    pub locked_agents: u64,
    pub total_audit_events: u64,
    pub checked_at_unix: u64,
    pub last_error: String,
}

impl Default for PolicyHealthSnapshot {
    fn default() -> Self {
        Self {
            reachable: false,
            engine_version: String::new(),
            default_deny: false,
            self_modification_lock: false,
            recovery_locks_protected: false,
            state_corrupted: false,
            locked_agents: 0,
            total_audit_events: 0,
            checked_at_unix: 0,
            last_error: "Not probed yet".to_string(),
        }
    }
}

fn get_bool(map: &HashMap<String, OwnedValue>, key: &str) -> Option<bool> {
    map.get(key).and_then(|v| bool::try_from(v).ok())
}

fn get_string(map: &HashMap<String, OwnedValue>, key: &str) -> Option<String> {
    map.get(key)
        .and_then(|v| v.try_clone().ok())
        .and_then(|v| String::try_from(v).ok())
}

fn get_u32(map: &HashMap<String, OwnedValue>, key: &str) -> Option<u32> {
    let v = map.get(key)?;
    u32::try_from(v)
        .ok()
        .or_else(|| u64::try_from(v).ok().and_then(|n| u32::try_from(n).ok()))
}

fn get_u64(map: &HashMap<String, OwnedValue>, key: &str) -> Option<u64> {
    let v = map.get(key)?;
    u64::try_from(v)
        .ok()
        .or_else(|| u32::try_from(v).ok().map(u64::from))
}

fn get_string_vec(map: &HashMap<String, OwnedValue>, key: &str) -> Vec<String> {
    map.get(key)
        .and_then(|v| v.try_clone().ok())
        .and_then(|v| Vec::<String>::try_from(v).ok())
        .unwrap_or_default()
}

fn get_data_map(
    envelope: &HashMap<String, OwnedValue>,
) -> Result<HashMap<String, OwnedValue>, String> {
    let raw = envelope
        .get("data")
        .ok_or_else(|| "Policy Engine response missing 'data' map.".to_string())?;
    raw.try_clone()
        .and_then(HashMap::try_from)
        .map_err(|e| format!("Malformed Policy Engine 'data' map: {e}"))
}

async fn build_policy_proxy<'a>(
    connection: &'a zbus::Connection,
    cfg: &DaemonConfig,
) -> Result<zbus::Proxy<'a>, String> {
    zbus::proxy::Builder::new(connection)
        .destination(cfg.policy_bus_name.clone())
        .map_err(|e| format!("Invalid policy bus destination: {e}"))?
        .path(cfg.policy_object_path.clone())
        .map_err(|e| format!("Invalid policy object path: {e}"))?
        .interface(cfg.policy_interface.clone())
        .map_err(|e| format!("Invalid policy interface: {e}"))?
        .cache_properties(CacheProperties::No)
        .build()
        .await
        .map_err(|e| format!("Failed to construct policy proxy: {e}"))
}

/// 10.02 — Mandatory fail-closed call to `org.zethropol.Security.Policy.v1.EvaluateAction`.
pub async fn evaluate_action_remote(
    connection: &zbus::Connection,
    cfg: &DaemonConfig,
    agent_id: &str,
    operation: &str,
    target: &str,
    dbus_interface: &str,
    untrusted_payload: &str,
) -> Result<PolicyEvalReply, String> {
    let timeout = Duration::from_millis(cfg.policy_timeout_ms);
    let proxy = tokio::time::timeout(timeout, build_policy_proxy(connection, cfg))
        .await
        .map_err(|_| "Timed out connecting to Security Policy Engine.".to_string())??;

    let reply: HashMap<String, OwnedValue> = tokio::time::timeout(
        timeout,
        proxy.call(
            "EvaluateAction",
            &(agent_id, operation, target, dbus_interface, untrusted_payload),
        ),
    )
    .await
    .map_err(|_| "Timed out awaiting EvaluateAction from Security Policy Engine.".to_string())?
    .map_err(|e| format!("EvaluateAction D-Bus call failed: {e}"))?;

    let envelope_success = get_bool(&reply, "success")
        .ok_or_else(|| "Policy Engine reply missing boolean 'success'.".to_string())?;
    let code = get_string(&reply, "code").unwrap_or_else(|| "policy-denied".to_string());
    let envelope_message = get_string(&reply, "message").unwrap_or_default();
    let data = get_data_map(&reply)?;

    let data_allowed = get_bool(&data, "allowed")
        .ok_or_else(|| "Policy Engine data missing boolean 'allowed'.".to_string())?;
    let action_class = get_string(&data, "action_class")
        .ok_or_else(|| "Policy Engine data missing 'action_class'.".to_string())?;

    Ok(PolicyEvalReply {
        allowed: envelope_success && data_allowed,
        decision: get_string(&data, "decision").unwrap_or_else(|| "deny".to_string()),
        action_class,
        requires_polkit: get_bool(&data, "requires_polkit").unwrap_or(false),
        polkit_action: get_string(&data, "polkit_action").unwrap_or_default(),
        rule_id: get_string(&data, "rule_id").unwrap_or_else(|| "UNKNOWN_RULE".to_string()),
        code,
        reason: get_string(&data, "reason").unwrap_or(envelope_message),
        penalty_score: get_u32(&data, "penalty_score").unwrap_or(0),
        lockout_active: get_bool(&data, "lockout_active").unwrap_or(false),
        hard_locked: get_bool(&data, "hard_locked").unwrap_or(false),
        cooldown_remaining_secs: get_u64(&data, "cooldown_remaining_secs").unwrap_or(0),
        untrusted_flags: get_string_vec(&data, "untrusted_flags"),
        audit_event_id: get_string(&data, "audit_event_id").unwrap_or_default(),
    })
}

/// 10.03 — Mandatory fail-closed untrusted content sealing via `WrapUntrustedData`.
pub async fn wrap_untrusted_remote(
    connection: &zbus::Connection,
    cfg: &DaemonConfig,
    agent_id: &str,
    source_kind: &str,
    content: &str,
) -> Result<WrappedDataReply, String> {
    let timeout = Duration::from_millis(cfg.policy_timeout_ms);
    let proxy = tokio::time::timeout(timeout, build_policy_proxy(connection, cfg))
        .await
        .map_err(|_| "Timed out connecting to Security Policy Engine for WrapUntrustedData.".to_string())??;

    let reply: HashMap<String, OwnedValue> = tokio::time::timeout(
        timeout,
        proxy.call("WrapUntrustedData", &(agent_id, source_kind, content)),
    )
    .await
    .map_err(|_| "Timed out awaiting WrapUntrustedData from Security Policy Engine.".to_string())?
    .map_err(|e| format!("WrapUntrustedData D-Bus call failed: {e}"))?;

    let success = get_bool(&reply, "success").unwrap_or(false);
    if !success {
        return Err("Policy Engine rejected WrapUntrustedData call.".to_string());
    }
    let code = get_string(&reply, "code").unwrap_or_else(|| "ok".to_string());
    let data = get_data_map(&reply)?;

    let executable = get_bool(&data, "executable").unwrap_or(true);
    let policy_authority = get_string(&data, "policy_authority").unwrap_or_default();
    let wrapped_payload = get_string(&data, "wrapped_payload")
        .ok_or_else(|| "WrapUntrustedData reply missing 'wrapped_payload'.".to_string())?;

    // Strict fail-closed verification of non-executable isolation guarantees.
    if executable || policy_authority != "none" || wrapped_payload.is_empty() {
        return Err(
            "WrapUntrustedData reply violated non-executable / zero-authority contract."
                .to_string(),
        );
    }

    Ok(WrappedDataReply {
        trust_level: get_string(&data, "trust_level")
            .unwrap_or_else(|| "untrusted-data-payload".to_string()),
        executable,
        policy_authority,
        source_kind: get_string(&data, "source_kind").unwrap_or_else(|| source_kind.to_string()),
        content_sha256: get_string(&data, "content_sha256").unwrap_or_default(),
        byte_length: get_u64(&data, "byte_length").unwrap_or(content.len() as u64),
        oversized: get_bool(&data, "oversized").unwrap_or(false),
        injection_detected: get_bool(&data, "injection_detected").unwrap_or(false),
        self_modification_injection: get_bool(&data, "self_modification_injection")
            .unwrap_or(false),
        detected_patterns: get_string_vec(&data, "detected_patterns"),
        envelope_id: get_string(&data, "envelope_id").unwrap_or_default(),
        wrapped_payload,
        code,
    })
}

/// Read-only agent lockout query against `org.zethropol.Security.Policy.v1.GetAgentStatus`.
pub async fn get_agent_status_remote(
    connection: &zbus::Connection,
    cfg: &DaemonConfig,
    agent_id: &str,
) -> Result<AgentPolicyStatusReply, String> {
    let timeout = Duration::from_millis(cfg.policy_timeout_ms);
    let proxy = tokio::time::timeout(timeout, build_policy_proxy(connection, cfg))
        .await
        .map_err(|_| "Timed out connecting to Security Policy Engine.".to_string())??;

    let reply: HashMap<String, OwnedValue> = tokio::time::timeout(
        timeout,
        proxy.call("GetAgentStatus", &(agent_id,)),
    )
    .await
    .map_err(|_| "Timed out awaiting GetAgentStatus.".to_string())?
    .map_err(|e| format!("GetAgentStatus D-Bus call failed: {e}"))?;

    if !get_bool(&reply, "success").unwrap_or(false) {
        return Err("GetAgentStatus returned success=false.".to_string());
    }
    let data = get_data_map(&reply)?;
    Ok(AgentPolicyStatusReply {
        known_agent: get_bool(&data, "known_agent").unwrap_or(false),
        lockout_active: get_bool(&data, "lockout_active").unwrap_or(true),
        hard_locked: get_bool(&data, "hard_locked").unwrap_or(false),
        cooldown_remaining_secs: get_u64(&data, "cooldown_remaining_secs").unwrap_or(0),
        strike_score: get_u32(&data, "strike_score").unwrap_or(0),
    })
}

/// 10.06 — Probe Policy Engine connectivity and security posture via `GetPolicySummary`.
pub async fn probe_policy_health(
    connection: &zbus::Connection,
    cfg: &DaemonConfig,
) -> PolicyHealthSnapshot {
    let checked_at = now_unix_secs();
    let timeout = Duration::from_millis(cfg.policy_timeout_ms.min(2_500));

    let result: Result<PolicyHealthSnapshot, String> = async {
        let proxy = tokio::time::timeout(timeout, build_policy_proxy(connection, cfg))
            .await
            .map_err(|_| "Policy Engine probe timed out.".to_string())??;

        let reply: HashMap<String, OwnedValue> =
            tokio::time::timeout(timeout, proxy.call("GetPolicySummary", &()))
                .await
                .map_err(|_| "GetPolicySummary timed out.".to_string())?
                .map_err(|e| format!("GetPolicySummary D-Bus error: {e}"))?;

        if !get_bool(&reply, "success").unwrap_or(false) {
            return Err("GetPolicySummary returned success=false.".to_string());
        }
        let data = get_data_map(&reply)?;
        Ok(PolicyHealthSnapshot {
            reachable: true,
            engine_version: get_string(&data, "engine_version").unwrap_or_default(),
            default_deny: get_bool(&data, "default_deny").unwrap_or(false),
            self_modification_lock: get_bool(&data, "self_modification_lock").unwrap_or(false),
            recovery_locks_protected: get_bool(&data, "recovery_locks_protected").unwrap_or(false),
            state_corrupted: get_bool(&data, "state_corrupted").unwrap_or(false),
            locked_agents: get_u64(&data, "locked_agents").unwrap_or(0),
            total_audit_events: get_u64(&data, "total_audit_events").unwrap_or(0),
            checked_at_unix: checked_at,
            last_error: String::new(),
        })
    }
    .await;

    match result {
        Ok(snapshot) => snapshot,
        Err(err) => PolicyHealthSnapshot {
            reachable: false,
            checked_at_unix: checked_at,
            last_error: err,
            ..PolicyHealthSnapshot::default()
        },
    }
}
