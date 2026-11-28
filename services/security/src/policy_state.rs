use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;

pub const POLICY_STATE_SCHEMA_VERSION: u32 = 1;
pub const HARD_LOCKOUT_STRIKE_THRESHOLD: u32 = 9;

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct AgentPenaltyState {
    pub agent_id: String,
    pub total_violations: u64,
    pub strike_score: u32,
    pub penalty_level: u8,
    pub cooldown_until_unix: u64,
    pub hard_locked: bool,
    pub last_violation_unix: u64,
    pub last_rule_id: String,
    pub self_mod_attempts: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct PersistedPolicyState {
    schema_version: u32,
    agents: BTreeMap<String, AgentPenaltyState>,
}

#[derive(Debug, Clone)]
pub struct PolicyStateStore {
    path: PathBuf,
    agents: BTreeMap<String, AgentPenaltyState>,
    state_corrupted: bool,
    corruption_detail: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LockoutCheck {
    pub locked: bool,
    pub hard_locked: bool,
    pub state_corrupted: bool,
    pub penalty_level: u8,
    pub strike_score: u32,
    pub cooldown_until_unix: u64,
    pub remaining_secs: u64,
}

/// Progressive penalty tiers (08.06):
/// score 0..=1 => level 0 (0s cooldown)
/// score 2     => level 1 (15s cooldown)
/// score 3..=4 => level 2 (60s cooldown)
/// score 5..=6 => level 3 (300s cooldown)
/// score 7..=8 => level 4 (1800s cooldown)
/// score >= 9  => level 5 (hard_lockout — requires Polkit auth_admin ResetAgentLockout)
pub fn penalty_tier_for_score(score: u32) -> (u8, u64, bool) {
    match score {
        0..=1 => (0, 0, false),
        2 => (1, 15, false),
        3..=4 => (2, 60, false),
        5..=6 => (3, 300, false),
        7..=8 => (4, 1800, false),
        _ => (5, 0, true),
    }
}

impl PolicyStateStore {
    pub fn load(path: PathBuf) -> Self {
        if !path.exists() {
            return Self {
                path,
                agents: BTreeMap::new(),
                state_corrupted: false,
                corruption_detail: None,
            };
        }

        // Reject symlink state files fail-closed.
        if let Ok(meta) = fs::symlink_metadata(&path) {
            if meta.file_type().is_symlink() || !meta.file_type().is_file() {
                return Self {
                    path,
                    agents: BTreeMap::new(),
                    state_corrupted: true,
                    corruption_detail: Some(
                        "Policy state file is not a regular file (symlink or special node rejected)."
                            .to_string(),
                    ),
                };
            }
        }

        match fs::read_to_string(&path) {
            Ok(raw) => match serde_json::from_str::<PersistedPolicyState>(&raw) {
                Ok(parsed) if parsed.schema_version == POLICY_STATE_SCHEMA_VERSION => Self {
                    path,
                    agents: parsed.agents,
                    state_corrupted: false,
                    corruption_detail: None,
                },
                Ok(parsed) => Self {
                    path,
                    agents: BTreeMap::new(),
                    state_corrupted: true,
                    corruption_detail: Some(format!(
                        "Unsupported policy state schema version {}",
                        parsed.schema_version
                    )),
                },
                Err(err) => Self {
                    path,
                    agents: BTreeMap::new(),
                    state_corrupted: true,
                    corruption_detail: Some(format!(
                        "Corrupted persistent policy state JSON: {err}"
                    )),
                },
            },
            Err(err) => Self {
                path,
                agents: BTreeMap::new(),
                state_corrupted: true,
                corruption_detail: Some(format!("Unable to read persistent policy state: {err}")),
            },
        }
    }

    pub fn is_corrupted(&self) -> bool {
        self.state_corrupted
    }

    pub fn corruption_detail(&self) -> Option<&str> {
        self.corruption_detail.as_deref()
    }

    pub fn get_agent(&self, agent_id: &str) -> AgentPenaltyState {
        self.agents
            .get(agent_id)
            .cloned()
            .unwrap_or_else(|| AgentPenaltyState {
                agent_id: agent_id.to_string(),
                ..Default::default()
            })
    }

    pub fn check_lockout(&self, agent_id: &str, now_unix: u64) -> LockoutCheck {
        if self.state_corrupted {
            return LockoutCheck {
                locked: true,
                hard_locked: true,
                state_corrupted: true,
                penalty_level: 5,
                strike_score: HARD_LOCKOUT_STRIKE_THRESHOLD,
                cooldown_until_unix: 0,
                remaining_secs: 0,
            };
        }

        let state = self.get_agent(agent_id);
        if state.hard_locked {
            return LockoutCheck {
                locked: true,
                hard_locked: true,
                state_corrupted: false,
                penalty_level: state.penalty_level.max(5),
                strike_score: state.strike_score,
                cooldown_until_unix: state.cooldown_until_unix,
                remaining_secs: 0,
            };
        }

        if state.cooldown_until_unix > now_unix {
            return LockoutCheck {
                locked: true,
                hard_locked: false,
                state_corrupted: false,
                penalty_level: state.penalty_level,
                strike_score: state.strike_score,
                cooldown_until_unix: state.cooldown_until_unix,
                remaining_secs: state.cooldown_until_unix - now_unix,
            };
        }

        LockoutCheck {
            locked: false,
            hard_locked: false,
            state_corrupted: false,
            penalty_level: state.penalty_level,
            strike_score: state.strike_score,
            cooldown_until_unix: state.cooldown_until_unix,
            remaining_secs: 0,
        }
    }

    pub fn record_violation(
        &mut self,
        agent_id: &str,
        penalty_delta: u32,
        rule_id: &str,
        is_self_mod: bool,
        now_unix: u64,
    ) -> Result<AgentPenaltyState, String> {
        if self.state_corrupted {
            return Err("Persistent policy state is corrupted; refusing mutation until admin reset."
                .to_string());
        }

        let entry = self
            .agents
            .entry(agent_id.to_string())
            .or_insert_with(|| AgentPenaltyState {
                agent_id: agent_id.to_string(),
                ..Default::default()
            });

        entry.total_violations = entry.total_violations.saturating_add(1);
        entry.strike_score = entry.strike_score.saturating_add(penalty_delta.max(1));
        entry.last_violation_unix = now_unix;
        entry.last_rule_id = rule_id.to_string();
        if is_self_mod {
            entry.self_mod_attempts = entry.self_mod_attempts.saturating_add(1);
        }

        let (level, cooldown_secs, hard_lock) = penalty_tier_for_score(entry.strike_score);
        entry.penalty_level = level;
        if hard_lock {
            entry.hard_locked = true;
            entry.cooldown_until_unix = 0;
        } else if cooldown_secs > 0 {
            let new_until = now_unix.saturating_add(cooldown_secs);
            entry.cooldown_until_unix = entry.cooldown_until_unix.max(new_until);
        }

        let snapshot = entry.clone();
        self.persist()?;
        Ok(snapshot)
    }

    /// Administrative reset (MUST only be invoked after Polkit `auth_admin` verification).
    pub fn admin_reset_agent(&mut self, agent_id: &str) -> Result<(), String> {
        if agent_id == "*" {
            self.agents.clear();
            self.state_corrupted = false;
            self.corruption_detail = None;
        } else {
            if self.state_corrupted {
                // Only full "*" recovery clears a corrupted state store safely.
                return Err(
                    "State store is corrupted; pass agent_id=\"*\" with admin auth to reinitialize."
                        .to_string(),
                );
            }
            self.agents.remove(agent_id);
        }
        self.persist()
    }

    pub fn locked_agent_count(&self, now_unix: u64) -> usize {
        if self.state_corrupted {
            return self.agents.len().max(1);
        }
        self.agents
            .keys()
            .filter(|id| self.check_lockout(id, now_unix).locked)
            .count()
    }

    fn persist(&self) -> Result<(), String> {
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent)
                .map_err(|e| format!("Failed to create policy state directory: {e}"))?;
            let _ = fs::set_permissions(parent, fs::Permissions::from_mode(0o700));
        }

        let payload = PersistedPolicyState {
            schema_version: POLICY_STATE_SCHEMA_VERSION,
            agents: self.agents.clone(),
        };
        let encoded = serde_json::to_string_pretty(&payload)
            .map_err(|e| format!("Failed to serialize policy state: {e}"))?;

        let tmp_path = self.path.with_extension(format!("json.tmp.{}", std::process::id()));
        let mut file = OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .open(&tmp_path)
            .map_err(|e| format!("Failed to open temporary policy state file: {e}"))?;
        let _ = file.set_permissions(fs::Permissions::from_mode(0o600));
        file.write_all(encoded.as_bytes())
            .map_err(|e| format!("Failed to write policy state: {e}"))?;
        file.write_all(b"\n")
            .map_err(|e| format!("Failed to flush policy state newline: {e}"))?;
        file.sync_all()
            .map_err(|e| format!("Failed to sync policy state: {e}"))?;
        drop(file);

        fs::rename(&tmp_path, &self.path)
            .map_err(|e| format!("Failed to atomically commit policy state: {e}"))?;
        let _ = fs::set_permissions(&self.path, fs::Permissions::from_mode(0o600));
        Ok(())
    }
}
