use crate::crypto_util::{format_utc_ms, now_unix_ms, sha256_hex};
use serde::{Deserialize, Serialize};
use std::fs::{self, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;
use uuid::Uuid;

pub const AUDIT_GENESIS_HASH: &str =
    "0000000000000000000000000000000000000000000000000000000000000000";
const MAX_FIELD_LEN: usize = 512;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct AuditEvent {
    pub event_id: String,
    pub sequence: u64,
    pub timestamp_unix_ms: u64,
    pub timestamp_utc: String,
    pub agent_id: String,
    pub caller_bus_name: String,
    pub caller_uid: u32,
    pub operation: String,
    pub target: String,
    pub dbus_interface: String,
    pub action_class: String,
    pub decision: String,
    pub rule_id: String,
    pub reason: String,
    pub penalty_delta: u32,
    pub penalty_score_after: u32,
    pub penalty_level_after: u8,
    pub cooldown_until_unix: u64,
    pub hard_locked: bool,
    pub untrusted_flags: Vec<String>,
    pub prev_hash: String,
    pub entry_hash: String,
}

#[derive(Debug, Clone)]
pub struct AuditEventInput {
    pub agent_id: String,
    pub caller_bus_name: String,
    pub caller_uid: u32,
    pub operation: String,
    pub target: String,
    pub dbus_interface: String,
    pub action_class: String,
    pub decision: String,
    pub rule_id: String,
    pub reason: String,
    pub penalty_delta: u32,
    pub penalty_score_after: u32,
    pub penalty_level_after: u8,
    pub cooldown_until_unix: u64,
    pub hard_locked: bool,
    pub untrusted_flags: Vec<String>,
}

#[derive(Debug, Clone)]
pub struct AuditQueryResult {
    pub total_events: u64,
    pub chain_valid: bool,
    pub events: Vec<AuditEvent>,
}

/// Sanitize text before persisting into audit records (prevents log/control injection
/// and bounds field size). Never logs raw untrusted payloads or secrets.
pub fn sanitize_audit_field(raw: &str) -> String {
    let mut out = String::with_capacity(raw.len().min(MAX_FIELD_LEN));
    for ch in raw.chars() {
        if out.len() >= MAX_FIELD_LEN {
            break;
        }
        if ch.is_control() {
            out.push(' ');
        } else {
            out.push(ch);
        }
    }
    out.trim().to_string()
}

fn compute_event_hash(event: &AuditEvent) -> String {
    let canonical = format!(
        "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
        event.event_id,
        event.sequence,
        event.timestamp_unix_ms,
        event.agent_id,
        event.caller_bus_name,
        event.caller_uid,
        event.operation,
        event.target,
        event.dbus_interface,
        event.action_class,
        event.decision,
        event.rule_id,
        event.reason,
        event.penalty_delta,
        event.penalty_score_after,
        event.penalty_level_after,
        event.cooldown_until_unix,
        event.hard_locked,
        event.prev_hash
    );
    sha256_hex(canonical.as_bytes())
}

#[derive(Debug, Clone)]
pub struct AuditLogger {
    path: PathBuf,
    last_sequence: u64,
    last_hash: String,
    emit_stderr: bool,
}

impl AuditLogger {
    pub fn open(path: PathBuf, emit_stderr: bool) -> Self {
        let mut last_sequence = 0u64;
        let mut last_hash = AUDIT_GENESIS_HASH.to_string();

        if path.is_file() {
            if let Ok(file) = fs::File::open(&path) {
                for line in BufReader::new(file).lines().map_while(Result::ok) {
                    let trimmed = line.trim();
                    if trimmed.is_empty() {
                        continue;
                    }
                    if let Ok(ev) = serde_json::from_str::<AuditEvent>(trimmed) {
                        last_sequence = ev.sequence;
                        last_hash = ev.entry_hash;
                    }
                }
            }
        }

        Self {
            path,
            last_sequence,
            last_hash,
            emit_stderr,
        }
    }

    pub fn total_events(&self) -> u64 {
        self.last_sequence
    }

    pub fn append(&mut self, input: AuditEventInput) -> Result<AuditEvent, String> {
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent)
                .map_err(|e| format!("Failed to create audit log directory: {e}"))?;
            let _ = fs::set_permissions(parent, fs::Permissions::from_mode(0o750));
        }

        if let Ok(meta) = fs::symlink_metadata(&self.path) {
            if meta.file_type().is_symlink() || !meta.file_type().is_file() {
                return Err("Audit log target is not a regular file; refusing write.".to_string());
            }
        }

        let ts_ms = now_unix_ms();
        let next_seq = self.last_sequence.saturating_add(1);
        let mut event = AuditEvent {
            event_id: Uuid::new_v4().to_string(),
            sequence: next_seq,
            timestamp_unix_ms: ts_ms,
            timestamp_utc: format_utc_ms(ts_ms),
            agent_id: sanitize_audit_field(&input.agent_id),
            caller_bus_name: sanitize_audit_field(&input.caller_bus_name),
            caller_uid: input.caller_uid,
            operation: sanitize_audit_field(&input.operation),
            target: sanitize_audit_field(&input.target),
            dbus_interface: sanitize_audit_field(&input.dbus_interface),
            action_class: sanitize_audit_field(&input.action_class),
            decision: sanitize_audit_field(&input.decision),
            rule_id: sanitize_audit_field(&input.rule_id),
            reason: sanitize_audit_field(&input.reason),
            penalty_delta: input.penalty_delta,
            penalty_score_after: input.penalty_score_after,
            penalty_level_after: input.penalty_level_after,
            cooldown_until_unix: input.cooldown_until_unix,
            hard_locked: input.hard_locked,
            untrusted_flags: input
                .untrusted_flags
                .into_iter()
                .take(16)
                .map(|f| sanitize_audit_field(&f))
                .collect(),
            prev_hash: self.last_hash.clone(),
            entry_hash: String::new(),
        };
        event.entry_hash = compute_event_hash(&event);

        let line = serde_json::to_string(&event)
            .map_err(|e| format!("Failed to serialize audit event: {e}"))?;

        let mut file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&self.path)
            .map_err(|e| format!("Failed to open audit log file: {e}"))?;
        let _ = file.set_permissions(fs::Permissions::from_mode(0o640));
        file.write_all(line.as_bytes())
            .map_err(|e| format!("Failed to write audit record: {e}"))?;
        file.write_all(b"\n")
            .map_err(|e| format!("Failed to write audit record newline: {e}"))?;
        file.sync_data()
            .map_err(|e| format!("Failed to sync audit record: {e}"))?;

        self.last_sequence = event.sequence;
        self.last_hash = event.entry_hash.clone();

        if self.emit_stderr {
            eprintln!(
                "zci-audit seq={} ts={} agent={} op={} class={} decision={} rule={} penalty_score={} locked={}",
                event.sequence,
                event.timestamp_utc,
                event.agent_id,
                event.operation,
                event.action_class,
                event.decision,
                event.rule_id,
                event.penalty_score_after,
                event.hard_locked
            );
        }

        Ok(event)
    }

    pub fn query_recent(&self, limit: u32) -> AuditQueryResult {
        let clamped = limit.clamp(1, 200) as usize;
        if !self.path.is_file() {
            return AuditQueryResult {
                total_events: 0,
                chain_valid: true,
                events: Vec::new(),
            };
        }

        let file = match fs::File::open(&self.path) {
            Ok(f) => f,
            Err(_) => {
                return AuditQueryResult {
                    total_events: self.last_sequence,
                    chain_valid: false,
                    events: Vec::new(),
                };
            }
        };

        let mut expected_prev = AUDIT_GENESIS_HASH.to_string();
        let mut expected_seq = 1u64;
        let mut chain_valid = true;
        let mut total_events = 0u64;
        let mut ring: std::collections::VecDeque<AuditEvent> =
            std::collections::VecDeque::with_capacity(clamped + 1);

        for line in BufReader::new(file).lines() {
            let line = match line {
                Ok(l) => l,
                Err(_) => {
                    chain_valid = false;
                    break;
                }
            };
            let trimmed = line.trim();
            if trimmed.is_empty() {
                continue;
            }
            match serde_json::from_str::<AuditEvent>(trimmed) {
                Ok(ev) => {
                    total_events += 1;
                    let expected_hash = compute_event_hash(&ev);
                    if ev.sequence != expected_seq
                        || ev.prev_hash != expected_prev
                        || ev.entry_hash != expected_hash
                    {
                        chain_valid = false;
                    }
                    expected_seq = ev.sequence.saturating_add(1);
                    expected_prev = ev.entry_hash.clone();
                    if ring.len() == clamped {
                        ring.pop_front();
                    }
                    ring.push_back(ev);
                }
                Err(_) => {
                    chain_valid = false;
                }
            }
        }

        AuditQueryResult {
            total_events,
            chain_valid,
            events: ring.into_iter().collect(),
        }
    }
}
