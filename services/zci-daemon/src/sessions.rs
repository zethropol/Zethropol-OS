use crate::config::{
    DaemonConfig, DEFAULT_PASSIVE_MODEL_ID, DEFAULT_PASSIVE_MODEL_PROFILE,
};
use crate::policy_client::{now_unix_secs, PolicyHealthSnapshot};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, HashMap, VecDeque};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use uuid::Uuid;

const MAX_RECENT_EVENTS: usize = 50;
const MAX_ACTIVE_SESSIONS: usize = 64;

/// 10.04 — Agent session lifecycle state aligned with Gemma 4B passive standby.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum SessionState {
    Idle,
    Busy,
    Standby,
    LockedOut,
}

impl SessionState {
    pub fn as_str(&self) -> &'static str {
        match self {
            SessionState::Idle => "Idle",
            SessionState::Busy => "Busy",
            SessionState::Standby => "Standby",
            SessionState::LockedOut => "LockedOut",
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AgentSession {
    pub session_id: String,
    pub agent_id: String,
    pub caller_uid: u32,
    pub caller_bus_name: String,
    pub state: SessionState,
    pub active_requests: u32,
    pub total_requests: u64,
    pub approved_actions: u64,
    pub denied_actions: u64,
    pub created_at_unix: u64,
    pub last_activity_unix: u64,
    pub last_operation: String,
    pub last_rule_id: String,
    pub lockout_cooldown_secs: u64,
    pub hard_locked: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PassiveModelSummary {
    pub model_id: String,
    pub profile: String,
    pub state: String,
    pub vram_allocation_policy: String,
    pub estimated_vram_mb: u64,
    pub active_model_requests: u32,
    pub total_model_queries: u64,
    pub last_active_unix: u64,
    pub lease_file_present: bool,
    pub system_mem_available_mb: u64,
    pub system_mem_total_mb: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GatewayAuditRecord {
    pub timestamp_unix: u64,
    pub session_id: String,
    pub agent_id: String,
    pub action_kind: String,
    pub operation: String,
    pub action_class: String,
    pub decision: String,
    pub code: String,
    pub rule_id: String,
    pub policy_audit_event_id: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct GatewayMetrics {
    pub total_actions_requested: u64,
    pub approved_actions: u64,
    pub denied_actions: u64,
    pub fail_closed_denials: u64,
    pub rate_limited_denials: u64,
    pub concurrency_denials: u64,
    pub timeout_denials: u64,
    pub wrapped_payloads_total: u64,
    pub injections_flagged_total: u64,
    pub config_reload_count: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SlotAcquireError {
    SessionNotFound,
    SessionAgentMismatch,
    SessionOwnerMismatch,
    RateLimited { limit: u32, window_secs: u64 },
    ConcurrencyLimit { scope: &'static str, limit: u32 },
}

pub struct SessionRegistry {
    pub config: DaemonConfig,
    sessions: BTreeMap<String, AgentSession>,
    rate_windows: HashMap<String, VecDeque<u64>>,
    active_requests_global: u32,
    active_model_requests: u32,
    total_model_queries: u64,
    last_model_active_unix: u64,
    pub metrics: GatewayMetrics,
    pub last_policy_health: PolicyHealthSnapshot,
    recent_events: VecDeque<GatewayAuditRecord>,
}

impl SessionRegistry {
    pub fn new(mut config: DaemonConfig) -> Self {
        config.clamp_bounds();
        Self {
            config,
            sessions: BTreeMap::new(),
            rate_windows: HashMap::new(),
            active_requests_global: 0,
            active_model_requests: 0,
            total_model_queries: 0,
            last_model_active_unix: 0,
            metrics: GatewayMetrics::default(),
            last_policy_health: PolicyHealthSnapshot::default(),
            recent_events: VecDeque::with_capacity(MAX_RECENT_EVENTS),
        }
    }

    pub fn sweep_idle_to_standby(&mut self, now_unix: u64) {
        let threshold = self.config.session_idle_timeout_secs;
        for session in self.sessions.values_mut() {
            if session.state == SessionState::Idle
                && session.active_requests == 0
                && now_unix.saturating_sub(session.last_activity_unix) >= threshold
            {
                session.state = SessionState::Standby;
            }
        }
    }

    pub fn open_session(
        &mut self,
        agent_id: &str,
        caller_uid: u32,
        caller_bus_name: &str,
        locked_out: bool,
        hard_locked: bool,
        cooldown_secs: u64,
    ) -> Result<AgentSession, String> {
        let now = now_unix_secs();
        self.sweep_idle_to_standby(now);

        if self.sessions.len() >= MAX_ACTIVE_SESSIONS {
            return Err(format!(
                "Maximum active ZCI sessions limit ({MAX_ACTIVE_SESSIONS}) reached."
            ));
        }

        let initial_state = if locked_out || hard_locked {
            SessionState::LockedOut
        } else {
            SessionState::Idle
        };

        let session = AgentSession {
            session_id: Uuid::new_v4().to_string(),
            agent_id: agent_id.to_string(),
            caller_uid,
            caller_bus_name: caller_bus_name.to_string(),
            state: initial_state,
            active_requests: 0,
            total_requests: 0,
            approved_actions: 0,
            denied_actions: 0,
            created_at_unix: now,
            last_activity_unix: now,
            last_operation: String::new(),
            last_rule_id: String::new(),
            lockout_cooldown_secs: cooldown_secs,
            hard_locked,
        };
        self.sessions
            .insert(session.session_id.clone(), session.clone());
        Ok(session)
    }

    pub fn set_session_standby(
        &mut self,
        session_id: &str,
        caller_uid: u32,
    ) -> Result<AgentSession, (&'static str, String)> {
        let session = self
            .sessions
            .get_mut(session_id)
            .ok_or(("session-not-found", "ZCI session not found.".to_string()))?;
        if caller_uid != 0 && session.caller_uid != caller_uid {
            return Err((
                "permission-denied",
                "Cannot modify a session owned by another UID.".to_string(),
            ));
        }
        if session.state == SessionState::LockedOut {
            return Err((
                "agent-locked-out",
                "Locked-out session cannot enter Standby until Policy Engine lockout is cleared."
                    .to_string(),
            ));
        }
        if session.active_requests > 0 {
            return Err((
                "session-busy",
                "Busy session cannot enter Standby while actions are running.".to_string(),
            ));
        }
        session.state = SessionState::Standby;
        session.last_activity_unix = now_unix_secs();
        Ok(session.clone())
    }

    pub fn close_session(
        &mut self,
        session_id: &str,
        caller_uid: u32,
    ) -> Result<AgentSession, (&'static str, String)> {
        let Some(existing) = self.sessions.get(session_id) else {
            return Err(("session-not-found", "ZCI session not found.".to_string()));
        };
        if caller_uid != 0 && existing.caller_uid != caller_uid {
            return Err((
                "permission-denied",
                "Cannot close a session owned by another UID.".to_string(),
            ));
        }
        if existing.active_requests > 0 {
            return Err((
                "session-busy",
                "Cannot close a session while actions are in flight.".to_string(),
            ));
        }
        Ok(self.sessions.remove(session_id).expect("verified present"))
    }

    /// 10.05 — Polkit-authorized administrative session termination (single ID or `*` for all).
    pub fn admin_terminate_session(&mut self, session_id: &str) -> Result<Vec<AgentSession>, String> {
        let trimmed = session_id.trim();
        if trimmed == "*" {
            let drained: Vec<AgentSession> = self.sessions.values().cloned().collect();
            self.sessions.clear();
            return Ok(drained);
        }
        match self.sessions.remove(trimmed) {
            Some(removed) => Ok(vec![removed]),
            None => Err(format!("Session '{trimmed}' not found.")),
        }
    }

    pub fn update_agent_lockout(
        &mut self,
        agent_id: &str,
        lockout_active: bool,
        hard_locked: bool,
        cooldown_secs: u64,
    ) {
        for session in self.sessions.values_mut() {
            if session.agent_id == agent_id {
                session.hard_locked = hard_locked;
                session.lockout_cooldown_secs = cooldown_secs;
                if lockout_active || hard_locked {
                    session.state = SessionState::LockedOut;
                } else if session.state == SessionState::LockedOut {
                    session.state = if session.active_requests > 0 {
                        SessionState::Busy
                    } else {
                        SessionState::Idle
                    };
                }
            }
        }
    }

    pub fn try_acquire_slot(
        registry: &Arc<Mutex<Self>>,
        session_id: &str,
        agent_id: &str,
        caller_uid: u32,
        is_model_query: bool,
    ) -> Result<ActiveSlotGuard, SlotAcquireError> {
        let now = now_unix_secs();
        let mut guard = registry.lock().expect("session registry lock");
        guard.sweep_idle_to_standby(now);

        // 1. Validate session existence, agent binding, and caller UID ownership.
        {
            let Some(sess) = guard.sessions.get(session_id) else {
                return Err(SlotAcquireError::SessionNotFound);
            };
            if sess.agent_id != agent_id {
                return Err(SlotAcquireError::SessionAgentMismatch);
            }
            if caller_uid != 0 && sess.caller_uid != caller_uid {
                return Err(SlotAcquireError::SessionOwnerMismatch);
            }
        }

        // 2. Per-agent sliding-window rate limit check (10.04).
        let window_secs = guard.config.rate_limit_window_secs;
        let max_req = guard.config.rate_limit_max_requests;
        {
            let q = guard.rate_windows.entry(agent_id.to_string()).or_default();
            while let Some(&ts) = q.front() {
                if now.saturating_sub(ts) >= window_secs {
                    q.pop_front();
                } else {
                    break;
                }
            }
            if q.len() >= max_req as usize {
                guard.metrics.total_actions_requested += 1;
                guard.metrics.denied_actions += 1;
                guard.metrics.rate_limited_denials += 1;
                if let Some(sess) = guard.sessions.get_mut(session_id) {
                    sess.denied_actions += 1;
                    sess.last_activity_unix = now;
                }
                return Err(SlotAcquireError::RateLimited {
                    limit: max_req,
                    window_secs,
                });
            }
        }

        // 3. Global & per-agent concurrency cap check (10.04).
        let max_global = guard.config.max_concurrent_global;
        let max_per_agent = guard.config.max_concurrent_per_agent;
        if guard.active_requests_global >= max_global {
            guard.metrics.total_actions_requested += 1;
            guard.metrics.denied_actions += 1;
            guard.metrics.concurrency_denials += 1;
            if let Some(sess) = guard.sessions.get_mut(session_id) {
                sess.denied_actions += 1;
                sess.last_activity_unix = now;
            }
            return Err(SlotAcquireError::ConcurrencyLimit {
                scope: "global",
                limit: max_global,
            });
        }

        let agent_active: u32 = guard
            .sessions
            .values()
            .filter(|s| s.agent_id == agent_id)
            .map(|s| s.active_requests)
            .sum();
        if agent_active >= max_per_agent {
            guard.metrics.total_actions_requested += 1;
            guard.metrics.denied_actions += 1;
            guard.metrics.concurrency_denials += 1;
            if let Some(sess) = guard.sessions.get_mut(session_id) {
                sess.denied_actions += 1;
                sess.last_activity_unix = now;
            }
            return Err(SlotAcquireError::ConcurrencyLimit {
                scope: "agent",
                limit: max_per_agent,
            });
        }

        // 4. Commit slot reservation & transition session to Busy (and model to Active if model_query).
        guard
            .rate_windows
            .entry(agent_id.to_string())
            .or_default()
            .push_back(now);
        guard.active_requests_global += 1;
        guard.metrics.total_actions_requested += 1;

        if is_model_query {
            guard.active_model_requests += 1;
            guard.total_model_queries += 1;
            guard.last_model_active_unix = now;
        }

        if let Some(sess) = guard.sessions.get_mut(session_id) {
            sess.active_requests += 1;
            sess.total_requests += 1;
            sess.last_activity_unix = now;
            if sess.state != SessionState::LockedOut {
                sess.state = SessionState::Busy;
            }
        }

        Ok(ActiveSlotGuard {
            registry: Arc::clone(registry),
            session_id: session_id.to_string(),
            is_model_query,
        })
    }

    #[allow(clippy::too_many_arguments)]
    pub fn record_action_outcome(
        &mut self,
        session_id: &str,
        agent_id: &str,
        action_kind: &str,
        operation: &str,
        action_class: &str,
        approved: bool,
        code: &str,
        rule_id: &str,
        policy_audit_event_id: &str,
        fail_closed: bool,
        timed_out: bool,
    ) {
        let now = now_unix_secs();
        if approved {
            self.metrics.approved_actions += 1;
        } else {
            self.metrics.denied_actions += 1;
            if fail_closed {
                self.metrics.fail_closed_denials += 1;
            }
            if timed_out {
                self.metrics.timeout_denials += 1;
            }
        }

        if let Some(sess) = self.sessions.get_mut(session_id) {
            sess.last_activity_unix = now;
            sess.last_operation = operation.to_string();
            sess.last_rule_id = rule_id.to_string();
            if approved {
                sess.approved_actions += 1;
            } else {
                sess.denied_actions += 1;
            }
        }

        if self.recent_events.len() >= MAX_RECENT_EVENTS {
            self.recent_events.pop_front();
        }
        self.recent_events.push_back(GatewayAuditRecord {
            timestamp_unix: now,
            session_id: session_id.to_string(),
            agent_id: agent_id.to_string(),
            action_kind: action_kind.to_string(),
            operation: operation.to_string(),
            action_class: action_class.to_string(),
            decision: if approved {
                "allow".to_string()
            } else {
                "deny".to_string()
            },
            code: code.to_string(),
            rule_id: rule_id.to_string(),
            policy_audit_event_id: policy_audit_event_id.to_string(),
        });
    }

    pub fn record_wrapped_payload(&mut self, injection_detected: bool) {
        self.metrics.wrapped_payloads_total += 1;
        if injection_detected {
            self.metrics.injections_flagged_total += 1;
        }
    }

    pub fn list_sessions(&mut self) -> Vec<AgentSession> {
        self.sweep_idle_to_standby(now_unix_secs());
        self.sessions.values().cloned().collect()
    }

    pub fn active_requests_global(&self) -> u32 {
        self.active_requests_global
    }

    pub fn recent_events_json(&self) -> Vec<String> {
        self.recent_events
            .iter()
            .filter_map(|ev| serde_json::to_string(ev).ok())
            .collect()
    }

    pub fn passive_model_summary(&self) -> PassiveModelSummary {
        let (mem_avail_mb, mem_total_mb) = read_system_meminfo_mb();
        let lease_present = check_model_lease_present(self.config.model_runtime_dir_override.as_deref());
        let active = self.active_model_requests > 0;
        // Gemma 4B passive standby model: zero VRAM while in Standby; on-demand VRAM only during active request.
        let estimated_vram_mb = if active {
            self.config.passive_model_active_vram_estimate_mb
        } else {
            0
        };

        PassiveModelSummary {
            model_id: DEFAULT_PASSIVE_MODEL_ID.to_string(),
            profile: DEFAULT_PASSIVE_MODEL_PROFILE.to_string(),
            state: if active {
                "Active".to_string()
            } else {
                "Standby".to_string()
            },
            vram_allocation_policy: "on-demand-only".to_string(),
            estimated_vram_mb,
            active_model_requests: self.active_model_requests,
            total_model_queries: self.total_model_queries,
            last_active_unix: self.last_model_active_unix,
            lease_file_present: lease_present,
            system_mem_available_mb: mem_avail_mb,
            system_mem_total_mb: mem_total_mb,
        }
    }
}

/// RAII concurrency & passive-standby model guard.
/// Always releases slots and returns Gemma 4B to Standby (`estimated_vram_mb=0`) on drop.
pub struct ActiveSlotGuard {
    registry: Arc<Mutex<SessionRegistry>>,
    session_id: String,
    is_model_query: bool,
}

impl Drop for ActiveSlotGuard {
    fn drop(&mut self) {
        if let Ok(mut guard) = self.registry.lock() {
            guard.active_requests_global = guard.active_requests_global.saturating_sub(1);
            if self.is_model_query {
                guard.active_model_requests = guard.active_model_requests.saturating_sub(1);
            }
            if let Some(sess) = guard.sessions.get_mut(&self.session_id) {
                sess.active_requests = sess.active_requests.saturating_sub(1);
                sess.last_activity_unix = now_unix_secs();
                if sess.active_requests == 0 && sess.state == SessionState::Busy {
                    // Return model-driven sessions promptly to Standby or Idle.
                    sess.state = if self.is_model_query {
                        SessionState::Standby
                    } else {
                        SessionState::Idle
                    };
                }
            }
        }
    }
}

fn read_system_meminfo_mb() -> (u64, u64) {
    let Ok(text) = fs::read_to_string("/proc/meminfo") else {
        return (0, 0);
    };
    let mut avail_kb = 0u64;
    let mut total_kb = 0u64;
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix("MemAvailable:") {
            avail_kb = rest
                .split_whitespace()
                .next()
                .and_then(|s| s.parse::<u64>().ok())
                .unwrap_or(0);
        } else if let Some(rest) = line.strip_prefix("MemTotal:") {
            total_kb = rest
                .split_whitespace()
                .next()
                .and_then(|s| s.parse::<u64>().ok())
                .unwrap_or(0);
        }
    }
    (avail_kb / 1024, total_kb / 1024)
}

fn check_model_lease_present(override_dir: Option<&Path>) -> bool {
    if let Some(dir) = override_dir {
        return dir.join("zethropol-inference/cpu.lock").exists();
    }
    // Best-effort read-only check across `/run/user/*/zethropol-inference/cpu.lock`.
    let run_user = PathBuf::from("/run/user");
    let Ok(entries) = fs::read_dir(run_user) else {
        return false;
    };
    for entry in entries.flatten() {
        let candidate = entry.path().join("zethropol-inference/cpu.lock");
        if candidate.exists() {
            return true;
        }
    }
    false
}
