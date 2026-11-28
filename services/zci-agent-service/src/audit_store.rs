use serde::{Deserialize, Serialize};
use std::fs::{self, File, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};
use uuid::Uuid;

pub const GENESIS_SHA256: &str =
    "0000000000000000000000000000000000000000000000000000000000000000";

const SHA256_K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
];

/// Pure-Rust RFC 6234 SHA-256 (zero external crypto crates required).
pub fn sha256_bytes(data: &[u8]) -> [u8; 32] {
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
        0x5be0cd19,
    ];
    let bit_len = (data.len() as u64).wrapping_mul(8);
    let mut padded = Vec::with_capacity(((data.len() + 9 + 63) / 64) * 64);
    padded.extend_from_slice(data);
    padded.push(0x80);
    while (padded.len() % 64) != 56 {
        padded.push(0);
    }
    padded.extend_from_slice(&bit_len.to_be_bytes());

    for chunk in padded.chunks_exact(64) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([
                chunk[i * 4],
                chunk[i * 4 + 1],
                chunk[i * 4 + 2],
                chunk[i * 4 + 3],
            ]);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }

        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh] = h;
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ ((!e) & g);
            let temp1 = hh
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(SHA256_K[i])
                .wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let temp2 = s0.wrapping_add(maj);

            hh = g;
            g = f;
            f = e;
            e = d.wrapping_add(temp1);
            d = c;
            c = b;
            b = a;
            a = temp1.wrapping_add(temp2);
        }

        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
        h[5] = h[5].wrapping_add(f);
        h[6] = h[6].wrapping_add(g);
        h[7] = h[7].wrapping_add(hh);
    }

    let mut out = [0u8; 32];
    for (idx, word) in h.iter().enumerate() {
        out[idx * 4..idx * 4 + 4].copy_from_slice(&word.to_be_bytes());
    }
    out
}

pub fn sha256_hex(data: &[u8]) -> String {
    let digest = sha256_bytes(data);
    let mut s = String::with_capacity(64);
    for b in digest {
        use std::fmt::Write as _;
        let _ = write!(&mut s, "{b:02x}");
    }
    s
}

pub fn now_unix_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct AuditEntry {
    pub seq: u64,
    pub timestamp_unix: u64,
    pub event_id: String,
    pub event_type: String,
    pub plan_id: String,
    pub step_id: String,
    pub agent_profile: String,
    pub risk_level: String,
    pub outcome: String,
    pub detail: String,
    pub prev_sha256: String,
    pub entry_sha256: String,
}

#[derive(Serialize)]
struct AuditHashInput<'a> {
    seq: u64,
    timestamp_unix: u64,
    event_id: &'a str,
    event_type: &'a str,
    plan_id: &'a str,
    step_id: &'a str,
    agent_profile: &'a str,
    risk_level: &'a str,
    outcome: &'a str,
    detail: &'a str,
    prev_sha256: &'a str,
}

impl AuditEntry {
    pub fn compute_hash(&self) -> String {
        let input = AuditHashInput {
            seq: self.seq,
            timestamp_unix: self.timestamp_unix,
            event_id: &self.event_id,
            event_type: &self.event_type,
            plan_id: &self.plan_id,
            step_id: &self.step_id,
            agent_profile: &self.agent_profile,
            risk_level: &self.risk_level,
            outcome: &self.outcome,
            detail: &self.detail,
            prev_sha256: &self.prev_sha256,
        };
        let canonical = serde_json::to_vec(&input).unwrap_or_default();
        sha256_hex(&canonical)
    }
}

#[derive(Debug, Clone)]
pub struct AuditVerification {
    pub valid: bool,
    pub entry_count: u64,
    pub head_sha256: String,
    pub error: String,
}

/// 13.06 — Permission-restricted (`0700`/`0600`), symlink-refusing, hash-chained immutable JSONL
/// audit log strictly inside `~/.local/share/zethropol/zci-agent/`.
pub struct HashChainedAuditStore {
    storage_dir: PathBuf,
    audit_file: PathBuf,
    plans_file: PathBuf,
    last_seq: u64,
    head_sha256: String,
    chain_valid: bool,
    chain_error: String,
}

fn ensure_safe_dir_0700(dir: &Path) -> Result<(), String> {
    if dir.exists() {
        let meta = fs::symlink_metadata(dir)
            .map_err(|e| format!("Failed to stat audit dir '{}': {e}", dir.display()))?;
        if meta.file_type().is_symlink() || !meta.is_dir() {
            return Err("Audit storage directory must be a real non-symlink directory.".to_string());
        }
    } else {
        fs::create_dir_all(dir)
            .map_err(|e| format!("Failed creating audit dir '{}': {e}", dir.display()))?;
    }
    fs::set_permissions(dir, fs::Permissions::from_mode(0o700))
        .map_err(|e| format!("Failed setting 0700 on audit dir: {e}"))?;
    Ok(())
}

fn ensure_non_symlink_0600(path: &Path) -> Result<(), String> {
    if path.exists() {
        let meta = fs::symlink_metadata(path)
            .map_err(|e| format!("Failed to stat '{}': {e}", path.display()))?;
        if meta.file_type().is_symlink() || !meta.is_file() {
            return Err(format!("Refusing symlink or non-file '{}'.", path.display()));
        }
        fs::set_permissions(path, fs::Permissions::from_mode(0o600))
            .map_err(|e| format!("Failed setting 0600 on '{}': {e}", path.display()))?;
    }
    Ok(())
}

impl HashChainedAuditStore {
    pub fn open(storage_dir: PathBuf) -> Result<Self, String> {
        ensure_safe_dir_0700(&storage_dir)?;
        let audit_file = storage_dir.join("audit-trail.jsonl");
        let plans_file = storage_dir.join("plans.json");
        ensure_non_symlink_0600(&audit_file)?;
        ensure_non_symlink_0600(&plans_file)?;

        if !audit_file.exists() {
            let mut f = OpenOptions::new()
                .create_new(true)
                .write(true)
                .mode(0o600)
                .open(&audit_file)
                .map_err(|e| format!("Failed creating audit-trail.jsonl: {e}"))?;
            f.flush().map_err(|e| format!("Failed flushing audit file: {e}"))?;
            fs::set_permissions(&audit_file, fs::Permissions::from_mode(0o600))
                .map_err(|e| format!("Failed setting 0600 on audit-trail.jsonl: {e}"))?;
        }

        let mut store = Self {
            storage_dir,
            audit_file,
            plans_file,
            last_seq: 0,
            head_sha256: GENESIS_SHA256.to_string(),
            chain_valid: true,
            chain_error: String::new(),
        };
        let verification = store.verify_chain();
        store.last_seq = verification.entry_count;
        store.head_sha256 = verification.head_sha256;
        store.chain_valid = verification.valid;
        store.chain_error = verification.error;
        Ok(store)
    }

    pub fn storage_dir(&self) -> &Path {
        &self.storage_dir
    }

    #[cfg_attr(not(test), allow(dead_code))]
    pub fn audit_file(&self) -> &Path {
        &self.audit_file
    }

    #[cfg_attr(not(test), allow(dead_code))]
    pub fn head_sha256(&self) -> &str {
        &self.head_sha256
    }

    #[cfg_attr(not(test), allow(dead_code))]
    pub fn entry_count(&self) -> u64 {
        self.last_seq
    }

    pub fn chain_valid(&self) -> bool {
        self.chain_valid
    }

    pub fn chain_error(&self) -> &str {
        &self.chain_error
    }

    pub fn verify_strict_permissions(&self) -> bool {
        let dir_ok = fs::symlink_metadata(&self.storage_dir)
            .map(|m| !m.file_type().is_symlink() && m.is_dir() && (m.permissions().mode() & 0o777) == 0o700)
            .unwrap_or(false);
        let audit_ok = fs::symlink_metadata(&self.audit_file)
            .map(|m| !m.file_type().is_symlink() && m.is_file() && (m.permissions().mode() & 0o777) == 0o600)
            .unwrap_or(false);
        let plans_ok = !self.plans_file.exists()
            || fs::symlink_metadata(&self.plans_file)
                .map(|m| !m.file_type().is_symlink() && m.is_file() && (m.permissions().mode() & 0o777) == 0o600)
                .unwrap_or(false);
        dir_ok && audit_ok && plans_ok
    }

    /// Walks the entire JSONL audit file from seq=1 verifying every `prev_sha256` link and digest.
    pub fn verify_chain(&self) -> AuditVerification {
        let Ok(file) = File::open(&self.audit_file) else {
            return AuditVerification {
                valid: false,
                entry_count: 0,
                head_sha256: GENESIS_SHA256.to_string(),
                error: "Cannot open audit-trail.jsonl.".to_string(),
            };
        };
        let reader = BufReader::new(file);
        let mut expected_prev = GENESIS_SHA256.to_string();
        let mut expected_seq = 1u64;
        let mut count = 0u64;

        for (line_no, line_res) in reader.lines().enumerate() {
            let Ok(line) = line_res else {
                return AuditVerification {
                    valid: false,
                    entry_count: count,
                    head_sha256: expected_prev,
                    error: format!("I/O error reading audit line {}.", line_no + 1),
                };
            };
            let trimmed = line.trim();
            if trimmed.is_empty() {
                continue;
            }
            let entry: AuditEntry = match serde_json::from_str(trimmed) {
                Ok(e) => e,
                Err(err) => {
                    return AuditVerification {
                        valid: false,
                        entry_count: count,
                        head_sha256: expected_prev,
                        error: format!("Malformed JSON at audit line {}: {err}", line_no + 1),
                    };
                }
            };
            if entry.seq != expected_seq {
                return AuditVerification {
                    valid: false,
                    entry_count: count,
                    head_sha256: expected_prev,
                    error: format!(
                        "Audit sequence mismatch at line {} (expected {}, got {}).",
                        line_no + 1,
                        expected_seq,
                        entry.seq
                    ),
                };
            }
            if entry.prev_sha256 != expected_prev {
                return AuditVerification {
                    valid: false,
                    entry_count: count,
                    head_sha256: expected_prev,
                    error: format!("Audit hash chain broken at seq {}.", entry.seq),
                };
            }
            let recomputed = entry.compute_hash();
            if entry.entry_sha256 != recomputed {
                return AuditVerification {
                    valid: false,
                    entry_count: count,
                    head_sha256: expected_prev,
                    error: format!("Audit entry digest tampered at seq {}.", entry.seq),
                };
            }
            expected_prev = entry.entry_sha256;
            expected_seq += 1;
            count += 1;
        }

        AuditVerification {
            valid: true,
            entry_count: count,
            head_sha256: expected_prev,
            error: String::new(),
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub fn append(
        &mut self,
        event_type: &str,
        plan_id: &str,
        step_id: &str,
        agent_profile: &str,
        risk_level: &str,
        outcome: &str,
        detail: &str,
    ) -> Result<AuditEntry, String> {
        ensure_safe_dir_0700(&self.storage_dir)?;
        ensure_non_symlink_0600(&self.audit_file)?;
        if !self.chain_valid {
            return Err(format!(
                "Fail-closed: audit chain integrity compromised ({}).",
                self.chain_error
            ));
        }

        let seq = self.last_seq + 1;
        let mut entry = AuditEntry {
            seq,
            timestamp_unix: now_unix_secs(),
            event_id: format!("aud-{}", Uuid::new_v4()),
            event_type: event_type.to_string(),
            plan_id: plan_id.to_string(),
            step_id: step_id.to_string(),
            agent_profile: agent_profile.to_string(),
            risk_level: risk_level.to_string(),
            outcome: outcome.to_string(),
            detail: detail.chars().take(512).collect(),
            prev_sha256: self.head_sha256.clone(),
            entry_sha256: String::new(),
        };
        entry.entry_sha256 = entry.compute_hash();

        let serialized = serde_json::to_string(&entry)
            .map_err(|e| format!("Failed serializing audit entry: {e}"))?;

        let mut f = OpenOptions::new()
            .create(true)
            .append(true)
            .mode(0o600)
            .open(&self.audit_file)
            .map_err(|e| format!("Failed opening audit-trail.jsonl for append: {e}"))?;
        writeln!(f, "{serialized}")
            .map_err(|e| format!("Failed writing audit-trail.jsonl: {e}"))?;
        f.sync_all()
            .map_err(|e| format!("Failed syncing audit-trail.jsonl: {e}"))?;
        fs::set_permissions(&self.audit_file, fs::Permissions::from_mode(0o600))
            .map_err(|e| format!("Failed setting 0600 on audit-trail.jsonl: {e}"))?;

        self.last_seq = seq;
        self.head_sha256 = entry.entry_sha256.clone();
        Ok(entry)
    }

    pub fn read_recent(&self, limit: usize) -> Vec<AuditEntry> {
        let Ok(file) = File::open(&self.audit_file) else {
            return Vec::new();
        };
        let reader = BufReader::new(file);
        let mut all = Vec::new();
        for line in reader.lines().flatten() {
            let trimmed = line.trim();
            if trimmed.is_empty() {
                continue;
            }
            if let Ok(entry) = serde_json::from_str::<AuditEntry>(trimmed) {
                all.push(entry);
            }
        }
        let max_n = limit.clamp(1, 200);
        if all.len() > max_n {
            all.split_off(all.len() - max_n)
        } else {
            all
        }
    }

    pub fn save_plans_json(&self, json_bytes: &[u8]) -> Result<(), String> {
        ensure_safe_dir_0700(&self.storage_dir)?;
        ensure_non_symlink_0600(&self.plans_file)?;
        let tmp_path = self
            .storage_dir
            .join(format!(".plans.tmp.{}", Uuid::new_v4()));
        {
            let mut f = OpenOptions::new()
                .create_new(true)
                .write(true)
                .mode(0o600)
                .open(&tmp_path)
                .map_err(|e| format!("Failed creating temp plans file: {e}"))?;
            f.write_all(json_bytes)
                .map_err(|e| format!("Failed writing temp plans file: {e}"))?;
            f.sync_all()
                .map_err(|e| format!("Failed syncing temp plans file: {e}"))?;
        }
        fs::set_permissions(&tmp_path, fs::Permissions::from_mode(0o600))
            .map_err(|e| format!("Failed setting 0600 on temp plans file: {e}"))?;
        fs::rename(&tmp_path, &self.plans_file)
            .map_err(|e| format!("Failed renaming plans file: {e}"))?;
        Ok(())
    }

    pub fn load_plans_json(&self) -> Option<String> {
        if !self.plans_file.exists() {
            return None;
        }
        let meta = fs::symlink_metadata(&self.plans_file).ok()?;
        if meta.file_type().is_symlink() || !meta.is_file() || meta.len() > 4_194_304 {
            return None;
        }
        fs::read_to_string(&self.plans_file).ok()
    }
}
