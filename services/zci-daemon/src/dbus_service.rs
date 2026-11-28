use crate::actions::{
    execute_bounded_file_read, execute_bounded_project_analysis, execute_loopback_model_query,
    execute_readonly_dbus_diagnostic, lookup_readonly_diagnostic_target, ModelQueryOutcome,
};
use crate::config::{
    is_supported_agent, supported_agent_list, DaemonConfig, DAEMON_BUS_NAME, DAEMON_VERSION,
    POLKIT_ACTION_DAEMON_ADMIN,
};
use crate::policy_client::{
    evaluate_action_remote, get_agent_status_remote, is_gateway_permitted_action_class,
    probe_policy_health, wrap_untrusted_remote,
};
use crate::sessions::{SessionRegistry, SessionState, SlotAcquireError};
use std::collections::HashMap;
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use uuid::Uuid;
use zbus::interface;
use zbus::object_server::SignalEmitter;
use zbus::proxy::CacheProperties;
use zbus::zvariant::{Array, OwnedValue, Signature, Str, Value};

fn vec_to_owned(items: &[String]) -> OwnedValue {
    let mut array = Array::new(&Signature::Str);
    for item in items {
        let _ = array.append(Value::from(item.as_str()));
    }
    OwnedValue::try_from(Value::Array(array))
        .expect("string array should encode to OwnedValue")
}

fn envelope(
    success: bool,
    code: &str,
    message: &str,
    operation_id: &str,
    data: HashMap<String, OwnedValue>,
) -> HashMap<String, OwnedValue> {
    let mut out = HashMap::new();
    out.insert("success".to_string(), OwnedValue::from(success));
    out.insert("code".to_string(), OwnedValue::from(Str::from(code)));
    out.insert("message".to_string(), OwnedValue::from(Str::from(message)));
    out.insert(
        "operationId".to_string(),
        OwnedValue::from(Str::from(operation_id)),
    );
    out.insert("data".to_string(), OwnedValue::from(data));
    out
}

fn has_control_or_nul(s: &str) -> bool {
    s.chars().any(|c| c == '\0' || c.is_control())
}

async fn resolve_caller_uid(connection: &zbus::Connection, sender: &str) -> u32 {
    if sender.is_empty() {
        return u32::MAX;
    }
    let proxy: Result<zbus::Proxy<'_>, _> = zbus::proxy::Builder::new(connection)
        .destination("org.freedesktop.DBus")
        .and_then(|b| b.path("/org/freedesktop/DBus"))
        .and_then(|b| b.interface("org.freedesktop.DBus"))
        .map(|b| b.cache_properties(CacheProperties::No))
        .expect("static org.freedesktop.DBus proxy builder")
        .build()
        .await;
    if let Ok(proxy) = proxy {
        if let Ok(uid) = proxy
            .call::<_, _, u32>("GetConnectionUnixUser", &(sender,))
            .await
        {
            return uid;
        }
    }
    u32::MAX
}

async fn check_polkit_admin(pkcheck_bin: std::path::PathBuf, sender: String) -> bool {
    tokio::task::spawn_blocking(move || {
        Command::new(&pkcheck_bin)
            .args([
                "--action-id",
                POLKIT_ACTION_DAEMON_ADMIN,
                "--system-bus-name",
                &sender,
                "--allow-user-interaction",
            ])
            .status()
            .map(|status| status.success())
            .unwrap_or(false)
    })
    .await
    .unwrap_or(false)
}

pub struct ZciDaemonService {
    registry: Arc<Mutex<SessionRegistry>>,
}

impl ZciDaemonService {
    pub fn with_config(config: DaemonConfig) -> Self {
        Self {
            registry: Arc::new(Mutex::new(SessionRegistry::new(config))),
        }
    }

    fn snapshot_config(&self) -> Result<DaemonConfig, zbus::fdo::Error> {
        self.registry
            .lock()
            .map(|g| g.config.clone())
            .map_err(|_| zbus::fdo::Error::Failed("Daemon registry lock poisoned".to_string()))
    }

    async fn build_daemon_status(
        &self,
        connection: &zbus::Connection,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let cfg = self.snapshot_config()?;
        let health = probe_policy_health(connection, &cfg).await;

        let (
            sessions,
            active_global,
            metrics,
            passive_model,
            recent_events_json,
            cfg_snapshot,
        ) = {
            let mut guard = self
                .registry
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Daemon registry lock poisoned".to_string()))?;
            guard.last_policy_health = health.clone();
            let sessions = guard.list_sessions();
            let active_global = guard.active_requests_global();
            let metrics = guard.metrics.clone();
            let passive_model = guard.passive_model_summary();
            let recent_events_json = guard.recent_events_json();
            let cfg_snapshot = guard.config.clone();
            (
                sessions,
                active_global,
                metrics,
                passive_model,
                recent_events_json,
                cfg_snapshot,
            )
        };

        let total_sessions = sessions.len() as u64;
        let busy_sessions = sessions
            .iter()
            .filter(|s| s.state == SessionState::Busy)
            .count() as u64;
        let standby_sessions = sessions
            .iter()
            .filter(|s| s.state == SessionState::Standby)
            .count() as u64;
        let locked_out_sessions = sessions
            .iter()
            .filter(|s| s.state == SessionState::LockedOut)
            .count() as u64;
        let active_sessions = sessions
            .iter()
            .filter(|s| s.state != SessionState::LockedOut)
            .count() as u64;

        let allowed_classes = vec!["ReadOnly".to_string(), "UserSpaceSafe".to_string()];

        let mut data = HashMap::new();
        data.insert(
            "daemon_version".to_string(),
            OwnedValue::from(Str::from(DAEMON_VERSION)),
        );
        data.insert(
            "service_name".to_string(),
            OwnedValue::from(Str::from(DAEMON_BUS_NAME)),
        );
        data.insert("fail_closed".to_string(), OwnedValue::from(true));
        data.insert(
            "allowed_action_classes".to_string(),
            vec_to_owned(&allowed_classes),
        );
        data.insert("recovery_locks_protected".to_string(), OwnedValue::from(true));
        data.insert(
            "supported_agents".to_string(),
            vec_to_owned(&supported_agent_list()),
        );
        data.insert(
            "admin_polkit_action".to_string(),
            OwnedValue::from(Str::from(POLKIT_ACTION_DAEMON_ADMIN)),
        );

        // Session & concurrency telemetry
        data.insert(
            "total_session_count".to_string(),
            OwnedValue::from(total_sessions),
        );
        data.insert(
            "active_session_count".to_string(),
            OwnedValue::from(active_sessions),
        );
        data.insert(
            "busy_session_count".to_string(),
            OwnedValue::from(busy_sessions),
        );
        data.insert(
            "standby_session_count".to_string(),
            OwnedValue::from(standby_sessions),
        );
        data.insert(
            "locked_out_session_count".to_string(),
            OwnedValue::from(locked_out_sessions),
        );
        data.insert(
            "active_requests_global".to_string(),
            OwnedValue::from(active_global),
        );
        data.insert(
            "max_concurrent_global".to_string(),
            OwnedValue::from(cfg_snapshot.max_concurrent_global),
        );
        data.insert(
            "max_concurrent_per_agent".to_string(),
            OwnedValue::from(cfg_snapshot.max_concurrent_per_agent),
        );
        data.insert(
            "rate_limit_max_requests".to_string(),
            OwnedValue::from(cfg_snapshot.rate_limit_max_requests),
        );
        data.insert(
            "rate_limit_window_secs".to_string(),
            OwnedValue::from(cfg_snapshot.rate_limit_window_secs),
        );
        data.insert(
            "action_timeout_ms".to_string(),
            OwnedValue::from(cfg_snapshot.action_timeout_ms),
        );

        // Policy Engine connectivity health (10.06)
        data.insert(
            "policy_engine_reachable".to_string(),
            OwnedValue::from(health.reachable),
        );
        data.insert(
            "policy_engine_version".to_string(),
            OwnedValue::from(Str::from(health.engine_version.as_str())),
        );
        data.insert(
            "policy_default_deny".to_string(),
            OwnedValue::from(health.default_deny),
        );
        data.insert(
            "policy_self_mod_lock".to_string(),
            OwnedValue::from(health.self_modification_lock),
        );
        data.insert(
            "policy_state_corrupted".to_string(),
            OwnedValue::from(health.state_corrupted),
        );
        data.insert(
            "policy_locked_agents".to_string(),
            OwnedValue::from(health.locked_agents),
        );
        data.insert(
            "policy_total_audit_events".to_string(),
            OwnedValue::from(health.total_audit_events),
        );
        data.insert(
            "policy_last_error".to_string(),
            OwnedValue::from(Str::from(health.last_error.as_str())),
        );

        // Passive standby Gemma 4B model telemetry (10.04 & 10.06)
        data.insert(
            "passive_model_id".to_string(),
            OwnedValue::from(Str::from(passive_model.model_id.as_str())),
        );
        data.insert(
            "passive_model_profile".to_string(),
            OwnedValue::from(Str::from(passive_model.profile.as_str())),
        );
        data.insert(
            "passive_model_state".to_string(),
            OwnedValue::from(Str::from(passive_model.state.as_str())),
        );
        data.insert(
            "passive_model_vram_policy".to_string(),
            OwnedValue::from(Str::from(passive_model.vram_allocation_policy.as_str())),
        );
        data.insert(
            "passive_model_estimated_vram_mb".to_string(),
            OwnedValue::from(passive_model.estimated_vram_mb),
        );
        data.insert(
            "passive_model_active_requests".to_string(),
            OwnedValue::from(passive_model.active_model_requests),
        );
        data.insert(
            "passive_model_total_queries".to_string(),
            OwnedValue::from(passive_model.total_model_queries),
        );
        data.insert(
            "passive_model_lease_present".to_string(),
            OwnedValue::from(passive_model.lease_file_present),
        );
        data.insert(
            "system_mem_available_mb".to_string(),
            OwnedValue::from(passive_model.system_mem_available_mb),
        );
        data.insert(
            "system_mem_total_mb".to_string(),
            OwnedValue::from(passive_model.system_mem_total_mb),
        );

        // Action & security statistics (10.06)
        data.insert(
            "total_actions_requested".to_string(),
            OwnedValue::from(metrics.total_actions_requested),
        );
        data.insert(
            "approved_actions".to_string(),
            OwnedValue::from(metrics.approved_actions),
        );
        data.insert(
            "denied_actions".to_string(),
            OwnedValue::from(metrics.denied_actions),
        );
        data.insert(
            "fail_closed_denials".to_string(),
            OwnedValue::from(metrics.fail_closed_denials),
        );
        data.insert(
            "rate_limited_denials".to_string(),
            OwnedValue::from(metrics.rate_limited_denials),
        );
        data.insert(
            "concurrency_denials".to_string(),
            OwnedValue::from(metrics.concurrency_denials),
        );
        data.insert(
            "timeout_denials".to_string(),
            OwnedValue::from(metrics.timeout_denials),
        );
        data.insert(
            "wrapped_payloads_total".to_string(),
            OwnedValue::from(metrics.wrapped_payloads_total),
        );
        data.insert(
            "injections_flagged_total".to_string(),
            OwnedValue::from(metrics.injections_flagged_total),
        );
        data.insert(
            "config_reload_count".to_string(),
            OwnedValue::from(metrics.config_reload_count),
        );
        data.insert(
            "recent_events_json".to_string(),
            vec_to_owned(&recent_events_json),
        );

        let status_code = if health.reachable && !health.state_corrupted {
            "ok"
        } else {
            "degraded-policy-unreachable"
        };
        let msg = if health.reachable && !health.state_corrupted {
            "ZCI Daemon healthy; Policy Engine connected and fail-closed gateway active."
        } else {
            "ZCI Daemon active in fail-closed degraded mode: Security Policy Engine unreachable or corrupted."
        };

        Ok(envelope(true, status_code, msg, &operation_id, data))
    }
}

#[interface(name = "org.zethropol.ZciDaemon.v1")]
impl ZciDaemonService {
    /// 10.04 — Register a new agent session (`zci-chat`, `zekans-code`, `zci-command`, `zci-vision`).
    async fn open_session(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        agent_id: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        if has_control_or_nul(&agent_id) || !is_supported_agent(&agent_id) {
            return Ok(envelope(
                false,
                "unknown-agent",
                "Unknown or invalid ZCI agent identity denied.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let sender = header
            .sender()
            .map(|s| s.to_string())
            .unwrap_or_default();
        let caller_uid = resolve_caller_uid(connection, &sender).await;
        let cfg = self.snapshot_config()?;

        // Check Policy Engine lockout posture on session open (fail-closed if locked out;
        // allow registering session in Idle if Policy Engine is temporarily offline so health/audit can track it,
        // while ExecuteAction remains strictly fail-closed!).
        let (locked_out, hard_locked, cooldown_secs, strike_score) =
            match get_agent_status_remote(connection, &cfg, &agent_id).await {
                Ok(st) => {
                    if !st.known_agent {
                        return Ok(envelope(
                            false,
                            "unknown-agent",
                            "Security Policy Engine does not recognize this ZCI agent identity.",
                            &operation_id,
                            HashMap::new(),
                        ));
                    }
                    (
                        st.lockout_active,
                        st.hard_locked,
                        st.cooldown_remaining_secs,
                        st.strike_score,
                    )
                }
                Err(_) => (false, false, 0, 0),
            };

        let created = {
            let mut guard = self
                .registry
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Daemon registry lock poisoned".to_string()))?;
            guard.open_session(
                &agent_id,
                caller_uid,
                &sender,
                locked_out,
                hard_locked,
                cooldown_secs,
            )
        };

        match created {
            Ok(sess) => {
                let _ = Self::session_state_changed(
                    &emitter,
                    sess.session_id.clone(),
                    sess.agent_id.clone(),
                    sess.state.as_str().to_string(),
                    "session-opened".to_string(),
                )
                .await;

                let mut data = HashMap::new();
                data.insert(
                    "session_id".to_string(),
                    OwnedValue::from(Str::from(sess.session_id.as_str())),
                );
                data.insert(
                    "agent_id".to_string(),
                    OwnedValue::from(Str::from(sess.agent_id.as_str())),
                );
                data.insert(
                    "state".to_string(),
                    OwnedValue::from(Str::from(sess.state.as_str())),
                );
                data.insert(
                    "hard_locked".to_string(),
                    OwnedValue::from(sess.hard_locked),
                );
                data.insert(
                    "lockout_cooldown_secs".to_string(),
                    OwnedValue::from(sess.lockout_cooldown_secs),
                );
                data.insert("policy_strike_score".to_string(), OwnedValue::from(strike_score));

                if sess.state == SessionState::LockedOut {
                    Ok(envelope(
                        false,
                        "agent-locked-out",
                        "Session recorded in LockedOut state due to active Security Policy Engine lockout.",
                        &operation_id,
                        data,
                    ))
                } else {
                    Ok(envelope(
                        true,
                        "ok",
                        "ZCI agent session opened.",
                        &operation_id,
                        data,
                    ))
                }
            }
            Err(err) => Ok(envelope(
                false,
                "session-limit-reached",
                &err,
                &operation_id,
                HashMap::new(),
            )),
        }
    }

    /// 10.04 — Explicitly place an idle session into passive Standby.
    async fn set_session_standby(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        session_id: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let sender = header
            .sender()
            .map(|s| s.to_string())
            .unwrap_or_default();
        let caller_uid = resolve_caller_uid(connection, &sender).await;

        let res = {
            let mut guard = self
                .registry
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Daemon registry lock poisoned".to_string()))?;
            guard.set_session_standby(&session_id, caller_uid)
        };

        match res {
            Ok(sess) => {
                let _ = Self::session_state_changed(
                    &emitter,
                    sess.session_id.clone(),
                    sess.agent_id.clone(),
                    sess.state.as_str().to_string(),
                    "passive-standby-entered".to_string(),
                )
                .await;
                let mut data = HashMap::new();
                data.insert(
                    "session_id".to_string(),
                    OwnedValue::from(Str::from(sess.session_id.as_str())),
                );
                data.insert(
                    "state".to_string(),
                    OwnedValue::from(Str::from(sess.state.as_str())),
                );
                Ok(envelope(
                    true,
                    "ok",
                    "Session transitioned to passive Standby.",
                    &operation_id,
                    data,
                ))
            }
            Err((code, msg)) => Ok(envelope(false, code, &msg, &operation_id, HashMap::new())),
        }
    }

    /// 10.04 — Close caller's own non-busy agent session.
    async fn close_session(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        session_id: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let sender = header
            .sender()
            .map(|s| s.to_string())
            .unwrap_or_default();
        let caller_uid = resolve_caller_uid(connection, &sender).await;

        let res = {
            let mut guard = self
                .registry
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Daemon registry lock poisoned".to_string()))?;
            guard.close_session(&session_id, caller_uid)
        };

        match res {
            Ok(sess) => {
                let _ = Self::session_state_changed(
                    &emitter,
                    sess.session_id.clone(),
                    sess.agent_id.clone(),
                    "Closed".to_string(),
                    "session-closed-by-owner".to_string(),
                )
                .await;
                let mut data = HashMap::new();
                data.insert(
                    "session_id".to_string(),
                    OwnedValue::from(Str::from(sess.session_id.as_str())),
                );
                Ok(envelope(
                    true,
                    "ok",
                    "ZCI agent session closed.",
                    &operation_id,
                    data,
                ))
            }
            Err((code, msg)) => Ok(envelope(false, code, &msg, &operation_id, HashMap::new())),
        }
    }

    /// 10.03 — Seal external file, command output, or user text strictly via Policy Engine `WrapUntrustedData`.
    /// Fail-closed: never returns unwrapped content if Security Policy Engine is unreachable.
    async fn wrap_untrusted_content(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
        agent_id: String,
        source_kind: String,
        content: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        if has_control_or_nul(&agent_id) || has_control_or_nul(&source_kind) {
            return Ok(envelope(
                false,
                "invalid-request",
                "Control characters or NUL bytes are forbidden in agent_id/source_kind.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let cfg = self.snapshot_config()?;
        match wrap_untrusted_remote(connection, &cfg, &agent_id, &source_kind, &content).await {
            Ok(wrapped) => {
                if let Ok(mut guard) = self.registry.lock() {
                    guard.record_wrapped_payload(wrapped.injection_detected);
                }
                let mut data = HashMap::new();
                data.insert(
                    "trust_level".to_string(),
                    OwnedValue::from(Str::from(wrapped.trust_level.as_str())),
                );
                data.insert("executable".to_string(), OwnedValue::from(wrapped.executable));
                data.insert(
                    "policy_authority".to_string(),
                    OwnedValue::from(Str::from(wrapped.policy_authority.as_str())),
                );
                data.insert(
                    "source_kind".to_string(),
                    OwnedValue::from(Str::from(wrapped.source_kind.as_str())),
                );
                data.insert(
                    "content_sha256".to_string(),
                    OwnedValue::from(Str::from(wrapped.content_sha256.as_str())),
                );
                data.insert("byte_length".to_string(), OwnedValue::from(wrapped.byte_length));
                data.insert("oversized".to_string(), OwnedValue::from(wrapped.oversized));
                data.insert(
                    "injection_detected".to_string(),
                    OwnedValue::from(wrapped.injection_detected),
                );
                data.insert(
                    "self_modification_injection".to_string(),
                    OwnedValue::from(wrapped.self_modification_injection),
                );
                data.insert(
                    "detected_patterns".to_string(),
                    vec_to_owned(&wrapped.detected_patterns),
                );
                data.insert(
                    "envelope_id".to_string(),
                    OwnedValue::from(Str::from(wrapped.envelope_id.as_str())),
                );
                data.insert(
                    "wrapped_payload".to_string(),
                    OwnedValue::from(Str::from(wrapped.wrapped_payload.as_str())),
                );

                Ok(envelope(
                    true,
                    &wrapped.code,
                    if wrapped.injection_detected {
                        "Untrusted content sealed as non-executable data; injection patterns flagged."
                    } else {
                        "Untrusted content sealed as non-executable data."
                    },
                    &operation_id,
                    data,
                ))
            }
            Err(err) => {
                if let Ok(mut guard) = self.registry.lock() {
                    guard.metrics.denied_actions += 1;
                    guard.metrics.fail_closed_denials += 1;
                }
                Ok(envelope(
                    false,
                    "policy-engine-unavailable",
                    &format!("Fail-closed denial: {err}"),
                    &operation_id,
                    HashMap::new(),
                ))
            }
        }
    }

    /// 10.02 & 10.03 — Policy-Gated Action Gateway.
    /// Every agent request (`read_file`, `project_analysis`, `model_query`, `system_diagnostics`,
    /// or bounded `userspace_preflight`) MUST pass `org.zethropol.Security.Policy.v1.EvaluateAction`
    /// and be classified strictly as `ReadOnly` or `UserSpaceSafe` before execution.
    #[allow(clippy::too_many_arguments)]
    async fn execute_action(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        session_id: String,
        agent_id: String,
        action_kind: String,
        operation: String,
        target: String,
        dbus_interface: String,
        payload: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();

        // 1. Basic syntax & control character check.
        if has_control_or_nul(&session_id)
            || has_control_or_nul(&agent_id)
            || has_control_or_nul(&action_kind)
            || has_control_or_nul(&operation)
            || has_control_or_nul(&target)
            || has_control_or_nul(&dbus_interface)
            || action_kind.trim().is_empty()
            || operation.trim().is_empty()
        {
            return Ok(envelope(
                false,
                "invalid-request",
                "Control characters, NUL bytes, or empty action_kind/operation rejected.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let sender = header
            .sender()
            .map(|s| s.to_string())
            .unwrap_or_default();
        let caller_uid = resolve_caller_uid(connection, &sender).await;
        let is_model_query = action_kind == "model_query";

        // 2. Acquire session concurrency & rate-limit slot (RAII guard releases on every return path!).
        let _slot_guard = match SessionRegistry::try_acquire_slot(
            &self.registry,
            &session_id,
            &agent_id,
            caller_uid,
            is_model_query,
        ) {
            Ok(g) => g,
            Err(SlotAcquireError::SessionNotFound) => {
                return Ok(envelope(
                    false,
                    "session-not-found",
                    "Valid open ZCI session is required before executing actions.",
                    &operation_id,
                    HashMap::new(),
                ));
            }
            Err(SlotAcquireError::SessionAgentMismatch) => {
                return Ok(envelope(
                    false,
                    "session-agent-mismatch",
                    "Requested agent_id does not match the registered session identity.",
                    &operation_id,
                    HashMap::new(),
                ));
            }
            Err(SlotAcquireError::SessionOwnerMismatch) => {
                return Ok(envelope(
                    false,
                    "permission-denied",
                    "Caller UID does not own this ZCI session.",
                    &operation_id,
                    HashMap::new(),
                ));
            }
            Err(SlotAcquireError::RateLimited { limit, window_secs }) => {
                let _ = Self::action_audited(
                    &emitter,
                    agent_id,
                    action_kind,
                    operation,
                    "ReadOnly".to_string(),
                    "deny".to_string(),
                    "rate-limited".to_string(),
                    "DAEMON_RATE_LIMIT".to_string(),
                )
                .await;
                return Ok(envelope(
                    false,
                    "rate-limited",
                    &format!("Agent exceeded gateway rate limit ({limit} requests / {window_secs}s)."),
                    &operation_id,
                    HashMap::new(),
                ));
            }
            Err(SlotAcquireError::ConcurrencyLimit { scope, limit }) => {
                let _ = Self::action_audited(
                    &emitter,
                    agent_id,
                    action_kind,
                    operation,
                    "ReadOnly".to_string(),
                    "deny".to_string(),
                    "busy-concurrency-limit".to_string(),
                    "DAEMON_CONCURRENCY_LIMIT".to_string(),
                )
                .await;
                return Ok(envelope(
                    false,
                    "busy-concurrency-limit",
                    &format!("Gateway {scope} concurrency limit ({limit}) reached; retry later."),
                    &operation_id,
                    HashMap::new(),
                ));
            }
        };

        let cfg = self.snapshot_config()?;

        // 3. Map gateway action_kind to Policy Engine evaluation parameters cleanly without weakening checks.
        let (policy_op, policy_target, policy_iface) = match action_kind.as_str() {
            "read_file" => (operation.as_str(), target.as_str(), ""),
            "project_analysis" => ("list_dir", target.as_str(), ""),
            "model_query" => ("inspect_untrusted", "", ""),
            "system_diagnostics" => ("dbus_call", target.as_str(), dbus_interface.as_str()),
            _ => (operation.as_str(), target.as_str(), dbus_interface.as_str()),
        };

        // 4. 10.02 MANDATORY Policy Engine Evaluation (STRICT FAIL-CLOSED).
        let eval = match evaluate_action_remote(
            connection,
            &cfg,
            &agent_id,
            policy_op,
            policy_target,
            policy_iface,
            &payload,
        )
        .await
        {
            Ok(reply) => reply,
            Err(err) => {
                if let Ok(mut guard) = self.registry.lock() {
                    guard.last_policy_health.reachable = false;
                    guard.last_policy_health.last_error = err.clone();
                    guard.record_action_outcome(
                        &session_id,
                        &agent_id,
                        &action_kind,
                        &operation,
                        "SystemStateChange",
                        false,
                        "policy-engine-unavailable",
                        "DAEMON_FAIL_CLOSED_POLICY_UNREACHABLE",
                        "",
                        true,
                        false,
                    );
                }
                let _ = Self::action_audited(
                    &emitter,
                    agent_id,
                    action_kind,
                    operation,
                    "SystemStateChange".to_string(),
                    "deny".to_string(),
                    "policy-engine-unavailable".to_string(),
                    "DAEMON_FAIL_CLOSED_POLICY_UNREACHABLE".to_string(),
                )
                .await;
                return Ok(envelope(
                    false,
                    "policy-engine-unavailable",
                    &format!("Fail-closed denial: Security Policy Engine unreachable ({err})."),
                    &operation_id,
                    HashMap::new(),
                ));
            }
        };

        // Sync lockout posture from Policy Engine into session registry (10.04).
        if let Ok(mut guard) = self.registry.lock() {
            guard.update_agent_lockout(
                &agent_id,
                eval.lockout_active,
                eval.hard_locked,
                eval.cooldown_remaining_secs,
            );
        }
        if eval.lockout_active || eval.hard_locked {
            let _ = Self::session_state_changed(
                &emitter,
                session_id.clone(),
                agent_id.clone(),
                SessionState::LockedOut.as_str().to_string(),
                eval.rule_id.clone(),
            )
            .await;
        }

        // 5. Reject if Policy Engine denied the action.
        if !eval.allowed {
            if let Ok(mut guard) = self.registry.lock() {
                guard.record_action_outcome(
                    &session_id,
                    &agent_id,
                    &action_kind,
                    &operation,
                    &eval.action_class,
                    false,
                    &eval.code,
                    &eval.rule_id,
                    &eval.audit_event_id,
                    false,
                    false,
                );
            }
            let _ = Self::action_audited(
                &emitter,
                agent_id,
                action_kind,
                operation,
                eval.action_class.clone(),
                "deny".to_string(),
                eval.code.clone(),
                eval.rule_id.clone(),
            )
            .await;

            let mut data = HashMap::new();
            data.insert(
                "action_class".to_string(),
                OwnedValue::from(Str::from(eval.action_class.as_str())),
            );
            data.insert(
                "rule_id".to_string(),
                OwnedValue::from(Str::from(eval.rule_id.as_str())),
            );
            data.insert(
                "decision".to_string(),
                OwnedValue::from(Str::from(eval.decision.as_str())),
            );
            data.insert(
                "requires_polkit".to_string(),
                OwnedValue::from(eval.requires_polkit),
            );
            data.insert(
                "polkit_action".to_string(),
                OwnedValue::from(Str::from(eval.polkit_action.as_str())),
            );
            data.insert(
                "lockout_active".to_string(),
                OwnedValue::from(eval.lockout_active),
            );
            data.insert(
                "hard_locked".to_string(),
                OwnedValue::from(eval.hard_locked),
            );
            data.insert(
                "penalty_score".to_string(),
                OwnedValue::from(eval.penalty_score),
            );
            data.insert(
                "cooldown_remaining_secs".to_string(),
                OwnedValue::from(eval.cooldown_remaining_secs),
            );
            data.insert(
                "untrusted_flags".to_string(),
                vec_to_owned(&eval.untrusted_flags),
            );
            data.insert(
                "policy_audit_event_id".to_string(),
                OwnedValue::from(Str::from(eval.audit_event_id.as_str())),
            );

            return Ok(envelope(false, &eval.code, &eval.reason, &operation_id, data));
        }

        // 6. 10.02 DEFENSE-IN-DEPTH ACTION CLASS GATE:
        // Strictly reject anything outside `ReadOnly` and `UserSpaceSafe` EVEN IF upstream allowed it!
        if !is_gateway_permitted_action_class(&eval.action_class) {
            if let Ok(mut guard) = self.registry.lock() {
                guard.record_action_outcome(
                    &session_id,
                    &agent_id,
                    &action_kind,
                    &operation,
                    &eval.action_class,
                    false,
                    "gateway-action-class-denied",
                    "DAEMON_CLASS_GATE_ONLY_READONLY_OR_USERSPACE_SAFE",
                    &eval.audit_event_id,
                    true,
                    false,
                );
            }
            let _ = Self::action_audited(
                &emitter,
                agent_id,
                action_kind,
                operation,
                eval.action_class.clone(),
                "deny".to_string(),
                "gateway-action-class-denied".to_string(),
                "DAEMON_CLASS_GATE_ONLY_READONLY_OR_USERSPACE_SAFE".to_string(),
            )
            .await;

            let mut data = HashMap::new();
            data.insert(
                "action_class".to_string(),
                OwnedValue::from(Str::from(eval.action_class.as_str())),
            );
            data.insert(
                "rule_id".to_string(),
                OwnedValue::from(Str::from("DAEMON_CLASS_GATE_ONLY_READONLY_OR_USERSPACE_SAFE")),
            );
            data.insert(
                "policy_audit_event_id".to_string(),
                OwnedValue::from(Str::from(eval.audit_event_id.as_str())),
            );
            return Ok(envelope(
                false,
                "gateway-action-class-denied",
                "zci-daemon refuses all actions outside ReadOnly and UserSpaceSafe (fail-closed).",
                &operation_id,
                data,
            ));
        }

        // 7. Execute bounded gateway action with strict timeout and mandatory 10.03 WrapUntrustedData sealing.
        let action_timeout = Duration::from_millis(cfg.action_timeout_ms);
        let mut data = HashMap::new();
        data.insert(
            "action_class".to_string(),
            OwnedValue::from(Str::from(eval.action_class.as_str())),
        );
        data.insert(
            "policy_rule_id".to_string(),
            OwnedValue::from(Str::from(eval.rule_id.as_str())),
        );
        data.insert(
            "policy_audit_event_id".to_string(),
            OwnedValue::from(Str::from(eval.audit_event_id.as_str())),
        );

        match action_kind.as_str() {
            "read_file" => {
                let target_clone = target.clone();
                let max_bytes = cfg.max_read_file_bytes;
                let read_res = tokio::time::timeout(
                    action_timeout,
                    tokio::task::spawn_blocking(move || {
                        execute_bounded_file_read(&target_clone, caller_uid, max_bytes)
                    }),
                )
                .await;

                let (raw_text, bytes_read) = match read_res {
                    Ok(Ok(Ok(pair))) => pair,
                    Ok(Ok(Err(err))) => {
                        if let Ok(mut guard) = self.registry.lock() {
                            guard.record_action_outcome(
                                &session_id,
                                &agent_id,
                                &action_kind,
                                &operation,
                                &eval.action_class,
                                false,
                                "read-denied",
                                "DAEMON_SAFE_READ_REJECTED",
                                &eval.audit_event_id,
                                false,
                                false,
                            );
                        }
                        return Ok(envelope(false, "read-denied", &err, &operation_id, data));
                    }
                    Ok(Err(join_err)) => {
                        if let Ok(mut guard) = self.registry.lock() {
                            guard.record_action_outcome(
                                &session_id,
                                &agent_id,
                                &action_kind,
                                &operation,
                                &eval.action_class,
                                false,
                                "execution-failed",
                                "DAEMON_READ_TASK_ERROR",
                                &eval.audit_event_id,
                                true,
                                false,
                            );
                        }
                        return Ok(envelope(
                            false,
                            "execution-failed",
                            &format!("File read worker failed: {join_err}"),
                            &operation_id,
                            data,
                        ));
                    }
                    Err(_) => {
                        if let Ok(mut guard) = self.registry.lock() {
                            guard.record_action_outcome(
                                &session_id,
                                &agent_id,
                                &action_kind,
                                &operation,
                                &eval.action_class,
                                false,
                                "action-timeout",
                                "DAEMON_ACTION_TIMEOUT",
                                &eval.audit_event_id,
                                true,
                                true,
                            );
                        }
                        return Ok(envelope(
                            false,
                            "action-timeout",
                            "File read timed out under gateway protection.",
                            &operation_id,
                            data,
                        ));
                    }
                };

                // 10.03 — Seal file content before returning to agent!
                let wrapped = match wrap_untrusted_remote(
                    connection,
                    &cfg,
                    &agent_id,
                    "file_content",
                    &raw_text,
                )
                .await
                {
                    Ok(w) => w,
                    Err(err) => {
                        if let Ok(mut guard) = self.registry.lock() {
                            guard.record_action_outcome(
                                &session_id,
                                &agent_id,
                                &action_kind,
                                &operation,
                                &eval.action_class,
                                false,
                                "wrap-untrusted-failed",
                                "DAEMON_FAIL_CLOSED_WRAP_FAILED",
                                &eval.audit_event_id,
                                true,
                                false,
                            );
                        }
                        return Ok(envelope(
                            false,
                            "wrap-untrusted-failed",
                            &format!("Fail-closed: could not seal file content via WrapUntrustedData ({err})."),
                            &operation_id,
                            data,
                        ));
                    }
                };

                if let Ok(mut guard) = self.registry.lock() {
                    guard.record_wrapped_payload(wrapped.injection_detected);
                    guard.record_action_outcome(
                        &session_id,
                        &agent_id,
                        &action_kind,
                        &operation,
                        &eval.action_class,
                        true,
                        "ok",
                        &eval.rule_id,
                        &eval.audit_event_id,
                        false,
                        false,
                    );
                }

                data.insert("bytes_read".to_string(), OwnedValue::from(bytes_read));
                data.insert("executable".to_string(), OwnedValue::from(wrapped.executable));
                data.insert(
                    "policy_authority".to_string(),
                    OwnedValue::from(Str::from(wrapped.policy_authority.as_str())),
                );
                data.insert(
                    "content_sha256".to_string(),
                    OwnedValue::from(Str::from(wrapped.content_sha256.as_str())),
                );
                data.insert(
                    "injection_detected".to_string(),
                    OwnedValue::from(wrapped.injection_detected),
                );
                data.insert(
                    "detected_patterns".to_string(),
                    vec_to_owned(&wrapped.detected_patterns),
                );
                data.insert(
                    "wrapped_payload".to_string(),
                    OwnedValue::from(Str::from(wrapped.wrapped_payload.as_str())),
                );

                let _ = Self::action_audited(
                    &emitter,
                    agent_id,
                    action_kind,
                    operation,
                    eval.action_class,
                    "allow".to_string(),
                    "ok".to_string(),
                    eval.rule_id,
                )
                .await;

                Ok(envelope(
                    true,
                    "ok",
                    "Read-only file fetched and sealed as non-executable untrusted data.",
                    &operation_id,
                    data,
                ))
            }
            "project_analysis" => {
                let target_clone = target.clone();
                let max_depth = cfg.max_project_depth;
                let max_entries = cfg.max_project_entries;
                let analysis_res = tokio::time::timeout(
                    action_timeout,
                    tokio::task::spawn_blocking(move || {
                        execute_bounded_project_analysis(
                            &target_clone,
                            caller_uid,
                            max_depth,
                            max_entries,
                        )
                    }),
                )
                .await;

                let summary_json = match analysis_res {
                    Ok(Ok(Ok(s))) => s,
                    Ok(Ok(Err(err))) => {
                        if let Ok(mut guard) = self.registry.lock() {
                            guard.record_action_outcome(
                                &session_id,
                                &agent_id,
                                &action_kind,
                                &operation,
                                &eval.action_class,
                                false,
                                "analysis-denied",
                                "DAEMON_PROJECT_ANALYSIS_REJECTED",
                                &eval.audit_event_id,
                                false,
                                false,
                            );
                        }
                        return Ok(envelope(false, "analysis-denied", &err, &operation_id, data));
                    }
                    Ok(Err(join_err)) => {
                        return Ok(envelope(
                            false,
                            "execution-failed",
                            &format!("Project analysis task error: {join_err}"),
                            &operation_id,
                            data,
                        ));
                    }
                    Err(_) => {
                        if let Ok(mut guard) = self.registry.lock() {
                            guard.record_action_outcome(
                                &session_id,
                                &agent_id,
                                &action_kind,
                                &operation,
                                &eval.action_class,
                                false,
                                "action-timeout",
                                "DAEMON_ACTION_TIMEOUT",
                                &eval.audit_event_id,
                                true,
                                true,
                            );
                        }
                        return Ok(envelope(
                            false,
                            "action-timeout",
                            "Project analysis timed out under gateway protection.",
                            &operation_id,
                            data,
                        ));
                    }
                };

                let wrapped = match wrap_untrusted_remote(
                    connection,
                    &cfg,
                    &agent_id,
                    "project_analysis",
                    &summary_json,
                )
                .await
                {
                    Ok(w) => w,
                    Err(err) => {
                        if let Ok(mut guard) = self.registry.lock() {
                            guard.record_action_outcome(
                                &session_id,
                                &agent_id,
                                &action_kind,
                                &operation,
                                &eval.action_class,
                                false,
                                "wrap-untrusted-failed",
                                "DAEMON_FAIL_CLOSED_WRAP_FAILED",
                                &eval.audit_event_id,
                                true,
                                false,
                            );
                        }
                        return Ok(envelope(
                            false,
                            "wrap-untrusted-failed",
                            &format!("Fail-closed: could not seal project analysis ({err})."),
                            &operation_id,
                            data,
                        ));
                    }
                };

                if let Ok(mut guard) = self.registry.lock() {
                    guard.record_wrapped_payload(wrapped.injection_detected);
                    guard.record_action_outcome(
                        &session_id,
                        &agent_id,
                        &action_kind,
                        &operation,
                        &eval.action_class,
                        true,
                        "ok",
                        &eval.rule_id,
                        &eval.audit_event_id,
                        false,
                        false,
                    );
                }

                data.insert("executable".to_string(), OwnedValue::from(wrapped.executable));
                data.insert(
                    "policy_authority".to_string(),
                    OwnedValue::from(Str::from(wrapped.policy_authority.as_str())),
                );
                data.insert(
                    "content_sha256".to_string(),
                    OwnedValue::from(Str::from(wrapped.content_sha256.as_str())),
                );
                data.insert(
                    "wrapped_payload".to_string(),
                    OwnedValue::from(Str::from(wrapped.wrapped_payload.as_str())),
                );

                let _ = Self::action_audited(
                    &emitter,
                    agent_id,
                    action_kind,
                    operation,
                    eval.action_class,
                    "allow".to_string(),
                    "ok".to_string(),
                    eval.rule_id,
                )
                .await;

                Ok(envelope(
                    true,
                    "ok",
                    "Bounded read-only project analysis completed and sealed.",
                    &operation_id,
                    data,
                ))
            }
            "system_diagnostics" => {
                let Some(diag_target) = lookup_readonly_diagnostic_target(&dbus_interface, &target)
                else {
                    if let Ok(mut guard) = self.registry.lock() {
                        guard.record_action_outcome(
                            &session_id,
                            &agent_id,
                            &action_kind,
                            &operation,
                            &eval.action_class,
                            false,
                            "diagnostic-not-allowlisted",
                            "DAEMON_DIAGNOSTIC_ALLOWLIST_DENY",
                            &eval.audit_event_id,
                            true,
                            false,
                        );
                    }
                    return Ok(envelope(
                        false,
                        "diagnostic-not-allowlisted",
                        "Requested D-Bus diagnostic interface/method is outside zci-daemon read-only allowlist.",
                        &operation_id,
                        data,
                    ));
                };

                let diag_json = match execute_readonly_dbus_diagnostic(
                    connection,
                    diag_target,
                    action_timeout,
                )
                .await
                {
                    Ok(s) => s,
                    Err(err) => {
                        let timed_out = err.contains("Timed out");
                        if let Ok(mut guard) = self.registry.lock() {
                            guard.record_action_outcome(
                                &session_id,
                                &agent_id,
                                &action_kind,
                                &operation,
                                &eval.action_class,
                                false,
                                if timed_out {
                                    "action-timeout"
                                } else {
                                    "diagnostic-unavailable"
                                },
                                "DAEMON_DIAGNOSTIC_ERROR",
                                &eval.audit_event_id,
                                false,
                                timed_out,
                            );
                        }
                        return Ok(envelope(
                            false,
                            if timed_out {
                                "action-timeout"
                            } else {
                                "diagnostic-unavailable"
                            },
                            &err,
                            &operation_id,
                            data,
                        ));
                    }
                };

                let wrapped = match wrap_untrusted_remote(
                    connection,
                    &cfg,
                    &agent_id,
                    "system_diagnostics",
                    &diag_json,
                )
                .await
                {
                    Ok(w) => w,
                    Err(err) => {
                        if let Ok(mut guard) = self.registry.lock() {
                            guard.record_action_outcome(
                                &session_id,
                                &agent_id,
                                &action_kind,
                                &operation,
                                &eval.action_class,
                                false,
                                "wrap-untrusted-failed",
                                "DAEMON_FAIL_CLOSED_WRAP_FAILED",
                                &eval.audit_event_id,
                                true,
                                false,
                            );
                        }
                        return Ok(envelope(
                            false,
                            "wrap-untrusted-failed",
                            &format!("Fail-closed: could not seal diagnostic output ({err})."),
                            &operation_id,
                            data,
                        ));
                    }
                };

                if let Ok(mut guard) = self.registry.lock() {
                    guard.record_wrapped_payload(wrapped.injection_detected);
                    guard.record_action_outcome(
                        &session_id,
                        &agent_id,
                        &action_kind,
                        &operation,
                        &eval.action_class,
                        true,
                        "ok",
                        &eval.rule_id,
                        &eval.audit_event_id,
                        false,
                        false,
                    );
                }

                data.insert("executable".to_string(), OwnedValue::from(wrapped.executable));
                data.insert(
                    "policy_authority".to_string(),
                    OwnedValue::from(Str::from(wrapped.policy_authority.as_str())),
                );
                data.insert(
                    "content_sha256".to_string(),
                    OwnedValue::from(Str::from(wrapped.content_sha256.as_str())),
                );
                data.insert(
                    "wrapped_payload".to_string(),
                    OwnedValue::from(Str::from(wrapped.wrapped_payload.as_str())),
                );

                let _ = Self::action_audited(
                    &emitter,
                    agent_id,
                    action_kind,
                    operation,
                    eval.action_class,
                    "allow".to_string(),
                    "ok".to_string(),
                    eval.rule_id,
                )
                .await;

                Ok(envelope(
                    true,
                    "ok",
                    "Read-only system diagnostics fetched and sealed.",
                    &operation_id,
                    data,
                ))
            }
            "model_query" => {
                // 10.03 — Seal user prompt BEFORE forwarding to local loopback Gemma 4B gateway.
                let wrapped_prompt = match wrap_untrusted_remote(
                    connection,
                    &cfg,
                    &agent_id,
                    "user_prompt",
                    &payload,
                )
                .await
                {
                    Ok(w) => w,
                    Err(err) => {
                        if let Ok(mut guard) = self.registry.lock() {
                            guard.record_action_outcome(
                                &session_id,
                                &agent_id,
                                &action_kind,
                                &operation,
                                &eval.action_class,
                                false,
                                "wrap-untrusted-failed",
                                "DAEMON_FAIL_CLOSED_WRAP_FAILED",
                                &eval.audit_event_id,
                                true,
                                false,
                            );
                        }
                        return Ok(envelope(
                            false,
                            "wrap-untrusted-failed",
                            &format!("Fail-closed: could not seal model prompt ({err})."),
                            &operation_id,
                            data,
                        ));
                    }
                };
                if let Ok(mut guard) = self.registry.lock() {
                    guard.record_wrapped_payload(wrapped_prompt.injection_detected);
                }

                let outcome = match execute_loopback_model_query(
                    &cfg,
                    &agent_id,
                    &wrapped_prompt.wrapped_payload,
                    action_timeout,
                )
                .await
                {
                    Ok(o) => o,
                    Err(err) => {
                        let timed_out = err.contains("timed out") || err.contains("Timed out");
                        if let Ok(mut guard) = self.registry.lock() {
                            guard.record_action_outcome(
                                &session_id,
                                &agent_id,
                                &action_kind,
                                &operation,
                                &eval.action_class,
                                false,
                                if timed_out {
                                    "action-timeout"
                                } else {
                                    "model-gateway-error"
                                },
                                "DAEMON_MODEL_QUERY_ERROR",
                                &eval.audit_event_id,
                                false,
                                timed_out,
                            );
                        }
                        return Ok(envelope(
                            false,
                            if timed_out {
                                "action-timeout"
                            } else {
                                "model-gateway-error"
                            },
                            &err,
                            &operation_id,
                            data,
                        ));
                    }
                };

                match outcome {
                    ModelQueryOutcome::PassiveStandby {
                        gateway_addr,
                        detail,
                    } => {
                        if let Ok(mut guard) = self.registry.lock() {
                            guard.record_action_outcome(
                                &session_id,
                                &agent_id,
                                &action_kind,
                                &operation,
                                &eval.action_class,
                                true,
                                "model-passive-standby",
                                &eval.rule_id,
                                &eval.audit_event_id,
                                false,
                                false,
                            );
                        }
                        data.insert(
                            "model_state".to_string(),
                            OwnedValue::from(Str::from("Standby")),
                        );
                        data.insert("passive_standby".to_string(), OwnedValue::from(true));
                        data.insert(
                            "gateway_addr".to_string(),
                            OwnedValue::from(Str::from(gateway_addr.as_str())),
                        );
                        data.insert(
                            "prompt_sha256".to_string(),
                            OwnedValue::from(Str::from(wrapped_prompt.content_sha256.as_str())),
                        );
                        data.insert(
                            "wrapped_prompt".to_string(),
                            OwnedValue::from(Str::from(wrapped_prompt.wrapped_payload.as_str())),
                        );

                        let _ = Self::action_audited(
                            &emitter,
                            agent_id,
                            action_kind,
                            operation,
                            eval.action_class,
                            "allow".to_string(),
                            "model-passive-standby".to_string(),
                            eval.rule_id,
                        )
                        .await;

                        Ok(envelope(
                            true,
                            "model-passive-standby",
                            &detail,
                            &operation_id,
                            data,
                        ))
                    }
                    ModelQueryOutcome::Responded { status_code, body } => {
                        let wrapped_out = match wrap_untrusted_remote(
                            connection,
                            &cfg,
                            &agent_id,
                            "model_output",
                            &body,
                        )
                        .await
                        {
                            Ok(w) => w,
                            Err(err) => {
                                if let Ok(mut guard) = self.registry.lock() {
                                    guard.record_action_outcome(
                                        &session_id,
                                        &agent_id,
                                        &action_kind,
                                        &operation,
                                        &eval.action_class,
                                        false,
                                        "wrap-untrusted-failed",
                                        "DAEMON_FAIL_CLOSED_WRAP_FAILED",
                                        &eval.audit_event_id,
                                        true,
                                        false,
                                    );
                                }
                                return Ok(envelope(
                                    false,
                                    "wrap-untrusted-failed",
                                    &format!("Fail-closed: could not seal model output ({err})."),
                                    &operation_id,
                                    data,
                                ));
                            }
                        };

                        if let Ok(mut guard) = self.registry.lock() {
                            guard.record_wrapped_payload(wrapped_out.injection_detected);
                            guard.record_action_outcome(
                                &session_id,
                                &agent_id,
                                &action_kind,
                                &operation,
                                &eval.action_class,
                                true,
                                "ok",
                                &eval.rule_id,
                                &eval.audit_event_id,
                                false,
                                false,
                            );
                        }

                        data.insert(
                            "model_state".to_string(),
                            OwnedValue::from(Str::from("Standby")),
                        );
                        data.insert("passive_standby".to_string(), OwnedValue::from(false));
                        data.insert(
                            "http_status".to_string(),
                            OwnedValue::from(u32::from(status_code)),
                        );
                        data.insert(
                            "executable".to_string(),
                            OwnedValue::from(wrapped_out.executable),
                        );
                        data.insert(
                            "policy_authority".to_string(),
                            OwnedValue::from(Str::from(wrapped_out.policy_authority.as_str())),
                        );
                        data.insert(
                            "content_sha256".to_string(),
                            OwnedValue::from(Str::from(wrapped_out.content_sha256.as_str())),
                        );
                        data.insert(
                            "wrapped_payload".to_string(),
                            OwnedValue::from(Str::from(wrapped_out.wrapped_payload.as_str())),
                        );

                        let _ = Self::action_audited(
                            &emitter,
                            agent_id,
                            action_kind,
                            operation,
                            eval.action_class,
                            "allow".to_string(),
                            "ok".to_string(),
                            eval.rule_id,
                        )
                        .await;

                        Ok(envelope(
                            true,
                            "ok",
                            "Local loopback Gemma 4B model query completed and sealed; model returned to Standby.",
                            &operation_id,
                            data,
                        ))
                    }
                }
            }
            "userspace_preflight" => {
                // Bounded `UserSpaceSafe` delegation check: zci-daemon NEVER mutates `/home` directly
                // (enforced also by `ProtectHome=read-only`), returning a policy-attested authorization
                // token/audit ID for the unprivileged user-space sandbox only when class == UserSpaceSafe.
                if eval.action_class != "UserSpaceSafe" && eval.action_class != "ReadOnly" {
                    return Ok(envelope(
                        false,
                        "gateway-action-class-denied",
                        "User-space preflight requires ReadOnly or UserSpaceSafe classification.",
                        &operation_id,
                        data,
                    ));
                }
                if let Ok(mut guard) = self.registry.lock() {
                    guard.record_action_outcome(
                        &session_id,
                        &agent_id,
                        &action_kind,
                        &operation,
                        &eval.action_class,
                        true,
                        "delegated-userspace-approved",
                        &eval.rule_id,
                        &eval.audit_event_id,
                        false,
                        false,
                    );
                }
                data.insert(
                    "execution_mode".to_string(),
                    OwnedValue::from(Str::from("delegated-userspace-sandbox")),
                );
                let _ = Self::action_audited(
                    &emitter,
                    agent_id,
                    action_kind,
                    operation,
                    eval.action_class,
                    "allow".to_string(),
                    "delegated-userspace-approved".to_string(),
                    eval.rule_id,
                )
                .await;
                Ok(envelope(
                    true,
                    "delegated-userspace-approved",
                    "Policy-approved UserSpaceSafe action delegated to unprivileged user sandbox; daemon performed zero host writes.",
                    &operation_id,
                    data,
                ))
            }
            _ => {
                if let Ok(mut guard) = self.registry.lock() {
                    guard.record_action_outcome(
                        &session_id,
                        &agent_id,
                        &action_kind,
                        &operation,
                        &eval.action_class,
                        false,
                        "unsupported-action-kind",
                        "DAEMON_UNSUPPORTED_ACTION_KIND",
                        &eval.audit_event_id,
                        true,
                        false,
                    );
                }
                let _ = Self::action_audited(
                    &emitter,
                    agent_id,
                    action_kind,
                    operation,
                    eval.action_class,
                    "deny".to_string(),
                    "unsupported-action-kind".to_string(),
                    "DAEMON_UNSUPPORTED_ACTION_KIND".to_string(),
                )
                .await;
                Ok(envelope(
                    false,
                    "unsupported-action-kind",
                    "Requested action_kind is not supported by zci-daemon (default-deny).",
                    &operation_id,
                    data,
                ))
            }
        }
    }

    /// 10.05 & 10.06 — Local session read-only health & observability snapshot.
    async fn get_daemon_status(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        self.build_daemon_status(connection).await
    }

    /// Standard Zethropol `GetState` read-only alias for `GetDaemonStatus`.
    async fn get_state(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        self.build_daemon_status(connection).await
    }

    /// 10.05 & 10.06 — Local session read-only agent session listing.
    async fn list_sessions(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let sessions = {
            let mut guard = self
                .registry
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Daemon registry lock poisoned".to_string()))?;
            guard.list_sessions()
        };

        let sessions_json: Vec<String> = sessions
            .iter()
            .filter_map(|s| serde_json::to_string(s).ok())
            .collect();

        let mut data = HashMap::new();
        data.insert(
            "session_count".to_string(),
            OwnedValue::from(sessions.len() as u64),
        );
        data.insert("sessions_json".to_string(), vec_to_owned(&sessions_json));

        Ok(envelope(
            true,
            "ok",
            "Active ZCI daemon sessions retrieved.",
            &operation_id,
            data,
        ))
    }

    /// 10.05 — Polkit `org.zethropol.zci.daemon.admin` (`auth_admin`) gated session termination.
    async fn terminate_session(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        session_id: String,
        reason: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let sender = match header.sender() {
            Some(s) => s.to_string(),
            None => {
                return Ok(envelope(
                    false,
                    "invalid-caller",
                    "D-Bus caller identity unavailable.",
                    &operation_id,
                    HashMap::new(),
                ));
            }
        };

        let cfg = self.snapshot_config()?;
        if !check_polkit_admin(cfg.pkcheck_bin, sender).await {
            return Ok(envelope(
                false,
                "permission-denied",
                "Polkit auth_admin authorization (org.zethropol.zci.daemon.admin) is required to terminate ZCI sessions.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let removed = {
            let mut guard = self
                .registry
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Daemon registry lock poisoned".to_string()))?;
            guard.admin_terminate_session(&session_id)
        };

        match removed {
            Ok(list) => {
                let note = if reason.trim().is_empty() {
                    "terminated-by-polkit-admin"
                } else {
                    reason.trim()
                };
                for sess in &list {
                    let _ = Self::session_state_changed(
                        &emitter,
                        sess.session_id.clone(),
                        sess.agent_id.clone(),
                        "Terminated".to_string(),
                        note.to_string(),
                    )
                    .await;
                }
                let mut data = HashMap::new();
                data.insert(
                    "terminated_count".to_string(),
                    OwnedValue::from(list.len() as u64),
                );
                Ok(envelope(
                    true,
                    "ok",
                    "ZCI session(s) terminated by Polkit-authorized administrator.",
                    &operation_id,
                    data,
                ))
            }
            Err(err) => Ok(envelope(
                false,
                "session-not-found",
                &err,
                &operation_id,
                HashMap::new(),
            )),
        }
    }

    /// 10.05 — Polkit `org.zethropol.zci.daemon.admin` (`auth_admin`) gated configuration reload.
    async fn reload_config(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
        reason: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let sender = match header.sender() {
            Some(s) => s.to_string(),
            None => {
                return Ok(envelope(
                    false,
                    "invalid-caller",
                    "D-Bus caller identity unavailable.",
                    &operation_id,
                    HashMap::new(),
                ));
            }
        };

        let cfg = self.snapshot_config()?;
        if !check_polkit_admin(cfg.pkcheck_bin, sender).await {
            return Ok(envelope(
                false,
                "permission-denied",
                "Polkit auth_admin authorization (org.zethropol.zci.daemon.admin) is required to reload ZCI Daemon configuration.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let reload_res = {
            let mut guard = self
                .registry
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Daemon registry lock poisoned".to_string()))?;
            let res = guard.config.reload_from_file();
            if res.is_ok() {
                guard.metrics.config_reload_count += 1;
            }
            (
                res,
                guard.config.clone(),
                guard.metrics.config_reload_count,
            )
        };

        match reload_res.0 {
            Ok(loaded_file) => {
                let refreshed_cfg = reload_res.1;
                let health = probe_policy_health(connection, &refreshed_cfg).await;
                if let Ok(mut guard) = self.registry.lock() {
                    guard.last_policy_health = health.clone();
                }

                let mut data = HashMap::new();
                data.insert("config_file_loaded".to_string(), OwnedValue::from(loaded_file));
                data.insert(
                    "config_reload_count".to_string(),
                    OwnedValue::from(reload_res.2),
                );
                data.insert(
                    "max_concurrent_global".to_string(),
                    OwnedValue::from(refreshed_cfg.max_concurrent_global),
                );
                data.insert(
                    "max_concurrent_per_agent".to_string(),
                    OwnedValue::from(refreshed_cfg.max_concurrent_per_agent),
                );
                data.insert(
                    "rate_limit_max_requests".to_string(),
                    OwnedValue::from(refreshed_cfg.rate_limit_max_requests),
                );
                data.insert(
                    "action_timeout_ms".to_string(),
                    OwnedValue::from(refreshed_cfg.action_timeout_ms),
                );
                data.insert(
                    "policy_engine_reachable".to_string(),
                    OwnedValue::from(health.reachable),
                );
                data.insert(
                    "reason".to_string(),
                    OwnedValue::from(Str::from(reason.trim())),
                );

                Ok(envelope(
                    true,
                    "ok",
                    "ZCI Daemon runtime configuration reloaded and clamped within safety bounds.",
                    &operation_id,
                    data,
                ))
            }
            Err(err) => Ok(envelope(
                false,
                "config-reload-failed",
                &err,
                &operation_id,
                HashMap::new(),
            )),
        }
    }

    #[zbus(signal)]
    async fn session_state_changed(
        ctxt: &SignalEmitter<'_>,
        session_id: String,
        agent_id: String,
        state: String,
        reason: String,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    #[allow(clippy::too_many_arguments)]
    async fn action_audited(
        ctxt: &SignalEmitter<'_>,
        agent_id: String,
        action_kind: String,
        operation: String,
        action_class: String,
        decision: String,
        code: String,
        rule_id: String,
    ) -> zbus::Result<()>;
}
