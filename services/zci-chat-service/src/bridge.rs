use crate::config::{
    ChatConfig, CHAT_AGENT_ID, DEFAULT_FALLBACK_CONTEXT_TOKENS, DEFAULT_FALLBACK_OUTPUT_TOKENS,
};
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::collections::HashMap;
use std::time::Duration;
use zbus::proxy::CacheProperties;
use zbus::zvariant::OwnedValue;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DownstreamHealth {
    pub runtime_reachable: bool,
    pub daemon_reachable: bool,
    pub policy_reachable: bool,
    pub runtime_state: String,
    pub vram_locked: bool,
    pub active_vram_mb: u64,
    pub hardware_profile: String,
    pub active_model_id: String,
    pub max_context_tokens: u32,
    pub max_output_tokens: u32,
    pub loopback_only: bool,
    pub cpu_cores: u32,
    pub ram_total_mb: u64,
    pub ram_available_mb: u64,
    pub gpu_vram_total_mb: u64,
    pub last_error: String,
}

impl Default for DownstreamHealth {
    fn default() -> Self {
        Self {
            runtime_reachable: false,
            daemon_reachable: false,
            policy_reachable: false,
            runtime_state: "Unavailable".to_string(),
            vram_locked: false,
            active_vram_mb: 0,
            hardware_profile: "dev-baseline".to_string(),
            active_model_id: String::new(),
            max_context_tokens: DEFAULT_FALLBACK_CONTEXT_TOKENS,
            max_output_tokens: DEFAULT_FALLBACK_OUTPUT_TOKENS,
            loopback_only: true,
            cpu_cores: 0,
            ram_total_mb: 0,
            ram_available_mb: 0,
            gpu_vram_total_mb: 0,
            last_error: String::new(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct SealedUntrusted {
    pub executable: bool,
    pub policy_authority: String,
    pub envelope_id: String,
    pub wrapped_payload: String,
    pub injection_detected: bool,
    pub self_modification_injection: bool,
}

#[derive(Debug, Clone)]
pub enum RuntimeInferenceResult {
    Completed {
        model_id: String,
        output_wrapped_payload: String,
        output_envelope_id: String,
        output_executable: bool,
        output_policy_authority: String,
        output_injection_detected: bool,
        latency_ms: u64,
    },
    PassiveStandby {
        model_id: String,
        message: String,
        latency_ms: u64,
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
        .ok_or_else(|| "D-Bus reply missing 'data' map.".to_string())?;
    raw.try_clone()
        .and_then(HashMap::try_from)
        .map_err(|e| format!("Malformed D-Bus 'data' map: {e}"))
}

async fn build_proxy<'a>(
    conn: &'a zbus::Connection,
    dest: &str,
    path: &str,
    iface: &str,
) -> Result<zbus::Proxy<'a>, String> {
    zbus::proxy::Builder::new(conn)
        .destination(dest.to_string())
        .map_err(|e| format!("Invalid destination '{dest}': {e}"))?
        .path(path.to_string())
        .map_err(|e| format!("Invalid path '{path}': {e}"))?
        .interface(iface.to_string())
        .map_err(|e| format!("Invalid interface '{iface}': {e}"))?
        .cache_properties(CacheProperties::No)
        .build()
        .await
        .map_err(|e| format!("Proxy build error for '{dest}': {e}"))
}

/// Probes `zci-runtime`, `zci-daemon`, and `security` health over local Unix D-Bus.
pub async fn probe_downstream_health(
    sys_conn: &zbus::Connection,
    cfg: &ChatConfig,
) -> DownstreamHealth {
    let timeout = Duration::from_millis(cfg.dbus_timeout_ms.min(2_500));
    let mut health = DownstreamHealth::default();

    // 1. Probe org.zethropol.ZciRuntime.v1.GetRuntimeStatus
    let rt_res: Result<(), String> = async {
        let proxy = tokio::time::timeout(
            timeout,
            build_proxy(
                sys_conn,
                &cfg.runtime_bus_name,
                &cfg.runtime_object_path,
                &cfg.runtime_interface,
            ),
        )
        .await
        .map_err(|_| "zci-runtime probe timed out.".to_string())??;
        let reply: HashMap<String, OwnedValue> =
            tokio::time::timeout(timeout, proxy.call("GetRuntimeStatus", &()))
                .await
                .map_err(|_| "GetRuntimeStatus timed out.".to_string())?
                .map_err(|e| format!("GetRuntimeStatus failed: {e}"))?;
        if !get_bool(&reply, "success").unwrap_or(false) {
            return Err("GetRuntimeStatus returned success=false.".to_string());
        }
        let d = get_data_map(&reply)?;
        health.runtime_reachable = true;
        health.runtime_state =
            get_string(&d, "runtime_state").unwrap_or_else(|| "Standby".to_string());
        health.vram_locked = get_bool(&d, "vram_locked").unwrap_or(false);
        health.active_vram_mb = get_u64(&d, "active_vram_mb").unwrap_or(0);
        health.hardware_profile =
            get_string(&d, "hardware_profile").unwrap_or_else(|| "dev-baseline".to_string());
        health.active_model_id = get_string(&d, "active_model_id").unwrap_or_default();
        health.max_context_tokens =
            get_u32(&d, "max_context_tokens").unwrap_or(DEFAULT_FALLBACK_CONTEXT_TOKENS);
        health.max_output_tokens =
            get_u32(&d, "max_output_tokens").unwrap_or(DEFAULT_FALLBACK_OUTPUT_TOKENS);
        health.loopback_only = get_bool(&d, "loopback_only").unwrap_or(true);
        health.cpu_cores = get_u32(&d, "cpu_cores").unwrap_or(0);
        health.ram_total_mb = get_u64(&d, "ram_total_mb").unwrap_or(0);
        health.ram_available_mb = get_u64(&d, "ram_available_mb").unwrap_or(0);
        health.gpu_vram_total_mb = get_u64(&d, "gpu_vram_total_mb").unwrap_or(0);
        if get_bool(&d, "daemon_bridge_reachable").unwrap_or(false) {
            health.daemon_reachable = true;
        }
        if get_bool(&d, "policy_engine_reachable").unwrap_or(false) {
            health.policy_reachable = true;
        }
        Ok(())
    }
    .await;
    if let Err(err) = rt_res {
        health.last_error = err;
    }

    // 2. Directly verify ZCI Daemon reachability if not already confirmed
    if !health.daemon_reachable {
        if let Ok(Ok(proxy)) = tokio::time::timeout(
            timeout,
            build_proxy(
                sys_conn,
                &cfg.daemon_bus_name,
                &cfg.daemon_object_path,
                &cfg.daemon_interface,
            ),
        )
        .await
        {
            if let Ok(Ok(reply)) = tokio::time::timeout(
                timeout,
                proxy.call::<_, _, HashMap<String, OwnedValue>>("GetDaemonStatus", &()),
            )
            .await
            {
                if get_bool(&reply, "success").unwrap_or(false) {
                    health.daemon_reachable = true;
                    if let Ok(d) = get_data_map(&reply) {
                        if get_bool(&d, "policy_engine_reachable").unwrap_or(false) {
                            health.policy_reachable = true;
                        }
                    }
                }
            }
        }
    }

    // 3. Directly verify Security Policy Engine reachability if not already confirmed
    if !health.policy_reachable {
        if let Ok(Ok(proxy)) = tokio::time::timeout(
            timeout,
            build_proxy(
                sys_conn,
                &cfg.policy_bus_name,
                &cfg.policy_object_path,
                &cfg.policy_interface,
            ),
        )
        .await
        {
            if let Ok(Ok(reply)) = tokio::time::timeout(
                timeout,
                proxy.call::<_, _, HashMap<String, OwnedValue>>("GetPolicySummary", &()),
            )
            .await
            {
                if get_bool(&reply, "success").unwrap_or(false) {
                    health.policy_reachable = true;
                }
            }
        }
    }

    health
}

/// 12.03 — Mandatory fail-closed untrusted content sealing via `org.zethropol.ZciDaemon.v1.WrapUntrustedContent`
/// (which delegates to `org.zethropol.Security.Policy.v1.WrapUntrustedData`).
pub async fn seal_untrusted_via_daemon(
    sys_conn: &zbus::Connection,
    cfg: &ChatConfig,
    source_kind: &str,
    content: &str,
) -> Result<SealedUntrusted, String> {
    let timeout = Duration::from_millis(cfg.dbus_timeout_ms);
    let proxy = tokio::time::timeout(
        timeout,
        build_proxy(
            sys_conn,
            &cfg.daemon_bus_name,
            &cfg.daemon_object_path,
            &cfg.daemon_interface,
        ),
    )
    .await
    .map_err(|_| "Timed out connecting to ZCI Daemon for untrusted sealing.".to_string())??;

    let reply: HashMap<String, OwnedValue> = tokio::time::timeout(
        timeout,
        proxy.call("WrapUntrustedContent", &(CHAT_AGENT_ID, source_kind, content)),
    )
    .await
    .map_err(|_| "Timed out awaiting ZCI Daemon WrapUntrustedContent.".to_string())?
    .map_err(|e| format!("ZCI Daemon WrapUntrustedContent failed: {e}"))?;

    if !get_bool(&reply, "success").unwrap_or(false) {
        let code = get_string(&reply, "code").unwrap_or_else(|| "wrap-failed".to_string());
        let msg = get_string(&reply, "message").unwrap_or_default();
        return Err(format!("Fail-closed untrusted sealing denied ({code}): {msg}"));
    }

    let data = get_data_map(&reply)?;
    let executable = get_bool(&data, "executable").unwrap_or(true);
    let policy_authority = get_string(&data, "policy_authority").unwrap_or_default();
    let wrapped_payload = get_string(&data, "wrapped_payload")
        .ok_or_else(|| "WrapUntrustedContent reply missing wrapped_payload.".to_string())?;

    if executable || policy_authority != "none" || wrapped_payload.is_empty() {
        return Err(
            "Fail-closed violation: sealed payload did not guarantee executable=false & policy_authority=none."
                .to_string(),
        );
    }

    Ok(SealedUntrusted {
        executable,
        policy_authority,
        envelope_id: get_string(&data, "envelope_id").unwrap_or_default(),
        wrapped_payload,
        injection_detected: get_bool(&data, "injection_detected").unwrap_or(false),
        self_modification_injection: get_bool(&data, "self_modification_injection")
            .unwrap_or(false),
    })
}

/// 12.02 — Opt-in ReadOnly System Status & Context Collector.
/// Uses ONLY authorized ReadOnly D-Bus status interfaces (zero shell subprocesses) and seals the
/// assembled context via `WrapUntrustedContent` (`source_kind="system_context"`).
pub async fn collect_readonly_system_context(
    sys_conn: &zbus::Connection,
    cfg: &ChatConfig,
    health: &DownstreamHealth,
) -> Result<SealedUntrusted, String> {
    let timeout = Duration::from_millis(cfg.dbus_timeout_ms.min(2_500));

    let mut recent_events: Vec<String> = Vec::new();
    let mut policy_events_total: u64 = 0;
    if let Ok(Ok(proxy)) = tokio::time::timeout(
        timeout,
        build_proxy(
            sys_conn,
            &cfg.daemon_bus_name,
            &cfg.daemon_object_path,
            &cfg.daemon_interface,
        ),
    )
    .await
    {
        if let Ok(Ok(reply)) = tokio::time::timeout(
            timeout,
            proxy.call::<_, _, HashMap<String, OwnedValue>>("GetDaemonStatus", &()),
        )
        .await
        {
            if let Ok(d) = get_data_map(&reply) {
                policy_events_total = get_u64(&d, "policy_total_audit_events").unwrap_or(0);
                recent_events = get_string_vec(&d, "recent_events_json")
                    .into_iter()
                    .take(5)
                    .map(|raw| {
                        if let Ok(val) = serde_json::from_str::<serde_json::Value>(&raw) {
                            let kind = val
                                .get("event")
                                .or_else(|| val.get("action"))
                                .and_then(|v| v.as_str())
                                .unwrap_or("daemon-event");
                            let decision = val
                                .get("decision")
                                .or_else(|| val.get("outcome"))
                                .and_then(|v| v.as_str())
                                .unwrap_or("recorded");
                            format!("{kind}:{decision}")
                        } else {
                            "daemon-event:recorded".to_string()
                        }
                    })
                    .collect();
            }
        }
    }

    let summary = json!({
        "scope": "readonly-system-context",
        "hardware_profile": health.hardware_profile,
        "cpu_cores": health.cpu_cores,
        "ram_available_mb": health.ram_available_mb,
        "ram_total_mb": health.ram_total_mb,
        "gpu_vram_total_mb": health.gpu_vram_total_mb,
        "runtime_state": health.runtime_state,
        "vram_locked": health.vram_locked,
        "active_vram_mb": health.active_vram_mb,
        "active_model_id": health.active_model_id,
        "loopback_only": health.loopback_only,
        "policy_engine_reachable": health.policy_reachable,
        "daemon_reachable": health.daemon_reachable,
        "policy_audit_events_total": policy_events_total,
        "recent_daemon_events": recent_events,
    });
    let summary_text = serde_json::to_string(&summary)
        .map_err(|e| format!("Failed serializing readonly system context: {e}"))?;

    seal_untrusted_via_daemon(sys_conn, cfg, "system_context", &summary_text).await
}

/// 12.01 & 12.03 — Invoke `org.zethropol.ZciRuntime.v1.RunInference` with strict output isolation check.
pub async fn invoke_runtime_inference(
    sys_conn: &zbus::Connection,
    cfg: &ChatConfig,
    model_id: &str,
    assembled_prompt: &str,
    max_tokens: u32,
) -> Result<RuntimeInferenceResult, (String, String)> {
    let timeout = Duration::from_millis(cfg.inference_timeout_ms);
    let proxy = tokio::time::timeout(
        Duration::from_millis(cfg.dbus_timeout_ms),
        build_proxy(
            sys_conn,
            &cfg.runtime_bus_name,
            &cfg.runtime_object_path,
            &cfg.runtime_interface,
        ),
    )
    .await
    .map_err(|_| {
        (
            "zci-runtime-unavailable".to_string(),
            "Timed out connecting to ZCI Runtime.".to_string(),
        )
    })?
    .map_err(|e| ("zci-runtime-unavailable".to_string(), e))?;

    let reply: HashMap<String, OwnedValue> = tokio::time::timeout(
        timeout,
        proxy.call(
            "RunInference",
            &(CHAT_AGENT_ID, model_id, assembled_prompt, max_tokens),
        ),
    )
    .await
    .map_err(|_| {
        (
            "inference-timeout".to_string(),
            "Timed out awaiting local ZCI Runtime inference response.".to_string(),
        )
    })?
    .map_err(|e| {
        (
            "zci-runtime-unavailable".to_string(),
            format!("ZCI Runtime RunInference call failed: {e}"),
        )
    })?;

    let success = get_bool(&reply, "success").unwrap_or(false);
    let code = get_string(&reply, "code").unwrap_or_else(|| "runtime-error".to_string());
    let message = get_string(&reply, "message").unwrap_or_default();
    let data = get_data_map(&reply).unwrap_or_default();

    if !success {
        return Err((code, message));
    }

    if code == "passive-standby" {
        return Ok(RuntimeInferenceResult::PassiveStandby {
            model_id: get_string(&data, "model_id").unwrap_or_else(|| model_id.to_string()),
            message,
            latency_ms: get_u64(&data, "latency_ms").unwrap_or(0),
        });
    }

    let out_exec = get_bool(&data, "output_executable").unwrap_or(true);
    let out_auth = get_string(&data, "output_policy_authority").unwrap_or_default();
    let out_wrapped = get_string(&data, "output_wrapped_payload").unwrap_or_default();
    if out_exec || out_auth != "none" || out_wrapped.is_empty() {
        return Err((
            "output-isolation-violation".to_string(),
            "Fail-closed drop: ZCI Runtime output violated non-executable isolation contract."
                .to_string(),
        ));
    }

    Ok(RuntimeInferenceResult::Completed {
        model_id: get_string(&data, "model_id").unwrap_or_else(|| model_id.to_string()),
        output_wrapped_payload: out_wrapped,
        output_envelope_id: get_string(&data, "output_envelope_id").unwrap_or_default(),
        output_executable: out_exec,
        output_policy_authority: out_auth,
        output_injection_detected: get_bool(&data, "output_injection_detected").unwrap_or(false),
        latency_ms: get_u64(&data, "latency_ms").unwrap_or(0),
    })
}
