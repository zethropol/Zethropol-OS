use crate::config::{parse_loopback_endpoint, RuntimeConfig};
use crate::crypto_util::now_unix_secs;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value as JsonValue};
use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::{Duration, Instant};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use zbus::proxy::CacheProperties;
use zbus::zvariant::OwnedValue;

const MAX_ENGINE_RESPONSE_BYTES: usize = 262_144;

/// Defense-in-depth invariant: `zci-runtime` only executes inference when Policy Engine
/// approves the request strictly inside `ReadOnly` or `UserSpaceSafe`.
pub fn is_permitted_inference_class(action_class: &str) -> bool {
    matches!(action_class, "ReadOnly" | "UserSpaceSafe")
}

#[derive(Debug, Clone)]
pub struct PolicyEvalReply {
    pub allowed: bool,
    pub action_class: String,
    pub rule_id: String,
    pub code: String,
    pub reason: String,
    pub lockout_active: bool,
    pub hard_locked: bool,
    pub audit_event_id: String,
}

#[derive(Debug, Clone)]
pub struct SealedUntrustedReply {
    pub trust_level: String,
    pub executable: bool,
    pub policy_authority: String,
    pub source_kind: String,
    pub content_sha256: String,
    pub injection_detected: bool,
    pub self_modification_injection: bool,
    pub detected_patterns: Vec<String>,
    pub envelope_id: String,
    pub wrapped_payload: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BridgeHealthSnapshot {
    pub policy_reachable: bool,
    pub policy_default_deny: bool,
    pub policy_self_mod_lock: bool,
    pub policy_state_corrupted: bool,
    pub daemon_reachable: bool,
    pub daemon_version: String,
    pub daemon_fail_closed: bool,
    pub checked_at_unix: u64,
    pub last_error: String,
}

impl Default for BridgeHealthSnapshot {
    fn default() -> Self {
        Self {
            policy_reachable: false,
            policy_default_deny: false,
            policy_self_mod_lock: false,
            policy_state_corrupted: false,
            daemon_reachable: false,
            daemon_version: String::new(),
            daemon_fail_closed: false,
            checked_at_unix: 0,
            last_error: "Not probed yet".to_string(),
        }
    }
}

#[derive(Debug, Clone)]
pub enum LoopbackInferenceOutcome {
    Responded {
        status_code: u16,
        raw_output: String,
        latency_ms: u64,
    },
    EngineSleeping {
        endpoint: String,
        latency_ms: u64,
        detail: String,
    },
}

fn get_bool(map: &HashMap<String, OwnedValue>, key: &str) -> Option<bool> {
    map.get(key).and_then(|v| bool::try_from(v).ok())
}

fn get_string(map: &HashMap<String, OwnedValue>, key: &str) -> Option<String> {
    map.get(key)
        .and_then(|v| v.try_clone().ok())
        .and_then(|v| String::try_from(v).ok())
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
        .ok_or_else(|| "D-Bus reply envelope missing 'data' map.".to_string())?;
    raw.try_clone()
        .and_then(HashMap::try_from)
        .map_err(|e| format!("Malformed D-Bus 'data' map: {e}"))
}

async fn build_proxy<'a>(
    connection: &'a zbus::Connection,
    dest: &str,
    path: &str,
    iface: &str,
) -> Result<zbus::Proxy<'a>, String> {
    zbus::proxy::Builder::new(connection)
        .destination(dest.to_string())
        .map_err(|e| format!("Invalid bus destination '{dest}': {e}"))?
        .path(path.to_string())
        .map_err(|e| format!("Invalid object path '{path}': {e}"))?
        .interface(iface.to_string())
        .map_err(|e| format!("Invalid interface '{iface}': {e}"))?
        .cache_properties(CacheProperties::No)
        .build()
        .await
        .map_err(|e| format!("Failed building D-Bus proxy for '{dest}': {e}"))
}

/// Fail-closed action check against `org.zethropol.Security.Policy.v1.EvaluateAction`.
pub async fn evaluate_inference_policy(
    connection: &zbus::Connection,
    cfg: &RuntimeConfig,
    agent_id: &str,
    untrusted_payload: &str,
) -> Result<PolicyEvalReply, String> {
    let timeout = Duration::from_millis(cfg.policy_timeout_ms);
    let proxy = tokio::time::timeout(
        timeout,
        build_proxy(
            connection,
            &cfg.policy_bus_name,
            &cfg.policy_object_path,
            &cfg.policy_interface,
        ),
    )
    .await
    .map_err(|_| "Timed out connecting to Security Policy Engine.".to_string())??;

    let reply: HashMap<String, OwnedValue> = tokio::time::timeout(
        timeout,
        proxy.call(
            "EvaluateAction",
            &(agent_id, "inspect_untrusted", "", "", untrusted_payload),
        ),
    )
    .await
    .map_err(|_| "Timed out awaiting EvaluateAction.".to_string())?
    .map_err(|e| format!("EvaluateAction D-Bus call failed: {e}"))?;

    let envelope_success = get_bool(&reply, "success")
        .ok_or_else(|| "Policy Engine reply missing 'success'.".to_string())?;
    let code = get_string(&reply, "code").unwrap_or_else(|| "policy-denied".to_string());
    let envelope_msg = get_string(&reply, "message").unwrap_or_default();
    let data = get_data_map(&reply)?;

    let data_allowed = get_bool(&data, "allowed")
        .ok_or_else(|| "Policy Engine data missing 'allowed'.".to_string())?;
    let action_class = get_string(&data, "action_class")
        .ok_or_else(|| "Policy Engine data missing 'action_class'.".to_string())?;

    Ok(PolicyEvalReply {
        allowed: envelope_success && data_allowed,
        action_class,
        rule_id: get_string(&data, "rule_id").unwrap_or_else(|| "UNKNOWN_RULE".to_string()),
        code,
        reason: get_string(&data, "reason").unwrap_or(envelope_msg),
        lockout_active: get_bool(&data, "lockout_active").unwrap_or(false),
        hard_locked: get_bool(&data, "hard_locked").unwrap_or(false),
        audit_event_id: get_string(&data, "audit_event_id").unwrap_or_default(),
    })
}

/// 11.01 & 11.05 — Seal untrusted input/output via `org.zethropol.ZciDaemon.v1.WrapUntrustedContent`
/// (which delegates to `org.zethropol.Security.Policy.v1.WrapUntrustedData`).
/// Strictly enforces `executable == false` and `policy_authority == "none"` fail-closed.
pub async fn seal_untrusted_via_bridge(
    connection: &zbus::Connection,
    cfg: &RuntimeConfig,
    agent_id: &str,
    source_kind: &str,
    content: &str,
) -> Result<SealedUntrustedReply, String> {
    let timeout = Duration::from_millis(cfg.policy_timeout_ms);

    let reply: HashMap<String, OwnedValue> = if cfg.require_daemon_bridge {
        let proxy = tokio::time::timeout(
            timeout,
            build_proxy(
                connection,
                &cfg.daemon_bus_name,
                &cfg.daemon_object_path,
                &cfg.daemon_interface,
            ),
        )
        .await
        .map_err(|_| "Timed out connecting to ZCI Daemon bridge.".to_string())??;

        tokio::time::timeout(
            timeout,
            proxy.call("WrapUntrustedContent", &(agent_id, source_kind, content)),
        )
        .await
        .map_err(|_| "Timed out awaiting ZCI Daemon WrapUntrustedContent.".to_string())?
        .map_err(|e| format!("ZCI Daemon WrapUntrustedContent failed: {e}"))?
    } else {
        let proxy = tokio::time::timeout(
            timeout,
            build_proxy(
                connection,
                &cfg.policy_bus_name,
                &cfg.policy_object_path,
                &cfg.policy_interface,
            ),
        )
        .await
        .map_err(|_| "Timed out connecting to Security Policy Engine.".to_string())??;

        tokio::time::timeout(
            timeout,
            proxy.call("WrapUntrustedData", &(agent_id, source_kind, content)),
        )
        .await
        .map_err(|_| "Timed out awaiting Security Policy WrapUntrustedData.".to_string())?
        .map_err(|e| format!("Security Policy WrapUntrustedData failed: {e}"))?
    };

    if !get_bool(&reply, "success").unwrap_or(false) {
        let code = get_string(&reply, "code").unwrap_or_else(|| "wrap-failed".to_string());
        let msg = get_string(&reply, "message").unwrap_or_else(|| "Untrusted sealing rejected.".to_string());
        return Err(format!("{code}: {msg}"));
    }

    let data = get_data_map(&reply)?;
    let executable = get_bool(&data, "executable").unwrap_or(true);
    let policy_authority = get_string(&data, "policy_authority").unwrap_or_default();
    let wrapped_payload = get_string(&data, "wrapped_payload")
        .ok_or_else(|| "WrapUntrusted reply missing 'wrapped_payload'.".to_string())?;

    if executable || policy_authority != "none" || wrapped_payload.is_empty() {
        return Err(
            "Fail-closed violation: sealed payload did not assert executable=false and policy_authority=none."
                .to_string(),
        );
    }

    Ok(SealedUntrustedReply {
        trust_level: get_string(&data, "trust_level")
            .unwrap_or_else(|| "untrusted-data-payload".to_string()),
        executable,
        policy_authority,
        source_kind: get_string(&data, "source_kind").unwrap_or_else(|| source_kind.to_string()),
        content_sha256: get_string(&data, "content_sha256").unwrap_or_default(),
        injection_detected: get_bool(&data, "injection_detected").unwrap_or(false),
        self_modification_injection: get_bool(&data, "self_modification_injection")
            .unwrap_or(false),
        detected_patterns: get_string_vec(&data, "detected_patterns"),
        envelope_id: get_string(&data, "envelope_id").unwrap_or_default(),
        wrapped_payload,
    })
}

/// 11.01 & 11.06 — Probe both `org.zethropol.Security.Policy.v1` and `org.zethropol.ZciDaemon.v1`.
pub async fn probe_bridge_health(
    connection: &zbus::Connection,
    cfg: &RuntimeConfig,
) -> BridgeHealthSnapshot {
    let checked_at = now_unix_secs();
    let timeout = Duration::from_millis(cfg.policy_timeout_ms.min(2_500));
    let mut snap = BridgeHealthSnapshot {
        checked_at_unix: checked_at,
        last_error: String::new(),
        ..BridgeHealthSnapshot::default()
    };

    // 1. Probe Security Policy Engine
    let pol_res: Result<(), String> = async {
        let proxy = tokio::time::timeout(
            timeout,
            build_proxy(
                connection,
                &cfg.policy_bus_name,
                &cfg.policy_object_path,
                &cfg.policy_interface,
            ),
        )
        .await
        .map_err(|_| "Policy Engine probe timed out.".to_string())??;
        let reply: HashMap<String, OwnedValue> =
            tokio::time::timeout(timeout, proxy.call("GetPolicySummary", &()))
                .await
                .map_err(|_| "GetPolicySummary timed out.".to_string())?
                .map_err(|e| format!("GetPolicySummary error: {e}"))?;
        if !get_bool(&reply, "success").unwrap_or(false) {
            return Err("GetPolicySummary returned success=false.".to_string());
        }
        let data = get_data_map(&reply)?;
        snap.policy_reachable = true;
        snap.policy_default_deny = get_bool(&data, "default_deny").unwrap_or(false);
        snap.policy_self_mod_lock = get_bool(&data, "self_modification_lock").unwrap_or(false);
        snap.policy_state_corrupted = get_bool(&data, "state_corrupted").unwrap_or(false);
        Ok(())
    }
    .await;
    if let Err(err) = pol_res {
        snap.last_error = err;
    }

    // 2. Probe ZCI Daemon bridge
    let daemon_res: Result<(), String> = async {
        let proxy = tokio::time::timeout(
            timeout,
            build_proxy(
                connection,
                &cfg.daemon_bus_name,
                &cfg.daemon_object_path,
                &cfg.daemon_interface,
            ),
        )
        .await
        .map_err(|_| "ZCI Daemon bridge probe timed out.".to_string())??;
        let reply: HashMap<String, OwnedValue> =
            tokio::time::timeout(timeout, proxy.call("GetDaemonStatus", &()))
                .await
                .map_err(|_| "GetDaemonStatus timed out.".to_string())?
                .map_err(|e| format!("GetDaemonStatus error: {e}"))?;
        if !get_bool(&reply, "success").unwrap_or(false) {
            return Err("GetDaemonStatus returned success=false.".to_string());
        }
        let data = get_data_map(&reply)?;
        snap.daemon_reachable = true;
        snap.daemon_version = get_string(&data, "daemon_version").unwrap_or_default();
        snap.daemon_fail_closed = get_bool(&data, "fail_closed").unwrap_or(false);
        Ok(())
    }
    .await;
    if let Err(err) = daemon_res {
        if snap.last_error.is_empty() {
            snap.last_error = err;
        } else {
            snap.last_error = format!("{}; {err}", snap.last_error);
        }
    }

    snap
}

fn extract_completion_text(resp_body: &str) -> String {
    if let Ok(JsonValue::Object(map)) = serde_json::from_str::<JsonValue>(resp_body) {
        if let Some(content) = map
            .get("choices")
            .and_then(|c| c.as_array())
            .and_then(|arr| arr.first())
            .and_then(|choice| choice.get("message"))
            .and_then(|msg| msg.get("content"))
            .and_then(|s| s.as_str())
        {
            return content.to_string();
        }
        if let Some(content) = map.get("content").and_then(|s| s.as_str()) {
            return content.to_string();
        }
    }
    resp_body.to_string()
}

/// 11.01 — Loopback-only (`127.0.0.1` / `::1`) local inference HTTP query.
/// Never opens external sockets and strictly enforces response size + timeout caps.
pub async fn execute_loopback_inference(
    endpoint: SocketAddr,
    model_id: &str,
    wrapped_prompt: &str,
    max_tokens: u32,
    timeout: Duration,
) -> Result<LoopbackInferenceOutcome, String> {
    // Re-validate endpoint string as defense-in-depth before any socket call.
    let validated = parse_loopback_endpoint(&endpoint.to_string())?;
    let started = Instant::now();

    let mut stream = match tokio::time::timeout(timeout, TcpStream::connect(validated)).await {
        Ok(Ok(s)) => s,
        Ok(Err(err)) if err.kind() == std::io::ErrorKind::ConnectionRefused => {
            return Ok(LoopbackInferenceOutcome::EngineSleeping {
                endpoint: validated.to_string(),
                latency_ms: started.elapsed().as_millis() as u64,
                detail: "Local loopback AI inference backend is in passive standby (not listening)."
                    .to_string(),
            });
        }
        Ok(Err(err)) => {
            return Err(format!("Loopback AI engine connection error: {err}"));
        }
        Err(_) => {
            return Err("Timed out connecting to loopback AI engine.".to_string());
        }
    };

    let body_json = json!({
        "model": model_id,
        "stream": false,
        "max_tokens": max_tokens,
        "messages": [
            { "role": "user", "content": wrapped_prompt }
        ]
    });
    let body_bytes =
        serde_json::to_vec(&body_json).map_err(|e| format!("Failed encoding inference body: {e}"))?;

    let req = format!(
        "POST /v1/chat/completions HTTP/1.1\r\nHost: {validated}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body_bytes.len()
    );

    tokio::time::timeout(timeout, async {
        stream
            .write_all(req.as_bytes())
            .await
            .map_err(|e| format!("Failed writing loopback inference request: {e}"))?;
        stream
            .write_all(&body_bytes)
            .await
            .map_err(|e| format!("Failed writing loopback inference payload: {e}"))?;
        stream
            .flush()
            .await
            .map_err(|e| format!("Failed flushing loopback inference request: {e}"))?;

        let mut raw = Vec::new();
        let mut chunk = [0u8; 4096];
        loop {
            let n = stream
                .read(&mut chunk)
                .await
                .map_err(|e| format!("Failed reading loopback inference response: {e}"))?;
            if n == 0 {
                break;
            }
            if raw.len() + n > MAX_ENGINE_RESPONSE_BYTES {
                return Err("Local AI engine response exceeded 256 KiB safety cap.".to_string());
            }
            raw.extend_from_slice(&chunk[..n]);
        }

        let text = String::from_utf8_lossy(&raw);
        let (head, resp_body) = text
            .split_once("\r\n\r\n")
            .ok_or_else(|| "Malformed HTTP response from local AI engine.".to_string())?;
        let status_code = head
            .lines()
            .next()
            .and_then(|line| line.split_whitespace().nth(1))
            .and_then(|s| s.parse::<u16>().ok())
            .unwrap_or(502);

        let raw_output = extract_completion_text(resp_body);
        Ok(LoopbackInferenceOutcome::Responded {
            status_code,
            raw_output,
            latency_ms: started.elapsed().as_millis() as u64,
        })
    })
    .await
    .map_err(|_| "Local loopback inference request timed out.".to_string())?
}
