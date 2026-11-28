use crate::audit_store::{now_unix_secs, HashChainedAuditStore};
use crate::bridge::{
    execute_step_via_daemon, invoke_runtime_planner, probe_downstream_health,
    run_recovery_snapper_preflight, seal_untrusted_via_daemon, RuntimePlanInference,
};
use crate::config::{
    is_supported_profile, AgentConfig, AGENT_BUS_NAME, AGENT_VERSION, DEFAULT_AGENT_PROFILE,
    MAX_GOAL_BYTES, MAX_PLANS,
};
use crate::planner::{
    build_deterministic_safe_plan, build_planning_prompt, extract_display_text_from_envelope,
    inspect_text_safety, parse_and_validate_model_plan, validate_target_path, AgentPlan,
    SafetyVerdict,
};
use serde_json::json;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use uuid::Uuid;
use zbus::interface;
use zbus::object_server::SignalEmitter;
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
    s.chars()
        .any(|c| c == '\0' || (c.is_control() && c != '\n' && c != '\r' && c != '\t'))
}

struct AgentState {
    plans: Vec<AgentPlan>,
    audit: Option<HashChainedAuditStore>,
    store_error: String,
    blocked_shell_attempts: u64,
    blocked_protected_path_attempts: u64,
    blocked_unapproved_attempts: u64,
    executed_steps_total: u64,
}

pub struct ZciAgentService {
    config: AgentConfig,
    system_conn: zbus::Connection,
    state: Arc<Mutex<AgentState>>,
}

impl ZciAgentService {
    pub fn new(config: AgentConfig, system_conn: zbus::Connection) -> Self {
        let mut plans = Vec::new();
        let mut store_error = String::new();
        let audit = match HashChainedAuditStore::open(config.storage_dir.clone()) {
            Ok(s) => {
                if let Some(raw) = s.load_plans_json() {
                    if let Ok(loaded) = serde_json::from_str::<Vec<AgentPlan>>(&raw) {
                        plans = loaded;
                    }
                }
                if !s.chain_valid() {
                    store_error = s.chain_error().to_string();
                }
                Some(s)
            }
            Err(err) => {
                store_error = err;
                None
            }
        };

        Self {
            config,
            system_conn,
            state: Arc::new(Mutex::new(AgentState {
                plans,
                audit,
                store_error,
                blocked_shell_attempts: 0,
                blocked_protected_path_attempts: 0,
                blocked_unapproved_attempts: 0,
                executed_steps_total: 0,
            })),
        }
    }

    fn persist_plans_locked(st: &mut AgentState) {
        if let Some(ref store) = st.audit {
            if let Ok(bytes) = serde_json::to_vec_pretty(&st.plans) {
                if let Err(err) = store.save_plans_json(&bytes) {
                    st.store_error = err;
                }
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn record_audit_locked(
        st: &mut AgentState,
        event_type: &str,
        plan_id: &str,
        step_id: &str,
        agent_profile: &str,
        risk_level: &str,
        outcome: &str,
        detail: &str,
    ) -> String {
        let Some(ref mut store) = st.audit else {
            return String::new();
        };
        match store.append(
            event_type,
            plan_id,
            step_id,
            agent_profile,
            risk_level,
            outcome,
            detail,
        ) {
            Ok(entry) => entry.entry_sha256,
            Err(err) => {
                st.store_error = err;
                String::new()
            }
        }
    }

    async fn build_agent_status(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let health = probe_downstream_health(&self.system_conn, &self.config).await;

        let data = {
            let guard = self
                .state
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Agent state lock poisoned".to_string()))?;

            let (audit_dir, strict_perms, chain_valid, entry_count, head_sha) =
                if let Some(ref a) = guard.audit {
                    let v = a.verify_chain();
                    (
                        a.storage_dir().display().to_string(),
                        a.verify_strict_permissions(),
                        v.valid,
                        v.entry_count,
                        v.head_sha256,
                    )
                } else {
                    (
                        self.config.storage_dir.display().to_string(),
                        false,
                        false,
                        0,
                        String::new(),
                    )
                };

            let pending_approvals: u64 = guard
                .plans
                .iter()
                .filter(|p| p.status == "proposed" || p.status == "awaiting-approval")
                .flat_map(|p| p.steps.iter())
                .filter(|s| s.requires_approval && s.approval_status == "pending")
                .count() as u64;

            let mut d = HashMap::new();
            d.insert("agent_version".to_string(), OwnedValue::from(Str::from(AGENT_VERSION)));
            d.insert("service_name".to_string(), OwnedValue::from(Str::from(AGENT_BUS_NAME)));
            d.insert("fail_closed".to_string(), OwnedValue::from(true));
            d.insert("raw_shell_execution_enabled".to_string(), OwnedValue::from(false));
            d.insert("rollback_execution_enabled".to_string(), OwnedValue::from(false));
            d.insert("repair_execution_enabled".to_string(), OwnedValue::from(false));
            d.insert("external_telemetry_enabled".to_string(), OwnedValue::from(false));
            d.insert("zekans_code_aligned".to_string(), OwnedValue::from(true));
            d.insert("loopback_only".to_string(), OwnedValue::from(health.loopback_only));

            d.insert("runtime_reachable".to_string(), OwnedValue::from(health.runtime_reachable));
            d.insert("daemon_reachable".to_string(), OwnedValue::from(health.daemon_reachable));
            d.insert("policy_reachable".to_string(), OwnedValue::from(health.policy_reachable));
            d.insert("recovery_reachable".to_string(), OwnedValue::from(health.recovery_reachable));
            d.insert(
                "runtime_state".to_string(),
                OwnedValue::from(Str::from(health.runtime_state.as_str())),
            );
            d.insert("vram_locked".to_string(), OwnedValue::from(health.vram_locked));
            d.insert("active_vram_mb".to_string(), OwnedValue::from(health.active_vram_mb));
            d.insert(
                "hardware_profile".to_string(),
                OwnedValue::from(Str::from(health.hardware_profile.as_str())),
            );
            d.insert(
                "active_model_id".to_string(),
                OwnedValue::from(Str::from(health.active_model_id.as_str())),
            );
            d.insert(
                "downstream_error".to_string(),
                OwnedValue::from(Str::from(health.last_error.as_str())),
            );

            d.insert("audit_dir".to_string(), OwnedValue::from(Str::from(audit_dir.as_str())));
            d.insert("audit_permissions_strict".to_string(), OwnedValue::from(strict_perms));
            d.insert("audit_chain_valid".to_string(), OwnedValue::from(chain_valid));
            d.insert("audit_entry_count".to_string(), OwnedValue::from(entry_count));
            d.insert("audit_head_sha256".to_string(), OwnedValue::from(Str::from(head_sha.as_str())));
            d.insert(
                "store_error".to_string(),
                OwnedValue::from(Str::from(guard.store_error.as_str())),
            );
            d.insert("plan_count".to_string(), OwnedValue::from(guard.plans.len() as u64));
            d.insert("pending_approval_steps".to_string(), OwnedValue::from(pending_approvals));
            d.insert(
                "blocked_shell_attempts".to_string(),
                OwnedValue::from(guard.blocked_shell_attempts),
            );
            d.insert(
                "blocked_protected_path_attempts".to_string(),
                OwnedValue::from(guard.blocked_protected_path_attempts),
            );
            d.insert(
                "blocked_unapproved_attempts".to_string(),
                OwnedValue::from(guard.blocked_unapproved_attempts),
            );
            d.insert(
                "executed_steps_total".to_string(),
                OwnedValue::from(guard.executed_steps_total),
            );
            d
        };

        let ready = health.daemon_reachable && health.policy_reachable && health.runtime_reachable;
        let code = if ready {
            "ok"
        } else {
            "degraded-downstream-unavailable"
        };
        let msg = if ready {
            "ZCI Komut Ajanı & ZEKANS planlayıcısı hazır; ham kabuk kapalı, tüm adımlar politika ve onay geçidine bağlı."
        } else {
            "ZCI Komut Ajanı fail-closed beklemede; ZCI Runtime/Daemon/Security servis bağlantısı bekleniyor."
        };
        Ok(envelope(true, code, msg, &operation_id, data))
    }
}

#[interface(name = "org.zethropol.ZciAgent.v1")]
impl ZciAgentService {
    /// 13.01 & 13.04 — Transform natural-language goal into a structured, non-executable allowlisted plan.
    async fn propose_plan(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        goal: String,
        agent_profile: String,
        workspace_dir: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();

        if has_control_or_nul(&goal)
            || has_control_or_nul(&agent_profile)
            || has_control_or_nul(&workspace_dir)
        {
            return Ok(envelope(
                false,
                "invalid-request",
                "Kontrol karakteri veya NUL bayt içeren plan isteği reddedildi.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let trimmed_goal = goal.trim();
        if trimmed_goal.is_empty() {
            return Ok(envelope(
                false,
                "empty-goal",
                "Boş görev hedefi için plan oluşturulamaz.",
                &operation_id,
                HashMap::new(),
            ));
        }
        if trimmed_goal.len() > MAX_GOAL_BYTES {
            return Ok(envelope(
                false,
                "goal-too-large",
                "Görev metni azami 8 KiB sınırını aşıyor.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let profile = if agent_profile.trim().is_empty() {
            DEFAULT_AGENT_PROFILE.to_string()
        } else {
            agent_profile.trim().to_string()
        };
        if !is_supported_profile(&profile) {
            return Ok(envelope(
                false,
                "unknown-agent-profile",
                "Yalnızca 'zekans-code' veya 'zci-command' ajan profilleri desteklenir.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let clean_workspace = if workspace_dir.trim().is_empty() {
            String::new()
        } else {
            match validate_target_path(workspace_dir.trim()) {
                Ok(p) => p.display().to_string(),
                Err(err) => {
                    if let Ok(mut g) = self.state.lock() {
                        g.blocked_protected_path_attempts += 1;
                        Self::record_audit_locked(
                            &mut g,
                            "protected_path_blocked",
                            "",
                            "",
                            &profile,
                            "ApprovalRequired",
                            "deny",
                            &err,
                        );
                    }
                    return Ok(envelope(
                        false,
                        "protected-path-denied",
                        &err,
                        &operation_id,
                        HashMap::new(),
                    ));
                }
            }
        };

        // 1. 13.01 & 13.02 Hard fail-closed safety inspection BEFORE touching runtime or daemon!
        match inspect_text_safety(trimmed_goal) {
            SafetyVerdict::Allow => {}
            SafetyVerdict::DenyShellInjection { pattern, reason } => {
                if let Ok(mut g) = self.state.lock() {
                    g.blocked_shell_attempts += 1;
                    Self::record_audit_locked(
                        &mut g,
                        "shell_injection_blocked",
                        "",
                        "",
                        &profile,
                        "ApprovalRequired",
                        "deny",
                        &format!("pattern={pattern}: {reason}"),
                    );
                }
                let mut d = HashMap::new();
                d.insert("blocked_pattern".to_string(), OwnedValue::from(Str::from(pattern.as_str())));
                d.insert("raw_shell_execution_enabled".to_string(), OwnedValue::from(false));
                return Ok(envelope(
                    false,
                    "shell-injection-blocked",
                    &reason,
                    &operation_id,
                    d,
                ));
            }
            SafetyVerdict::DenyProtectedPath { pattern, reason } => {
                if let Ok(mut g) = self.state.lock() {
                    g.blocked_protected_path_attempts += 1;
                    Self::record_audit_locked(
                        &mut g,
                        "protected_path_blocked",
                        "",
                        "",
                        &profile,
                        "ApprovalRequired",
                        "deny",
                        &format!("pattern={pattern}: {reason}"),
                    );
                }
                let mut d = HashMap::new();
                d.insert("blocked_pattern".to_string(), OwnedValue::from(Str::from(pattern.as_str())));
                return Ok(envelope(
                    false,
                    "protected-path-denied",
                    &reason,
                    &operation_id,
                    d,
                ));
            }
            SafetyVerdict::DenyPolicyTamper { pattern, reason } => {
                if let Ok(mut g) = self.state.lock() {
                    g.blocked_shell_attempts += 1;
                    Self::record_audit_locked(
                        &mut g,
                        "policy_tamper_blocked",
                        "",
                        "",
                        &profile,
                        "ApprovalRequired",
                        "deny",
                        &format!("pattern={pattern}: {reason}"),
                    );
                }
                let mut d = HashMap::new();
                d.insert("blocked_pattern".to_string(), OwnedValue::from(Str::from(pattern.as_str())));
                d.insert("rollback_execution_enabled".to_string(), OwnedValue::from(false));
                d.insert("repair_execution_enabled".to_string(), OwnedValue::from(false));
                return Ok(envelope(
                    false,
                    "policy-tamper-blocked",
                    &reason,
                    &operation_id,
                    d,
                ));
            }
        }

        // 2. Verify downstream Security Policy Engine + ZCI Daemon reachability (fail-closed)
        let health = probe_downstream_health(&self.system_conn, &self.config).await;
        if !health.daemon_reachable || !health.policy_reachable {
            return Ok(envelope(
                false,
                "zci-daemon-unavailable",
                "Fail-closed engel: ZCI Daemon veya Security Policy Engine erişilemiyor.",
                &operation_id,
                HashMap::new(),
            ));
        }

        // 3. Mandatory untrusted sealing of user goal (`executable=false`, `policy_authority="none"`)
        let sealed_goal = match seal_untrusted_via_daemon(
            &self.system_conn,
            &self.config,
            &profile,
            "user_text",
            trimmed_goal,
        )
        .await
        {
            Ok(s) => s,
            Err(err) => {
                return Ok(envelope(
                    false,
                    "untrusted-seal-failed",
                    &err,
                    &operation_id,
                    HashMap::new(),
                ));
            }
        };
        if sealed_goal.self_modification_injection || sealed_goal.injection_detected {
            if let Ok(mut g) = self.state.lock() {
                g.blocked_shell_attempts += 1;
                Self::record_audit_locked(
                    &mut g,
                    "policy_injection_blocked",
                    "",
                    "",
                    &profile,
                    "ApprovalRequired",
                    "deny",
                    "Security Policy Engine flagged injection/self-modification in planning goal.",
                );
            }
            return Ok(envelope(
                false,
                "prompt-injection-blocked",
                "Security Policy Engine plan hedefinde enjeksiyon/yetki aşımı tespit etti; reddedildi.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let safe_goal_body = extract_display_text_from_envelope(&sealed_goal.wrapped_payload);

        // 4. Generate structured allowlisted steps via `zci-runtime` (or deterministic safe fallback in Standby)
        let mut planner_source = "zci-runtime-structured".to_string();
        let mut output_envelope_id = String::new();
        let mut plan_summary_and_steps: Option<(String, Vec<crate::planner::PlanStep>)> = None;

        if health.runtime_reachable && !health.active_model_id.is_empty() {
            let prompt = build_planning_prompt(&safe_goal_body, &profile, &clean_workspace);
            match invoke_runtime_planner(
                &self.system_conn,
                &self.config,
                &profile,
                &health.active_model_id,
                &prompt,
            )
            .await
            {
                Ok(RuntimePlanInference::Completed {
                    output_wrapped_payload,
                    output_envelope_id: out_env,
                    output_executable,
                    output_policy_authority,
                }) => {
                    if output_executable || output_policy_authority != "none" {
                        return Ok(envelope(
                            false,
                            "output-isolation-violation",
                            "Fail-closed: model plan çıktısı executable=false / policy_authority=none sözleşmesini ihlal etti.",
                            &operation_id,
                            HashMap::new(),
                        ));
                    }
                    output_envelope_id = out_env;
                    let raw_plan = extract_display_text_from_envelope(&output_wrapped_payload);
                    match parse_and_validate_model_plan(
                        &raw_plan,
                        &profile,
                        &clean_workspace,
                        &self.config.home_dir,
                        &self.config.extra_workspace_roots,
                    ) {
                        Ok(parsed) => {
                            plan_summary_and_steps = Some(parsed);
                        }
                        Err(validation_err) => {
                            // If model proposed a raw shell command or protected path, FAIL CLOSED immediately!
                            if validation_err.contains("kabuk")
                                || validation_err.contains("Korumalı")
                                || validation_err.contains("izinli çalışma alanı")
                                || validation_err.contains("default-deny")
                            {
                                if let Ok(mut g) = self.state.lock() {
                                    g.blocked_shell_attempts += 1;
                                    Self::record_audit_locked(
                                        &mut g,
                                        "model_plan_rejected",
                                        "",
                                        "",
                                        &profile,
                                        "ApprovalRequired",
                                        "deny",
                                        &validation_err,
                                    );
                                }
                                return Ok(envelope(
                                    false,
                                    "unsafe-model-plan-rejected",
                                    &validation_err,
                                    &operation_id,
                                    HashMap::new(),
                                ));
                            }
                            planner_source = "deterministic-safe-fallback".to_string();
                        }
                    }
                }
                Ok(RuntimePlanInference::PassiveStandby) => {
                    planner_source = "passive-standby-deterministic".to_string();
                }
                Err((code, msg)) => {
                    if code == "untrusted-injection-blocked" || code == "output-isolation-violation" {
                        return Ok(envelope(false, &code, &msg, &operation_id, HashMap::new()));
                    }
                    planner_source = "deterministic-safe-fallback".to_string();
                }
            }
        } else {
            planner_source = "passive-standby-deterministic".to_string();
        }

        let (summary, steps) = match plan_summary_and_steps {
            Some(pair) => pair,
            None => match build_deterministic_safe_plan(
                &safe_goal_body,
                &profile,
                &clean_workspace,
                &self.config.home_dir,
                &self.config.extra_workspace_roots,
            ) {
                Ok(pair) => pair,
                Err(err) => {
                    return Ok(envelope(
                        false,
                        "plan-validation-failed",
                        &err,
                        &operation_id,
                        HashMap::new(),
                    ));
                }
            },
        };

        // 5. 13.03 Automatic ReadOnly Snapper & Recovery Lock Preflight Check
        let snapper_preflight = match run_recovery_snapper_preflight(&self.system_conn, &self.config).await {
            Ok(report) => report,
            Err(err) => {
                return Ok(envelope(
                    false,
                    "recovery-lock-violation",
                    &err,
                    &operation_id,
                    HashMap::new(),
                ));
            }
        };

        let approval_required_count = steps.iter().filter(|s| s.requires_approval).count() as u32;
        let now = now_unix_secs();
        let plan_id = format!("plan-{}", Uuid::new_v4());
        let initial_status = if approval_required_count > 0 {
            "awaiting-approval"
        } else {
            "proposed"
        };

        let plan = AgentPlan {
            plan_id: plan_id.clone(),
            goal: trimmed_goal.to_string(),
            summary: summary.clone(),
            agent_profile: profile.clone(),
            workspace_dir: clean_workspace,
            status: initial_status.to_string(),
            created_at_unix: now,
            updated_at_unix: now,
            planner_source: planner_source.clone(),
            input_envelope_id: sealed_goal.envelope_id.clone(),
            output_envelope_id,
            executable: false,
            policy_authority: "none".to_string(),
            snapper_preflight: snapper_preflight.clone(),
            steps: steps.clone(),
        };

        let audit_hash = {
            let mut guard = self
                .state
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Agent state lock poisoned".to_string()))?;
            if guard.plans.len() >= MAX_PLANS {
                guard.plans.remove(0);
            }
            guard.plans.push(plan.clone());
            Self::persist_plans_locked(&mut guard);
            Self::record_audit_locked(
                &mut guard,
                "plan_proposed",
                &plan_id,
                "",
                &profile,
                if approval_required_count > 0 {
                    "ApprovalRequired"
                } else {
                    "ReadOnly"
                },
                initial_status,
                &format!(
                    "steps={} approval_required={} planner={} recovery={}",
                    steps.len(),
                    approval_required_count,
                    planner_source,
                    snapper_preflight.recovery_status
                ),
            )
        };

        let _ = Self::plan_proposed(
            &emitter,
            plan_id.clone(),
            profile.clone(),
            steps.len() as u32,
            approval_required_count,
        )
        .await;

        for st in &steps {
            if st.requires_approval {
                let _ = Self::step_approval_required(
                    &emitter,
                    plan_id.clone(),
                    st.step_id.clone(),
                    st.action_kind.clone(),
                    st.risk_level.clone(),
                    st.target.clone(),
                    st.description.clone(),
                )
                .await;
            }
        }

        let steps_json: Vec<String> = steps
            .iter()
            .filter_map(|s| serde_json::to_string(s).ok())
            .collect();
        let plan_json = serde_json::to_string(&plan).unwrap_or_default();

        let mut d = HashMap::new();
        d.insert("plan_id".to_string(), OwnedValue::from(Str::from(plan_id.as_str())));
        d.insert("status".to_string(), OwnedValue::from(Str::from(initial_status)));
        d.insert("summary".to_string(), OwnedValue::from(Str::from(summary.as_str())));
        d.insert("agent_profile".to_string(), OwnedValue::from(Str::from(profile.as_str())));
        d.insert(
            "planner_source".to_string(),
            OwnedValue::from(Str::from(planner_source.as_str())),
        );
        d.insert("step_count".to_string(), OwnedValue::from(steps.len() as u32));
        d.insert(
            "approval_required_count".to_string(),
            OwnedValue::from(approval_required_count),
        );
        d.insert("executable".to_string(), OwnedValue::from(false));
        d.insert("policy_authority".to_string(), OwnedValue::from(Str::from("none")));
        d.insert(
            "snapper_preflight_checked".to_string(),
            OwnedValue::from(snapper_preflight.checked),
        );
        d.insert(
            "snapper_preflight_summary".to_string(),
            OwnedValue::from(Str::from(snapper_preflight.summary.as_str())),
        );
        d.insert("rollback_execution_enabled".to_string(), OwnedValue::from(false));
        d.insert("repair_execution_enabled".to_string(), OwnedValue::from(false));
        d.insert("audit_hash".to_string(), OwnedValue::from(Str::from(audit_hash.as_str())));
        d.insert("steps_json".to_string(), vec_to_owned(&steps_json));
        d.insert("plan_json".to_string(), OwnedValue::from(Str::from(plan_json.as_str())));

        Ok(envelope(
            true,
            "ok",
            if approval_required_count > 0 {
                "Plan oluşturuldu; yazma/değişiklik adımları için kullanıcı onayı bekleniyor."
            } else {
                "Salt-okunur doğrulama planı oluşturuldu."
            },
            &operation_id,
            d,
        ))
    }

    /// 13.02 & 13.04 — Explicitly approve or reject a step requiring user confirmation.
    async fn approve_step(
        &self,
        plan_id: String,
        step_id: String,
        approved: bool,
        reason: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        if has_control_or_nul(&plan_id) || has_control_or_nul(&step_id) || has_control_or_nul(&reason) {
            return Ok(envelope(
                false,
                "invalid-request",
                "Kontrol karakteri içeren onay isteği reddedildi.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let mut guard = self
            .state
            .lock()
            .map_err(|_| zbus::fdo::Error::Failed("Agent state lock poisoned".to_string()))?;

        let Some(plan_idx) = guard.plans.iter().position(|p| p.plan_id == plan_id.trim()) else {
            return Ok(envelope(
                false,
                "plan-not-found",
                "Belirtilen plan bulunamadı.",
                &operation_id,
                HashMap::new(),
            ));
        };

        if guard.plans[plan_idx].status == "cancelled" || guard.plans[plan_idx].status == "completed" {
            return Ok(envelope(
                false,
                "plan-closed",
                "Tamamlanmış veya iptal edilmiş bir planın adımı değiştirilemez.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let Some(step_idx) = guard.plans[plan_idx]
            .steps
            .iter()
            .position(|s| s.step_id == step_id.trim())
        else {
            return Ok(envelope(
                false,
                "step-not-found",
                "Belirtilen plan adımı bulunamadı.",
                &operation_id,
                HashMap::new(),
            ));
        };

        let new_status = if approved { "approved" } else { "rejected" };
        let clean_note: String = reason.trim().chars().take(160).collect();
        let profile = guard.plans[plan_idx].agent_profile.clone();
        let risk = guard.plans[plan_idx].steps[step_idx].risk_level.clone();
        let target = guard.plans[plan_idx].steps[step_idx].target.clone();

        {
            let st = &mut guard.plans[plan_idx].steps[step_idx];
            st.approval_status = new_status.to_string();
            st.approval_note = clean_note.clone();
        }
        guard.plans[plan_idx].updated_at_unix = now_unix_secs();

        let remaining_pending = guard.plans[plan_idx]
            .steps
            .iter()
            .filter(|s| s.requires_approval && s.approval_status == "pending")
            .count() as u32;
        if remaining_pending == 0 {
            guard.plans[plan_idx].status = "ready".to_string();
        }
        let plan_status = guard.plans[plan_idx].status.clone();

        let audit_hash = Self::record_audit_locked(
            &mut guard,
            if approved {
                "step_approved"
            } else {
                "step_rejected"
            },
            plan_id.trim(),
            step_id.trim(),
            &profile,
            &risk,
            new_status,
            &format!("target={target} note={clean_note}"),
        );
        guard.plans[plan_idx].steps[step_idx].audit_hash = audit_hash.clone();
        Self::persist_plans_locked(&mut guard);

        let mut d = HashMap::new();
        d.insert("plan_id".to_string(), OwnedValue::from(Str::from(plan_id.trim())));
        d.insert("step_id".to_string(), OwnedValue::from(Str::from(step_id.trim())));
        d.insert(
            "approval_status".to_string(),
            OwnedValue::from(Str::from(new_status)),
        );
        d.insert("plan_status".to_string(), OwnedValue::from(Str::from(plan_status.as_str())));
        d.insert(
            "remaining_pending_approvals".to_string(),
            OwnedValue::from(remaining_pending),
        );
        d.insert("audit_hash".to_string(), OwnedValue::from(Str::from(audit_hash.as_str())));

        Ok(envelope(
            true,
            "ok",
            if approved {
                "Adım kullanıcı tarafından onaylandı ve denetim zincirine işlendi."
            } else {
                "Adım kullanıcı tarafından reddedildi ve yürütme dışı bırakıldı."
            },
            &operation_id,
            d,
        ))
    }

    /// 13.02, 13.03, 13.04 — Execute an approved plan strictly step-by-step through `zci-daemon`.
    /// Strictly fails closed if any `ApprovalRequired` step is still `pending`!
    async fn execute_approved_plan(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        plan_id: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let pid = plan_id.trim().to_string();

        // 1. Validate plan exists, is not cancelled/completed, and has ZERO pending approvals!
        let (plan_snapshot, has_mutating_approved) = {
            let mut guard = self
                .state
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Agent state lock poisoned".to_string()))?;
            let Some(plan_idx) = guard.plans.iter().position(|p| p.plan_id == pid) else {
                return Ok(envelope(
                    false,
                    "plan-not-found",
                    "Yürütülecek plan bulunamadı.",
                    &operation_id,
                    HashMap::new(),
                ));
            };
            let plan = &guard.plans[plan_idx];
            if plan.status == "cancelled" {
                return Ok(envelope(
                    false,
                    "plan-cancelled",
                    "İptal edilmiş plan yürütülemez.",
                    &operation_id,
                    HashMap::new(),
                ));
            }
            if plan.status == "completed" {
                return Ok(envelope(
                    false,
                    "plan-already-completed",
                    "Bu plan zaten yürütülüp tamamlandı.",
                    &operation_id,
                    HashMap::new(),
                ));
            }

            let pending_ids: Vec<String> = plan
                .steps
                .iter()
                .filter(|s| s.requires_approval && s.approval_status == "pending")
                .map(|s| s.step_id.clone())
                .collect();
            if !pending_ids.is_empty() {
                let profile = plan.agent_profile.clone();
                guard.blocked_unapproved_attempts += 1;
                Self::record_audit_locked(
                    &mut guard,
                    "unapproved_execution_blocked",
                    &pid,
                    &pending_ids.join(","),
                    &profile,
                    "ApprovalRequired",
                    "deny",
                    "Attempted ExecuteApprovedPlan while ApprovalRequired steps remained pending.",
                );
                let mut d = HashMap::new();
                d.insert("plan_id".to_string(), OwnedValue::from(Str::from(pid.as_str())));
                d.insert("pending_step_ids".to_string(), vec_to_owned(&pending_ids));
                return Ok(envelope(
                    false,
                    "unapproved-steps-pending",
                    "Fail-closed engel: Onay bekleyen yazma/değişiklik adımları ApproveStep ile onaylanmadan plan yürütülemez.",
                    &operation_id,
                    d,
                ));
            }

            let has_mut = plan
                .steps
                .iter()
                .any(|s| s.requires_approval && s.approval_status == "approved");
            guard.plans[plan_idx].status = "running".to_string();
            (guard.plans[plan_idx].clone(), has_mut)
        };

        // 2. 13.03 Mandatory Snapper & Recovery Lock Preflight right before any approved mutating execution!
        let mut fresh_preflight = plan_snapshot.snapper_preflight.clone();
        if has_mutating_approved {
            fresh_preflight = match run_recovery_snapper_preflight(&self.system_conn, &self.config).await {
                Ok(rep) => {
                    if let Ok(mut g) = self.state.lock() {
                        Self::record_audit_locked(
                            &mut g,
                            "snapper_preflight",
                            &pid,
                            "",
                            &plan_snapshot.agent_profile,
                            "ReadOnly",
                            "pass",
                            &rep.summary,
                        );
                    }
                    rep
                }
                Err(err) => {
                    if let Ok(mut g) = self.state.lock() {
                        if let Some(p) = g.plans.iter_mut().find(|p| p.plan_id == pid) {
                            p.status = "failed".to_string();
                        }
                        Self::persist_plans_locked(&mut g);
                    }
                    return Ok(envelope(
                        false,
                        "recovery-lock-violation",
                        &err,
                        &operation_id,
                        HashMap::new(),
                    ));
                }
            };
        }

        // 3. Execute steps sequentially through `zci-daemon`
        let mut completed_steps = 0u32;
        let mut denied_steps = 0u32;
        let mut updated_steps = plan_snapshot.steps.clone();

        for step in &mut updated_steps {
            if step.requires_approval && step.approval_status == "rejected" {
                step.execution_status = "skipped-rejected".to_string();
                step.output_preview = "Kullanıcı tarafından reddedildiği için atlandı.".to_string();
                let hash = if let Ok(mut g) = self.state.lock() {
                    Self::record_audit_locked(
                        &mut g,
                        "step_skipped",
                        &pid,
                        &step.step_id,
                        &plan_snapshot.agent_profile,
                        &step.risk_level,
                        "skipped-rejected",
                        "Skipped user-rejected step.",
                    )
                } else {
                    String::new()
                };
                step.audit_hash = hash.clone();
                let _ = Self::step_completed(
                    &emitter,
                    pid.clone(),
                    step.step_id.clone(),
                    "skipped-rejected".to_string(),
                    step.risk_level.clone(),
                    "USER_REJECTED".to_string(),
                    hash,
                )
                .await;
                continue;
            }

            let exec_outcome = execute_step_via_daemon(&self.system_conn, &self.config, step).await;
            match exec_outcome {
                Ok(res) => {
                    step.policy_rule_id = res.policy_rule_id.clone();
                    step.policy_action_class = res.policy_action_class.clone();
                    step.output_preview = res.output_preview.clone();
                    step.executable = res.executable;
                    step.policy_authority = res.policy_authority.clone();
                    if res.succeeded {
                        step.execution_status = "completed".to_string();
                        completed_steps += 1;
                    } else {
                        step.execution_status = "denied".to_string();
                        denied_steps += 1;
                    }
                    let hash = if let Ok(mut g) = self.state.lock() {
                        if res.succeeded {
                            g.executed_steps_total += 1;
                        }
                        Self::record_audit_locked(
                            &mut g,
                            if res.succeeded {
                                "step_executed"
                            } else {
                                "step_denied"
                            },
                            &pid,
                            &step.step_id,
                            &plan_snapshot.agent_profile,
                            &step.risk_level,
                            &res.code,
                            &format!(
                                "action={} target={} rule={} class={}",
                                step.action_kind,
                                step.target,
                                res.policy_rule_id,
                                res.policy_action_class
                            ),
                        )
                    } else {
                        String::new()
                    };
                    step.audit_hash = hash.clone();
                    let _ = Self::step_completed(
                        &emitter,
                        pid.clone(),
                        step.step_id.clone(),
                        step.execution_status.clone(),
                        step.risk_level.clone(),
                        step.policy_rule_id.clone(),
                        hash,
                    )
                    .await;
                }
                Err(err) => {
                    step.execution_status = "failed".to_string();
                    step.output_preview = err.clone();
                    denied_steps += 1;
                    let hash = if let Ok(mut g) = self.state.lock() {
                        Self::record_audit_locked(
                            &mut g,
                            "step_failed",
                            &pid,
                            &step.step_id,
                            &plan_snapshot.agent_profile,
                            &step.risk_level,
                            "error",
                            &err,
                        )
                    } else {
                        String::new()
                    };
                    step.audit_hash = hash.clone();
                    let _ = Self::step_completed(
                        &emitter,
                        pid.clone(),
                        step.step_id.clone(),
                        "failed".to_string(),
                        step.risk_level.clone(),
                        "EXECUTION_ERROR".to_string(),
                        hash,
                    )
                    .await;
                }
            }
        }

        let final_status = if denied_steps == 0 {
            "completed"
        } else if completed_steps > 0 {
            "completed-with-denials"
        } else {
            "failed"
        };

        let (audit_head, steps_json) = {
            let mut guard = self
                .state
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Agent state lock poisoned".to_string()))?;
            if let Some(p) = guard.plans.iter_mut().find(|p| p.plan_id == pid) {
                p.steps = updated_steps.clone();
                p.snapper_preflight = fresh_preflight.clone();
                p.status = final_status.to_string();
                p.updated_at_unix = now_unix_secs();
            }
            let head = Self::record_audit_locked(
                &mut guard,
                "plan_finished",
                &pid,
                "",
                &plan_snapshot.agent_profile,
                if has_mutating_approved {
                    "ApprovalRequired"
                } else {
                    "ReadOnly"
                },
                final_status,
                &format!("completed={completed_steps} denied={denied_steps}"),
            );
            Self::persist_plans_locked(&mut guard);
            let sj: Vec<String> = updated_steps
                .iter()
                .filter_map(|s| serde_json::to_string(s).ok())
                .collect();
            (head, sj)
        };

        let _ = Self::plan_finished(
            &emitter,
            pid.clone(),
            final_status.to_string(),
            completed_steps,
            denied_steps,
            audit_head.clone(),
        )
        .await;

        let mut d = HashMap::new();
        d.insert("plan_id".to_string(), OwnedValue::from(Str::from(pid.as_str())));
        d.insert("status".to_string(), OwnedValue::from(Str::from(final_status)));
        d.insert("completed_steps".to_string(), OwnedValue::from(completed_steps));
        d.insert("denied_steps".to_string(), OwnedValue::from(denied_steps));
        d.insert(
            "audit_head_sha256".to_string(),
            OwnedValue::from(Str::from(audit_head.as_str())),
        );
        d.insert("rollback_execution_enabled".to_string(), OwnedValue::from(false));
        d.insert("repair_execution_enabled".to_string(), OwnedValue::from(false));
        d.insert(
            "snapper_preflight_summary".to_string(),
            OwnedValue::from(Str::from(fresh_preflight.summary.as_str())),
        );
        d.insert("steps_json".to_string(), vec_to_owned(&steps_json));

        Ok(envelope(
            denied_steps == 0,
            if denied_steps == 0 {
                "ok"
            } else {
                "plan-execution-denied"
            },
            if denied_steps == 0 {
                "Onaylı plan tüm güvenlik ve politika geçitlerinden geçerek tamamlandı."
            } else {
                "Plan yürütmesinde politika veya güvenlik engeline takılan adımlar oldu."
            },
            &operation_id,
            d,
        ))
    }

    /// 13.04 — Cancel a proposed or awaiting-approval plan and record immutable audit event.
    async fn cancel_plan(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        plan_id: String,
        reason: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let pid = plan_id.trim().to_string();
        let clean_reason: String = reason.trim().chars().take(160).collect();

        let (audit_head, profile) = {
            let mut guard = self
                .state
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Agent state lock poisoned".to_string()))?;
            let Some(plan_idx) = guard.plans.iter().position(|p| p.plan_id == pid) else {
                return Ok(envelope(
                    false,
                    "plan-not-found",
                    "İptal edilecek plan bulunamadı.",
                    &operation_id,
                    HashMap::new(),
                ));
            };
            let profile = guard.plans[plan_idx].agent_profile.clone();
            guard.plans[plan_idx].status = "cancelled".to_string();
            guard.plans[plan_idx].updated_at_unix = now_unix_secs();
            let head = Self::record_audit_locked(
                &mut guard,
                "plan_cancelled",
                &pid,
                "",
                &profile,
                "ReadOnly",
                "cancelled",
                if clean_reason.is_empty() {
                    "Cancelled by user."
                } else {
                    clean_reason.as_str()
                },
            );
            Self::persist_plans_locked(&mut guard);
            (head, profile)
        };

        let _ = Self::plan_finished(
            &emitter,
            pid.clone(),
            "cancelled".to_string(),
            0,
            0,
            audit_head.clone(),
        )
        .await;

        let mut d = HashMap::new();
        d.insert("plan_id".to_string(), OwnedValue::from(Str::from(pid.as_str())));
        d.insert("status".to_string(), OwnedValue::from(Str::from("cancelled")));
        d.insert("agent_profile".to_string(), OwnedValue::from(Str::from(profile.as_str())));
        d.insert(
            "audit_head_sha256".to_string(),
            OwnedValue::from(Str::from(audit_head.as_str())),
        );
        Ok(envelope(
            true,
            "ok",
            "Plan iptal edildi ve denetim kaydına işlendi.",
            &operation_id,
            d,
        ))
    }

    /// 13.04 — Retrieve full structured status and step audit trail for a plan.
    async fn get_plan_status(
        &self,
        plan_id: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let pid = plan_id.trim();
        let guard = self
            .state
            .lock()
            .map_err(|_| zbus::fdo::Error::Failed("Agent state lock poisoned".to_string()))?;

        let Some(plan) = guard.plans.iter().find(|p| p.plan_id == pid) else {
            return Ok(envelope(
                false,
                "plan-not-found",
                "Plan bulunamadı.",
                &operation_id,
                HashMap::new(),
            ));
        };

        let steps_json: Vec<String> = plan
            .steps
            .iter()
            .filter_map(|s| serde_json::to_string(s).ok())
            .collect();
        let plan_json = serde_json::to_string(plan).unwrap_or_default();
        let pending = plan
            .steps
            .iter()
            .filter(|s| s.requires_approval && s.approval_status == "pending")
            .count() as u32;

        let mut d = HashMap::new();
        d.insert("plan_id".to_string(), OwnedValue::from(Str::from(plan.plan_id.as_str())));
        d.insert("status".to_string(), OwnedValue::from(Str::from(plan.status.as_str())));
        d.insert("summary".to_string(), OwnedValue::from(Str::from(plan.summary.as_str())));
        d.insert("goal".to_string(), OwnedValue::from(Str::from(plan.goal.as_str())));
        d.insert(
            "agent_profile".to_string(),
            OwnedValue::from(Str::from(plan.agent_profile.as_str())),
        );
        d.insert("step_count".to_string(), OwnedValue::from(plan.steps.len() as u32));
        d.insert("pending_approvals".to_string(), OwnedValue::from(pending));
        d.insert(
            "snapper_preflight_summary".to_string(),
            OwnedValue::from(Str::from(plan.snapper_preflight.summary.as_str())),
        );
        d.insert("rollback_execution_enabled".to_string(), OwnedValue::from(false));
        d.insert("repair_execution_enabled".to_string(), OwnedValue::from(false));
        d.insert("steps_json".to_string(), vec_to_owned(&steps_json));
        d.insert("plan_json".to_string(), OwnedValue::from(Str::from(plan_json.as_str())));

        Ok(envelope(true, "ok", "Plan durumu yüklendi.", &operation_id, d))
    }

    /// List all local agent plans (summary metadata).
    async fn list_plans(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let guard = self
            .state
            .lock()
            .map_err(|_| zbus::fdo::Error::Failed("Agent state lock poisoned".to_string()))?;

        let plans_json: Vec<String> = guard
            .plans
            .iter()
            .rev()
            .map(|p| {
                json!({
                    "plan_id": p.plan_id,
                    "goal": p.goal,
                    "summary": p.summary,
                    "agent_profile": p.agent_profile,
                    "status": p.status,
                    "step_count": p.steps.len(),
                    "created_at_unix": p.created_at_unix,
                    "updated_at_unix": p.updated_at_unix,
                })
                .to_string()
            })
            .collect();

        let mut d = HashMap::new();
        d.insert("plan_count".to_string(), OwnedValue::from(guard.plans.len() as u64));
        d.insert("plans_json".to_string(), vec_to_owned(&plans_json));
        Ok(envelope(true, "ok", "Yerel ajan planları listelendi.", &operation_id, d))
    }

    /// 13.06 — Retrieve recent hash-chained audit entries and full chain integrity status.
    async fn get_audit_trail(&self, limit: u32) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let guard = self
            .state
            .lock()
            .map_err(|_| zbus::fdo::Error::Failed("Agent state lock poisoned".to_string()))?;

        let Some(ref store) = guard.audit else {
            return Ok(envelope(
                false,
                "audit-unavailable",
                "Denetim günlüğü deposu erişilemiyor.",
                &operation_id,
                HashMap::new(),
            ));
        };
        let verification = store.verify_chain();
        let entries = store.read_recent(limit.clamp(1, 200) as usize);
        let entries_json: Vec<String> = entries
            .iter()
            .filter_map(|e| serde_json::to_string(e).ok())
            .collect();

        let mut d = HashMap::new();
        d.insert("chain_valid".to_string(), OwnedValue::from(verification.valid));
        d.insert("entry_count".to_string(), OwnedValue::from(verification.entry_count));
        d.insert(
            "head_sha256".to_string(),
            OwnedValue::from(Str::from(verification.head_sha256.as_str())),
        );
        d.insert(
            "chain_error".to_string(),
            OwnedValue::from(Str::from(verification.error.as_str())),
        );
        d.insert(
            "permissions_strict".to_string(),
            OwnedValue::from(store.verify_strict_permissions()),
        );
        d.insert("entries_json".to_string(), vec_to_owned(&entries_json));

        Ok(envelope(
            verification.valid,
            if verification.valid {
                "ok"
            } else {
                "audit-chain-tampered"
            },
            if verification.valid {
                "Hash-zincirli denetim günlüğü doğrulandı."
            } else {
                "UYARI: Denetim günlüğü hash zinciri bozulmuş!"
            },
            &operation_id,
            d,
        ))
    }

    /// 13.04 — Full agent health, runtime standby, security locks, and audit integrity snapshot.
    async fn get_agent_status(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        self.build_agent_status().await
    }

    async fn get_state(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        self.build_agent_status().await
    }

    // 13.04 Signals
    #[zbus(signal)]
    pub async fn plan_proposed(
        emitter: &SignalEmitter<'_>,
        plan_id: String,
        agent_profile: String,
        step_count: u32,
        approval_required_count: u32,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    pub async fn step_approval_required(
        emitter: &SignalEmitter<'_>,
        plan_id: String,
        step_id: String,
        action_kind: String,
        risk_level: String,
        target: String,
        summary: String,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    pub async fn step_completed(
        emitter: &SignalEmitter<'_>,
        plan_id: String,
        step_id: String,
        status: String,
        risk_level: String,
        policy_rule_id: String,
        audit_hash: String,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    pub async fn plan_finished(
        emitter: &SignalEmitter<'_>,
        plan_id: String,
        status: String,
        completed_steps: u32,
        denied_steps: u32,
        audit_head_sha256: String,
    ) -> zbus::Result<()>;
}
