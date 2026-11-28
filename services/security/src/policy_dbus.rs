use crate::crypto_util::now_unix_secs;
use crate::policy_engine::{
    supported_agent_ids, ActionRequest, PolicyConfig, PolicyDecision, PolicyEngine,
    POLICY_ENGINE_VERSION,
};
use std::collections::HashMap;
use std::path::PathBuf;
use std::process::Command;
use std::sync::Mutex;
use uuid::Uuid;
use zbus::interface;
use zbus::object_server::SignalEmitter;
use zbus::proxy::CacheProperties;
use zbus::zvariant::{Array, OwnedValue, Signature, Str, Value};

pub const POLICY_DBUS_PATH: &str = "/org/zethropol/Security/Policy";
#[cfg_attr(not(test), allow(dead_code))]
pub const POLICY_DBUS_INTERFACE: &str = "org.zethropol.Security.Policy.v1";
pub const POLKIT_ACTION_POLICY_ADMIN: &str = "org.zethropol.security.policy.admin";
pub const DEFAULT_PKCHECK_BIN: &str = "/usr/bin/pkcheck";

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

fn decision_to_map(decision: &PolicyDecision) -> HashMap<String, OwnedValue> {
    let mut data = HashMap::new();
    data.insert("allowed".to_string(), OwnedValue::from(decision.allowed));
    data.insert(
        "decision".to_string(),
        OwnedValue::from(Str::from(decision.decision.as_str())),
    );
    data.insert(
        "action_class".to_string(),
        OwnedValue::from(Str::from(decision.action_class.as_str())),
    );
    data.insert(
        "requires_polkit".to_string(),
        OwnedValue::from(decision.requires_polkit),
    );
    data.insert(
        "polkit_action".to_string(),
        OwnedValue::from(Str::from(decision.polkit_action.as_str())),
    );
    data.insert(
        "rule_id".to_string(),
        OwnedValue::from(Str::from(decision.rule_id.as_str())),
    );
    data.insert(
        "reason".to_string(),
        OwnedValue::from(Str::from(decision.reason.as_str())),
    );
    data.insert(
        "penalty_delta".to_string(),
        OwnedValue::from(decision.penalty_delta),
    );
    data.insert(
        "penalty_score".to_string(),
        OwnedValue::from(decision.penalty_score),
    );
    data.insert(
        "penalty_level".to_string(),
        OwnedValue::from(u32::from(decision.penalty_level)),
    );
    data.insert(
        "lockout_active".to_string(),
        OwnedValue::from(decision.lockout_active),
    );
    data.insert(
        "hard_locked".to_string(),
        OwnedValue::from(decision.hard_locked),
    );
    data.insert(
        "cooldown_remaining_secs".to_string(),
        OwnedValue::from(decision.cooldown_remaining_secs),
    );
    data.insert(
        "untrusted_flags".to_string(),
        vec_to_owned(&decision.untrusted_flags),
    );
    data.insert(
        "audit_event_id".to_string(),
        OwnedValue::from(Str::from(decision.audit_event_id.as_str())),
    );
    data
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
        if let Ok(uid) = proxy.call::<_, _, u32>("GetConnectionUnixUser", &(sender,)).await {
            return uid;
        }
    }
    u32::MAX
}

pub struct PolicyDbusService {
    engine: Mutex<PolicyEngine>,
    pkcheck_bin: PathBuf,
}

impl PolicyDbusService {
    pub fn with_config(config: PolicyConfig, pkcheck_bin: PathBuf) -> Self {
        Self {
            engine: Mutex::new(PolicyEngine::with_config(config)),
            pkcheck_bin,
        }
    }
}

#[interface(name = "org.zethropol.Security.Policy.v1")]
impl PolicyDbusService {
    /// 08.01–08.07 — Evaluate a ZCI agent action request under strict default-deny policy.
    /// Never executes privileged actions and never weakens Polkit or recovery locks.
    async fn evaluate_action(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        agent_id: String,
        operation: String,
        target: String,
        dbus_interface: String,
        untrusted_payload: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let sender = header
            .sender()
            .map(|s| s.to_string())
            .unwrap_or_default();
        let caller_uid = resolve_caller_uid(connection, &sender).await;

        let req = ActionRequest {
            agent_id: agent_id.clone(),
            operation: operation.clone(),
            target,
            dbus_interface,
            untrusted_payload,
            caller_bus_name: sender,
            caller_uid,
        };

        let decision = {
            let mut guard = self
                .engine
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Policy engine lock poisoned".to_string()))?;
            guard.evaluate_action(&req)
        };

        let _ = Self::policy_decision_recorded(
            &emitter,
            decision.audit_event_id.clone(),
            agent_id,
            operation,
            decision.action_class.as_str().to_string(),
            decision.decision.clone(),
            decision.rule_id.clone(),
            decision.penalty_score,
            decision.lockout_active,
        )
        .await;

        let data = decision_to_map(&decision);
        Ok(envelope(
            decision.allowed,
            &decision.code,
            &decision.reason,
            &operation_id,
            data,
        ))
    }

    /// 08.04 — Wrap external/user/web/file/tool text strictly as non-executable untrusted data.
    async fn wrap_untrusted_data(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
        agent_id: String,
        source_kind: String,
        content: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let sender = header
            .sender()
            .map(|s| s.to_string())
            .unwrap_or_default();
        let caller_uid = resolve_caller_uid(connection, &sender).await;

        let inspection = {
            let mut guard = self
                .engine
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Policy engine lock poisoned".to_string()))?;
            guard.wrap_untrusted_data(&agent_id, &source_kind, &content, &sender, caller_uid)
        };

        let mut data = HashMap::new();
        data.insert(
            "trust_level".to_string(),
            OwnedValue::from(Str::from(inspection.trust_level.as_str())),
        );
        data.insert("executable".to_string(), OwnedValue::from(inspection.executable));
        data.insert(
            "policy_authority".to_string(),
            OwnedValue::from(Str::from(inspection.policy_authority.as_str())),
        );
        data.insert(
            "source_kind".to_string(),
            OwnedValue::from(Str::from(inspection.source_kind.as_str())),
        );
        data.insert(
            "content_sha256".to_string(),
            OwnedValue::from(Str::from(inspection.content_sha256.as_str())),
        );
        data.insert(
            "byte_length".to_string(),
            OwnedValue::from(inspection.byte_length as u64),
        );
        data.insert("oversized".to_string(), OwnedValue::from(inspection.oversized));
        data.insert(
            "injection_detected".to_string(),
            OwnedValue::from(inspection.injection_detected),
        );
        data.insert(
            "self_modification_injection".to_string(),
            OwnedValue::from(inspection.self_modification_injection),
        );
        data.insert(
            "detected_patterns".to_string(),
            vec_to_owned(&inspection.detected_patterns),
        );
        data.insert(
            "envelope_id".to_string(),
            OwnedValue::from(Str::from(inspection.envelope_id.as_str())),
        );
        data.insert(
            "wrapped_payload".to_string(),
            OwnedValue::from(Str::from(inspection.wrapped_payload.as_str())),
        );

        let code = if inspection.injection_detected {
            "injection-detected"
        } else {
            "ok"
        };
        let msg = if inspection.injection_detected {
            "Untrusted content isolated as non-executable data; suspicious injection patterns flagged."
        } else {
            "Untrusted content isolated as non-executable data."
        };
        Ok(envelope(true, code, msg, &operation_id, data))
    }

    /// Read-only agent penalty, lockout, and allowlist status.
    async fn get_agent_status(
        &self,
        agent_id: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let now = now_unix_secs();

        let (known, state, allowlist, corrupted) = {
            let guard = self
                .engine
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Policy engine lock poisoned".to_string()))?;
            let allowlist = guard.describe_agent_allowlist(&agent_id);
            let known = allowlist.is_some();
            let state = guard.get_agent_state(&agent_id);
            let corrupted = guard.state_corrupted();
            (known, state, allowlist, corrupted)
        };

        let remaining = if state.cooldown_until_unix > now {
            state.cooldown_until_unix - now
        } else {
            0
        };
        let locked = corrupted || state.hard_locked || remaining > 0;

        let mut data = HashMap::new();
        data.insert("agent_id".to_string(), OwnedValue::from(Str::from(agent_id.as_str())));
        data.insert("known_agent".to_string(), OwnedValue::from(known));
        data.insert("state_corrupted".to_string(), OwnedValue::from(corrupted));
        data.insert(
            "total_violations".to_string(),
            OwnedValue::from(state.total_violations),
        );
        data.insert("strike_score".to_string(), OwnedValue::from(state.strike_score));
        data.insert(
            "penalty_level".to_string(),
            OwnedValue::from(u32::from(state.penalty_level)),
        );
        data.insert("lockout_active".to_string(), OwnedValue::from(locked));
        data.insert("hard_locked".to_string(), OwnedValue::from(state.hard_locked || corrupted));
        data.insert("cooldown_remaining_secs".to_string(), OwnedValue::from(remaining));
        data.insert(
            "self_mod_attempts".to_string(),
            OwnedValue::from(state.self_mod_attempts),
        );
        data.insert(
            "last_rule_id".to_string(),
            OwnedValue::from(Str::from(state.last_rule_id.as_str())),
        );
        let (ops, dbus_calls) = allowlist.unwrap_or_default();
        data.insert("allowed_operations".to_string(), vec_to_owned(&ops));
        data.insert("allowed_dbus_calls".to_string(), vec_to_owned(&dbus_calls));

        Ok(envelope(
            true,
            "ok",
            "Agent policy status retrieved.",
            &operation_id,
            data,
        ))
    }

    /// Read-only ZCI Security & Policy Engine posture summary.
    async fn get_policy_summary(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let (corrupted, locked_count, total_events, state_path, log_path) = {
            let guard = self
                .engine
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Policy engine lock poisoned".to_string()))?;
            (
                guard.state_corrupted(),
                guard.locked_agent_count() as u64,
                guard.audit_total_events(),
                guard.config().state_file.display().to_string(),
                guard.config().audit_log_file.display().to_string(),
            )
        };

        let mut data = HashMap::new();
        data.insert(
            "engine_version".to_string(),
            OwnedValue::from(Str::from(POLICY_ENGINE_VERSION)),
        );
        data.insert("default_deny".to_string(), OwnedValue::from(true));
        data.insert("self_modification_lock".to_string(), OwnedValue::from(true));
        data.insert("untrusted_content_isolation".to_string(), OwnedValue::from(true));
        data.insert("recovery_locks_protected".to_string(), OwnedValue::from(true));
        data.insert("state_corrupted".to_string(), OwnedValue::from(corrupted));
        data.insert("locked_agents".to_string(), OwnedValue::from(locked_count));
        data.insert("total_audit_events".to_string(), OwnedValue::from(total_events));
        data.insert("state_path".to_string(), OwnedValue::from(Str::from(state_path)));
        data.insert("audit_log_path".to_string(), OwnedValue::from(Str::from(log_path)));
        data.insert(
            "supported_agents".to_string(),
            vec_to_owned(&supported_agent_ids()),
        );
        data.insert(
            "admin_polkit_action".to_string(),
            OwnedValue::from(Str::from(POLKIT_ACTION_POLICY_ADMIN)),
        );

        Ok(envelope(
            true,
            "ok",
            "ZCI Security & Policy Engine summary retrieved.",
            &operation_id,
            data,
        ))
    }

    /// 08.07 — Query recent bounded structured audit events and SHA-256 hash-chain integrity.
    async fn get_audit_events(
        &self,
        limit: u32,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let query = {
            let guard = self
                .engine
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Policy engine lock poisoned".to_string()))?;
            guard.get_audit_events(limit)
        };

        let events_json: Vec<String> = query
            .events
            .iter()
            .filter_map(|e| serde_json::to_string(e).ok())
            .collect();

        let mut data = HashMap::new();
        data.insert("total_events".to_string(), OwnedValue::from(query.total_events));
        data.insert("chain_valid".to_string(), OwnedValue::from(query.chain_valid));
        data.insert(
            "returned_count".to_string(),
            OwnedValue::from(events_json.len() as u64),
        );
        data.insert("events_json".to_string(), vec_to_owned(&events_json));

        Ok(envelope(
            true,
            if query.chain_valid {
                "ok"
            } else {
                "audit-chain-integrity-warning"
            },
            "Structured ZCI security audit log retrieved.",
            &operation_id,
            data,
        ))
    }

    /// 08.06 — Polkit `auth_admin` gated administrative lockout reset.
    /// ZCI agents cannot self-reset their own penalties or lockouts.
    async fn reset_agent_lockout(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
        agent_id: String,
        reason: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let sender = match header.sender() {
            Some(s) => s.to_string(),
            None => {
                return Ok(envelope(
                    false,
                    "invalid-caller",
                    "D-Bus caller identity is unavailable.",
                    &operation_id,
                    HashMap::new(),
                ));
            }
        };
        let caller_uid = resolve_caller_uid(connection, &sender).await;

        let pkcheck_bin = self.pkcheck_bin.clone();
        let sender_for_pk = sender.clone();
        let polkit_ok = tokio::task::spawn_blocking(move || {
            Command::new(&pkcheck_bin)
                .args([
                    "--action-id",
                    POLKIT_ACTION_POLICY_ADMIN,
                    "--system-bus-name",
                    &sender_for_pk,
                    "--allow-user-interaction",
                ])
                .status()
                .map(|status| status.success())
                .unwrap_or(false)
        })
        .await
        .unwrap_or(false);

        if !polkit_ok {
            return Ok(envelope(
                false,
                "permission-denied",
                "Polkit auth_admin authorization (org.zethropol.security.policy.admin) is required to reset ZCI penalties.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let reset_result = {
            let mut guard = self
                .engine
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Policy engine lock poisoned".to_string()))?;
            guard.admin_reset_lockout(&agent_id, &reason, &sender, caller_uid)
        };

        match reset_result {
            Ok(audit_ev) => {
                let mut data = HashMap::new();
                data.insert("agent_id".to_string(), OwnedValue::from(Str::from(agent_id.as_str())));
                data.insert(
                    "audit_event_id".to_string(),
                    OwnedValue::from(Str::from(audit_ev.event_id.as_str())),
                );
                Ok(envelope(
                    true,
                    "ok",
                    "Agent penalty and lockout state reset by authorized administrator.",
                    &operation_id,
                    data,
                ))
            }
            Err(err) => Ok(envelope(
                false,
                "reset-failed",
                &err,
                &operation_id,
                HashMap::new(),
            )),
        }
    }

    #[zbus(signal)]
    #[allow(clippy::too_many_arguments)]
    async fn policy_decision_recorded(
        ctxt: &SignalEmitter<'_>,
        event_id: String,
        agent_id: String,
        operation: String,
        action_class: String,
        decision: String,
        rule_id: String,
        penalty_score: u32,
        lockout_active: bool,
    ) -> zbus::Result<()>;
}
