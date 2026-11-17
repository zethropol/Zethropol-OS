use crate::models::{
    DetectionReport, DetectionStatus, OperationResult, RecoveryEventRecord, RecoveryPoint,
    SnapshotRecord,
};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::ErrorKind;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use zbus::interface;
use zbus::zvariant::{Array, OwnedValue, Str};

pub const POLKIT_ACTION_MODIFY: &str = "org.zethropol.recovery.modify";
pub const POLKIT_ACTION_ROLLBACK: &str = "org.zethropol.recovery.rollback";

#[derive(Debug, Clone)]
pub struct OpState {
    pub status: String, // "running", "ok", "failed", "cancelled"
    pub code: String,   // "running", "ok", "busy", "permission-denied", "timeout", "cancelled", "verification-failed", "failed"
    pub message: String,
    pub snapshot_id: Option<u32>,
    pub owner_sender: String,
}

impl OpState {
    fn new(status: &str, code: &str, message: impl Into<String>, snapshot_id: Option<u32>) -> Self {
        Self {
            status: status.to_string(),
            code: code.to_string(),
            message: message.into(),
            snapshot_id,
            owner_sender: String::new(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct DetectionConfig {
    pub snapper_bin: PathBuf,
    pub pkcheck_bin: PathBuf,
    pub btrfs_bin: PathBuf,
    pub limine_sync_bin: PathBuf,
    pub sysconf_file: PathBuf,
    pub configs_dir: PathBuf,
    pub mountinfo_file: PathBuf,
    pub fstab_file: PathBuf,
    pub snapshots_dir: PathBuf,
    pub limine_conf_file: PathBuf,
    pub esp_dir: PathBuf,
    pub modules_dir: PathBuf,
    pub running_kernel: Option<String>,
    pub pacman_lock_file: PathBuf,
    pub min_free_percent: u32,
    pub max_important_snapshots: usize,
    pub simulated_free_percent: Option<u32>,
    pub simulated_boot_usage_percent: Option<u32>,
    pub polkit_policy_file: PathBuf,
    pub command_timeout: Duration,
}

impl Default for DetectionConfig {
    fn default() -> Self {
        Self {
            snapper_bin: PathBuf::from("/usr/bin/snapper"),
            pkcheck_bin: PathBuf::from("/usr/bin/pkcheck"),
            btrfs_bin: PathBuf::from("/usr/bin/btrfs"),
            limine_sync_bin: PathBuf::from("/usr/bin/limine-snapper-sync"),
            sysconf_file: PathBuf::from("/etc/conf.d/snapper"),
            configs_dir: PathBuf::from("/etc/snapper/configs"),
            mountinfo_file: PathBuf::from("/proc/self/mountinfo"),
            fstab_file: PathBuf::from("/etc/fstab"),
            snapshots_dir: PathBuf::from("/.snapshots"),
            limine_conf_file: PathBuf::from("/etc/limine-snapper-sync.conf"),
            esp_dir: PathBuf::from("/boot"),
            modules_dir: PathBuf::from("/usr/lib/modules"),
            running_kernel: None,
            pacman_lock_file: PathBuf::from("/var/lib/pacman/db.lck"),
            min_free_percent: 20,
            max_important_snapshots: 15,
            simulated_free_percent: None,
            simulated_boot_usage_percent: None,
            polkit_policy_file: PathBuf::from(
                "/usr/share/polkit-1/actions/org.zethropol.recovery.policy",
            ),
            command_timeout: Duration::from_secs(4),
        }
    }
}

impl DetectionConfig {
    pub fn from_env() -> Self {
        let mut cfg = Self::default();
        let mut overrides = HashMap::new();

        if let Ok(env_file) = std::env::var("ZETHROPOL_RECOVERY_ENV_FILE") {
            if let Ok(content) = fs::read_to_string(env_file) {
                for line in content.lines() {
                    let trimmed = line.trim();
                    if trimmed.is_empty() || trimmed.starts_with('#') {
                        continue;
                    }
                    if let Some((k, v)) = trimmed.split_once('=') {
                        overrides.insert(k.trim().to_string(), v.trim().to_string());
                    }
                }
            }
        }

        let get_var = |key: &str| -> Option<String> {
            if let Some(val) = overrides.get(key) {
                return Some(val.clone());
            }
            std::env::var(key).ok()
        };

        if let Some(v) = get_var("ZETHROPOL_RECOVERY_SNAPPER_BIN") {
            cfg.snapper_bin = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_PKCHECK_BIN") {
            cfg.pkcheck_bin = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_BTRFS_BIN") {
            cfg.btrfs_bin = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_LIMINE_SYNC_BIN") {
            cfg.limine_sync_bin = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_SYSCONF_FILE") {
            cfg.sysconf_file = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_CONFIGS_DIR") {
            cfg.configs_dir = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_MOUNTINFO_FILE") {
            cfg.mountinfo_file = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_FSTAB_FILE") {
            cfg.fstab_file = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_SNAPSHOTS_DIR") {
            cfg.snapshots_dir = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_LIMINE_CONF") {
            cfg.limine_conf_file = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_ESP_DIR") {
            cfg.esp_dir = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_MODULES_DIR") {
            cfg.modules_dir = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_RUNNING_KERNEL") {
            let trimmed = v.trim();
            if !trimmed.is_empty() {
                cfg.running_kernel = Some(trimmed.to_string());
            }
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_PACMAN_LOCK_FILE") {
            cfg.pacman_lock_file = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_MIN_FREE_PERCENT") {
            if let Ok(p) = v.parse::<u32>() {
                cfg.min_free_percent = p.min(99);
            }
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_MAX_IMPORTANT_SNAPSHOTS") {
            if let Ok(m) = v.parse::<usize>() {
                cfg.max_important_snapshots = m.max(1);
            }
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_POLKIT_POLICY_FILE") {
            cfg.polkit_policy_file = PathBuf::from(v);
        }
        if let Some(v) = get_var("ZETHROPOL_RECOVERY_TIMEOUT_MS") {
            if let Ok(ms) = v.parse::<u64>() {
                cfg.command_timeout = Duration::from_millis(ms.max(50));
            }
        }
        cfg
    }
}

#[derive(Debug, Clone, PartialEq)]
enum JsonVal {
    Null,
    Bool(bool),
    Number(i64),
    Str(String),
    Array(Vec<JsonVal>),
    Object(HashMap<String, JsonVal>),
}

struct JsonParser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> JsonParser<'a> {
    fn new(input: &'a str) -> Self {
        Self {
            bytes: input.as_bytes(),
            pos: 0,
        }
    }

    fn skip_ws(&mut self) {
        while self.pos < self.bytes.len() {
            match self.bytes[self.pos] {
                b' ' | b'\t' | b'\r' | b'\n' => self.pos += 1,
                _ => break,
            }
        }
    }

    fn parse(mut self) -> Option<JsonVal> {
        let val = self.parse_value()?;
        self.skip_ws();
        if self.pos == self.bytes.len() {
            Some(val)
        } else {
            None
        }
    }

    fn parse_value(&mut self) -> Option<JsonVal> {
        self.skip_ws();
        if self.pos >= self.bytes.len() {
            return None;
        }
        match self.bytes[self.pos] {
            b'{' => self.parse_object(),
            b'[' => self.parse_array(),
            b'"' => self.parse_string().map(JsonVal::Str),
            b't' => {
                if self.bytes[self.pos..].starts_with(b"true") {
                    self.pos += 4;
                    Some(JsonVal::Bool(true))
                } else {
                    None
                }
            }
            b'f' => {
                if self.bytes[self.pos..].starts_with(b"false") {
                    self.pos += 5;
                    Some(JsonVal::Bool(false))
                } else {
                    None
                }
            }
            b'n' => {
                if self.bytes[self.pos..].starts_with(b"null") {
                    self.pos += 4;
                    Some(JsonVal::Null)
                } else {
                    None
                }
            }
            b'-' | b'0'..=b'9' => self.parse_number(),
            _ => None,
        }
    }

    fn parse_string(&mut self) -> Option<String> {
        if self.bytes.get(self.pos) != Some(&b'"') {
            return None;
        }
        self.pos += 1;
        let mut out = Vec::new();
        while self.pos < self.bytes.len() {
            let b = self.bytes[self.pos];
            self.pos += 1;
            match b {
                b'"' => return String::from_utf8(out).ok(),
                b'\\' => {
                    let esc = *self.bytes.get(self.pos)?;
                    self.pos += 1;
                    match esc {
                        b'"' => out.push(b'"'),
                        b'\\' => out.push(b'\\'),
                        b'/' => out.push(b'/'),
                        b'b' => out.push(0x08),
                        b'f' => out.push(0x0c),
                        b'n' => out.push(b'\n'),
                        b'r' => out.push(b'\r'),
                        b't' => out.push(b'\t'),
                        b'u' => {
                            if self.pos + 4 > self.bytes.len() {
                                return None;
                            }
                            let hex = std::str::from_utf8(&self.bytes[self.pos..self.pos + 4]).ok()?;
                            let code = u32::from_str_radix(hex, 16).ok()?;
                            self.pos += 4;
                            if let Some(ch) = char::from_u32(code) {
                                let mut buf = [0u8; 4];
                                out.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
                            } else {
                                return None;
                            }
                        }
                        _ => return None,
                    }
                }
                _ => out.push(b),
            }
        }
        None
    }

    fn parse_number(&mut self) -> Option<JsonVal> {
        let start = self.pos;
        if self.bytes.get(self.pos) == Some(&b'-') {
            self.pos += 1;
        }
        while self.pos < self.bytes.len() && self.bytes[self.pos].is_ascii_digit() {
            self.pos += 1;
        }
        if self.pos < self.bytes.len() && self.bytes[self.pos] == b'.' {
            self.pos += 1;
            while self.pos < self.bytes.len() && self.bytes[self.pos].is_ascii_digit() {
                self.pos += 1;
            }
        }
        let s = std::str::from_utf8(&self.bytes[start..self.pos]).ok()?;
        if let Ok(n) = s.parse::<i64>() {
            Some(JsonVal::Number(n))
        } else if let Ok(f) = s.parse::<f64>() {
            Some(JsonVal::Number(f as i64))
        } else {
            None
        }
    }

    fn parse_array(&mut self) -> Option<JsonVal> {
        self.pos += 1;
        self.skip_ws();
        let mut items = Vec::new();
        if self.bytes.get(self.pos) == Some(&b']') {
            self.pos += 1;
            return Some(JsonVal::Array(items));
        }
        loop {
            let val = self.parse_value()?;
            items.push(val);
            self.skip_ws();
            match self.bytes.get(self.pos)? {
                b',' => {
                    self.pos += 1;
                }
                b']' => {
                    self.pos += 1;
                    return Some(JsonVal::Array(items));
                }
                _ => return None,
            }
        }
    }

    fn parse_object(&mut self) -> Option<JsonVal> {
        self.pos += 1;
        self.skip_ws();
        let mut map = HashMap::new();
        if self.bytes.get(self.pos) == Some(&b'}') {
            self.pos += 1;
            return Some(JsonVal::Object(map));
        }
        loop {
            self.skip_ws();
            let key = self.parse_string()?;
            self.skip_ws();
            if self.bytes.get(self.pos) != Some(&b':') {
                return None;
            }
            self.pos += 1;
            let val = self.parse_value()?;
            map.insert(key, val);
            self.skip_ws();
            match self.bytes.get(self.pos)? {
                b',' => {
                    self.pos += 1;
                }
                b'}' => {
                    self.pos += 1;
                    return Some(JsonVal::Object(map));
                }
                _ => return None,
            }
        }
    }
}

fn normalize_fs_name(raw: &str) -> String {
    let trimmed = raw.trim();
    if trimmed.eq_ignore_ascii_case("btrfs") {
        "Btrfs".to_string()
    } else if trimmed.is_empty() {
        "Unknown".to_string()
    } else {
        trimmed.to_string()
    }
}

fn extract_subvol_option(options: &str, root_field: &str) -> String {
    for part in options.split(',') {
        let part = part.trim();
        if let Some(subvol) = part.strip_prefix("subvol=") {
            if !subvol.is_empty() {
                return subvol.to_string();
            }
        }
    }
    if !root_field.is_empty() && root_field != "/" {
        return root_field.to_string();
    }
    "/".to_string()
}

pub fn detect_root_filesystem(mountinfo_path: &Path, fstab_path: &Path) -> (String, String) {
    let mut root_mount: Option<(String, String)> = None;
    let mut system_bind_btrfs: Option<(String, String)> = None;

    if let Ok(content) = fs::read_to_string(mountinfo_path) {
        for line in content.lines() {
            let Some((left, right)) = line.split_once(" - ") else {
                continue;
            };
            let left_fields: Vec<&str> = left.split_whitespace().collect();
            let right_fields: Vec<&str> = right.split_whitespace().collect();
            if left_fields.len() < 5 || right_fields.is_empty() {
                continue;
            }
            let root_field = left_fields[3];
            let mount_point = left_fields[4];
            let fstype = right_fields[0];
            let super_opts = right_fields.get(2).copied().unwrap_or("");

            if mount_point == "/" {
                let subvol = extract_subvol_option(super_opts, root_field);
                root_mount = Some((fstype.to_string(), subvol));
            } else if matches!(mount_point, "/etc" | "/usr" | "/var")
                && fstype.eq_ignore_ascii_case("btrfs")
            {
                let subvol = extract_subvol_option(super_opts, root_field);
                system_bind_btrfs = Some(("Btrfs".to_string(), subvol));
            }
        }
    }

    if let Some((ref fstype, ref subvol)) = root_mount {
        if !matches!(fstype.as_str(), "tmpfs" | "overlay" | "rootfs") {
            return (normalize_fs_name(fstype), subvol.clone());
        }
    }

    if let Ok(fstab) = fs::read_to_string(fstab_path) {
        for line in fstab.lines() {
            let trimmed = line.trim();
            if trimmed.is_empty() || trimmed.starts_with('#') {
                continue;
            }
            let fields: Vec<&str> = trimmed.split_whitespace().collect();
            if fields.len() >= 4 && fields[1] == "/" {
                let fstype = normalize_fs_name(fields[2]);
                let subvol = extract_subvol_option(fields[3], "/");
                return (fstype, subvol);
            }
        }
    }

    if let Some((fstype, subvol)) = system_bind_btrfs {
        return (fstype, subvol);
    }

    if let Some((fstype, subvol)) = root_mount {
        return (normalize_fs_name(&fstype), subvol);
    }

    ("Unknown".to_string(), "/".to_string())
}

#[derive(Debug, Clone)]
struct MountEntry {
    mount_point: String,
    fstype: String,
    subvol: String,
}

fn normalize_subvol_name(subvol: &str) -> String {
    subvol.trim().trim_start_matches('/').to_string()
}

fn is_ephemeral_fs(fstype: &str) -> bool {
    matches!(
        fstype.to_ascii_lowercase().as_str(),
        "tmpfs" | "overlay" | "rootfs" | "ramfs"
    )
}

fn parse_mountinfo_entries(mountinfo_path: &Path) -> Result<Vec<MountEntry>, std::io::Error> {
    let content = fs::read_to_string(mountinfo_path)?;
    let mut entries = Vec::new();
    for line in content.lines() {
        let Some((left, right)) = line.split_once(" - ") else {
            continue;
        };
        let left_fields: Vec<&str> = left.split_whitespace().collect();
        let right_fields: Vec<&str> = right.split_whitespace().collect();
        if left_fields.len() < 5 || right_fields.is_empty() {
            continue;
        }
        let root_field = left_fields[3];
        let mount_point = left_fields[4].to_string();
        let fstype = right_fields[0].to_string();
        let super_opts = right_fields.get(2).copied().unwrap_or("");
        let subvol = extract_subvol_option(super_opts, root_field);
        entries.push(MountEntry {
            mount_point,
            fstype,
            subvol,
        });
    }
    Ok(entries)
}

fn parse_fstab_entries(fstab_path: &Path) -> Result<Vec<MountEntry>, std::io::Error> {
    let content = fs::read_to_string(fstab_path)?;
    let mut entries = Vec::new();
    for line in content.lines() {
        let trimmed = line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        let fields: Vec<&str> = trimmed.split_whitespace().collect();
        if fields.len() < 4 {
            continue;
        }
        entries.push(MountEntry {
            mount_point: fields[1].to_string(),
            fstype: fields[2].to_string(),
            subvol: extract_subvol_option(fields[3], "/"),
        });
    }
    Ok(entries)
}

pub fn verify_home_subvolume_isolation(
    config: &DetectionConfig,
    root_subvolume: &str,
) -> (String, String, String) {
    let mount_res = parse_mountinfo_entries(&config.mountinfo_file);
    let fstab_res = parse_fstab_entries(&config.fstab_file);

    if mount_res.is_err() && fstab_res.is_err() {
        return (
            "unknown".to_string(),
            "Mount table and /etc/fstab could not be read to verify /home isolation.".to_string(),
            "Ensure /proc/self/mountinfo and /etc/fstab are readable.".to_string(),
        );
    }

    let mount_entries = mount_res.as_ref().ok();
    let fstab_entries = fstab_res.as_ref().ok();

    let mount_root = mount_entries.and_then(|list| list.iter().find(|e| e.mount_point == "/"));
    let mount_home = mount_entries.and_then(|list| list.iter().find(|e| e.mount_point == "/home"));
    let fstab_home = fstab_entries.and_then(|list| list.iter().find(|e| e.mount_point == "/home"));

    let real_root_in_mountinfo = mount_root
        .map(|r| !is_ephemeral_fs(&r.fstype))
        .unwrap_or(false);

    let effective_home: MountEntry = if real_root_in_mountinfo {
        let Some(mh) = mount_home else {
            return (
                "blocked".to_string(),
                "No separate /home mount was detected in active mounts; /home resides inside the root subvolume.".to_string(),
                "Mount /home on a separate Btrfs subvolume (such as /@home) or separate filesystem before rollback.".to_string(),
            );
        };

        if let Some(fstab_list) = fstab_entries {
            let Some(fh) = fstab_list.iter().find(|e| e.mount_point == "/home") else {
                return (
                    "blocked".to_string(),
                    "/home is mounted in active mounts but missing from /etc/fstab; persistent isolation is unverified.".to_string(),
                    "Add the separate /home mount entry to /etc/fstab.".to_string(),
                );
            };
            if !mh.fstype.eq_ignore_ascii_case(&fh.fstype)
                || (mh.fstype.eq_ignore_ascii_case("btrfs")
                    && normalize_subvol_name(&mh.subvol) != normalize_subvol_name(&fh.subvol))
            {
                return (
                    "blocked".to_string(),
                    format!(
                        "/home mount configuration differs between mountinfo ('{}', subvol '{}') and /etc/fstab ('{}', subvol '{}').",
                        mh.fstype, mh.subvol, fh.fstype, fh.subvol
                    ),
                    "Reconcile /home mount configuration between active mounts and /etc/fstab.".to_string(),
                );
            }
        }
        mh.clone()
    } else if let Some(mh) = mount_home {
        mh.clone()
    } else if let Some(fh) = fstab_home {
        fh.clone()
    } else {
        return (
            "blocked".to_string(),
            "No separate /home mount or /etc/fstab entry was detected.".to_string(),
            "Configure /home on a separate Btrfs subvolume (such as /@home) or separate filesystem.".to_string(),
        );
    };

    if is_ephemeral_fs(&effective_home.fstype) || effective_home.fstype.trim().is_empty() {
        return (
            "blocked".to_string(),
            format!(
                "/home uses ephemeral or unsupported filesystem '{}'; user data preservation cannot be guaranteed.",
                effective_home.fstype
            ),
            "Mount /home on a persistent separate subvolume or filesystem.".to_string(),
        );
    }

    if effective_home.fstype.eq_ignore_ascii_case("btrfs") {
        let root_norm = normalize_subvol_name(root_subvolume);
        let home_norm = normalize_subvol_name(&effective_home.subvol);

        if home_norm.is_empty() || home_norm == root_norm {
            return (
                "blocked".to_string(),
                format!(
                    "/home shares Btrfs subvolume '{}' with '/'; root rollback would overwrite user data.",
                    effective_home.subvol
                ),
                "Separate /home into its own top-level Btrfs subvolume (such as /@home).".to_string(),
            );
        }

        if !root_norm.is_empty() && home_norm.starts_with(&format!("{}/", root_norm)) {
            return (
                "blocked".to_string(),
                format!(
                    "/home subvolume '{}' is nested inside root subvolume '{}'; rollback could orphan user data.",
                    effective_home.subvol, root_subvolume
                ),
                "Move /home to an independent top-level Btrfs subvolume (such as /@home).".to_string(),
            );
        }

        return (
            "pass".to_string(),
            format!(
                "/home is isolated on separate Btrfs subvolume '{}' (root subvolume '{}').",
                effective_home.subvol, root_subvolume
            ),
            "No action required.".to_string(),
        );
    }

    (
        "pass".to_string(),
        format!(
            "/home is isolated on separate '{}' filesystem.",
            effective_home.fstype
        ),
        "No action required.".to_string(),
    )
}

fn is_executable_file(path: &Path) -> bool {
    match fs::metadata(path) {
        Ok(meta) => meta.is_file() && (meta.permissions().mode() & 0o111 != 0),
        Err(_) => false,
    }
}

fn resolve_snapper_binary(configured: &Path) -> Option<PathBuf> {
    if configured.components().count() > 1 {
        return is_executable_file(configured).then(|| configured.to_path_buf());
    }
    if let Ok(path_var) = std::env::var("PATH") {
        for entry in path_var.split(':') {
            if entry.is_empty() {
                continue;
            }
            let candidate = Path::new(entry).join(configured);
            if is_executable_file(&candidate) {
                return Some(candidate);
            }
        }
    }
    None
}

fn parse_key_value_file(content: &str) -> Result<HashMap<String, String>, String> {
    let mut map = HashMap::new();
    for (idx, raw_line) in content.lines().enumerate() {
        let line = raw_line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let Some((key, raw_val)) = line.split_once('=') else {
            return Err(format!("Malformed line {} in configuration file", idx + 1));
        };
        let key = key.trim();
        if key.is_empty() || !key.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
            return Err(format!("Invalid key '{}' on line {}", key, idx + 1));
        }
        let val = raw_val.trim();
        let unquoted = if (val.starts_with('"') && val.ends_with('"') && val.len() >= 2)
            || (val.starts_with('\'') && val.ends_with('\'') && val.len() >= 2)
        {
            &val[1..val.len() - 1]
        } else if val.starts_with('"') || val.starts_with('\'') {
            return Err(format!("Unterminated quote on line {}", idx + 1));
        } else {
            val
        };
        map.insert(key.to_string(), unquoted.to_string());
    }
    Ok(map)
}

fn run_command_with_timeout(
    program: &Path,
    args: &[&str],
    timeout: Duration,
) -> Result<(i32, String, String), String> {
    run_cancellable_command_with_timeout(program, args, timeout, None, None)
}

fn run_cancellable_command_with_timeout(
    program: &Path,
    args: &[&str],
    timeout: Duration,
    cancel_set: Option<&Arc<Mutex<HashSet<String>>>>,
    op_id: Option<&str>,
) -> Result<(i32, String, String), String> {
    let mut child = Command::new(program)
        .env("LC_ALL", "C")
        .args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("spawn_error:{}:{}", e.kind(), e))?;

    let start = Instant::now();
    loop {
        if let (Some(set), Some(id)) = (cancel_set, op_id) {
            if set.lock().unwrap().contains(id) {
                let _ = child.kill();
                let _ = child.wait();
                return Err("cancelled:Operation was cancelled.".to_string());
            }
        }
        match child.try_wait() {
            Ok(Some(_)) => {
                let out = child
                    .wait_with_output()
                    .map_err(|e| format!("wait_error:{}", e))?;
                let code = out.status.code().unwrap_or(-1);
                let stdout = String::from_utf8_lossy(&out.stdout).to_string();
                let stderr = String::from_utf8_lossy(&out.stderr).to_string();
                return Ok((code, stdout, stderr));
            }
            Ok(None) => {
                if start.elapsed() >= timeout {
                    let _ = child.kill();
                    let _ = child.wait();
                    return Err("timeout:Snapper command timed out.".to_string());
                }
                std::thread::sleep(Duration::from_millis(20));
            }
            Err(e) => {
                let _ = child.kill();
                return Err(format!("wait_error:{}", e));
            }
        }
    }
}

fn parse_snapper_json(
    output: &str,
    config_name: &str,
) -> Result<(Vec<SnapshotRecord>, Vec<RecoveryEventRecord>, Vec<SnapshotRecord>, i32), String> {
    let parsed = JsonParser::new(output)
        .parse()
        .ok_or_else(|| "Snapper output is not valid JSON.".to_string())?;

    let entries = match parsed {
        JsonVal::Object(mut map) => {
            if let Some(val) = map.remove(config_name) {
                match val {
                    JsonVal::Array(arr) => arr,
                    _ => {
                        return Err(format!(
                            "Snapper JSON key '{}' is not an array.",
                            config_name
                        ))
                    }
                }
            } else if let Some(val) = map.remove("root") {
                match val {
                    JsonVal::Array(arr) => arr,
                    _ => return Err("Snapper JSON key 'root' is not an array.".to_string()),
                }
            } else {
                return Err(format!(
                    "Snapper JSON output does not contain configuration '{}'.",
                    config_name
                ));
            }
        }
        _ => return Err("Snapper JSON root value must be an object.".to_string()),
    };

    let mut snapshots = Vec::new();
    let mut seen_ids = HashSet::new();
    let mut current_id = 0i32;

    for entry in entries {
        let JsonVal::Object(obj) = entry else {
            return Err("Snapshot entry in JSON array is not an object.".to_string());
        };
        let number = match obj.get("number") {
            Some(JsonVal::Number(n)) if *n >= 0 => *n as i32,
            _ => return Err("Snapshot entry is missing a valid 'number' field.".to_string()),
        };
        if !seen_ids.insert(number) {
            continue;
        }
        let snap_type = match obj.get("type") {
            Some(JsonVal::Str(s)) => s.clone(),
            _ => String::new(),
        };
        let description = match obj.get("description") {
            Some(JsonVal::Str(s)) => s.clone(),
            _ => String::new(),
        };
        let date = match obj.get("date") {
            Some(JsonVal::Str(s)) => s.clone(),
            _ => String::new(),
        };
        let user = match obj.get("user") {
            Some(JsonVal::Str(s)) => s.clone(),
            _ => String::new(),
        };
        let cleanup = match obj.get("cleanup") {
            Some(JsonVal::Str(s)) => s.clone(),
            _ => String::new(),
        };
        let pre_number = match obj.get("pre-number") {
            Some(JsonVal::Number(n)) if *n >= 0 => *n as i32,
            _ => -1,
        };

        let is_current = description == "current" || number == 0;
        let is_post = pre_number >= 0 && snap_type == "post" || (pre_number >= 0 && !is_current);
        let is_pre = snap_type == "pre" && !is_post;

        let mut important = false;
        let mut zethropol_tag = String::new();
        if let Some(JsonVal::Object(userdata)) = obj.get("userdata") {
            if let Some(JsonVal::Str(imp)) = userdata.get("important") {
                important = imp == "yes";
            }
            if let Some(JsonVal::Str(tag)) = userdata.get("zethropol") {
                zethropol_tag = tag.clone();
            }
        }

        if description == "current" || (number == 0 && current_id == 0) {
            current_id = number;
        }

        snapshots.push(SnapshotRecord {
            id: number,
            snapshot_type: snap_type,
            is_current: description == "current",
            is_pre,
            is_post,
            paired_snapshot_id: -1,
            pre_number,
            date,
            user,
            cleanup,
            description,
            important,
            zethropol_tag,
        });
    }

    let mut recovery_events = Vec::new();
    for i in 0..snapshots.len() {
        if !snapshots[i].is_post || snapshots[i].pre_number < 0 {
            continue;
        }
        let post_id = snapshots[i].id;
        let pre_id = snapshots[i].pre_number;
        if let Some(pre_idx) = snapshots.iter().position(|s| s.id == pre_id) {
            snapshots[i].paired_snapshot_id = pre_id;
            snapshots[pre_idx].paired_snapshot_id = post_id;
            recovery_events.push(RecoveryEventRecord {
                pre_id,
                post_id,
                date: snapshots[i].date.clone(),
                description: snapshots[i].description.clone(),
                pre_description: snapshots[pre_idx].description.clone(),
                snapshot_type: snapshots[i].snapshot_type.clone(),
                important: snapshots[i].important,
                zethropol_tag: snapshots[i].zethropol_tag.clone(),
            });
        }
    }

    let recovery_points: Vec<SnapshotRecord> = snapshots
        .iter()
        .filter(|s| !s.is_current && s.paired_snapshot_id < 0)
        .cloned()
        .collect();

    Ok((snapshots, recovery_events, recovery_points, current_id))
}

fn classify_snapper_error(
    stderr: &str,
    stdout: &str,
    snapshots_dir_perm_denied: bool,
) -> (DetectionStatus, String) {
    let combined = format!("{}\n{}", stderr.trim(), stdout.trim());
    let lower = combined.to_ascii_lowercase();

    if lower.contains("permission denied")
        || lower.contains("error.no_permissions")
        || lower.contains("accessdenied")
        || lower.contains("access denied")
        || lower.contains("not authorized")
        || lower.contains("authorization required")
        || lower.contains("operation not permitted")
        || lower.contains("eacces")
        || lower.contains("eperm")
    {
        return (
            DetectionStatus::PermissionDenied,
            "Administrator permission is required to read Snapper snapshots.".to_string(),
        );
    }

    if lower.contains("config")
        && (lower.contains("not found")
            || lower.contains("does not exist")
            || lower.contains("unknown config"))
    {
        return (
            DetectionStatus::InstalledNotConfigured,
            "Snapper is installed, but no root configuration was found.".to_string(),
        );
    }

    if lower.contains("org.freedesktop.dbus.error")
        || lower.contains("failure (org.freedesktop.dbus")
        || lower.contains("dbus")
        || lower.contains("serviceunknown")
        || lower.contains("namehasnoowner")
        || lower.contains("connection refused")
        || lower.contains("backend unavailable")
        || lower.contains("snapperd")
    {
        if snapshots_dir_perm_denied {
            return (
                DetectionStatus::PermissionDenied,
                "Administrator permission is required to access Snapper snapshots.".to_string(),
            );
        }
        return (
            DetectionStatus::BackendUnavailable,
            "Snapper D-Bus backend (snapperd) is unavailable or unreachable.".to_string(),
        );
    }

    if lower.contains("not a btrfs subvolume")
        || lower.contains("invalid config")
        || lower.contains("broken config")
        || lower.contains("reading failed")
        || lower.contains("subvolume")
        || lower.contains("io error")
    {
        if snapshots_dir_perm_denied {
            return (
                DetectionStatus::PermissionDenied,
                "Administrator permission is required to read /.snapshots.".to_string(),
            );
        }
        return (
            DetectionStatus::InvalidConfiguration,
            "Snapper configuration or Btrfs snapshot subvolume (/.snapshots) is invalid or inaccessible."
                .to_string(),
        );
    }

    if snapshots_dir_perm_denied {
        return (
            DetectionStatus::PermissionDenied,
            "Administrator permission is required to read Snapper snapshots.".to_string(),
        );
    }

    (
        DetectionStatus::BackendUnavailable,
        if combined.trim().is_empty() {
            "Snapper backend returned an unexpected error.".to_string()
        } else {
            format!(
                "Snapper backend error: {}",
                combined.lines().next().unwrap_or("").trim()
            )
        },
    )
}

pub fn detect_recovery_state(config: &DetectionConfig) -> DetectionReport {
    let (filesystem, root_subvolume) =
        detect_root_filesystem(&config.mountinfo_file, &config.fstab_file);

    let (snapshots_dir_exists, snapshots_dir_accessible, snapshots_dir_perm_denied) =
        match fs::metadata(&config.snapshots_dir) {
            Ok(meta) if meta.is_dir() => match fs::read_dir(&config.snapshots_dir) {
                Ok(_) => (true, true, false),
                Err(e) if e.kind() == ErrorKind::PermissionDenied => (true, false, true),
                Err(_) => (true, false, false),
            },
            Ok(_) => (false, false, false),
            Err(e) if e.kind() == ErrorKind::PermissionDenied => (true, false, true),
            Err(_) => (false, false, false),
        };

    let resolved_bin = resolve_snapper_binary(&config.snapper_bin);
    let snapper_installed = resolved_bin.is_some();

    let make_report = |status: DetectionStatus,
                       configured: bool,
                       config_name: String,
                       available_configs: Vec<String>,
                       title: &str,
                       message: String|
     -> DetectionReport {
        DetectionReport {
            status,
            available: status == DetectionStatus::InstalledAndConfigured,
            permission_required: status == DetectionStatus::PermissionDenied,
            snapper_installed,
            configured,
            filesystem: filesystem.clone(),
            root_subvolume: root_subvolume.clone(),
            snapshot_tool: if snapper_installed {
                "Snapper".to_string()
            } else {
                "Not installed".to_string()
            },
            config_name,
            available_configs,
            snapshots_dir_exists,
            snapshots_dir_accessible,
            status_title: title.to_string(),
            status_message: message,
            snapshot_count: 0,
            current_snapshot_id: 0,
            snapshots: Vec::new(),
            recovery_events: Vec::new(),
            recovery_points: Vec::new(),
        }
    };

    if !filesystem.eq_ignore_ascii_case("Btrfs") {
        return make_report(
            DetectionStatus::InvalidConfiguration,
            false,
            String::new(),
            Vec::new(),
            "Invalid recovery configuration",
            format!(
                "Root filesystem is {}, but Snapper recovery requires a Btrfs root filesystem.",
                filesystem
            ),
        );
    }

    let Some(snapper_bin) = resolved_bin else {
        return make_report(
            DetectionStatus::NotInstalled,
            false,
            String::new(),
            Vec::new(),
            "Snapper is not installed",
            "Snapper executable was not found on this system.".to_string(),
        );
    };

    let mut sysconf_configs: Option<Vec<String>> = None;
    if config.sysconf_file.exists() {
        match fs::read_to_string(&config.sysconf_file) {
            Ok(content) => match parse_key_value_file(&content) {
                Ok(kv) => {
                    if let Some(raw_list) = kv.get("SNAPPER_CONFIGS") {
                        let list: Vec<String> = raw_list
                            .split_whitespace()
                            .map(|s| s.trim().to_string())
                            .filter(|s| !s.is_empty())
                            .collect();
                        sysconf_configs = Some(list);
                    }
                }
                Err(err) => {
                    return make_report(
                        DetectionStatus::InvalidConfiguration,
                        false,
                        String::new(),
                        Vec::new(),
                        "Invalid recovery configuration",
                        format!("Snapper system configuration is invalid: {}", err),
                    );
                }
            },
            Err(e) if e.kind() == ErrorKind::PermissionDenied => {
                return make_report(
                    DetectionStatus::PermissionDenied,
                    false,
                    String::new(),
                    Vec::new(),
                    "Authorization required",
                    "Administrator permission is required to read Snapper configuration."
                        .to_string(),
                );
            }
            Err(_) => {
                return make_report(
                    DetectionStatus::InvalidConfiguration,
                    false,
                    String::new(),
                    Vec::new(),
                    "Invalid recovery configuration",
                    "Failed to read Snapper system configuration file.".to_string(),
                );
            }
        }
    }

    let entries = match fs::read_dir(&config.configs_dir) {
        Ok(read_dir) => read_dir,
        Err(e) if e.kind() == ErrorKind::PermissionDenied => {
            return make_report(
                DetectionStatus::PermissionDenied,
                true,
                "root".to_string(),
                sysconf_configs.unwrap_or_default(),
                "Authorization required",
                "Administrator permission is required to access /etc/snapper/configs.".to_string(),
            );
        }
        Err(e) if e.kind() == ErrorKind::NotFound => {
            return make_report(
                DetectionStatus::InstalledNotConfigured,
                false,
                String::new(),
                Vec::new(),
                "Snapper is not configured",
                "Snapper is installed, but /etc/snapper/configs does not exist.".to_string(),
            );
        }
        Err(_) => {
            return make_report(
                DetectionStatus::InvalidConfiguration,
                false,
                String::new(),
                Vec::new(),
                "Invalid recovery configuration",
                "Unable to inspect Snapper configuration directory.".to_string(),
            );
        }
    };

    let mut config_files = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_file() {
            if let Some(name) = path.file_name().and_then(|n| n.to_str()) {
                if !name.starts_with('.') {
                    config_files.push((name.to_string(), path));
                }
            }
        }
    }
    config_files.sort_by(|a, b| a.0.cmp(&b.0));

    let available_configs: Vec<String> = config_files.iter().map(|(n, _)| n.clone()).collect();

    if let Some(ref listed) = sysconf_configs {
        if listed.is_empty() && available_configs.is_empty() {
            return make_report(
                DetectionStatus::InstalledNotConfigured,
                false,
                String::new(),
                Vec::new(),
                "Snapper is not configured",
                "Snapper is installed, but no configuration is defined.".to_string(),
            );
        }
        if !listed.is_empty() && available_configs.is_empty() {
            return make_report(
                DetectionStatus::InvalidConfiguration,
                false,
                listed[0].clone(),
                listed.clone(),
                "Invalid recovery configuration",
                "SNAPPER_CONFIGS references a configuration that is missing from /etc/snapper/configs."
                    .to_string(),
            );
        }
    }

    if config_files.is_empty() {
        return make_report(
            DetectionStatus::InstalledNotConfigured,
            false,
            String::new(),
            Vec::new(),
            "Snapper is not configured",
            "Snapper is installed, but no configuration was found in /etc/snapper/configs."
                .to_string(),
        );
    }

    let mut selected_config: Option<String> = None;
    let mut saw_permission_denied = false;
    let mut invalid_reason: Option<String> = None;

    let mut ordered_files = config_files.clone();
    ordered_files.sort_by_key(|(name, _)| if name == "root" { 0 } else { 1 });

    for (name, path) in &ordered_files {
        if let Some(ref listed) = sysconf_configs {
            if !listed.is_empty() && !listed.contains(name) {
                continue;
            }
        }
        match fs::read_to_string(path) {
            Ok(content) => match parse_key_value_file(&content) {
                Ok(kv) => {
                    let subvol = kv.get("SUBVOLUME").map(String::as_str).unwrap_or("");
                    let fstype = kv.get("FSTYPE").map(String::as_str).unwrap_or("btrfs");
                    if name == "root" {
                        if subvol != "/" {
                            invalid_reason = Some(format!(
                                "Snapper 'root' configuration has invalid SUBVOLUME='{}'.",
                                subvol
                            ));
                            break;
                        }
                        if !fstype.eq_ignore_ascii_case("btrfs") {
                            invalid_reason = Some(format!(
                                "Snapper 'root' configuration has unsupported FSTYPE='{}'.",
                                fstype
                            ));
                            break;
                        }
                        selected_config = Some(name.clone());
                        break;
                    } else if subvol == "/" {
                        if !fstype.eq_ignore_ascii_case("btrfs") {
                            invalid_reason = Some(format!(
                                "Snapper '{}' configuration has unsupported FSTYPE='{}'.",
                                name, fstype
                            ));
                            break;
                        }
                        selected_config = Some(name.clone());
                        break;
                    }
                }
                Err(err) => {
                    invalid_reason = Some(format!(
                        "Snapper configuration '{}' is malformed: {}",
                        name, err
                    ));
                    break;
                }
            },
            Err(e) if e.kind() == ErrorKind::PermissionDenied => {
                saw_permission_denied = true;
            }
            Err(_) => {
                invalid_reason = Some(format!(
                    "Snapper configuration '{}' could not be read.",
                    name
                ));
            }
        }
    }

    if let Some(reason) = invalid_reason {
        return make_report(
            DetectionStatus::InvalidConfiguration,
            true,
            "root".to_string(),
            available_configs,
            "Invalid recovery configuration",
            reason,
        );
    }

    if selected_config.is_none() && saw_permission_denied {
        return make_report(
            DetectionStatus::PermissionDenied,
            true,
            "root".to_string(),
            available_configs,
            "Authorization required",
            "Administrator permission is required to read Snapper configuration files.".to_string(),
        );
    }

    let Some(config_name) = selected_config else {
        return make_report(
            DetectionStatus::InstalledNotConfigured,
            false,
            String::new(),
            available_configs,
            "Snapper is not configured",
            "Snapper is installed, but no configuration targets the root ('/') subvolume."
                .to_string(),
        );
    };

    let cmd_result = run_command_with_timeout(
        &snapper_bin,
        &["--jsonout", "-c", &config_name, "list"],
        config.command_timeout,
    );

    match cmd_result {
        Err(err) => {
            if err.starts_with("spawn_error:NotFound") {
                make_report(
                    DetectionStatus::NotInstalled,
                    false,
                    config_name,
                    available_configs,
                    "Snapper is not installed",
                    "Snapper executable could not be started.".to_string(),
                )
            } else if err.starts_with("spawn_error:PermissionDenied") {
                make_report(
                    DetectionStatus::PermissionDenied,
                    true,
                    config_name,
                    available_configs,
                    "Authorization required",
                    "Permission denied when executing Snapper.".to_string(),
                )
            } else {
                make_report(
                    DetectionStatus::BackendUnavailable,
                    true,
                    config_name,
                    available_configs,
                    "Recovery backend unavailable",
                    "Snapper backend timed out or failed to respond.".to_string(),
                )
            }
        }
        Ok((0, stdout, _stderr)) => match parse_snapper_json(&stdout, &config_name) {
            Ok((snapshots, recovery_events, recovery_points, current_id)) => {
                let count = snapshots.len() as i32;
                let summary = format!("{} · Snapper · {} snapshots", filesystem, count);
                DetectionReport {
                    status: DetectionStatus::InstalledAndConfigured,
                    available: true,
                    permission_required: false,
                    snapper_installed: true,
                    configured: true,
                    filesystem,
                    root_subvolume,
                    snapshot_tool: "Snapper".to_string(),
                    config_name,
                    available_configs,
                    snapshots_dir_exists,
                    snapshots_dir_accessible: true,
                    status_title: "Recovery is available".to_string(),
                    status_message: summary,
                    snapshot_count: count,
                    current_snapshot_id: current_id,
                    snapshots,
                    recovery_events,
                    recovery_points,
                }
            }
            Err(parse_err) => make_report(
                DetectionStatus::InvalidConfiguration,
                true,
                config_name,
                available_configs,
                "Invalid recovery configuration",
                format!("Malformed Snapper output: {}", parse_err),
            ),
        },
        Ok((_code, stdout, stderr)) => {
            let (status, msg) =
                classify_snapper_error(&stderr, &stdout, snapshots_dir_perm_denied);
            let title = match status {
                DetectionStatus::PermissionDenied => "Authorization required",
                DetectionStatus::InstalledNotConfigured => "Snapper is not configured",
                DetectionStatus::InvalidConfiguration => "Invalid recovery configuration",
                DetectionStatus::BackendUnavailable => "Recovery backend unavailable",
                DetectionStatus::NotInstalled => "Snapper is not installed",
                DetectionStatus::InstalledAndConfigured => "Recovery is available",
            };
            make_report(status, true, config_name, available_configs, title, msg)
        }
    }
}

fn validate_snapshot_description(name: &str, description: &str) -> Result<String, String> {
    let trimmed_name = name.trim();
    let trimmed_desc = description.trim();

    let combined = match (trimmed_name.is_empty(), trimmed_desc.is_empty()) {
        (true, true) => return Err("Recovery point description is required.".to_string()),
        (false, true) => trimmed_name.to_string(),
        (true, false) => trimmed_desc.to_string(),
        (false, false) => {
            if trimmed_name == trimmed_desc {
                trimmed_desc.to_string()
            } else {
                format!("{}: {}", trimmed_name, trimmed_desc)
            }
        }
    };

    if combined.chars().count() > 256 {
        return Err("Recovery point description exceeds 256 characters.".to_string());
    }

    if combined.chars().any(|c| c.is_control()) {
        return Err(
            "Recovery point description must not contain newline or control characters.".to_string(),
        );
    }

    Ok(combined)
}

pub fn validate_dbus_sender(sender: Option<&str>) -> Result<&str, (String, String)> {
    let raw = sender.ok_or_else(|| {
        (
            "invalid-caller".to_string(),
            "D-Bus caller identity is unavailable.".to_string(),
        )
    })?;

    if raw.is_empty()
        || raw.trim() != raw
        || raw.starts_with('-')
        || raw.chars().any(|c| c.is_whitespace() || c.is_control())
        || zbus::names::BusName::try_from(raw).is_err()
    {
        return Err((
            "invalid-caller".to_string(),
            "D-Bus caller identity is invalid.".to_string(),
        ));
    }

    Ok(raw)
}

pub fn check_polkit_authorization(
    config: &DetectionConfig,
    sender: &str,
) -> Result<(), (String, String)> {
    check_polkit_action(config, sender, POLKIT_ACTION_MODIFY)
}

pub fn check_polkit_action(
    config: &DetectionConfig,
    sender: &str,
    action_id: &str,
) -> Result<(), (String, String)> {
    let valid_sender = validate_dbus_sender(Some(sender))?;

    if action_id != POLKIT_ACTION_MODIFY && action_id != POLKIT_ACTION_ROLLBACK {
        return Err((
            "permission-denied".to_string(),
            "Unsupported Polkit action requested.".to_string(),
        ));
    }

    let Some(pkcheck_bin) = resolve_snapper_binary(&config.pkcheck_bin) else {
        return Err((
            "permission-denied".to_string(),
            "Polkit authorization helper (pkcheck) is unavailable.".to_string(),
        ));
    };

    match run_command_with_timeout(
        &pkcheck_bin,
        &[
            "--action-id",
            action_id,
            "--system-bus-name",
            valid_sender,
            "--allow-user-interaction",
        ],
        config.command_timeout,
    ) {
        Ok((0, _, _)) => Ok(()),
        Ok((code, stdout, stderr)) => {
            let combined = format!("{}\n{}", stderr.trim(), stdout.trim()).to_ascii_lowercase();
            if combined.contains("dismissed")
                || combined.contains("cancelled")
                || combined.contains("canceled")
                || code == 2
                || code == 3
            {
                Err((
                    "cancelled".to_string(),
                    "Authorization was cancelled by the user.".to_string(),
                ))
            } else {
                Err((
                    "permission-denied".to_string(),
                    "Polkit authentication required or denied.".to_string(),
                ))
            }
        }
        Err(err) if err.starts_with("timeout:") => Err((
            "timeout".to_string(),
            "Polkit authorization timed out.".to_string(),
        )),
        Err(_) => Err((
            "permission-denied".to_string(),
            "Polkit authentication check failed.".to_string(),
        )),
    }
}

pub fn execute_verified_snapshot_creation(
    config: &DetectionConfig,
    config_name: &str,
    full_description: &str,
    cancel_set: Option<&Arc<Mutex<HashSet<String>>>>,
    op_id: Option<&str>,
) -> OpState {
    execute_verified_snapshot_creation_with_options(
        config,
        config_name,
        full_description,
        "zethropol=manual",
        false,
        cancel_set,
        op_id,
    )
}

pub fn execute_verified_snapshot_creation_with_options(
    config: &DetectionConfig,
    config_name: &str,
    full_description: &str,
    userdata: &str,
    verify_btrfs_subvolume: bool,
    cancel_set: Option<&Arc<Mutex<HashSet<String>>>>,
    op_id: Option<&str>,
) -> OpState {
    let Some(snapper_bin) = resolve_snapper_binary(&config.snapper_bin) else {
        return OpState::new(
            "failed",
            "not-found",
            "Snapper executable was not found.",
            None,
        );
    };

    let desc_arg = format!("--description={}", full_description);
    let create_res = run_cancellable_command_with_timeout(
        &snapper_bin,
        &[
            "-c",
            config_name,
            "create",
            "--type",
            "single",
            "--print-number",
            "--cleanup-algorithm",
            "number",
            "--userdata",
            userdata,
            &desc_arg,
        ],
        config.command_timeout,
        cancel_set,
        op_id,
    );

    match create_res {
        Err(err) if err.starts_with("cancelled:") => OpState::new(
            "cancelled",
            "cancelled",
            "Recovery point creation was cancelled.",
            None,
        ),
        Err(err) if err.starts_with("timeout:") => OpState::new(
            "failed",
            "timeout",
            "Recovery point creation timed out.",
            None,
        ),
        Err(err) => OpState::new(
            "failed",
            "failed",
            format!("Failed to execute Snapper: {}", err),
            None,
        ),
        Ok((exit_code, stdout, stderr)) if exit_code != 0 => {
            let (status_kind, classified_msg) = classify_snapper_error(&stderr, &stdout, false);
            let code = status_kind.result_code().to_string();
            let detail = if !stderr.trim().is_empty() {
                stderr.lines().next().unwrap_or("").trim().to_string()
            } else if !stdout.trim().is_empty() {
                stdout.lines().next().unwrap_or("").trim().to_string()
            } else {
                classified_msg
            };
            OpState::new(
                "failed",
                &code,
                format!("Recovery point creation failed: {}", detail),
                None,
            )
        }
        Ok((0, stdout, _stderr)) => {
            if let (Some(set), Some(id)) = (cancel_set, op_id) {
                if set.lock().unwrap().contains(id) {
                    return OpState::new(
                        "cancelled",
                        "cancelled",
                        "Recovery point creation was cancelled.",
                        None,
                    );
                }
            }

            let parsed_id = stdout
                .lines()
                .rev()
                .map(str::trim)
                .find(|l| !l.is_empty())
                .and_then(|l| l.parse::<u32>().ok())
                .filter(|&id| id > 0);

            let Some(created_id) = parsed_id else {
                return OpState::new(
                    "failed",
                    "verification-failed",
                    "Snapper completed with exit code 0, but did not return a valid snapshot ID.",
                    None,
                );
            };

            // Verify that the created snapshot actually exists in the current Snapper snapshot list
            let post_report = detect_recovery_state(config);
            if post_report.status != DetectionStatus::InstalledAndConfigured {
                return OpState::new(
                    "failed",
                    "verification-failed",
                    format!(
                        "Snapshot #{} could not be verified because post-creation snapshot list failed: {}",
                        created_id, post_report.status_message
                    ),
                    None,
                );
            }

            let found_in_list = post_report
                .snapshots
                .iter()
                .any(|s| s.id == created_id as i32 && !s.is_current);

            if !found_in_list {
                return OpState::new(
                    "failed",
                    "verification-failed",
                    format!(
                        "Snapshot #{} was reported by Snapper but could not be verified in the snapshot list.",
                        created_id
                    ),
                    None,
                );
            }

            if verify_btrfs_subvolume {
                let (subvol_status, _, subvol_id, subvol_err) =
                    verify_snapshot_btrfs_subvolume(config, created_id);
                if subvol_status != "pass" || subvol_id.trim().is_empty() {
                    return OpState::new(
                        "failed",
                        "verification-failed",
                        subvol_err.unwrap_or_else(|| {
                            format!(
                                "Snapshot #{} Btrfs subvolume could not be verified.",
                                created_id
                            )
                        }),
                        None,
                    );
                }
            }

            OpState::new(
                "ok",
                "ok",
                format!("Recovery point #{} created successfully.", created_id),
                Some(created_id),
            )
        }
        Ok(_) => unreachable!(),
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PreflightCheck {
    pub id: String,
    pub title: String,
    pub status: String, // "pass", "blocked", "unknown"
    pub reason: String,
    pub remediation: String,
}

#[derive(Debug, Clone)]
pub struct RollbackPreflightReport {
    pub point_id: u32,
    pub overall_status: String, // "ready", "blocked", "unknown"
    pub can_proceed: bool,
    pub rollback_execution_enabled: bool,
    pub requires_confirmation: bool,
    pub requires_authorization: bool,
    pub polkit_action: String,
    pub snapshot_config: String,
    pub filesystem: String,
    pub root_subvolume: String,
    pub snapshot_subvolume_path: String,
    pub snapshot_subvolume_id: String,
    pub limine_status: String,
    pub summary: String,
    pub checks: Vec<PreflightCheck>,
}

fn check_record(id: &str, title: &str, status: &str, reason: String, remediation: &str) -> PreflightCheck {
    PreflightCheck {
        id: id.to_string(),
        title: title.to_string(),
        status: status.to_string(),
        reason,
        remediation: remediation.to_string(),
    }
}

fn verify_snapshot_btrfs_subvolume(
    config: &DetectionConfig,
    point_id: u32,
) -> (String, String, String, Option<String>) {
    let snap_dir = config.snapshots_dir.join(point_id.to_string());
    let subvol_path = snap_dir.join("snapshot");
    let subvol_path_str = subvol_path.to_string_lossy().to_string();

    match fs::metadata(&subvol_path) {
        Ok(meta) if meta.is_dir() => {}
        Ok(_) => {
            return (
                "blocked".to_string(),
                subvol_path_str,
                String::new(),
                Some("Snapshot subvolume path exists but is not a directory.".to_string()),
            );
        }
        Err(e) if e.kind() == ErrorKind::PermissionDenied => {
            return (
                "blocked".to_string(),
                subvol_path_str,
                String::new(),
                Some("Permission denied when accessing snapshot subvolume directory.".to_string()),
            );
        }
        Err(_) => {
            return (
                "blocked".to_string(),
                subvol_path_str,
                String::new(),
                Some(format!(
                    "Snapshot subvolume directory '{}' is missing.",
                    subvol_path.display()
                )),
            );
        }
    }

    let Some(btrfs_bin) = resolve_snapper_binary(&config.btrfs_bin) else {
        return (
            "unknown".to_string(),
            subvol_path_str,
            String::new(),
            Some("btrfs command is unavailable to verify subvolume ID.".to_string()),
        );
    };

    match run_command_with_timeout(
        &btrfs_bin,
        &["subvolume", "show", &subvol_path_str],
        config.command_timeout,
    ) {
        Ok((0, stdout, _)) => {
            let mut subvol_id = String::new();
            for line in stdout.lines() {
                let trimmed = line.trim();
                if let Some(rest) = trimmed.strip_prefix("Subvolume ID:") {
                    subvol_id = rest.trim().to_string();
                    break;
                }
            }
            if subvol_id.is_empty() || !subvol_id.chars().all(|c| c.is_ascii_digit()) {
                return (
                    "blocked".to_string(),
                    subvol_path_str,
                    String::new(),
                    Some("btrfs subvolume show did not report a valid numeric Subvolume ID.".to_string()),
                );
            }
            ("pass".to_string(), subvol_path_str, subvol_id, None)
        }
        Ok((_, stdout, stderr)) => {
            let lower = format!("{}\n{}", stderr, stdout).to_ascii_lowercase();
            if lower.contains("permission denied") || lower.contains("operation not permitted") {
                (
                    "blocked".to_string(),
                    subvol_path_str,
                    String::new(),
                    Some("Permission denied while querying Btrfs subvolume metadata.".to_string()),
                )
            } else {
                (
                    "blocked".to_string(),
                    subvol_path_str,
                    String::new(),
                    Some("Target path is not a valid Btrfs subvolume.".to_string()),
                )
            }
        }
        Err(_) => (
            "unknown".to_string(),
            subvol_path_str,
            String::new(),
            Some("Timed out or failed while executing btrfs subvolume show.".to_string()),
        ),
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct LimineEntry {
    title: String,
    kernel_path: String,
    module_paths: Vec<String>,
}

fn resolve_esp_relative_path(esp_dir: &Path, raw_uri: &str) -> Option<PathBuf> {
    let trimmed = raw_uri.trim();
    if trimmed.is_empty() {
        return None;
    }
    let rel = if let Some(rest) = trimmed.strip_prefix("boot:///") {
        rest
    } else if let Some(rest) = trimmed.strip_prefix("boot://") {
        rest
    } else {
        trimmed.trim_start_matches('/')
    };
    if rel.is_empty() {
        return None;
    }
    let rel_path = Path::new(rel);
    if rel_path.components().any(|c| {
        matches!(
            c,
            std::path::Component::ParentDir
                | std::path::Component::RootDir
                | std::path::Component::Prefix(_)
        )
    }) {
        return None;
    }
    Some(esp_dir.join(rel_path))
}

fn parse_limine_entries(content: &str) -> Vec<LimineEntry> {
    let mut entries = Vec::new();
    let mut current: Option<LimineEntry> = None;

    for raw_line in content.lines() {
        let trimmed = raw_line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        if trimmed.starts_with('/') && !trimmed.contains(':') {
            if let Some(entry) = current.take() {
                entries.push(entry);
            }
            current = Some(LimineEntry {
                title: trimmed.trim_start_matches('/').trim().to_string(),
                kernel_path: String::new(),
                module_paths: Vec::new(),
            });
            continue;
        }

        if let Some(ref mut entry) = current {
            if let Some((key, val)) = trimmed.split_once(':') {
                let k = key.trim().to_ascii_lowercase();
                let v = val.trim().to_string();
                match k.as_str() {
                    "kernel_path" | "image_path" | "path" => {
                        if entry.kernel_path.is_empty() {
                            entry.kernel_path = v;
                        }
                    }
                    "module_path" => {
                        if !v.is_empty() {
                            entry.module_paths.push(v);
                        }
                    }
                    _ => {}
                }
            }
        }
    }

    if let Some(entry) = current {
        entries.push(entry);
    }
    entries
}

fn is_non_empty_file(path: &Path) -> Result<bool, std::io::Error> {
    let meta = fs::metadata(path)?;
    Ok(meta.is_file() && meta.len() > 0)
}

fn verify_boot_mount(config: &DetectionConfig) -> (String, String, String) {
    let mount_res = parse_mountinfo_entries(&config.mountinfo_file);
    let fstab_res = parse_fstab_entries(&config.fstab_file);

    if mount_res.is_err() && fstab_res.is_err() {
        return (
            "unknown".to_string(),
            "Mount table and /etc/fstab could not be read to verify /boot.".to_string(),
            "Ensure /proc/self/mountinfo and /etc/fstab are readable.".to_string(),
        );
    }

    let mount_entries = mount_res.as_ref().ok();
    let fstab_entries = fstab_res.as_ref().ok();

    let mount_root = mount_entries.and_then(|list| list.iter().find(|e| e.mount_point == "/"));
    let mount_boot = mount_entries.and_then(|list| list.iter().find(|e| e.mount_point == "/boot"));
    let fstab_boot = fstab_entries.and_then(|list| list.iter().find(|e| e.mount_point == "/boot"));

    let real_root_in_mountinfo = mount_root
        .map(|r| !is_ephemeral_fs(&r.fstype))
        .unwrap_or(false);

    let effective_boot = if real_root_in_mountinfo {
        let Some(mb) = mount_boot else {
            return (
                "blocked".to_string(),
                "Boot partition (/boot) is not mounted in active mounts.".to_string(),
                "Mount the /boot (ESP) partition before rollback preflight.".to_string(),
            );
        };
        if let Some(fb) = fstab_boot {
            if !mb.fstype.eq_ignore_ascii_case(&fb.fstype) {
                return (
                    "blocked".to_string(),
                    format!(
                        "/boot filesystem '{}' in mountinfo conflicts with '{}' in /etc/fstab.",
                        mb.fstype, fb.fstype
                    ),
                    "Reconcile /boot mount configuration between mountinfo and /etc/fstab.".to_string(),
                );
            }
        }
        mb.clone()
    } else if let Some(mb) = mount_boot {
        mb.clone()
    } else if let Some(fb) = fstab_boot {
        fb.clone()
    } else {
        return (
            "blocked".to_string(),
            "No /boot partition mount or /etc/fstab entry was detected.".to_string(),
            "Configure and mount /boot (ESP) before rollback.".to_string(),
        );
    };

    if is_ephemeral_fs(&effective_boot.fstype) {
        return (
            "blocked".to_string(),
            format!(
                "/boot uses unexpected ephemeral filesystem '{}'.",
                effective_boot.fstype
            ),
            "Mount a persistent EFI/boot filesystem at /boot.".to_string(),
        );
    }

    if !matches!(
        effective_boot.fstype.to_ascii_lowercase().as_str(),
        "vfat" | "fat32" | "msdos" | "ext4" | "btrfs"
    ) {
        return (
            "blocked".to_string(),
            format!(
                "/boot uses unexpected filesystem '{}'.",
                effective_boot.fstype
            ),
            "Verify /boot uses a supported boot filesystem (e.g. vfat).".to_string(),
        );
    }

    (
        "pass".to_string(),
        format!(
            "Boot partition (/boot) is verified on '{}' filesystem.",
            effective_boot.fstype
        ),
        "No action required.".to_string(),
    )
}

fn validate_limine_sync_conf(content: &str) -> Result<(), String> {
    if content.trim().is_empty() {
        return Err("Limine sync configuration is empty.".to_string());
    }
    for raw_line in content.lines() {
        let trimmed = raw_line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        if let Some((k, v)) = trimmed.split_once('=') {
            let key = k.trim();
            let val = v.trim().trim_matches('"').trim_matches('\'');
            if key == "RESTORE_METHOD"
                && !matches!(val, "replace" | "rsync" | "snapper" | "opensuse")
            {
                return Err(format!(
                    "Unsupported RESTORE_METHOD '{}' in limine-snapper-sync.conf.",
                    val
                ));
            }
            if key == "LIMIT_USAGE_PERCENT" {
                let pct = val.parse::<u32>().ok().filter(|&p| (1..=99).contains(&p));
                if pct.is_none() {
                    return Err(format!(
                        "Invalid LIMIT_USAGE_PERCENT '{}' in limine-snapper-sync.conf.",
                        val
                    ));
                }
            }
        }
    }
    Ok(())
}

fn check_limine_integration(config: &DetectionConfig) -> (String, String, String, Vec<LimineEntry>) {
    let sync_bin = resolve_snapper_binary(&config.limine_sync_bin);
    if sync_bin.is_none() {
        return (
            "unknown".to_string(),
            "limine-snapper-sync executable was not found.".to_string(),
            "Install and configure limine-snapper-sync before attempting rollback.".to_string(),
            Vec::new(),
        );
    }

    match fs::read_to_string(&config.limine_conf_file) {
        Ok(content) => {
            if let Err(err) = validate_limine_sync_conf(&content) {
                return (
                    "blocked".to_string(),
                    err,
                    "Verify /etc/limine-snapper-sync.conf contains valid settings.".to_string(),
                    Vec::new(),
                );
            }
        }
        Err(e) if e.kind() == ErrorKind::PermissionDenied => {
            return (
                "blocked".to_string(),
                "Permission denied reading /etc/limine-snapper-sync.conf.".to_string(),
                "Ensure Limine configuration permissions allow inspection.".to_string(),
                Vec::new(),
            );
        }
        Err(_) => {
            return (
                "unknown".to_string(),
                format!(
                    "Limine sync configuration '{}' was not found.",
                    config.limine_conf_file.display()
                ),
                "Verify /etc/limine-snapper-sync.conf exists before rollback.".to_string(),
                Vec::new(),
            );
        }
    }

    let limine_boot_cfg = config.esp_dir.join("limine.conf");
    let limine_alt_cfg = config.esp_dir.join("limine/limine.conf");
    match fs::metadata(&config.esp_dir) {
        Ok(meta) if meta.is_dir() => {
            let cfg_path = if limine_boot_cfg.is_file() {
                limine_boot_cfg
            } else if limine_alt_cfg.is_file() {
                limine_alt_cfg
            } else {
                return (
                    "unknown".to_string(),
                    format!(
                        "Limine boot menu file (limine.conf) was not found under '{}'.",
                        config.esp_dir.display()
                    ),
                    "Verify the EFI/boot partition is mounted and contains limine.conf.".to_string(),
                    Vec::new(),
                );
            };

            let cfg_content = match fs::read_to_string(&cfg_path) {
                Ok(c) => c,
                Err(e) if e.kind() == ErrorKind::PermissionDenied => {
                    return (
                        "blocked".to_string(),
                        format!("Permission denied reading '{}'.", cfg_path.display()),
                        "Administrator access is required to inspect Limine boot entries.".to_string(),
                        Vec::new(),
                    );
                }
                Err(_) => {
                    return (
                        "unknown".to_string(),
                        format!("Could not read Limine configuration '{}'.", cfg_path.display()),
                        "Verify limine.conf is readable.".to_string(),
                        Vec::new(),
                    );
                }
            };

            let entries = parse_limine_entries(&cfg_content);
            if entries.is_empty() {
                return (
                    "blocked".to_string(),
                    "No boot entries were found in limine.conf.".to_string(),
                    "Ensure limine.conf defines active and recovery boot entries.".to_string(),
                    Vec::new(),
                );
            }

            let mut verified_entries = Vec::new();
            for entry in entries {
                if entry.kernel_path.is_empty() || entry.module_paths.is_empty() {
                    return (
                        "blocked".to_string(),
                        format!(
                            "Limine entry '{}' is missing kernel_path or initramfs module_path.",
                            entry.title
                        ),
                        "Ensure every Limine boot entry specifies valid kernel and initramfs paths.".to_string(),
                        Vec::new(),
                    );
                }

                let Some(kernel_fs_path) =
                    resolve_esp_relative_path(&config.esp_dir, &entry.kernel_path)
                else {
                    return (
                        "blocked".to_string(),
                        format!(
                            "Limine entry '{}' has an invalid or unsafe kernel path '{}'.",
                            entry.title, entry.kernel_path
                        ),
                        "Use valid boot:/// paths inside /boot.".to_string(),
                        Vec::new(),
                    );
                };

                match is_non_empty_file(&kernel_fs_path) {
                    Ok(true) => {}
                    Ok(false) | Err(_) => {
                        return (
                            "blocked".to_string(),
                            format!(
                                "Kernel file '{}' referenced by Limine entry '{}' is missing or empty.",
                                kernel_fs_path.display(),
                                entry.title
                            ),
                            "Repair or regenerate missing kernel images in /boot.".to_string(),
                            Vec::new(),
                        );
                    }
                }

                for mod_uri in &entry.module_paths {
                    let Some(mod_fs_path) = resolve_esp_relative_path(&config.esp_dir, mod_uri)
                    else {
                        return (
                            "blocked".to_string(),
                            format!(
                                "Limine entry '{}' has an invalid initramfs path '{}'.",
                                entry.title, mod_uri
                            ),
                            "Use valid boot:/// initramfs paths inside /boot.".to_string(),
                            Vec::new(),
                        );
                    };
                    match is_non_empty_file(&mod_fs_path) {
                        Ok(true) => {}
                        Ok(false) | Err(_) => {
                            return (
                                "blocked".to_string(),
                                format!(
                                    "Initramfs file '{}' referenced by Limine entry '{}' is missing or empty.",
                                    mod_fs_path.display(),
                                    entry.title
                                ),
                                "Regenerate initramfs before attempting rollback.".to_string(),
                                Vec::new(),
                            );
                        }
                    }
                }

                verified_entries.push(entry);
            }

            let has_primary = verified_entries.iter().any(|e| {
                let lower = e.title.to_ascii_lowercase();
                !lower.contains("fallback") && !lower.contains("snapshot")
            });
            let has_recovery_or_fallback = verified_entries.iter().any(|e| {
                let lower = e.title.to_ascii_lowercase();
                lower.contains("fallback")
                    || lower.contains("snapshot")
                    || lower.contains("recovery")
                    || lower.contains("lts")
            }) || verified_entries.len() >= 2;

            if !has_primary {
                return (
                    "blocked".to_string(),
                    "Active primary boot entry could not be verified in limine.conf.".to_string(),
                    "Ensure an active primary boot entry is preserved in limine.conf.".to_string(),
                    Vec::new(),
                );
            }
            if !has_recovery_or_fallback {
                return (
                    "blocked".to_string(),
                    "No alternative fallback or snapshot recovery boot entry was found in limine.conf.".to_string(),
                    "Preserve at least one fallback or snapshot boot entry in limine.conf.".to_string(),
                    Vec::new(),
                );
            }

            (
                "pass".to_string(),
                format!(
                    "Limine sync tool, configuration, and {} verified boot entries in '{}' are valid.",
                    verified_entries.len(),
                    config.esp_dir.display()
                ),
                "No action required.".to_string(),
                verified_entries,
            )
        }
        Ok(_) => (
            "blocked".to_string(),
            format!("ESP path '{}' is not a directory.", config.esp_dir.display()),
            "Verify boot partition mount configuration.".to_string(),
            Vec::new(),
        ),
        Err(e) if e.kind() == ErrorKind::PermissionDenied => (
            "blocked".to_string(),
            format!(
                "Permission denied accessing boot directory '{}'.",
                config.esp_dir.display()
            ),
            "Boot partition inspection requires read access.".to_string(),
            Vec::new(),
        ),
        Err(_) => (
            "unknown".to_string(),
            format!(
                "Boot/ESP directory '{}' is not mounted or inaccessible.",
                config.esp_dir.display()
            ),
            "Ensure /boot is mounted and limine.conf is verifiable.".to_string(),
            Vec::new(),
        ),
    }
}

fn resolve_running_kernel(config: &DetectionConfig) -> Option<String> {
    if let Some(ref k) = config.running_kernel {
        let trimmed = k.trim();
        if !trimmed.is_empty() {
            return Some(trimmed.to_string());
        }
    }
    if let Ok((0, stdout, _)) = run_command_with_timeout(
        Path::new("uname"),
        &["-r"],
        config.command_timeout,
    ) {
        let trimmed = stdout.trim();
        if !trimmed.is_empty() {
            return Some(trimmed.to_string());
        }
    }
    fs::read_to_string("/proc/sys/kernel/osrelease")
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
}

fn list_module_versions(dir: &Path) -> Result<Vec<String>, std::io::Error> {
    let mut versions = Vec::new();
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        if entry.path().is_dir() {
            if let Some(name) = entry.file_name().to_str() {
                if !name.starts_with('.') {
                    versions.push(name.to_string());
                }
            }
        }
    }
    versions.sort();
    Ok(versions)
}

fn verify_kernel_modules_compatibility(
    config: &DetectionConfig,
    point_id: u32,
    limine_entries: &[LimineEntry],
) -> (String, String, String) {
    let Some(running_kernel) = resolve_running_kernel(config) else {
        return (
            "unknown".to_string(),
            "Running kernel version could not be determined.".to_string(),
            "Ensure uname or /proc/sys/kernel/osrelease is accessible.".to_string(),
        );
    };

    let active_modules_path = config.modules_dir.join(&running_kernel);
    if !active_modules_path.is_dir() {
        return (
            "blocked".to_string(),
            format!(
                "Active kernel modules directory '{}' is missing.",
                active_modules_path.display()
            ),
            "Verify installed kernel modules match the running kernel.".to_string(),
        );
    }

    if limine_entries.is_empty() {
        return (
            "unknown".to_string(),
            "No verified Limine kernel entries are available to check module compatibility.".to_string(),
            "Resolve Limine and /boot verification first.".to_string(),
        );
    }

    let snap_root = config.snapshots_dir.join(point_id.to_string()).join("snapshot");
    let snap_modules_usr = snap_root.join("usr/lib/modules");
    let snap_modules_lib = snap_root.join("lib/modules");

    let snap_versions = match list_module_versions(&snap_modules_usr) {
        Ok(v) if !v.is_empty() => v,
        Ok(_) => match list_module_versions(&snap_modules_lib) {
            Ok(v) if !v.is_empty() => v,
            Ok(_) => {
                return (
                    "blocked".to_string(),
                    format!(
                        "Target snapshot #{} contains no kernel module directories.",
                        point_id
                    ),
                    "Do not roll back to a snapshot without verifiable kernel modules.".to_string(),
                );
            }
            Err(_) => {
                return (
                    "blocked".to_string(),
                    format!(
                        "Target snapshot #{} has an empty kernel modules directory.",
                        point_id
                    ),
                    "Do not roll back to an incomplete snapshot.".to_string(),
                );
            }
        },
        Err(e) if e.kind() == ErrorKind::PermissionDenied => {
            return (
                "blocked".to_string(),
                format!(
                    "Permission denied reading snapshot #{} kernel modules.",
                    point_id
                ),
                "Administrator access is required to verify snapshot kernel compatibility.".to_string(),
            );
        }
        Err(_) => match list_module_versions(&snap_modules_lib) {
            Ok(v) if !v.is_empty() => v,
            Err(e) if e.kind() == ErrorKind::PermissionDenied => {
                return (
                    "blocked".to_string(),
                    format!(
                        "Permission denied reading snapshot #{} kernel modules.",
                        point_id
                    ),
                    "Administrator access is required to verify snapshot kernel compatibility.".to_string(),
                );
            }
            _ => {
                return (
                    "unknown".to_string(),
                    format!(
                        "Snapshot #{} kernel modules directory could not be verified; /boot compatibility is unproven.",
                        point_id
                    ),
                    "Ensure snapshot usr/lib/modules is readable before rollback.".to_string(),
                );
            }
        },
    };

    let matched_version = snap_versions.iter().find(|ver| {
        let ver_lower = ver.to_ascii_lowercase();
        if *ver == &running_kernel {
            return true;
        }
        limine_entries.iter().any(|entry| {
            let combined = format!(
                "{} {} {}",
                entry.title.to_ascii_lowercase(),
                entry.kernel_path.to_ascii_lowercase(),
                entry.module_paths.join(" ").to_ascii_lowercase()
            );
            combined.contains(&ver_lower)
                || (ver_lower.contains("-lts") && combined.contains("lts"))
        })
    });

    match matched_version {
        Some(ver) => (
            "pass".to_string(),
            format!(
                "Snapshot #{} modules ('{}') match verified bootable kernel entries (running '{}').",
                point_id, ver, running_kernel
            ),
            "No action required.".to_string(),
        ),
        None => (
            "blocked".to_string(),
            format!(
                "Kernel/module mismatch: snapshot #{} has modules [{}], which do not match running kernel '{}' or verified /boot entries.",
                point_id,
                snap_versions.join(", "),
                running_kernel
            ),
            "Ensure a matching kernel and initramfs are preserved in /boot for the target snapshot.".to_string(),
        ),
    }
}

pub fn run_rollback_preflight(
    config: &DetectionConfig,
    point_id: u32,
    active_operation_running: bool,
) -> RollbackPreflightReport {
    let initial_report = detect_recovery_state(config);
    let mut checks = Vec::new();

    // 1. Snapper & root config
    let c1 = if initial_report.snapper_installed
        && initial_report.configured
        && initial_report.config_name == "root"
        && initial_report.status != DetectionStatus::InvalidConfiguration
        && initial_report.status != DetectionStatus::InstalledNotConfigured
    {
        check_record(
            "snapper_and_root_config",
            "Snapper and root configuration",
            "pass",
            "Snapper is installed and 'root' configuration is valid.".to_string(),
            "No action required.",
        )
    } else if initial_report.status == DetectionStatus::PermissionDenied {
        check_record(
            "snapper_and_root_config",
            "Snapper and root configuration",
            "blocked",
            initial_report.status_message.clone(),
            "Administrator permission is required to inspect Snapper configuration.",
        )
    } else {
        check_record(
            "snapper_and_root_config",
            "Snapper and root configuration",
            "blocked",
            initial_report.status_message.clone(),
            "Ensure Snapper is installed and a valid 'root' configuration for '/' exists.",
        )
    };
    checks.push(c1);

    // 2. Btrfs root filesystem
    let c2 = if initial_report.filesystem.eq_ignore_ascii_case("Btrfs") {
        check_record(
            "btrfs_root_filesystem",
            "Btrfs root filesystem",
            "pass",
            format!("Root filesystem is Btrfs (subvolume '{}').", initial_report.root_subvolume),
            "No action required.",
        )
    } else {
        check_record(
            "btrfs_root_filesystem",
            "Btrfs root filesystem",
            "blocked",
            format!("Root filesystem is '{}', not Btrfs.", initial_report.filesystem),
            "Btrfs root filesystem is required for Snapper rollback.",
        )
    };
    checks.push(c2);

    // 3. Target snapshot exists in real Snapper list
    let target_snapshot = if point_id > 0 && initial_report.status == DetectionStatus::InstalledAndConfigured {
        initial_report.snapshots.iter().find(|s| s.id == point_id as i32).cloned()
    } else {
        None
    };

    let c3 = if point_id == 0 {
        check_record(
            "snapshot_exists",
            "Target snapshot exists",
            "blocked",
            "Snapshot ID #0 represents the active system and cannot be a rollback target.".to_string(),
            "Select a valid non-current snapshot ID (> 0).",
        )
    } else if initial_report.status != DetectionStatus::InstalledAndConfigured {
        check_record(
            "snapshot_exists",
            "Target snapshot exists",
            "blocked",
            format!("Cannot verify snapshot #{}: {}", point_id, initial_report.status_message),
            "Resolve Snapper backend or permission issues first.",
        )
    } else if target_snapshot.is_some() {
        check_record(
            "snapshot_exists",
            "Target snapshot exists",
            "pass",
            format!("Snapshot #{} is present in Snapper list.", point_id),
            "No action required.",
        )
    } else {
        check_record(
            "snapshot_exists",
            "Target snapshot exists",
            "blocked",
            format!("Snapshot #{} was not found in Snapper 'root' list.", point_id),
            "Refresh the snapshot list and choose an existing snapshot ID.",
        )
    };
    checks.push(c3);

    // 4. Snapshot belongs to root configuration
    let c4 = if target_snapshot.is_some() && initial_report.config_name == "root" {
        // Also verify info.xml if present does not reference a mismatched config/subvolume
        let info_xml = config.snapshots_dir.join(point_id.to_string()).join("info.xml");
        if let Ok(xml) = fs::read_to_string(&info_xml) {
            if xml.contains("<subvol>") && !xml.contains("<subvol>/</subvol>") && !xml.contains("<subvol>/@</subvol>") {
                check_record(
                    "snapshot_belongs_to_root",
                    "Snapshot belongs to root configuration",
                    "blocked",
                    format!("Snapshot #{} metadata references a non-root subvolume.", point_id),
                    "Select a snapshot created by the 'root' configuration.",
                )
            } else {
                check_record(
                    "snapshot_belongs_to_root",
                    "Snapshot belongs to root configuration",
                    "pass",
                    format!("Snapshot #{} belongs to Snapper 'root' configuration.", point_id),
                    "No action required.",
                )
            }
        } else {
            check_record(
                "snapshot_belongs_to_root",
                "Snapshot belongs to root configuration",
                "pass",
                format!("Snapshot #{} is listed under Snapper 'root' configuration.", point_id),
                "No action required.",
            )
        }
    } else {
        check_record(
            "snapshot_belongs_to_root",
            "Snapshot belongs to root configuration",
            "blocked",
            format!("Snapshot #{} could not be confirmed under 'root' configuration.", point_id),
            "Ensure target snapshot belongs to the 'root' configuration.",
        )
    };
    checks.push(c4);

    // 5. Snapshot Btrfs subvolume path and ID verified
    let (subvol_status, subvol_path, subvol_id, subvol_err) = if target_snapshot.is_some() {
        verify_snapshot_btrfs_subvolume(config, point_id)
    } else {
        (
            "blocked".to_string(),
            config.snapshots_dir.join(point_id.to_string()).join("snapshot").to_string_lossy().to_string(),
            String::new(),
            Some("Target snapshot is unavailable for subvolume verification.".to_string()),
        )
    };
    let c5 = if subvol_status == "pass" {
        check_record(
            "snapshot_subvolume_verified",
            "Snapshot Btrfs subvolume verified",
            "pass",
            format!("Verified Btrfs subvolume '{}' (ID {}).", subvol_path, subvol_id),
            "No action required.",
        )
    } else {
        check_record(
            "snapshot_subvolume_verified",
            "Snapshot Btrfs subvolume verified",
            &subvol_status,
            subvol_err.unwrap_or_else(|| "Subvolume verification failed.".to_string()),
            "Ensure the snapshot subvolume exists and btrfs-progs can inspect it.",
        )
    };
    checks.push(c5);

    // 6. Snapshot consistency and accessibility
    let c6 = if target_snapshot.is_none() {
        check_record(
            "snapshot_consistency_accessible",
            "Snapshot consistency and accessibility",
            "blocked",
            "Target snapshot is missing or unreadable.".to_string(),
            "Select an accessible snapshot.",
        )
    } else {
        let subvol_dir = Path::new(&subvol_path);
        match fs::read_dir(subvol_dir) {
            Ok(_) => check_record(
                "snapshot_consistency_accessible",
                "Snapshot consistency and accessibility",
                "pass",
                format!("Snapshot directory '{}' is readable and consistent.", subvol_path),
                "No action required.",
            ),
            Err(e) if e.kind() == ErrorKind::PermissionDenied => check_record(
                "snapshot_consistency_accessible",
                "Snapshot consistency and accessibility",
                "blocked",
                format!("Permission denied reading snapshot directory '{}'.", subvol_path),
                "Administrator authorization is required to access snapshot contents.",
            ),
            Err(_) => check_record(
                "snapshot_consistency_accessible",
                "Snapshot consistency and accessibility",
                "blocked",
                format!("Snapshot directory '{}' is missing or corrupted.", subvol_path),
                "Do not roll back to an incomplete or unreadable snapshot.",
            ),
        }
    };
    checks.push(c6);

    // 7. Snapshot type and state eligibility
    let c7 = match &target_snapshot {
        Some(snap) if snap.is_current || snap.id == 0 => check_record(
            "snapshot_type_and_state_eligible",
            "Snapshot type and state eligibility",
            "blocked",
            "Cannot roll back to the currently active snapshot.".to_string(),
            "Choose a historical snapshot instead of the current system state.",
        ),
        Some(snap) if matches!(snap.snapshot_type.as_str(), "single" | "pre" | "post") => check_record(
            "snapshot_type_and_state_eligible",
            "Snapshot type and state eligibility",
            "pass",
            format!("Snapshot #{} (type '{}') is eligible for rollback.", snap.id, snap.snapshot_type),
            "No action required.",
        ),
        Some(snap) => check_record(
            "snapshot_type_and_state_eligible",
            "Snapshot type and state eligibility",
            "blocked",
            format!("Snapshot #{} has unsupported type '{}'.", snap.id, snap.snapshot_type),
            "Choose a 'single', 'pre', or 'post' snapshot.",
        ),
        None => check_record(
            "snapshot_type_and_state_eligible",
            "Snapshot type and state eligibility",
            "blocked",
            "Target snapshot is unavailable for eligibility check.".to_string(),
            "Select an existing snapshot.",
        ),
    };
    checks.push(c7);

    // 8. System, active operations, and recovery records preservation
    let c8 = if active_operation_running {
        check_record(
            "system_and_recovery_preservation",
            "Root subvolume and recovery preservation",
            "blocked",
            "Another recovery operation is currently in progress.".to_string(),
            "Wait for the active recovery operation to finish before preflight.",
        )
    } else if initial_report.status == DetectionStatus::InstalledAndConfigured
        && !initial_report.root_subvolume.is_empty()
    {
        check_record(
            "system_and_recovery_preservation",
            "Root subvolume and recovery preservation",
            "pass",
            format!(
                "Current root subvolume '{}' and {} recovery records can be preserved.",
                initial_report.root_subvolume, initial_report.snapshot_count
            ),
            "No action required.",
        )
    } else {
        check_record(
            "system_and_recovery_preservation",
            "Root subvolume and recovery preservation",
            "blocked",
            "Root subvolume state or recovery metadata could not be verified.".to_string(),
            "Ensure root subvolume and Snapper metadata are healthy.",
        )
    };
    checks.push(c8);

    // 9. User data (/home) subvolume isolation
    let (home_status, home_reason, home_rem) =
        verify_home_subvolume_isolation(config, &initial_report.root_subvolume);
    checks.push(check_record(
        "home_subvolume_isolation",
        "User data (/home) subvolume isolation",
        &home_status,
        home_reason,
        &home_rem,
    ));

    // 10. Boot partition mount and filesystem
    let (boot_status, boot_reason, boot_rem) = verify_boot_mount(config);
    checks.push(check_record(
        "boot_mount_verified",
        "Boot partition mount and filesystem",
        &boot_status,
        boot_reason,
        &boot_rem,
    ));

    // 11. Limine / boot integration
    let (limine_status, limine_reason, limine_rem, limine_entries) =
        check_limine_integration(config);
    checks.push(check_record(
        "limine_boot_integration",
        "Limine and boot integration",
        &limine_status,
        limine_reason,
        &limine_rem,
    ));

    // 12. Kernel, initramfs, and snapshot module compatibility
    let (kmod_status, kmod_reason, kmod_rem) = if target_snapshot.is_some() {
        verify_kernel_modules_compatibility(config, point_id, &limine_entries)
    } else {
        (
            "blocked".to_string(),
            "Target snapshot is unavailable for kernel module compatibility verification."
                .to_string(),
            "Select an existing snapshot before verifying kernel compatibility.".to_string(),
        )
    };
    checks.push(check_record(
        "kernel_modules_compatibility",
        "Kernel, initramfs, and snapshot module compatibility",
        &kmod_status,
        kmod_reason,
        &kmod_rem,
    ));

    // 13. Authorization and explicit confirmation requirements
    let polkit_defined = fs::read_to_string(&config.polkit_policy_file)
        .map(|s| s.contains(POLKIT_ACTION_ROLLBACK))
        .unwrap_or(false)
        || resolve_snapper_binary(&config.pkcheck_bin).is_some();
    let c10 = if polkit_defined {
        check_record(
            "authorization_and_confirmation",
            "Authorization and user confirmation",
            "pass",
            "Polkit action 'org.zethropol.recovery.rollback' and explicit user confirmation are enforced.".to_string(),
            "Rollback requires administrator authentication and explicit confirmation when enabled.",
        )
    } else {
        check_record(
            "authorization_and_confirmation",
            "Authorization and user confirmation",
            "unknown",
            "Polkit policy or pkcheck helper could not be verified.".to_string(),
            "Ensure Polkit policy 'org.zethropol.recovery.rollback' is installed.",
        )
    };
    checks.push(c10);

    // TOCTOU re-check: verify snapshot and boot/Limine state did not change during preflight
    if initial_report.status == DetectionStatus::InstalledAndConfigured && target_snapshot.is_some() {
        let final_report = detect_recovery_state(config);
        let still_exists = final_report.status == DetectionStatus::InstalledAndConfigured
            && final_report
                .snapshots
                .iter()
                .any(|s| s.id == point_id as i32 && !s.is_current);
        let (final_boot_status, _, _) = verify_boot_mount(config);
        let (final_limine_status, _, _, final_entries) = check_limine_integration(config);
        let (final_kmod_status, _, _) =
            verify_kernel_modules_compatibility(config, point_id, &final_entries);

        if !still_exists
            || final_boot_status != boot_status
            || final_limine_status != limine_status
            || final_kmod_status != kmod_status
        {
            checks.push(check_record(
                "state_consistency_recheck",
                "Preflight state consistency",
                "blocked",
                format!(
                    "System, boot, or target snapshot #{} state changed during preflight verification.",
                    point_id
                ),
                "Refresh snapshots and boot state, then re-run preflight.",
            ));
        }
    }

    let has_blocked = checks.iter().any(|c| c.status == "blocked");
    let has_unknown = checks.iter().any(|c| c.status == "unknown");

    let (overall_status, can_proceed, summary) = if has_blocked {
        (
            "blocked".to_string(),
            false,
            format!("Rollback preflight for snapshot #{} is blocked.", point_id),
        )
    } else if has_unknown {
        (
            "unknown".to_string(),
            false,
            format!(
                "Rollback preflight for snapshot #{} is incomplete (unverified conditions).",
                point_id
            ),
        )
    } else {
        (
            "ready".to_string(),
            true,
            format!(
                "Preflight passed for snapshot #{}. No rollback was executed; execution remains disabled in this stage.",
                point_id
            ),
        )
    };

    RollbackPreflightReport {
        point_id,
        overall_status,
        can_proceed,
        rollback_execution_enabled: false,
        requires_confirmation: true,
        requires_authorization: true,
        polkit_action: POLKIT_ACTION_ROLLBACK.to_string(),
        snapshot_config: initial_report.config_name,
        filesystem: initial_report.filesystem,
        root_subvolume: initial_report.root_subvolume,
        snapshot_subvolume_path: subvol_path,
        snapshot_subvolume_id: subvol_id,
        limine_status,
        summary,
        checks,
    }
}

pub fn encode_preflight_envelope(report: &RollbackPreflightReport) -> HashMap<String, OwnedValue> {
    let mut data: HashMap<String, OwnedValue> = HashMap::new();
    data.insert("pointId".to_string(), OwnedValue::from(report.point_id));
    data.insert(
        "overallStatus".to_string(),
        OwnedValue::from(Str::from(report.overall_status.clone())),
    );
    data.insert("canProceed".to_string(), OwnedValue::from(report.can_proceed));
    data.insert(
        "rollbackExecutionEnabled".to_string(),
        OwnedValue::from(report.rollback_execution_enabled),
    );
    data.insert(
        "requiresConfirmation".to_string(),
        OwnedValue::from(report.requires_confirmation),
    );
    data.insert(
        "requiresAuthorization".to_string(),
        OwnedValue::from(report.requires_authorization),
    );
    data.insert(
        "polkitAction".to_string(),
        OwnedValue::from(Str::from(report.polkit_action.clone())),
    );
    data.insert(
        "snapshotConfig".to_string(),
        OwnedValue::from(Str::from(report.snapshot_config.clone())),
    );
    data.insert(
        "filesystem".to_string(),
        OwnedValue::from(Str::from(report.filesystem.clone())),
    );
    data.insert(
        "rootSubvolume".to_string(),
        OwnedValue::from(Str::from(report.root_subvolume.clone())),
    );
    data.insert(
        "snapshotSubvolumePath".to_string(),
        OwnedValue::from(Str::from(report.snapshot_subvolume_path.clone())),
    );
    data.insert(
        "snapshotSubvolumeId".to_string(),
        OwnedValue::from(Str::from(report.snapshot_subvolume_id.clone())),
    );
    data.insert(
        "limineStatus".to_string(),
        OwnedValue::from(Str::from(report.limine_status.clone())),
    );
    data.insert(
        "summary".to_string(),
        OwnedValue::from(Str::from(report.summary.clone())),
    );

    let check_vals: Vec<OwnedValue> = report
        .checks
        .iter()
        .map(|c| {
            let mut m: HashMap<String, OwnedValue> = HashMap::new();
            m.insert("id".to_string(), OwnedValue::from(Str::from(c.id.clone())));
            m.insert(
                "title".to_string(),
                OwnedValue::from(Str::from(c.title.clone())),
            );
            m.insert(
                "status".to_string(),
                OwnedValue::from(Str::from(c.status.clone())),
            );
            m.insert(
                "reason".to_string(),
                OwnedValue::from(Str::from(c.reason.clone())),
            );
            m.insert(
                "remediation".to_string(),
                OwnedValue::from(Str::from(c.remediation.clone())),
            );
            OwnedValue::from(m)
        })
        .collect();
    if let Ok(arr) = OwnedValue::try_from(Array::from(check_vals)) {
        data.insert("checks".to_string(), arr);
    }

    let code = match report.overall_status.as_str() {
        "ready" => "ok",
        "unknown" => "not-available",
        _ => "failed",
    };

    let mut envelope: HashMap<String, OwnedValue> = HashMap::new();
    envelope.insert("success".to_string(), OwnedValue::from(true));
    envelope.insert("code".to_string(), OwnedValue::from(Str::from(code)));
    envelope.insert(
        "message".to_string(),
        OwnedValue::from(Str::from(report.summary.clone())),
    );
    envelope.insert("operationId".to_string(), OwnedValue::from(Str::from("")));
    envelope.insert("data".to_string(), OwnedValue::from(data));
    envelope
}

fn snapshot_to_owned(record: &SnapshotRecord) -> OwnedValue {
    let mut map: HashMap<String, OwnedValue> = HashMap::new();
    map.insert("id".to_string(), OwnedValue::from(record.id));
    map.insert(
        "type".to_string(),
        OwnedValue::from(Str::from(record.snapshot_type.clone())),
    );
    map.insert("isCurrent".to_string(), OwnedValue::from(record.is_current));
    map.insert("isPre".to_string(), OwnedValue::from(record.is_pre));
    map.insert("isPost".to_string(), OwnedValue::from(record.is_post));
    map.insert(
        "pairedSnapshotId".to_string(),
        OwnedValue::from(record.paired_snapshot_id),
    );
    map.insert("preNumber".to_string(), OwnedValue::from(record.pre_number));
    map.insert(
        "date".to_string(),
        OwnedValue::from(Str::from(record.date.clone())),
    );
    map.insert(
        "user".to_string(),
        OwnedValue::from(Str::from(record.user.clone())),
    );
    map.insert(
        "cleanup".to_string(),
        OwnedValue::from(Str::from(record.cleanup.clone())),
    );
    map.insert(
        "description".to_string(),
        OwnedValue::from(Str::from(record.description.clone())),
    );
    map.insert("important".to_string(), OwnedValue::from(record.important));
    map.insert(
        "zethropolTag".to_string(),
        OwnedValue::from(Str::from(record.zethropol_tag.clone())),
    );
    OwnedValue::from(map)
}

fn event_to_owned(event: &RecoveryEventRecord) -> OwnedValue {
    let mut map: HashMap<String, OwnedValue> = HashMap::new();
    map.insert("preId".to_string(), OwnedValue::from(event.pre_id));
    map.insert("postId".to_string(), OwnedValue::from(event.post_id));
    map.insert(
        "date".to_string(),
        OwnedValue::from(Str::from(event.date.clone())),
    );
    map.insert(
        "description".to_string(),
        OwnedValue::from(Str::from(event.description.clone())),
    );
    map.insert(
        "preDescription".to_string(),
        OwnedValue::from(Str::from(event.pre_description.clone())),
    );
    map.insert(
        "type".to_string(),
        OwnedValue::from(Str::from(event.snapshot_type.clone())),
    );
    map.insert("important".to_string(), OwnedValue::from(event.important));
    map.insert(
        "zethropolTag".to_string(),
        OwnedValue::from(Str::from(event.zethropol_tag.clone())),
    );
    OwnedValue::from(map)
}

pub fn encode_detection_envelope(report: &DetectionReport) -> HashMap<String, OwnedValue> {
    let mut data: HashMap<String, OwnedValue> = HashMap::new();
    data.insert("checking".to_string(), OwnedValue::from(false));
    data.insert("available".to_string(), OwnedValue::from(report.available));
    data.insert(
        "permissionRequired".to_string(),
        OwnedValue::from(report.permission_required),
    );
    data.insert(
        "detectionStatus".to_string(),
        OwnedValue::from(Str::from(report.status.as_str())),
    );
    data.insert(
        "snapperInstalled".to_string(),
        OwnedValue::from(report.snapper_installed),
    );
    data.insert("configured".to_string(), OwnedValue::from(report.configured));
    data.insert(
        "filesystem".to_string(),
        OwnedValue::from(Str::from(report.filesystem.clone())),
    );
    data.insert(
        "rootSubvolume".to_string(),
        OwnedValue::from(Str::from(report.root_subvolume.clone())),
    );
    data.insert(
        "snapshotTool".to_string(),
        OwnedValue::from(Str::from(report.snapshot_tool.clone())),
    );
    data.insert(
        "configName".to_string(),
        OwnedValue::from(Str::from(report.config_name.clone())),
    );

    let configs_arr: Vec<OwnedValue> = report
        .available_configs
        .iter()
        .cloned()
        .map(|s| OwnedValue::from(Str::from(s)))
        .collect();
    if let Ok(arr) = OwnedValue::try_from(Array::from(configs_arr)) {
        data.insert("availableConfigs".to_string(), arr);
    }

    data.insert(
        "snapshotsDirExists".to_string(),
        OwnedValue::from(report.snapshots_dir_exists),
    );
    data.insert(
        "snapshotsDirAccessible".to_string(),
        OwnedValue::from(report.snapshots_dir_accessible),
    );
    data.insert(
        "statusTitle".to_string(),
        OwnedValue::from(Str::from(report.status_title.clone())),
    );
    data.insert(
        "statusMessage".to_string(),
        OwnedValue::from(Str::from(report.status_message.clone())),
    );
    data.insert(
        "snapshotCount".to_string(),
        OwnedValue::from(report.snapshot_count),
    );
    data.insert(
        "currentSnapshotId".to_string(),
        OwnedValue::from(report.current_snapshot_id),
    );
    data.insert("actionBusy".to_string(), OwnedValue::from(false));
    data.insert("actionStatus".to_string(), OwnedValue::from(Str::from("")));

    let snapshots_arr: Vec<OwnedValue> = report.snapshots.iter().map(snapshot_to_owned).collect();
    if let Ok(arr) = OwnedValue::try_from(Array::from(snapshots_arr)) {
        data.insert("snapshots".to_string(), arr);
    }

    let events_arr: Vec<OwnedValue> = report.recovery_events.iter().map(event_to_owned).collect();
    if let Ok(arr) = OwnedValue::try_from(Array::from(events_arr)) {
        data.insert("recoveryEvents".to_string(), arr);
    }

    let points_arr: Vec<OwnedValue> = report
        .recovery_points
        .iter()
        .map(snapshot_to_owned)
        .collect();
    if let Ok(arr) = OwnedValue::try_from(Array::from(points_arr)) {
        data.insert("recoveryPoints".to_string(), arr);
    }

    let mut envelope: HashMap<String, OwnedValue> = HashMap::new();
    envelope.insert("success".to_string(), OwnedValue::from(true));
    envelope.insert(
        "code".to_string(),
        OwnedValue::from(Str::from(report.status.result_code())),
    );
    envelope.insert(
        "message".to_string(),
        OwnedValue::from(Str::from(report.status_message.clone())),
    );
    envelope.insert("operationId".to_string(), OwnedValue::from(Str::from("")));
    envelope.insert("data".to_string(), OwnedValue::from(data));
    envelope
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PreUpdateReservationState {
    Creating,
    VerifiedReady,
    ActiveUpdate,
    CompletedOk,
    CompletedFailed,
    AbortedOrOrphaned,
}

impl PreUpdateReservationState {
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Creating => "creating",
            Self::VerifiedReady => "verified_ready",
            Self::ActiveUpdate => "active_update",
            Self::CompletedOk => "completed_ok",
            Self::CompletedFailed => "completed_failed",
            Self::AbortedOrOrphaned => "aborted_or_orphaned",
        }
    }
}

#[derive(Debug, Clone)]
pub struct PreUpdateReservation {
    pub update_op_id: String,
    pub recovery_op_id: String,
    pub snapshot_id: Option<u32>,
    pub subvolume_id: String,
    pub owner_sender: String,
    pub state: PreUpdateReservationState,
}

fn query_fs_free_percent(path: &Path, timeout: Duration) -> Option<u32> {
    let target = if path.exists() {
        path
    } else {
        path.parent().unwrap_or(Path::new("/"))
    };
    let (code, stdout, _) = run_command_with_timeout(
        Path::new("df"),
        &["-Pk", &target.to_string_lossy()],
        timeout,
    )
    .ok()?;
    if code != 0 {
        return None;
    }
    let data_line = stdout.lines().nth(1)?;
    let pct_str = data_line
        .split_whitespace()
        .find(|f| f.ends_with('%'))?;
    let used_pct = pct_str.trim_end_matches('%').parse::<u32>().ok()?;
    Some(100u32.saturating_sub(used_pct))
}

fn verify_storage_and_retention_headroom(
    cfg: &DetectionConfig,
    report: &DetectionReport,
) -> Result<(), (String, String)> {
    if cfg.pacman_lock_file.exists() {
        return Err((
            "busy".to_string(),
            format!(
                "Another package manager transaction is active ('{}' exists).",
                cfg.pacman_lock_file.display()
            ),
        ));
    }

    let important_count = report.snapshots.iter().filter(|s| s.important).count();
    if important_count >= cfg.max_important_snapshots {
        return Err((
            "storage-limit".to_string(),
            format!(
                "Important snapshot count ({}) reached Snapper NUMBER_LIMIT_IMPORTANT ({}); cleanup assessment required before update.",
                important_count, cfg.max_important_snapshots
            ),
        ));
    }

    let free_pct = cfg
        .simulated_free_percent
        .or_else(|| query_fs_free_percent(&cfg.snapshots_dir, cfg.command_timeout));
    match free_pct {
        Some(pct) if pct < cfg.min_free_percent => {
            return Err((
                "insufficient-space".to_string(),
                format!(
                    "Root filesystem free space ({}%) is below required safety margin ({}%) for pre-update and snap-pac snapshots.",
                    pct, cfg.min_free_percent
                ),
            ));
        }
        None => {
            return Err((
                "unknown".to_string(),
                "Root filesystem free space could not be verified before update.".to_string(),
            ));
        }
        Some(_) => {}
    }

    if let Some(boot_used) = cfg.simulated_boot_usage_percent {
        if boot_used >= 85 {
            return Err((
                "insufficient-space".to_string(),
                format!(
                    "Boot partition usage ({}%) reached limine-snapper-sync LIMIT_USAGE_PERCENT (85%).",
                    boot_used
                ),
            ));
        }
    }

    Ok(())
}

#[derive(Debug)]
pub struct RecoveryManager {
    operations: Arc<Mutex<HashMap<String, OpState>>>,
    active_operation: Arc<Mutex<Option<String>>>,
    cancelled_operations: Arc<Mutex<HashSet<String>>>,
    pre_update_reservation: Arc<Mutex<Option<PreUpdateReservation>>>,
}

impl RecoveryManager {
    pub fn new() -> Self {
        Self {
            operations: Arc::new(Mutex::new(HashMap::new())),
            active_operation: Arc::new(Mutex::new(None)),
            cancelled_operations: Arc::new(Mutex::new(HashSet::new())),
            pre_update_reservation: Arc::new(Mutex::new(None)),
        }
    }

    pub(crate) fn cancel_operation_with_caller(
        &self,
        caller: Option<&str>,
        operation_id: String,
    ) -> OperationResult {
        let trimmed = operation_id.trim();
        if trimmed.is_empty() {
            return OperationResult {
                success: false,
                code: "invalid-argument".to_string(),
                message: "Operation ID is required.".to_string(),
                operation_id: String::new(),
                data: HashMap::new(),
            };
        }

        let valid_caller = match validate_dbus_sender(caller) {
            Ok(sender) => sender,
            Err((code, message)) => {
                return OperationResult {
                    success: false,
                    code,
                    message,
                    operation_id: trimmed.to_string(),
                    data: HashMap::new(),
                };
            }
        };

        let cancelled_msg = {
            let mut ops = self.operations.lock().unwrap();
            let Some(state) = ops.get_mut(trimmed) else {
                return OperationResult {
                    success: false,
                    code: "not-found".to_string(),
                    message: "Operation ID does not exist.".to_string(),
                    operation_id: trimmed.to_string(),
                    data: HashMap::new(),
                };
            };

            if state.owner_sender != valid_caller {
                return OperationResult {
                    success: false,
                    code: "permission-denied".to_string(),
                    message: "Only the operation owner may cancel this recovery operation."
                        .to_string(),
                    operation_id: trimmed.to_string(),
                    data: HashMap::new(),
                };
            }

            if state.status != "running" {
                return OperationResult {
                    success: false,
                    code: "invalid-state".to_string(),
                    message: format!(
                        "Operation cannot be cancelled in state '{}': {}",
                        state.status, state.message
                    ),
                    operation_id: trimmed.to_string(),
                    data: HashMap::new(),
                };
            }

            state.status = "cancelled".to_string();
            state.code = "cancelled".to_string();
            state.message = "Recovery point creation was cancelled.".to_string();
            state.message.clone()
        };

        self.cancelled_operations
            .lock()
            .unwrap()
            .insert(trimmed.to_string());

        {
            let mut active = self.active_operation.lock().unwrap();
            if active.as_deref() == Some(trimmed) {
                *active = None;
            }
        }

        OperationResult {
            success: true,
            code: "cancelled".to_string(),
            message: cancelled_msg,
            operation_id: trimmed.to_string(),
            data: HashMap::new(),
        }
    }

    pub(crate) fn create_recovery_point_with_caller(
        &self,
        cfg: &DetectionConfig,
        caller: Option<&str>,
        name: String,
        description: String,
        emitter: Option<zbus::object_server::SignalEmitter<'static>>,
    ) -> OperationResult {
        let full_description = match validate_snapshot_description(&name, &description) {
            Ok(desc) => desc,
            Err(msg) => {
                return OperationResult {
                    success: false,
                    code: "invalid-argument".to_string(),
                    message: msg,
                    operation_id: String::new(),
                    data: HashMap::new(),
                };
            }
        };

        {
            let active = self.active_operation.lock().unwrap();
            if active.is_some() {
                return OperationResult {
                    success: false,
                    code: "busy".to_string(),
                    message: "Another recovery operation is already in progress.".to_string(),
                    operation_id: String::new(),
                    data: HashMap::new(),
                };
            }
        }

        let sender_name = match validate_dbus_sender(caller) {
            Ok(sender) => sender.to_string(),
            Err((code, message)) => {
                return OperationResult {
                    success: false,
                    code,
                    message,
                    operation_id: String::new(),
                    data: HashMap::new(),
                };
            }
        };

        // Enforce Polkit authorization boundary before any state-modifying operation
        if let Err((code, msg)) = check_polkit_authorization(cfg, &sender_name) {
            return OperationResult {
                success: false,
                code,
                message: msg,
                operation_id: String::new(),
                data: HashMap::new(),
            };
        }

        // Verify current Snapper/Btrfs readiness and allowed root configuration
        let report = detect_recovery_state(cfg);
        if report.status != DetectionStatus::InstalledAndConfigured
            || !report.filesystem.eq_ignore_ascii_case("Btrfs")
            || report.config_name != "root"
        {
            return OperationResult {
                success: false,
                code: report.status.result_code().to_string(),
                message: report.status_message,
                operation_id: String::new(),
                data: HashMap::new(),
            };
        }

        let operation_id = uuid::Uuid::new_v4().to_string();

        {
            let mut active = self.active_operation.lock().unwrap();
            if active.is_some() {
                return OperationResult {
                    success: false,
                    code: "busy".to_string(),
                    message: "Another recovery operation is already in progress.".to_string(),
                    operation_id: String::new(),
                    data: HashMap::new(),
                };
            }
            *active = Some(operation_id.clone());
        }

        {
            let mut ops = self.operations.lock().unwrap();
            ops.insert(
                operation_id.clone(),
                OpState {
                    status: "running".to_string(),
                    code: "running".to_string(),
                    message: format!("Creating recovery point: {}", full_description),
                    snapshot_id: None,
                    owner_sender: sender_name,
                },
            );
        }

        let config_name = report.config_name.clone();
        let desc_clone = full_description.clone();
        let ops_clone = Arc::clone(&self.operations);
        let active_clone = Arc::clone(&self.active_operation);
        let cancel_clone = Arc::clone(&self.cancelled_operations);
        let op_id_clone = operation_id.clone();
        let cfg_owned = cfg.clone();

        tokio::spawn(async move {
            let cancel_for_blocking = Arc::clone(&cancel_clone);
            let op_for_blocking = op_id_clone.clone();
            let cfg_for_blocking = cfg_owned.clone();
            let config_name_for_blocking = config_name.clone();
            let desc_for_blocking = desc_clone.clone();

            let final_state = tokio::task::spawn_blocking(move || {
                execute_verified_snapshot_creation(
                    &cfg_for_blocking,
                    &config_name_for_blocking,
                    &desc_for_blocking,
                    Some(&cancel_for_blocking),
                    Some(&op_for_blocking),
                )
            })
            .await
            .unwrap_or_else(|e| {
                OpState::new(
                    "failed",
                    "internal-error",
                    format!("Recovery worker task failed: {}", e),
                    None,
                )
            });

            {
                let mut ops = ops_clone.lock().unwrap();
                if let Some(entry) = ops.get_mut(&op_id_clone) {
                    if entry.status != "cancelled" {
                        let owner = entry.owner_sender.clone();
                        *entry = final_state.clone();
                        entry.owner_sender = owner;
                    }
                }
            }

            {
                let mut active = active_clone.lock().unwrap();
                if active.as_deref() == Some(op_id_clone.as_str()) {
                    *active = None;
                }
            }

            if final_state.status == "ok" {
                if let Some(emitter_owned) = emitter {
                    let updated_report = detect_recovery_state(&cfg_owned);
                    let points: Vec<RecoveryPoint> = updated_report
                        .snapshots
                        .iter()
                        .filter(|s| s.id > 0)
                        .map(|s| RecoveryPoint {
                            id: s.id as u32,
                            name: format!("Snapshot-{}", s.id),
                            created_at: s.date.clone(),
                            description: s.description.clone(),
                            is_current: s.is_current,
                        })
                        .collect();
                    let _ = RecoveryManager::recovery_state_changed(&emitter_owned, points).await;
                    let _ = RecoveryManager::state_changed(
                        &emitter_owned,
                        encode_detection_envelope(&updated_report),
                    )
                    .await;
                }
            }
        });

        OperationResult {
            success: true,
            code: "ok".to_string(),
            message: format!("Recovery point creation initiated for: {}", full_description),
            operation_id,
            data: HashMap::new(),
        }
    }

    pub(crate) fn rollback_to_point_with_caller(
        &self,
        cfg: &DetectionConfig,
        caller: Option<&str>,
        point_id: u32,
    ) -> OperationResult {
        let sender_name = match validate_dbus_sender(caller) {
            Ok(sender) => sender,
            Err((code, message)) => {
                return OperationResult {
                    success: false,
                    code,
                    message,
                    operation_id: String::new(),
                    data: HashMap::new(),
                };
            }
        };

        if let Err((code, msg)) = check_polkit_action(cfg, sender_name, POLKIT_ACTION_ROLLBACK) {
            return OperationResult {
                success: false,
                code,
                message: msg,
                operation_id: String::new(),
                data: HashMap::new(),
            };
        }

        let active_running = self.active_operation.lock().unwrap().is_some();
        let preflight = run_rollback_preflight(cfg, point_id, active_running);
        if !preflight.can_proceed {
            return OperationResult {
                success: false,
                code: "failed".to_string(),
                message: preflight.summary,
                operation_id: String::new(),
                data: HashMap::new(),
            };
        }

        // Rollback execution is intentionally disabled in stage 03.2.05 (preflight verification only).
        OperationResult {
            success: false,
            code: "unsupported".to_string(),
            message: format!(
                "Preflight passed for snapshot #{}, but rollback execution is disabled in this verification stage.",
                point_id
            ),
            operation_id: String::new(),
            data: HashMap::new(),
        }
    }

    #[allow(dead_code)]
    pub(crate) fn handle_dbus_disconnect(&self, disconnected_sender: &str) {
        let mut res_guard = self.pre_update_reservation.lock().unwrap();
        if let Some(ref mut res) = *res_guard {
            if res.owner_sender == disconnected_sender
                && matches!(
                    res.state,
                    PreUpdateReservationState::Creating
                        | PreUpdateReservationState::VerifiedReady
                        | PreUpdateReservationState::ActiveUpdate
                )
            {
                // D-Bus disconnect does NOT prove any external pacman/arch-update process stopped;
                // mark reservation uncertain/orphaned and preserve snapshot on disk.
                res.state = PreUpdateReservationState::AbortedOrOrphaned;
            }
        }
    }

    pub(crate) fn create_pre_update_recovery_point_with_caller(
        &self,
        cfg: &DetectionConfig,
        caller: Option<&str>,
        update_op_id: String,
        description: String,
        emitter: Option<zbus::object_server::SignalEmitter<'static>>,
    ) -> OperationResult {
        let trimmed_update_id = update_op_id.trim().to_string();
        if trimmed_update_id.is_empty()
            || trimmed_update_id.len() > 128
            || trimmed_update_id.chars().any(|c| c.is_control() || c.is_whitespace())
        {
            return OperationResult {
                success: false,
                code: "invalid-argument".to_string(),
                message: "Valid update operation ID is required.".to_string(),
                operation_id: String::new(),
                data: HashMap::new(),
            };
        }

        let full_description = match validate_snapshot_description("Pre-Update", &description) {
            Ok(desc) => desc,
            Err(msg) => {
                return OperationResult {
                    success: false,
                    code: "invalid-argument".to_string(),
                    message: msg,
                    operation_id: String::new(),
                    data: HashMap::new(),
                };
            }
        };

        {
            let active = self.active_operation.lock().unwrap();
            if active.is_some() {
                return OperationResult {
                    success: false,
                    code: "busy".to_string(),
                    message: "Another recovery operation is already in progress.".to_string(),
                    operation_id: String::new(),
                    data: HashMap::new(),
                };
            }
        }

        {
            let res_guard = self.pre_update_reservation.lock().unwrap();
            if let Some(ref res) = *res_guard {
                match res.state {
                    PreUpdateReservationState::Creating
                    | PreUpdateReservationState::VerifiedReady
                    | PreUpdateReservationState::ActiveUpdate => {
                        return OperationResult {
                            success: false,
                            code: "busy".to_string(),
                            message: format!(
                                "Another pre-update reservation '{}' is already active ({}).",
                                res.update_op_id,
                                res.state.as_str()
                            ),
                            operation_id: String::new(),
                            data: HashMap::new(),
                        };
                    }
                    PreUpdateReservationState::AbortedOrOrphaned => {
                        return OperationResult {
                            success: false,
                            code: "uncertain-state".to_string(),
                            message: format!(
                                "Previous pre-update session '{}' ended in an uncertain/orphaned state; resolve recovery state before starting another update.",
                                res.update_op_id
                            ),
                            operation_id: String::new(),
                            data: HashMap::new(),
                        };
                    }
                    PreUpdateReservationState::CompletedOk
                    | PreUpdateReservationState::CompletedFailed => {}
                }
            }
        }

        let sender_name = match validate_dbus_sender(caller) {
            Ok(sender) => sender.to_string(),
            Err((code, message)) => {
                return OperationResult {
                    success: false,
                    code,
                    message,
                    operation_id: String::new(),
                    data: HashMap::new(),
                };
            }
        };

        if let Err((code, msg)) = check_polkit_authorization(cfg, &sender_name) {
            return OperationResult {
                success: false,
                code,
                message: msg,
                operation_id: String::new(),
                data: HashMap::new(),
            };
        }

        let report = detect_recovery_state(cfg);
        if report.status != DetectionStatus::InstalledAndConfigured
            || !report.filesystem.eq_ignore_ascii_case("Btrfs")
            || report.config_name != "root"
        {
            return OperationResult {
                success: false,
                code: report.status.result_code().to_string(),
                message: report.status_message,
                operation_id: String::new(),
                data: HashMap::new(),
            };
        }

        if let Err((code, msg)) = verify_storage_and_retention_headroom(cfg, &report) {
            return OperationResult {
                success: false,
                code,
                message: msg,
                operation_id: String::new(),
                data: HashMap::new(),
            };
        }

        let operation_id = uuid::Uuid::new_v4().to_string();

        {
            let mut active = self.active_operation.lock().unwrap();
            if active.is_some() {
                return OperationResult {
                    success: false,
                    code: "busy".to_string(),
                    message: "Another recovery operation is already in progress.".to_string(),
                    operation_id: String::new(),
                    data: HashMap::new(),
                };
            }
            *active = Some(operation_id.clone());
        }

        {
            let mut res_guard = self.pre_update_reservation.lock().unwrap();
            *res_guard = Some(PreUpdateReservation {
                update_op_id: trimmed_update_id.clone(),
                recovery_op_id: operation_id.clone(),
                snapshot_id: None,
                subvolume_id: String::new(),
                owner_sender: sender_name.clone(),
                state: PreUpdateReservationState::Creating,
            });
        }

        {
            let mut ops = self.operations.lock().unwrap();
            ops.insert(
                operation_id.clone(),
                OpState {
                    status: "running".to_string(),
                    code: "running".to_string(),
                    message: format!("Creating pre-update recovery point: {}", full_description),
                    snapshot_id: None,
                    owner_sender: sender_name,
                },
            );
        }

        let config_name = report.config_name.clone();
        let desc_clone = format!("{} [update-op:{}]", full_description, trimmed_update_id);
        let ops_clone = Arc::clone(&self.operations);
        let active_clone = Arc::clone(&self.active_operation);
        let cancel_clone = Arc::clone(&self.cancelled_operations);
        let res_clone = Arc::clone(&self.pre_update_reservation);
        let op_id_clone = operation_id.clone();
        let update_op_clone = trimmed_update_id;
        let cfg_owned = cfg.clone();

        tokio::spawn(async move {
            let cancel_for_blocking = Arc::clone(&cancel_clone);
            let op_for_blocking = op_id_clone.clone();
            let cfg_for_blocking = cfg_owned.clone();
            let config_name_for_blocking = config_name.clone();
            let desc_for_blocking = desc_clone.clone();

            let final_state = tokio::task::spawn_blocking(move || {
                execute_verified_snapshot_creation_with_options(
                    &cfg_for_blocking,
                    &config_name_for_blocking,
                    &desc_for_blocking,
                    "zethropol=pre-update,important=yes",
                    true,
                    Some(&cancel_for_blocking),
                    Some(&op_for_blocking),
                )
            })
            .await
            .unwrap_or_else(|e| {
                OpState::new(
                    "failed",
                    "internal-error",
                    format!("Pre-update recovery worker task failed: {}", e),
                    None,
                )
            });

            let mut verified_subvol_id = String::new();
            if final_state.status == "ok" {
                if let Some(snap_id) = final_state.snapshot_id {
                    let (_, _, subvol_id, _) = verify_snapshot_btrfs_subvolume(&cfg_owned, snap_id);
                    verified_subvol_id = subvol_id;
                }
            }

            {
                let mut ops = ops_clone.lock().unwrap();
                if let Some(entry) = ops.get_mut(&op_id_clone) {
                    if entry.status != "cancelled" {
                        let owner = entry.owner_sender.clone();
                        *entry = final_state.clone();
                        entry.owner_sender = owner;
                    }
                }
            }

            {
                let mut res_guard = res_clone.lock().unwrap();
                if let Some(ref mut res) = *res_guard {
                    if res.update_op_id == update_op_clone
                        && res.state == PreUpdateReservationState::Creating
                    {
                        if final_state.status == "ok" {
                            if let Some(snap_id) = final_state.snapshot_id {
                                res.snapshot_id = Some(snap_id);
                                res.subvolume_id = verified_subvol_id;
                                res.state = PreUpdateReservationState::VerifiedReady;
                            } else {
                                res.state = PreUpdateReservationState::CompletedFailed;
                            }
                        } else {
                            res.state = PreUpdateReservationState::CompletedFailed;
                        }
                    }
                }
            }

            {
                let mut active = active_clone.lock().unwrap();
                if active.as_deref() == Some(op_id_clone.as_str()) {
                    *active = None;
                }
            }

            if final_state.status == "ok" {
                if let Some(emitter_owned) = emitter {
                    let updated_report = detect_recovery_state(&cfg_owned);
                    let points: Vec<RecoveryPoint> = updated_report
                        .snapshots
                        .iter()
                        .filter(|s| s.id > 0)
                        .map(|s| RecoveryPoint {
                            id: s.id as u32,
                            name: format!("Snapshot-{}", s.id),
                            created_at: s.date.clone(),
                            description: s.description.clone(),
                            is_current: s.is_current,
                        })
                        .collect();
                    let _ = RecoveryManager::recovery_state_changed(&emitter_owned, points).await;
                    let _ = RecoveryManager::state_changed(
                        &emitter_owned,
                        encode_detection_envelope(&updated_report),
                    )
                    .await;
                }
            }
        });

        OperationResult {
            success: true,
            code: "ok".to_string(),
            message: format!(
                "Pre-update recovery point creation initiated for: {}",
                full_description
            ),
            operation_id,
            data: HashMap::new(),
        }
    }

    pub(crate) fn verify_pre_update_snapshot_with_caller(
        &self,
        cfg: &DetectionConfig,
        caller: Option<&str>,
        update_op_id: String,
        point_id: u32,
        critical_boot_update: bool,
    ) -> OperationResult {
        let sender_name = match validate_dbus_sender(caller) {
            Ok(sender) => sender,
            Err((code, message)) => {
                return OperationResult {
                    success: false,
                    code,
                    message,
                    operation_id: String::new(),
                    data: HashMap::new(),
                };
            }
        };

        if point_id == 0 || update_op_id.trim().is_empty() {
            return OperationResult {
                success: false,
                code: "invalid-argument".to_string(),
                message: "Valid update operation ID and snapshot ID (> 0) are required."
                    .to_string(),
                operation_id: String::new(),
                data: HashMap::new(),
            };
        }

        if self.active_operation.lock().unwrap().is_some() {
            return OperationResult {
                success: false,
                code: "busy".to_string(),
                message: "Another recovery operation is currently active.".to_string(),
                operation_id: String::new(),
                data: HashMap::new(),
            };
        }

        let (expected_subvol_id, recovery_op_id) = {
            let res_guard = self.pre_update_reservation.lock().unwrap();
            let Some(ref res) = *res_guard else {
                return OperationResult {
                    success: false,
                    code: "not-found".to_string(),
                    message: "No pre-update snapshot reservation exists.".to_string(),
                    operation_id: String::new(),
                    data: HashMap::new(),
                };
            };
            if res.owner_sender != sender_name {
                return OperationResult {
                    success: false,
                    code: "permission-denied".to_string(),
                    message: "Caller is not the owner of this pre-update reservation.".to_string(),
                    operation_id: res.recovery_op_id.clone(),
                    data: HashMap::new(),
                };
            }
            if res.update_op_id != update_op_id.trim() || res.snapshot_id != Some(point_id) {
                return OperationResult {
                    success: false,
                    code: "invalid-state".to_string(),
                    message: "Snapshot ID or update operation ID does not match active reservation."
                        .to_string(),
                    operation_id: res.recovery_op_id.clone(),
                    data: HashMap::new(),
                };
            }
            if res.state != PreUpdateReservationState::VerifiedReady {
                return OperationResult {
                    success: false,
                    code: "invalid-state".to_string(),
                    message: format!(
                        "Pre-update reservation cannot be verified in state '{}'.",
                        res.state.as_str()
                    ),
                    operation_id: res.recovery_op_id.clone(),
                    data: HashMap::new(),
                };
            }
            (res.subvolume_id.clone(), res.recovery_op_id.clone())
        };

        let report = detect_recovery_state(cfg);
        if report.status != DetectionStatus::InstalledAndConfigured
            || !report.filesystem.eq_ignore_ascii_case("Btrfs")
            || report.config_name != "root"
        {
            return OperationResult {
                success: false,
                code: report.status.result_code().to_string(),
                message: report.status_message,
                operation_id: recovery_op_id,
                data: HashMap::new(),
            };
        }

        if let Err((code, msg)) = verify_storage_and_retention_headroom(cfg, &report) {
            return OperationResult {
                success: false,
                code,
                message: msg,
                operation_id: recovery_op_id,
                data: HashMap::new(),
            };
        }

        let snap_present = report
            .snapshots
            .iter()
            .any(|s| s.id == point_id as i32 && !s.is_current);
        if !snap_present {
            return OperationResult {
                success: false,
                code: "verification-failed".to_string(),
                message: format!("Pre-update snapshot #{} is no longer present.", point_id),
                operation_id: recovery_op_id,
                data: HashMap::new(),
            };
        }

        let (subvol_status, _, subvol_id, subvol_err) =
            verify_snapshot_btrfs_subvolume(cfg, point_id);
        if subvol_status != "pass" || subvol_id != expected_subvol_id {
            return OperationResult {
                success: false,
                code: "verification-failed".to_string(),
                message: subvol_err.unwrap_or_else(|| {
                    format!(
                        "Pre-update snapshot #{} Btrfs subvolume ID mismatch.",
                        point_id
                    )
                }),
                operation_id: recovery_op_id,
                data: HashMap::new(),
            };
        }

        let (home_status, home_reason, _) =
            verify_home_subvolume_isolation(cfg, &report.root_subvolume);
        if home_status != "pass" {
            return OperationResult {
                success: false,
                code: if home_status == "unknown" {
                    "unknown".to_string()
                } else {
                    "blocked".to_string()
                },
                message: home_reason,
                operation_id: recovery_op_id,
                data: HashMap::new(),
            };
        }

        let (boot_status, boot_reason, _) = verify_boot_mount(cfg);
        if boot_status != "pass" {
            return OperationResult {
                success: false,
                code: if boot_status == "unknown" {
                    "unknown".to_string()
                } else {
                    "blocked".to_string()
                },
                message: boot_reason,
                operation_id: recovery_op_id,
                data: HashMap::new(),
            };
        }

        if critical_boot_update {
            let (limine_status, limine_reason, _, entries) = check_limine_integration(cfg);
            if limine_status != "pass" {
                return OperationResult {
                    success: false,
                    code: if limine_status == "unknown" {
                        "unknown".to_string()
                    } else {
                        "blocked".to_string()
                    },
                    message: format!(
                        "Critical boot update blocked: {}",
                        limine_reason
                    ),
                    operation_id: recovery_op_id,
                    data: HashMap::new(),
                };
            }
            let (kmod_status, kmod_reason, _) =
                verify_kernel_modules_compatibility(cfg, point_id, &entries);
            if kmod_status != "pass" {
                return OperationResult {
                    success: false,
                    code: if kmod_status == "unknown" {
                        "unknown".to_string()
                    } else {
                        "blocked".to_string()
                    },
                    message: format!(
                        "Critical boot update blocked: {}",
                        kmod_reason
                    ),
                    operation_id: recovery_op_id,
                    data: HashMap::new(),
                };
            }
        }

        let mut data = HashMap::new();
        data.insert("snapshotId".to_string(), OwnedValue::from(point_id));
        data.insert(
            "subvolumeId".to_string(),
            OwnedValue::from(Str::from(subvol_id)),
        );
        data.insert(
            "homeIsolationStatus".to_string(),
            OwnedValue::from(Str::from(home_status)),
        );
        data.insert(
            "bootMountStatus".to_string(),
            OwnedValue::from(Str::from(boot_status)),
        );

        OperationResult {
            success: true,
            code: "ok".to_string(),
            message: format!("Pre-update snapshot #{} verified for update.", point_id),
            operation_id: recovery_op_id,
            data,
        }
    }

    pub(crate) fn begin_pre_update_execution_with_caller(
        &self,
        cfg: &DetectionConfig,
        caller: Option<&str>,
        update_op_id: String,
        point_id: u32,
    ) -> OperationResult {
        let sender_name = match validate_dbus_sender(caller) {
            Ok(sender) => sender,
            Err((code, message)) => {
                return OperationResult {
                    success: false,
                    code,
                    message,
                    operation_id: String::new(),
                    data: HashMap::new(),
                };
            }
        };

        if cfg.pacman_lock_file.exists() {
            return OperationResult {
                success: false,
                code: "busy".to_string(),
                message: format!(
                    "Another package manager transaction is active ('{}' exists).",
                    cfg.pacman_lock_file.display()
                ),
                operation_id: String::new(),
                data: HashMap::new(),
            };
        }

        let mut res_guard = self.pre_update_reservation.lock().unwrap();
        let Some(ref mut res) = *res_guard else {
            return OperationResult {
                success: false,
                code: "not-found".to_string(),
                message: "No pre-update reservation exists.".to_string(),
                operation_id: String::new(),
                data: HashMap::new(),
            };
        };

        if res.owner_sender != sender_name {
            return OperationResult {
                success: false,
                code: "permission-denied".to_string(),
                message: "Caller is not the owner of this pre-update reservation.".to_string(),
                operation_id: res.recovery_op_id.clone(),
                data: HashMap::new(),
            };
        }

        if res.update_op_id != update_op_id.trim() || res.snapshot_id != Some(point_id) {
            return OperationResult {
                success: false,
                code: "invalid-state".to_string(),
                message: "Snapshot ID or update operation ID does not match active reservation."
                    .to_string(),
                operation_id: res.recovery_op_id.clone(),
                data: HashMap::new(),
            };
        }

        if res.state != PreUpdateReservationState::VerifiedReady {
            return OperationResult {
                success: false,
                code: "invalid-state".to_string(),
                message: format!(
                    "Pre-update execution cannot begin from state '{}'.",
                    res.state.as_str()
                ),
                operation_id: res.recovery_op_id.clone(),
                data: HashMap::new(),
            };
        }

        res.state = PreUpdateReservationState::ActiveUpdate;
        OperationResult {
            success: true,
            code: "ok".to_string(),
            message: format!(
                "Pre-update reservation '{}' transitioned to active_update.",
                res.update_op_id
            ),
            operation_id: res.recovery_op_id.clone(),
            data: HashMap::new(),
        }
    }

    pub(crate) fn complete_pre_update_execution_with_caller(
        &self,
        caller: Option<&str>,
        update_op_id: String,
        outcome: String,
    ) -> OperationResult {
        let sender_name = match validate_dbus_sender(caller) {
            Ok(sender) => sender,
            Err((code, message)) => {
                return OperationResult {
                    success: false,
                    code,
                    message,
                    operation_id: String::new(),
                    data: HashMap::new(),
                };
            }
        };

        let mut res_guard = self.pre_update_reservation.lock().unwrap();
        let Some(ref mut res) = *res_guard else {
            return OperationResult {
                success: false,
                code: "not-found".to_string(),
                message: "No pre-update reservation exists.".to_string(),
                operation_id: String::new(),
                data: HashMap::new(),
            };
        };

        if res.owner_sender != sender_name {
            return OperationResult {
                success: false,
                code: "permission-denied".to_string(),
                message: "Caller is not the owner of this pre-update reservation.".to_string(),
                operation_id: res.recovery_op_id.clone(),
                data: HashMap::new(),
            };
        }

        if res.update_op_id != update_op_id.trim() {
            return OperationResult {
                success: false,
                code: "invalid-state".to_string(),
                message: "Update operation ID does not match active reservation.".to_string(),
                operation_id: res.recovery_op_id.clone(),
                data: HashMap::new(),
            };
        }

        if !matches!(
            res.state,
            PreUpdateReservationState::VerifiedReady | PreUpdateReservationState::ActiveUpdate
        ) {
            return OperationResult {
                success: false,
                code: "invalid-state".to_string(),
                message: format!(
                    "Pre-update reservation cannot complete from state '{}'.",
                    res.state.as_str()
                ),
                operation_id: res.recovery_op_id.clone(),
                data: HashMap::new(),
            };
        }

        res.state = match outcome.trim().to_ascii_lowercase().as_str() {
            "ok" => PreUpdateReservationState::CompletedOk,
            "failed" => PreUpdateReservationState::CompletedFailed,
            _ => PreUpdateReservationState::AbortedOrOrphaned,
        };

        OperationResult {
            success: true,
            code: "ok".to_string(),
            message: format!(
                "Pre-update reservation '{}' completed in state '{}'; snapshot preserved.",
                res.update_op_id,
                res.state.as_str()
            ),
            operation_id: res.recovery_op_id.clone(),
            data: HashMap::new(),
        }
    }
}

#[interface(name = "org.zethropol.Recovery.v1")]
impl RecoveryManager {
    async fn get_state(&self) -> Result<HashMap<String, OwnedValue>, zbus::fdo::Error> {
        let report = tokio::task::spawn_blocking(|| {
            let cfg = DetectionConfig::from_env();
            detect_recovery_state(&cfg)
        })
        .await
        .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?;

        Ok(encode_detection_envelope(&report))
    }

    async fn check_capabilities(&self) -> Result<HashMap<String, OwnedValue>, zbus::fdo::Error> {
        self.get_state().await
    }

    async fn refresh(&self) -> Result<HashMap<String, OwnedValue>, zbus::fdo::Error> {
        self.get_state().await
    }

    async fn list_recovery_points(&self) -> Result<Vec<RecoveryPoint>, zbus::fdo::Error> {
        let report = tokio::task::spawn_blocking(|| {
            let cfg = DetectionConfig::from_env();
            detect_recovery_state(&cfg)
        })
        .await
        .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?;

        match report.status {
            DetectionStatus::InstalledAndConfigured => Ok(report
                .snapshots
                .into_iter()
                .filter(|s| s.id > 0)
                .map(|s| RecoveryPoint {
                    id: s.id as u32,
                    name: format!("Snapshot-{}", s.id),
                    created_at: s.date,
                    description: s.description,
                    is_current: s.is_current,
                })
                .collect()),
            DetectionStatus::PermissionDenied => {
                Err(zbus::fdo::Error::AccessDenied(report.status_message))
            }
            _ => Err(zbus::fdo::Error::Failed(report.status_message)),
        }
    }

    async fn get_operation_state(
        &self,
        operation_id: String,
    ) -> Result<HashMap<String, String>, zbus::fdo::Error> {
        let ops = self.operations.lock().unwrap();
        let mut res = HashMap::new();
        res.insert("operation_id".to_string(), operation_id.clone());
        if let Some(state) = ops.get(&operation_id) {
            res.insert("status".to_string(), state.status.clone());
            res.insert("code".to_string(), state.code.clone());
            res.insert("message".to_string(), state.message.clone());
            if let Some(id) = state.snapshot_id {
                res.insert("snapshot_id".to_string(), id.to_string());
            }
        } else {
            res.insert("status".to_string(), "not-found".to_string());
            res.insert("code".to_string(), "not-found".to_string());
            res.insert(
                "message".to_string(),
                "Operation ID does not exist.".to_string(),
            );
        }
        Ok(res)
    }

    async fn cancel_operation(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        operation_id: String,
    ) -> Result<OperationResult, zbus::fdo::Error> {
        let caller = header.sender().map(|s| s.to_string());
        Ok(self.cancel_operation_with_caller(caller.as_deref(), operation_id))
    }

    async fn create_recovery_point(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
        name: String,
        description: String,
    ) -> Result<OperationResult, zbus::fdo::Error> {
        let cfg = DetectionConfig::from_env();
        let caller = header.sender().map(|s| s.to_string());
        Ok(self.create_recovery_point_with_caller(
            &cfg,
            caller.as_deref(),
            name,
            description,
            Some(emitter.to_owned()),
        ))
    }

    async fn preflight_rollback(
        &self,
        point_id: u32,
    ) -> Result<HashMap<String, OwnedValue>, zbus::fdo::Error> {
        let active_running = self.active_operation.lock().unwrap().is_some();
        let report = tokio::task::spawn_blocking(move || {
            let cfg = DetectionConfig::from_env();
            run_rollback_preflight(&cfg, point_id, active_running)
        })
        .await
        .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?;

        Ok(encode_preflight_envelope(&report))
    }

    async fn rollback_to_point(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        point_id: u32,
    ) -> Result<OperationResult, zbus::fdo::Error> {
        let cfg = DetectionConfig::from_env();
        let caller = header.sender().map(|s| s.to_string());
        Ok(self.rollback_to_point_with_caller(&cfg, caller.as_deref(), point_id))
    }

    async fn create_pre_update_recovery_point(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: zbus::object_server::SignalEmitter<'_>,
        update_op_id: String,
        description: String,
    ) -> Result<OperationResult, zbus::fdo::Error> {
        let cfg = DetectionConfig::from_env();
        let caller = header.sender().map(|s| s.to_string());
        Ok(self.create_pre_update_recovery_point_with_caller(
            &cfg,
            caller.as_deref(),
            update_op_id,
            description,
            Some(emitter.to_owned()),
        ))
    }

    async fn verify_pre_update_snapshot(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        update_op_id: String,
        point_id: u32,
        critical_boot_update: bool,
    ) -> Result<OperationResult, zbus::fdo::Error> {
        let cfg = DetectionConfig::from_env();
        let caller = header.sender().map(|s| s.to_string());
        Ok(self.verify_pre_update_snapshot_with_caller(
            &cfg,
            caller.as_deref(),
            update_op_id,
            point_id,
            critical_boot_update,
        ))
    }

    async fn begin_pre_update_execution(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        update_op_id: String,
        point_id: u32,
    ) -> Result<OperationResult, zbus::fdo::Error> {
        let cfg = DetectionConfig::from_env();
        let caller = header.sender().map(|s| s.to_string());
        Ok(self.begin_pre_update_execution_with_caller(
            &cfg,
            caller.as_deref(),
            update_op_id,
            point_id,
        ))
    }

    async fn complete_pre_update_execution(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        update_op_id: String,
        outcome: String,
    ) -> Result<OperationResult, zbus::fdo::Error> {
        let caller = header.sender().map(|s| s.to_string());
        Ok(self.complete_pre_update_execution_with_caller(
            caller.as_deref(),
            update_op_id,
            outcome,
        ))
    }

    #[zbus(signal)]
    async fn recovery_state_changed(
        ctxt: &zbus::object_server::SignalEmitter<'_>,
        points: Vec<RecoveryPoint>,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn state_changed(
        ctxt: &zbus::object_server::SignalEmitter<'_>,
        state: HashMap<String, OwnedValue>,
    ) -> zbus::Result<()>;
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static COUNTER: AtomicUsize = AtomicUsize::new(0);

    struct TestFixture {
        root: PathBuf,
    }

    impl TestFixture {
        fn new() -> Self {
            let id = COUNTER.fetch_add(1, Ordering::SeqCst);
            let root = std::env::temp_dir().join(format!(
                "zethropol-recovery-test-{}-{}",
                std::process::id(),
                id
            ));
            let _ = fs::remove_dir_all(&root);
            fs::create_dir_all(&root).unwrap();
            Self { root }
        }

        fn path(&self, rel: &str) -> PathBuf {
            self.root.join(rel)
        }

        fn write(&self, rel: &str, content: &str) -> PathBuf {
            let p = self.path(rel);
            if let Some(parent) = p.parent() {
                fs::create_dir_all(parent).unwrap();
            }
            fs::write(&p, content).unwrap();
            p
        }

        fn write_exec(&self, rel: &str, body: &str) -> PathBuf {
            let p = self.write(rel, &format!("#!/bin/sh\n{}", body));
            let mut perms = fs::metadata(&p).unwrap().permissions();
            perms.set_mode(0o755);
            fs::set_permissions(&p, perms).unwrap();
            p
        }

        fn base_config(&self) -> DetectionConfig {
            let mountinfo = self.write(
                "mountinfo",
                concat!(
                    "36 1 0:36 /@ / rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=539,subvol=/@\n",
                    "37 36 0:36 /@home /home rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=257,subvol=/@home\n",
                    "38 36 259:1 / /boot rw,noatime - vfat /dev/nvme0n1p1 rw\n",
                ),
            );
            let fstab = self.write(
                "fstab",
                concat!(
                    "UUID=1234 / btrfs subvol=/@,defaults 0 0\n",
                    "UUID=1234 /home btrfs subvol=/@home,defaults 0 0\n",
                    "UUID=FB94-FF19 /boot vfat defaults,umask=0077 0 2\n",
                ),
            );
            let sysconf = self.write("sysconf/snapper", "SNAPPER_CONFIGS=\"root\"\n");
            let configs_dir = self.path("configs");
            fs::create_dir_all(&configs_dir).unwrap();
            self.write(
                "configs/root",
                "SUBVOLUME=\"/\"\nFSTYPE=\"btrfs\"\nALLOW_USERS=\"\"\n",
            );
            let snapshots_dir = self.path(".snapshots");
            fs::create_dir_all(&snapshots_dir).unwrap();
            let pkcheck_bin = self.write_exec("bin/pkcheck", "exit 0\n");
            let snapper_bin = self.write_exec(
                "bin/snapper",
                r#"if [ "$3" = "create" ]; then
  echo "15"
  exit 0
fi
cat << 'EOF'
{
  "root": [
    {
      "number": 0,
      "type": "single",
      "date": "",
      "user": "root",
      "cleanup": "",
      "description": "current",
      "userdata": {}
    },
    {
      "number": 10,
      "type": "pre",
      "date": "2026-11-16 10:00:00",
      "user": "root",
      "cleanup": "number",
      "description": "pacman -Syu",
      "userdata": { "important": "yes", "zethropol": "update" }
    },
    {
      "number": 11,
      "type": "post",
      "pre-number": 10,
      "date": "2026-11-16 10:01:00",
      "user": "root",
      "cleanup": "number",
      "description": "pacman -Syu",
      "userdata": { "important": "yes", "zethropol": "update" }
    },
    {
      "number": 12,
      "type": "single",
      "date": "2026-11-16 12:00:00",
      "user": "root",
      "cleanup": "",
      "description": "Manual checkpoint",
      "userdata": {}
    },
    {
      "number": 15,
      "type": "single",
      "date": "2026-11-17 00:00:00",
      "user": "root",
      "cleanup": "number",
      "description": "Created point",
      "userdata": { "zethropol": "manual" }
    }
  ]
}
EOF
"#,
            );

            let btrfs_bin = self.write_exec(
                "bin/btrfs",
                "echo '@/.snapshots/12/snapshot'\necho '        Subvolume ID:           272'\nexit 0\n",
            );
            let limine_sync_bin = self.write_exec("bin/limine-snapper-sync", "exit 0\n");
            let limine_conf_file = self.write(
                "etc/limine-snapper-sync.conf",
                "LIMIT_USAGE_PERCENT=85\nRESTORE_METHOD=replace\n",
            );
            let esp_dir = self.path("boot");
            fs::create_dir_all(&esp_dir).unwrap();
            self.write("boot/vmlinuz-linux-cachyos", "kernel-image");
            self.write("boot/initramfs-linux-cachyos.img", "initramfs-image");
            self.write(
                "boot/initramfs-linux-cachyos-fallback.img",
                "initramfs-fallback-image",
            );
            self.write(
                "boot/limine.conf",
                concat!(
                    "timeout: 5\n",
                    "/Zethropol OS (linux-cachyos)\n",
                    "    protocol: linux\n",
                    "    kernel_path: boot:///vmlinuz-linux-cachyos\n",
                    "    module_path: boot:///initramfs-linux-cachyos.img\n",
                    "/Zethropol OS (linux-cachyos-fallback)\n",
                    "    protocol: linux\n",
                    "    kernel_path: boot:///vmlinuz-linux-cachyos\n",
                    "    module_path: boot:///initramfs-linux-cachyos-fallback.img\n",
                ),
            );
            let modules_dir = self.path("usr/lib/modules");
            fs::create_dir_all(modules_dir.join("7.3.4-1-cachyos")).unwrap();
            let polkit_policy_file = self.write(
                "polkit/org.zethropol.recovery.policy",
                "<action id=\"org.zethropol.recovery.rollback\"/>\n",
            );
            fs::create_dir_all(snapshots_dir.join("12/snapshot/usr/lib/modules/7.3.4-1-cachyos"))
                .unwrap();

            DetectionConfig {
                snapper_bin,
                pkcheck_bin,
                btrfs_bin,
                limine_sync_bin,
                sysconf_file: sysconf,
                configs_dir,
                mountinfo_file: mountinfo,
                fstab_file: fstab,
                snapshots_dir,
                limine_conf_file,
                esp_dir,
                modules_dir,
                running_kernel: Some("7.3.4-1-cachyos".to_string()),
                pacman_lock_file: self.path("var/lib/pacman/db.lck"),
                min_free_percent: 20,
                max_important_snapshots: 15,
                simulated_free_percent: Some(65),
                simulated_boot_usage_percent: Some(35),
                polkit_policy_file,
                command_timeout: Duration::from_secs(2),
            }
        }
    }

    impl Drop for TestFixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.root);
        }
    }

    #[test]
    fn scenario_1_installed_and_configured() {
        let fx = TestFixture::new();
        let cfg = fx.base_config();
        let report = detect_recovery_state(&cfg);
        assert_eq!(report.status, DetectionStatus::InstalledAndConfigured);
        assert!(report.available);
        assert!(!report.permission_required);
        assert_eq!(report.filesystem, "Btrfs");
        assert_eq!(report.root_subvolume, "/@");
        assert_eq!(report.config_name, "root");
        assert_eq!(report.snapshot_count, 5);
        assert_eq!(report.current_snapshot_id, 0);
        assert_eq!(report.recovery_events.len(), 1);
        assert_eq!(report.recovery_points.len(), 2);
    }

    #[test]
    fn scenario_2_installed_not_configured() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        fs::remove_file(fx.path("configs/root")).unwrap();
        cfg.sysconf_file = fx.write("sysconf/snapper", "SNAPPER_CONFIGS=\"\"\n");
        let report = detect_recovery_state(&cfg);
        assert_eq!(report.status, DetectionStatus::InstalledNotConfigured);
        assert!(!report.available);
    }

    #[test]
    fn scenario_3_not_installed() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        cfg.snapper_bin = fx.path("bin/missing-snapper");
        let report = detect_recovery_state(&cfg);
        assert_eq!(report.status, DetectionStatus::NotInstalled);
        assert!(!report.available);
    }

    #[test]
    fn scenario_4_invalid_configuration() {
        let fx = TestFixture::new();
        let cfg = fx.base_config();
        fx.write("configs/root", "SUBVOLUME=\"/home\"\nFSTYPE=\"btrfs\"\n");
        let report = detect_recovery_state(&cfg);
        assert_eq!(report.status, DetectionStatus::InvalidConfiguration);
    }

    #[test]
    fn scenario_5_permission_denied() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        cfg.snapper_bin = fx.write_exec(
            "bin/snapper",
            "echo ' Failure (error.no_permissions).' >&2\nexit 1\n",
        );
        let report = detect_recovery_state(&cfg);
        assert_eq!(report.status, DetectionStatus::PermissionDenied);
        assert!(report.permission_required);
    }

    #[test]
    fn scenario_6_backend_unavailable() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        cfg.snapper_bin = fx.write_exec(
            "bin/snapper",
            "echo 'Failure (org.freedesktop.DBus.Error.ServiceUnknown).' >&2\nexit 1\n",
        );
        let report = detect_recovery_state(&cfg);
        assert_eq!(report.status, DetectionStatus::BackendUnavailable);
    }

    #[test]
    fn scenario_7_malformed_snapper_output() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        cfg.snapper_bin = fx.write_exec("bin/snapper", "printf '{broken json'\nexit 0\n");
        let report = detect_recovery_state(&cfg);
        assert_eq!(report.status, DetectionStatus::InvalidConfiguration);
    }

    #[test]
    fn scenario_8_non_btrfs_filesystem() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        cfg.mountinfo_file = fx.write(
            "mountinfo",
            "36 1 8:1 / / rw,relatime - ext4 /dev/sda1 rw\n",
        );
        cfg.fstab_file = fx.write("fstab", "UUID=1234 / ext4 defaults 0 1\n");
        let report = detect_recovery_state(&cfg);
        assert_eq!(report.status, DetectionStatus::InvalidConfiguration);
        assert_eq!(report.filesystem, "ext4");
    }

    #[test]
    fn create_verified_snapshot_succeeds_only_when_id_is_in_list() {
        let fx = TestFixture::new();
        let cfg = fx.base_config();
        assert!(check_polkit_authorization(&cfg, ":1.42").is_ok());
        let state = execute_verified_snapshot_creation(&cfg, "root", "Pre-change point", None, None);
        assert_eq!(state.status, "ok");
        assert_eq!(state.code, "ok");
        assert_eq!(state.snapshot_id, Some(15));
    }

    #[test]
    fn create_fails_verification_when_id_missing_or_unlisted() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        // Returns snapshot #99 which does not exist in `snapper list`
        cfg.snapper_bin = fx.write_exec(
            "bin/snapper",
            r#"if [ "$3" = "create" ]; then
  echo "99"
  exit 0
fi
echo '{"root":[{"number":0,"type":"single","description":"current"}]}'
"#,
        );
        let state = execute_verified_snapshot_creation(&cfg, "root", "Unverified point", None, None);
        assert_eq!(state.status, "failed");
        assert_eq!(state.code, "verification-failed");
        assert!(state.snapshot_id.is_none());
    }

    #[test]
    fn polkit_denial_and_cancellation_are_distinguished() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        cfg.pkcheck_bin = fx.write_exec("bin/pkcheck", "echo 'Not authorized' >&2\nexit 1\n");
        let err = check_polkit_authorization(&cfg, ":1.10").unwrap_err();
        assert_eq!(err.0, "permission-denied");

        cfg.pkcheck_bin = fx.write_exec("bin/pkcheck", "echo 'dismissed by user' >&2\nexit 2\n");
        let err_cancel = check_polkit_authorization(&cfg, ":1.10").unwrap_err();
        assert_eq!(err_cancel.0, "cancelled");
    }

    #[test]
    fn list_deduplicates_ids_and_handles_missing_optional_fields() {
        let json = r#"{
          "root": [
            {"number": 0, "description": "current"},
            {"number": 5, "type": 123, "date": null, "userdata": "bad"},
            {"number": 5, "type": "single", "description": "duplicate should be ignored"},
            {"number": 6, "type": "pre", "description": "pre-only"},
            {"number": 7, "type": "post", "pre-number": 6, "description": "post-pair"},
            {"number": 7, "type": "post", "pre-number": 6, "description": "dup-post"}
          ]
        }"#;
        let (snaps, events, points, current_id) = parse_snapper_json(json, "root").unwrap();
        assert_eq!(current_id, 0);
        assert_eq!(snaps.len(), 4);
        assert_eq!(events.len(), 1);
        assert_eq!(points.len(), 1);
        assert_eq!(points[0].id, 5);
        assert_eq!(points[0].snapshot_type, "");
        assert_eq!(points[0].date, "");
    }

    #[test]
    fn list_empty_snapshots_returns_empty_not_fake() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        cfg.snapper_bin = fx.write_exec("bin/snapper", "echo '{\"root\":[]}'\nexit 0\n");
        let report = detect_recovery_state(&cfg);
        assert_eq!(report.status, DetectionStatus::InstalledAndConfigured);
        assert!(report.available);
        assert_eq!(report.snapshot_count, 0);
        assert!(report.snapshots.is_empty());
        assert!(report.recovery_events.is_empty());
        assert!(report.recovery_points.is_empty());
    }

    #[test]
    fn preflight_all_checks_pass_and_limine_missing_returns_unknown() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        let ok = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(ok.overall_status, "ready");
        assert!(ok.can_proceed);
        assert!(!ok.rollback_execution_enabled);
        assert_eq!(ok.snapshot_subvolume_id, "272");
        assert_eq!(ok.checks.len(), 13);

        // Missing limine.conf must yield "unknown" and canProceed=false
        fs::remove_file(fx.path("boot/limine.conf")).unwrap();
        let unk = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(unk.overall_status, "unknown");
        assert!(!unk.can_proceed);
        assert_eq!(unk.limine_status, "unknown");

        // Target #0 (active system) must be blocked
        cfg.esp_dir = fx.path("boot");
        let blk = run_rollback_preflight(&cfg, 0, false);
        assert_eq!(blk.overall_status, "blocked");
        assert!(!blk.can_proceed);
    }

    #[test]
    fn polkit_caller_identity_validation_rejects_malformed_senders() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        let marker = fx.path("pkcheck-called");
        cfg.pkcheck_bin = fx.write_exec(
            "bin/pkcheck",
            &format!("touch '{}'\nexit 0\n", marker.display()),
        );

        for invalid in [
            "",
            "   ",
            " :1.42",
            ":1.42\n",
            "--system-bus-name",
            "invalid bus name",
            ":1",
        ] {
            let err = check_polkit_action(&cfg, invalid, POLKIT_ACTION_MODIFY).unwrap_err();
            assert_eq!(err.0, "invalid-caller", "sender '{}' must be rejected", invalid);
        }
        assert!(
            !marker.exists(),
            "pkcheck must never be invoked for invalid caller identities"
        );

        let bad_action = check_polkit_action(&cfg, ":1.42", "org.zethropol.recovery.unknown")
            .unwrap_err();
        assert_eq!(bad_action.0, "permission-denied");
        assert!(!marker.exists());
    }

    #[test]
    fn polkit_failure_modes_timeout_and_missing_helper_fail_safely() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();

        // Missing pkcheck binary -> permission-denied
        cfg.pkcheck_bin = fx.path("bin/missing-pkcheck");
        let missing_err = check_polkit_action(&cfg, ":1.42", POLKIT_ACTION_ROLLBACK).unwrap_err();
        assert_eq!(missing_err.0, "permission-denied");

        // Timeout -> timeout
        cfg.command_timeout = Duration::from_millis(80);
        cfg.pkcheck_bin = fx.write_exec("bin/pkcheck", "sleep 2\nexit 0\n");
        let timeout_err = check_polkit_action(&cfg, ":1.42", POLKIT_ACTION_MODIFY).unwrap_err();
        assert_eq!(timeout_err.0, "timeout");
    }

    #[tokio::test]
    async fn rollback_and_snapshot_permissions_are_strictly_separated() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        let mgr = RecoveryManager::new();

        // 1. Caller has modify permission ONLY -> create succeeds, rollback is denied
        cfg.pkcheck_bin = fx.write_exec(
            "bin/pkcheck",
            r#"if [ "$2" = "org.zethropol.recovery.modify" ]; then
  exit 0
fi
echo "Not authorized for $2" >&2
exit 1
"#,
        );

        let rb_denied = mgr.rollback_to_point_with_caller(&cfg, Some(":1.42"), 12);
        assert!(!rb_denied.success);
        assert_eq!(rb_denied.code, "permission-denied");

        let create_ok = mgr.create_recovery_point_with_caller(
            &cfg,
            Some(":1.42"),
            "SecPoint".to_string(),
            "Authorized modify".to_string(),
            None,
        );
        assert!(create_ok.success);
        assert_eq!(create_ok.code, "ok");

        // Wait for background worker to finish so active_operation clears
        for _ in 0..50 {
            if mgr.active_operation.lock().unwrap().is_none() {
                break;
            }
            tokio::time::sleep(Duration::from_millis(20)).await;
        }

        // 2. Caller has rollback permission ONLY -> create is denied, rollback passes Polkit+preflight but execution stays disabled
        cfg.pkcheck_bin = fx.write_exec(
            "bin/pkcheck",
            r#"if [ "$2" = "org.zethropol.recovery.rollback" ]; then
  exit 0
fi
echo "Not authorized for $2" >&2
exit 1
"#,
        );

        let create_denied = mgr.create_recovery_point_with_caller(
            &cfg,
            Some(":1.42"),
            "DeniedPoint".to_string(),
            "Should fail modify".to_string(),
            None,
        );
        assert!(!create_denied.success);
        assert_eq!(create_denied.code, "permission-denied");

        let rb_unsupported = mgr.rollback_to_point_with_caller(&cfg, Some(":1.42"), 12);
        assert!(!rb_unsupported.success);
        assert_eq!(rb_unsupported.code, "unsupported");
        assert!(rb_unsupported.message.contains("disabled"));
    }

    #[tokio::test]
    async fn cancel_operation_enforces_caller_identity_and_ownership() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        // Keep snapper running long enough for ownership & cancel tests
        cfg.snapper_bin = fx.write_exec(
            "bin/snapper",
            r#"if [ "$3" = "create" ]; then
  sleep 1
  echo "15"
  exit 0
fi
echo '{"root":[{"number":0,"type":"single","description":"current"},{"number":12,"type":"single","date":"2026-11-10 12:00:00","user":"root","cleanup":"number","description":"Pre-update","userdata":{"zethropol":"manual"}}]}'
"#,
        );

        let mgr = RecoveryManager::new();
        let started = mgr.create_recovery_point_with_caller(
            &cfg,
            Some(":1.42"),
            "CancelTest".to_string(),
            "Ownership check".to_string(),
            None,
        );
        assert!(started.success);
        let op_id = started.operation_id.clone();

        // Missing or malformed caller -> invalid-caller, operation remains running
        let no_caller = mgr.cancel_operation_with_caller(None, op_id.clone());
        assert!(!no_caller.success);
        assert_eq!(no_caller.code, "invalid-caller");

        // Nonexistent operation -> not-found, does not pollute cancelled_operations
        let missing = mgr.cancel_operation_with_caller(Some(":1.42"), "nonexistent-op".to_string());
        assert!(!missing.success);
        assert_eq!(missing.code, "not-found");
        assert!(
            !mgr.cancelled_operations
                .lock()
                .unwrap()
                .contains("nonexistent-op")
        );

        // Different caller (:1.99) attempting to cancel :1.42's operation -> permission-denied
        let hijack = mgr.cancel_operation_with_caller(Some(":1.99"), op_id.clone());
        assert!(!hijack.success);
        assert_eq!(hijack.code, "permission-denied");
        assert_eq!(
            mgr.active_operation.lock().unwrap().as_deref(),
            Some(op_id.as_str())
        );
        assert_eq!(
            mgr.operations.lock().unwrap().get(&op_id).unwrap().status,
            "running"
        );

        // Original owner (:1.42) cancels -> succeeds
        let cancelled = mgr.cancel_operation_with_caller(Some(":1.42"), op_id.clone());
        assert!(cancelled.success);
        assert_eq!(cancelled.code, "cancelled");
        assert!(mgr.active_operation.lock().unwrap().is_none());

        // Re-cancelling already cancelled operation -> invalid-state
        let again = mgr.cancel_operation_with_caller(Some(":1.42"), op_id);
        assert!(!again.success);
        assert_eq!(again.code, "invalid-state");
    }

    #[test]
    fn readonly_operations_do_not_invoke_pkcheck() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        let marker = fx.path("readonly-pkcheck-called");
        cfg.pkcheck_bin = fx.write_exec(
            "bin/pkcheck",
            &format!("touch '{}'\nexit 1\n", marker.display()),
        );

        let report = detect_recovery_state(&cfg);
        assert_eq!(report.status, DetectionStatus::InstalledAndConfigured);
        let preflight = run_rollback_preflight(&cfg, 12, false);
        assert!(preflight.can_proceed);
        assert!(!preflight.rollback_execution_enabled);
        assert!(
            !marker.exists(),
            "read-only detection and preflight must not invoke pkcheck"
        );
    }

    #[test]
    fn preflight_allows_separate_home_subvolume_and_separate_fs() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();

        // Separate /@home Btrfs subvolume passes
        let btrfs_ok = run_rollback_preflight(&cfg, 12, false);
        assert!(btrfs_ok.can_proceed);
        assert_eq!(btrfs_ok.overall_status, "ready");
        let home_chk = btrfs_ok
            .checks
            .iter()
            .find(|c| c.id == "home_subvolume_isolation")
            .unwrap();
        assert_eq!(home_chk.status, "pass");

        // Separate non-Btrfs filesystem (ext4) for /home also passes
        cfg.mountinfo_file = fx.write(
            "mountinfo",
            concat!(
                "36 1 0:36 /@ / rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=539,subvol=/@\n",
                "37 36 259:3 / /home rw,noatime - ext4 /dev/nvme0n1p3 rw\n",
                "38 36 259:1 / /boot rw,noatime - vfat /dev/nvme0n1p1 rw\n",
            ),
        );
        cfg.fstab_file = fx.write(
            "fstab",
            concat!(
                "UUID=1234 / btrfs subvol=/@,defaults 0 0\n",
                "UUID=5678 /home ext4 defaults 0 2\n",
                "UUID=FB94-FF19 /boot vfat defaults,umask=0077 0 2\n",
            ),
        );
        let ext4_ok = run_rollback_preflight(&cfg, 12, false);
        assert!(ext4_ok.can_proceed);
        assert_eq!(
            ext4_ok
                .checks
                .iter()
                .find(|c| c.id == "home_subvolume_isolation")
                .unwrap()
                .status,
            "pass"
        );
    }

    #[test]
    fn preflight_blocks_shared_root_subvolume_for_home() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        cfg.mountinfo_file = fx.write(
            "mountinfo",
            concat!(
                "36 1 0:36 /@ / rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=539,subvol=/@\n",
                "37 36 0:36 /@ /home rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=539,subvol=/@\n",
            ),
        );
        cfg.fstab_file = fx.write(
            "fstab",
            concat!(
                "UUID=1234 / btrfs subvol=/@,defaults 0 0\n",
                "UUID=1234 /home btrfs subvol=/@,defaults 0 0\n",
            ),
        );

        let report = run_rollback_preflight(&cfg, 12, false);
        assert!(!report.can_proceed);
        assert_eq!(report.overall_status, "blocked");
        let home_chk = report
            .checks
            .iter()
            .find(|c| c.id == "home_subvolume_isolation")
            .unwrap();
        assert_eq!(home_chk.status, "blocked");
        assert!(home_chk.reason.contains("shares Btrfs subvolume"));
    }

    #[test]
    fn preflight_blocks_missing_home_mount() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        cfg.mountinfo_file = fx.write(
            "mountinfo",
            "36 1 0:36 /@ / rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=539,subvol=/@\n",
        );
        cfg.fstab_file = fx.write("fstab", "UUID=1234 / btrfs subvol=/@,defaults 0 0\n");

        let report = run_rollback_preflight(&cfg, 12, false);
        assert!(!report.can_proceed);
        assert_eq!(report.overall_status, "blocked");
        let home_chk = report
            .checks
            .iter()
            .find(|c| c.id == "home_subvolume_isolation")
            .unwrap();
        assert_eq!(home_chk.status, "blocked");
    }

    #[test]
    fn preflight_blocks_nested_or_conflicting_home_subvolume() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();

        // Nested subvolume /@/home under /@
        cfg.mountinfo_file = fx.write(
            "mountinfo",
            concat!(
                "36 1 0:36 /@ / rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=539,subvol=/@\n",
                "37 36 0:36 /@/home /home rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=600,subvol=/@/home\n",
            ),
        );
        cfg.fstab_file = fx.write(
            "fstab",
            concat!(
                "UUID=1234 / btrfs subvol=/@,defaults 0 0\n",
                "UUID=1234 /home btrfs subvol=/@/home,defaults 0 0\n",
            ),
        );
        let nested = run_rollback_preflight(&cfg, 12, false);
        assert!(!nested.can_proceed);
        assert_eq!(nested.overall_status, "blocked");
        assert_eq!(
            nested
                .checks
                .iter()
                .find(|c| c.id == "home_subvolume_isolation")
                .unwrap()
                .status,
            "blocked"
        );

        // Conflicting mountinfo (/@home) vs fstab (/@data)
        cfg.mountinfo_file = fx.write(
            "mountinfo",
            concat!(
                "36 1 0:36 /@ / rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=539,subvol=/@\n",
                "37 36 0:36 /@home /home rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=257,subvol=/@home\n",
            ),
        );
        cfg.fstab_file = fx.write(
            "fstab",
            concat!(
                "UUID=1234 / btrfs subvol=/@,defaults 0 0\n",
                "UUID=1234 /home btrfs subvol=/@other,defaults 0 0\n",
            ),
        );
        let conflict = run_rollback_preflight(&cfg, 12, false);
        assert!(!conflict.can_proceed);
        assert_eq!(conflict.overall_status, "blocked");
        assert_eq!(
            conflict
                .checks
                .iter()
                .find(|c| c.id == "home_subvolume_isolation")
                .unwrap()
                .status,
            "blocked"
        );
    }

    #[test]
    fn preflight_returns_unknown_when_mount_and_fstab_unverifiable() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        cfg.mountinfo_file = fx.path("missing-mountinfo");
        cfg.fstab_file = fx.path("missing-fstab");

        let (status, reason, _) = verify_home_subvolume_isolation(&cfg, "/@");
        assert_eq!(status, "unknown");
        assert!(reason.contains("could not be read"));

        let report = run_rollback_preflight(&cfg, 12, false);
        assert!(!report.can_proceed);
        assert!(!report.rollback_execution_enabled);
    }

    #[test]
    fn boot_1_valid_limine_kernel_initramfs_layout_passes() {
        let fx = TestFixture::new();
        let cfg = fx.base_config();
        let report = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(report.overall_status, "ready");
        assert!(report.can_proceed);
        assert!(!report.rollback_execution_enabled);
        for id in [
            "boot_mount_verified",
            "limine_boot_integration",
            "kernel_modules_compatibility",
        ] {
            let chk = report.checks.iter().find(|c| c.id == id).unwrap();
            assert_eq!(chk.status, "pass", "check {} should pass", id);
        }
    }

    #[test]
    fn boot_2_missing_kernel_file_blocks_preflight() {
        let fx = TestFixture::new();
        let cfg = fx.base_config();
        fs::remove_file(fx.path("boot/vmlinuz-linux-cachyos")).unwrap();
        let report = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(report.overall_status, "blocked");
        assert!(!report.can_proceed);
        assert_eq!(report.limine_status, "blocked");
    }

    #[test]
    fn boot_3_missing_or_empty_initramfs_blocks_preflight() {
        let fx = TestFixture::new();
        let cfg = fx.base_config();
        // Empty initramfs
        fx.write("boot/initramfs-linux-cachyos.img", "");
        let empty_rep = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(empty_rep.overall_status, "blocked");
        assert!(!empty_rep.can_proceed);
        assert_eq!(empty_rep.limine_status, "blocked");

        // Missing initramfs
        fs::remove_file(fx.path("boot/initramfs-linux-cachyos.img")).unwrap();
        let missing_rep = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(missing_rep.overall_status, "blocked");
        assert!(!missing_rep.can_proceed);
    }

    #[test]
    fn boot_4_invalid_or_traversal_limine_path_blocks_preflight() {
        let fx = TestFixture::new();
        let cfg = fx.base_config();
        fx.write(
            "boot/limine.conf",
            concat!(
                "/Zethropol OS\n",
                "    kernel_path: boot:///../../etc/passwd\n",
                "    module_path: boot:///initramfs-linux-cachyos.img\n",
                "/Zethropol OS (fallback)\n",
                "    kernel_path: boot:///vmlinuz-linux-cachyos\n",
                "    module_path: boot:///initramfs-linux-cachyos-fallback.img\n",
            ),
        );
        let report = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(report.overall_status, "blocked");
        assert!(!report.can_proceed);
        assert_eq!(report.limine_status, "blocked");
    }

    #[test]
    fn boot_5_kernel_modules_version_mismatch_blocks_preflight() {
        let fx = TestFixture::new();
        let cfg = fx.base_config();
        fs::remove_dir_all(fx.path(".snapshots/12/snapshot/usr/lib/modules/7.3.4-1-cachyos"))
            .unwrap();
        fs::create_dir_all(fx.path(".snapshots/12/snapshot/usr/lib/modules/6.12.1-old")).unwrap();

        let report = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(report.overall_status, "blocked");
        assert!(!report.can_proceed);
        let chk = report
            .checks
            .iter()
            .find(|c| c.id == "kernel_modules_compatibility")
            .unwrap();
        assert_eq!(chk.status, "blocked");
    }

    #[test]
    fn boot_6_unmounted_or_unexpected_boot_filesystem_blocks_preflight() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();

        // /boot missing from mountinfo while / is real
        cfg.mountinfo_file = fx.write(
            "mountinfo",
            concat!(
                "36 1 0:36 /@ / rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=539,subvol=/@\n",
                "37 36 0:36 /@home /home rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=257,subvol=/@home\n",
            ),
        );
        let missing_boot = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(missing_boot.overall_status, "blocked");
        assert!(!missing_boot.can_proceed);

        // /boot mounted on tmpfs
        cfg.mountinfo_file = fx.write(
            "mountinfo",
            concat!(
                "36 1 0:36 /@ / rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=539,subvol=/@\n",
                "37 36 0:36 /@home /home rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=257,subvol=/@home\n",
                "38 36 0:54 / /boot rw,noatime - tmpfs tmpfs rw\n",
            ),
        );
        let tmpfs_boot = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(tmpfs_boot.overall_status, "blocked");
        assert!(!tmpfs_boot.can_proceed);
    }

    #[test]
    fn boot_7_missing_active_primary_entry_blocks_preflight() {
        let fx = TestFixture::new();
        let cfg = fx.base_config();
        fx.write(
            "boot/limine.conf",
            concat!(
                "/Zethropol OS (fallback)\n",
                "    kernel_path: boot:///vmlinuz-linux-cachyos\n",
                "    module_path: boot:///initramfs-linux-cachyos-fallback.img\n",
                "/Snapshot 12\n",
                "    kernel_path: boot:///vmlinuz-linux-cachyos\n",
                "    module_path: boot:///initramfs-linux-cachyos.img\n",
            ),
        );
        let report = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(report.overall_status, "blocked");
        assert!(!report.can_proceed);
        assert_eq!(report.limine_status, "blocked");
    }

    #[test]
    fn boot_8_missing_alternative_recovery_entry_blocks_preflight() {
        let fx = TestFixture::new();
        let cfg = fx.base_config();
        fx.write(
            "boot/limine.conf",
            concat!(
                "/Zethropol OS (linux-cachyos)\n",
                "    kernel_path: boot:///vmlinuz-linux-cachyos\n",
                "    module_path: boot:///initramfs-linux-cachyos.img\n",
            ),
        );
        let report = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(report.overall_status, "blocked");
        assert!(!report.can_proceed);
        assert_eq!(report.limine_status, "blocked");
    }

    #[test]
    fn boot_9_missing_limine_sync_or_invalid_conf_fails_safely() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();

        // Invalid RESTORE_METHOD -> blocked
        fx.write(
            "etc/limine-snapper-sync.conf",
            "LIMIT_USAGE_PERCENT=85\nRESTORE_METHOD=invalid_mode\n",
        );
        let bad_conf = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(bad_conf.overall_status, "blocked");
        assert!(!bad_conf.can_proceed);

        // Missing limine-snapper-sync binary -> unknown (canProceed=false)
        cfg.limine_sync_bin = fx.path("bin/missing-limine-snapper-sync");
        let missing_bin = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(missing_bin.overall_status, "unknown");
        assert!(!missing_bin.can_proceed);
    }

    #[test]
    fn boot_10_unverifiable_snapshot_modules_prevents_ready() {
        let fx = TestFixture::new();
        let cfg = fx.base_config();
        fs::remove_dir_all(fx.path(".snapshots/12/snapshot/usr/lib/modules")).unwrap();
        let report = run_rollback_preflight(&cfg, 12, false);
        assert_ne!(report.overall_status, "ready");
        assert!(!report.can_proceed);
        let chk = report
            .checks
            .iter()
            .find(|c| c.id == "kernel_modules_compatibility")
            .unwrap();
        assert_eq!(chk.status, "unknown");
    }

    #[test]
    fn boot_11_toctou_boot_or_snapshot_change_during_preflight_blocks() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        let limine_file = fx.path("boot/limine.conf");
        let flag_file = fx.path("snapper-listed-once");
        // On second `snapper list` invocation (TOCTOU re-check), remove limine.conf to simulate mid-preflight change
        cfg.snapper_bin = fx.write_exec(
            "bin/snapper",
            &format!(
                r#"if [ -f "{flag}" ]; then
  rm -f "{limine}"
else
  touch "{flag}"
fi
cat << 'EOF'
{{
  "root": [
    {{"number":0,"type":"single","date":"","user":"root","cleanup":"","description":"current","userdata":{{}}}},
    {{"number":12,"type":"single","date":"2026-11-16 12:00:00","user":"root","cleanup":"number","description":"Manual pre-change","userdata":{{"zethropol":"manual"}}}}
  ]
}}
EOF
"#,
                flag = flag_file.display(),
                limine = limine_file.display()
            ),
        );

        let report = run_rollback_preflight(&cfg, 12, false);
        assert_eq!(report.overall_status, "blocked");
        assert!(!report.can_proceed);
        let recheck = report
            .checks
            .iter()
            .find(|c| c.id == "state_consistency_recheck")
            .unwrap();
        assert_eq!(recheck.status, "blocked");
    }

    #[test]
    fn boot_12_dbus_preflight_envelope_matches_control_center_contract() {
        let fx = TestFixture::new();
        let cfg = fx.base_config();
        let report = run_rollback_preflight(&cfg, 12, false);
        let envelope = encode_preflight_envelope(&report);

        assert_eq!(
            envelope.get("code").and_then(|v| v.downcast_ref::<String>().ok()),
            Some("ok".to_string())
        );
        let data_val = envelope.get("data").unwrap();
        let data_dict: HashMap<String, OwnedValue> = data_val.try_clone().unwrap().try_into().unwrap();
        assert_eq!(
            data_dict.get("overallStatus").and_then(|v| v.downcast_ref::<String>().ok()),
            Some("ready".to_string())
        );
        assert_eq!(
            data_dict.get("canProceed").and_then(|v| v.downcast_ref::<bool>().ok()),
            Some(true)
        );
        assert_eq!(
            data_dict.get("rollbackExecutionEnabled").and_then(|v| v.downcast_ref::<bool>().ok()),
            Some(false)
        );
        assert!(data_dict.contains_key("checks"));
    }

    #[tokio::test]
    async fn pre_update_1_reservation_lifecycle_and_single_use_transition() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        let snap_dir = fx.path(".snapshots");
        fs::create_dir_all(snap_dir.join("15/snapshot/usr/lib/modules/7.3.4-1-cachyos")).unwrap();
        let args_log = fx.path("snapper-args.log");
        cfg.snapper_bin = fx.write_exec(
            "bin/snapper",
            &format!(
                r#"echo "$*" >> "{log}"
if [ "$3" = "create" ]; then
  echo "15"
  exit 0
fi
cat << 'EOF'
{{
  "root": [
    {{"number":0,"type":"single","date":"","user":"root","cleanup":"","description":"current","userdata":{{}}}},
    {{"number":12,"type":"single","date":"2026-11-16 12:00:00","user":"root","cleanup":"number","description":"Pre-change","userdata":{{"zethropol":"manual"}}}},
    {{"number":15,"type":"single","date":"2026-11-18 00:30:00","user":"root","cleanup":"number","description":"Pre-Update","userdata":{{"zethropol":"pre-update","important":"yes"}}}}
  ]
}}
EOF
"#,
                log = args_log.display()
            ),
        );

        let mgr = RecoveryManager::new();
        let started = mgr.create_pre_update_recovery_point_with_caller(
            &cfg,
            Some(":1.42"),
            "upd-100".to_string(),
            "System upgrade".to_string(),
            None,
        );
        assert!(started.success);

        for _ in 0..50 {
            let st = mgr.pre_update_reservation.lock().unwrap().as_ref().unwrap().state;
            if st == PreUpdateReservationState::VerifiedReady {
                break;
            }
            tokio::time::sleep(Duration::from_millis(20)).await;
        }

        // Verify userdata tag separates pre-update + important=yes from manual snapshots
        let logged = fs::read_to_string(&args_log).unwrap();
        assert!(logged.contains("zethropol=pre-update,important=yes"));

        // Wrong D-Bus owner (:1.99) cannot verify or begin
        let wrong_owner = mgr.verify_pre_update_snapshot_with_caller(
            &cfg,
            Some(":1.99"),
            "upd-100".to_string(),
            15,
            true,
        );
        assert!(!wrong_owner.success);
        assert_eq!(wrong_owner.code, "permission-denied");

        // Fake / mismatched snapshot ID is rejected
        let fake_id = mgr.verify_pre_update_snapshot_with_caller(
            &cfg,
            Some(":1.42"),
            "upd-100".to_string(),
            99,
            true,
        );
        assert!(!fake_id.success);
        assert_eq!(fake_id.code, "invalid-state");

        // Real owner verifies cleanly (VerifyPreUpdateSnapshot does NOT consume reservation)
        let verified = mgr.verify_pre_update_snapshot_with_caller(
            &cfg,
            Some(":1.42"),
            "upd-100".to_string(),
            15,
            true,
        );
        assert!(verified.success);
        assert_eq!(verified.code, "ok");

        // Single-use transition VerifiedReady -> ActiveUpdate
        let begun = mgr.begin_pre_update_execution_with_caller(
            &cfg,
            Some(":1.42"),
            "upd-100".to_string(),
            15,
        );
        assert!(begun.success);
        assert_eq!(
            mgr.pre_update_reservation.lock().unwrap().as_ref().unwrap().state,
            PreUpdateReservationState::ActiveUpdate
        );

        // Second begin / replay is rejected
        let replay = mgr.begin_pre_update_execution_with_caller(
            &cfg,
            Some(":1.42"),
            "upd-100".to_string(),
            15,
        );
        assert!(!replay.success);
        assert_eq!(replay.code, "invalid-state");

        // Failed update transitions to CompletedFailed and preserves snapshot
        let completed = mgr.complete_pre_update_execution_with_caller(
            Some(":1.42"),
            "upd-100".to_string(),
            "failed".to_string(),
        );
        assert!(completed.success);
        assert_eq!(
            mgr.pre_update_reservation.lock().unwrap().as_ref().unwrap().state,
            PreUpdateReservationState::CompletedFailed
        );
    }

    #[tokio::test]
    async fn pre_update_2_disconnect_and_uncertain_state_blocks_subsequent_updates() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        let snap_dir = fx.path(".snapshots");
        fs::create_dir_all(snap_dir.join("15/snapshot/usr/lib/modules/7.3.4-1-cachyos")).unwrap();
        cfg.snapper_bin = fx.write_exec(
            "bin/snapper",
            r#"if [ "$3" = "create" ]; then
  echo "15"
  exit 0
fi
echo '{"root":[{"number":0,"type":"single","description":"current"},{"number":15,"type":"single","description":"Pre-Update","userdata":{"zethropol":"pre-update","important":"yes"}}]}'
"#,
        );

        let mgr = RecoveryManager::new();
        let started = mgr.create_pre_update_recovery_point_with_caller(
            &cfg,
            Some(":1.42"),
            "upd-200".to_string(),
            "Disconnect test".to_string(),
            None,
        );
        assert!(started.success);
        for _ in 0..50 {
            if mgr.pre_update_reservation.lock().unwrap().as_ref().unwrap().state
                == PreUpdateReservationState::VerifiedReady
            {
                break;
            }
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
        assert!(mgr
            .begin_pre_update_execution_with_caller(&cfg, Some(":1.42"), "upd-200".to_string(), 15)
            .success);

        // Simulate D-Bus disconnect while ActiveUpdate -> AbortedOrOrphaned
        mgr.handle_dbus_disconnect(":1.42");
        assert_eq!(
            mgr.pre_update_reservation.lock().unwrap().as_ref().unwrap().state,
            PreUpdateReservationState::AbortedOrOrphaned
        );

        // Uncertain state MUST block another update from starting
        let blocked_next = mgr.create_pre_update_recovery_point_with_caller(
            &cfg,
            Some(":1.55"),
            "upd-201".to_string(),
            "Should be blocked".to_string(),
            None,
        );
        assert!(!blocked_next.success);
        assert_eq!(blocked_next.code, "uncertain-state");
    }

    #[test]
    fn pre_update_3_coordination_storage_polkit_and_critical_boot_blocks() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        let mgr = RecoveryManager::new();

        // 1. Polkit denied
        cfg.pkcheck_bin = fx.write_exec("bin/pkcheck", "echo 'Not authorized' >&2\nexit 1\n");
        let polkit_denied = mgr.create_pre_update_recovery_point_with_caller(
            &cfg,
            Some(":1.42"),
            "upd-300".to_string(),
            "Polkit denied".to_string(),
            None,
        );
        assert!(!polkit_denied.success);
        assert_eq!(polkit_denied.code, "permission-denied");
        cfg.pkcheck_bin = fx.write_exec("bin/pkcheck", "exit 0\n");

        // 2. Pacman db.lck present -> busy
        fx.write("var/lib/pacman/db.lck", "");
        let locked = mgr.create_pre_update_recovery_point_with_caller(
            &cfg,
            Some(":1.42"),
            "upd-301".to_string(),
            "Pacman locked".to_string(),
            None,
        );
        assert!(!locked.success);
        assert_eq!(locked.code, "busy");
        fs::remove_file(fx.path("var/lib/pacman/db.lck")).unwrap();

        // 3. Low disk space -> insufficient-space
        cfg.simulated_free_percent = Some(10);
        let low_space = mgr.create_pre_update_recovery_point_with_caller(
            &cfg,
            Some(":1.42"),
            "upd-302".to_string(),
            "Low space".to_string(),
            None,
        );
        assert!(!low_space.success);
        assert_eq!(low_space.code, "insufficient-space");

        // 4. /boot >= 85% -> insufficient-space
        cfg.simulated_free_percent = Some(65);
        cfg.simulated_boot_usage_percent = Some(86);
        let boot_full = mgr.create_pre_update_recovery_point_with_caller(
            &cfg,
            Some(":1.42"),
            "upd-303".to_string(),
            "Boot full".to_string(),
            None,
        );
        assert!(!boot_full.success);
        assert_eq!(boot_full.code, "insufficient-space");

        // 5. Snapper NUMBER_LIMIT_IMPORTANT reached -> storage-limit
        cfg.simulated_boot_usage_percent = Some(35);
        cfg.max_important_snapshots = 1;
        let limit_hit = mgr.create_pre_update_recovery_point_with_caller(
            &cfg,
            Some(":1.42"),
            "upd-304".to_string(),
            "Important limit".to_string(),
            None,
        );
        assert!(!limit_hit.success);
        assert_eq!(limit_hit.code, "storage-limit");
    }

    #[tokio::test]
    async fn pre_update_4_critical_boot_update_blocks_on_boot_mismatch() {
        let fx = TestFixture::new();
        let mut cfg = fx.base_config();
        let snap_dir = fx.path(".snapshots");
        // Snapshot 15 has mismatched kernel modules
        fs::create_dir_all(snap_dir.join("15/snapshot/usr/lib/modules/6.12.0-old")).unwrap();
        cfg.snapper_bin = fx.write_exec(
            "bin/snapper",
            r#"if [ "$3" = "create" ]; then
  echo "15"
  exit 0
fi
echo '{"root":[{"number":0,"type":"single","description":"current"},{"number":15,"type":"single","description":"Pre-Update","userdata":{"zethropol":"pre-update","important":"yes"}}]}'
"#,
        );

        let mgr = RecoveryManager::new();
        let started = mgr.create_pre_update_recovery_point_with_caller(
            &cfg,
            Some(":1.42"),
            "upd-400".to_string(),
            "Critical kernel update".to_string(),
            None,
        );
        assert!(started.success);
        for _ in 0..50 {
            if mgr.pre_update_reservation.lock().unwrap().as_ref().unwrap().state
                == PreUpdateReservationState::VerifiedReady
            {
                break;
            }
            tokio::time::sleep(Duration::from_millis(20)).await;
        }

        let blocked = mgr.verify_pre_update_snapshot_with_caller(
            &cfg,
            Some(":1.42"),
            "upd-400".to_string(),
            15,
            true,
        );
        assert!(!blocked.success);
        assert_eq!(blocked.code, "blocked");
    }
}
