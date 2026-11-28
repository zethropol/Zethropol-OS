use crate::config::DaemonConfig;
use serde_json::{json, Map, Value as JsonValue};
use std::collections::{BTreeMap, HashMap, VecDeque};
use std::fs::{self, File};
use std::io::Read;
use std::net::SocketAddr;
use std::os::unix::fs::MetadataExt;
use std::path::{Component, Path, PathBuf};
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use zbus::proxy::CacheProperties;
use zbus::zvariant::{OwnedValue, Value as ZValue};

const MAX_MODEL_RESPONSE_BYTES: usize = 262_144;

/// Strictly allowlisted read-only Zethropol system D-Bus diagnostic targets (10.02).
#[derive(Debug, Clone, Copy)]
pub struct DiagnosticTarget {
    pub destination: &'static str,
    pub path: &'static str,
    pub interface: &'static str,
    pub method: &'static str,
}

const ALLOWED_DIAGNOSTIC_TARGETS: &[DiagnosticTarget] = &[
    DiagnosticTarget {
        destination: "org.zethropol.Hardware",
        path: "/org/zethropol/Hardware",
        interface: "org.zethropol.Hardware.v1",
        method: "GetTelemetry",
    },
    DiagnosticTarget {
        destination: "org.zethropol.Hardware",
        path: "/org/zethropol/Hardware",
        interface: "org.zethropol.Hardware.v1",
        method: "GetState",
    },
    DiagnosticTarget {
        destination: "org.zethropol.Monitoring",
        path: "/org/zethropol/Monitoring",
        interface: "org.zethropol.Monitoring.v1",
        method: "GetState",
    },
    DiagnosticTarget {
        destination: "org.zethropol.Diagnostics",
        path: "/org/zethropol/Diagnostics",
        interface: "org.zethropol.Diagnostics.v1",
        method: "GetState",
    },
    DiagnosticTarget {
        destination: "org.zethropol.Performance",
        path: "/org/zethropol/Performance",
        interface: "org.zethropol.Performance.v1",
        method: "GetState",
    },
    DiagnosticTarget {
        destination: "org.zethropol.Performance",
        path: "/org/zethropol/Performance",
        interface: "org.zethropol.Performance.v1",
        method: "GetProfileState",
    },
    DiagnosticTarget {
        destination: "org.zethropol.Performance",
        path: "/org/zethropol/Performance",
        interface: "org.zethropol.Performance.v1",
        method: "GetGamingReadiness",
    },
    DiagnosticTarget {
        destination: "org.zethropol.Power",
        path: "/org/zethropol/Power",
        interface: "org.zethropol.Power.v1",
        method: "GetState",
    },
    DiagnosticTarget {
        destination: "org.zethropol.Services",
        path: "/org/zethropol/Services",
        interface: "org.zethropol.Services.v1",
        method: "ListServices",
    },
    DiagnosticTarget {
        destination: "org.zethropol.Services",
        path: "/org/zethropol/Services",
        interface: "org.zethropol.Services.v1",
        method: "GetState",
    },
    DiagnosticTarget {
        destination: "org.zethropol.Security",
        path: "/org/zethropol/Security",
        interface: "org.zethropol.Security.v1",
        method: "GetState",
    },
    DiagnosticTarget {
        destination: "org.zethropol.Security",
        path: "/org/zethropol/Security/Policy",
        interface: "org.zethropol.Security.Policy.v1",
        method: "GetPolicySummary",
    },
];

pub fn lookup_readonly_diagnostic_target(
    interface: &str,
    method: &str,
) -> Option<DiagnosticTarget> {
    ALLOWED_DIAGNOSTIC_TARGETS
        .iter()
        .copied()
        .find(|t| t.interface == interface.trim() && t.method == method.trim())
}

fn has_control_or_nul(s: &str) -> bool {
    s.chars().any(|c| c == '\0' || c.is_control())
}

fn is_sensitive_or_protected_path(path: &Path) -> bool {
    let Some(s) = path.to_str() else {
        return true;
    };
    let lower = s.to_ascii_lowercase();
    const DENIED_PREFIXES: &[&str] = &[
        "/etc/shadow",
        "/etc/gshadow",
        "/etc/sudoers",
        "/etc/polkit-1",
        "/etc/dbus-1",
        "/var/lib/zethropol-security",
        "/var/log/zethropol-security",
        "/root",
        "/boot",
        "/proc/",
        "/sys/",
        "/dev/",
    ];
    if DENIED_PREFIXES
        .iter()
        .any(|p| lower == *p || lower.starts_with(p))
    {
        return true;
    }

    const DENIED_FRAGMENTS: &[&str] = &[
        "/.ssh/",
        "/.gnupg/",
        "/.pki/",
        "/.aws/",
        "/.kube/",
        "/.local/share/keyrings/",
        "/.netrc",
        "/.git-credentials",
        "zci-policy-state.json",
        "zci-audit.jsonl",
        "services/recovery/src/recovery_service.rs",
    ];
    if DENIED_FRAGMENTS.iter().any(|f| lower.contains(f))
        || lower.ends_with("/.ssh")
        || lower.ends_with("/.gnupg")
        || lower.ends_with("/.env")
        || lower.ends_with(".pem")
        || lower.ends_with(".key")
        || lower.ends_with("/agents.md")
    {
        return true;
    }
    false
}

/// Defense-in-depth path validation & DAC check (on top of `org.zethropol.Security.Policy.v1`).
pub fn validate_safe_read_path(raw: &str, caller_uid: u32) -> Result<PathBuf, String> {
    if raw.is_empty() || raw.len() > 2048 || has_control_or_nul(raw) {
        return Err("Invalid target path syntax.".to_string());
    }
    let lower = raw.to_ascii_lowercase();
    if raw.contains('\\')
        || lower.contains("%2e")
        || lower.contains("%2f")
        || lower.contains("%5c")
        || lower.contains("%00")
    {
        return Err("Encoded or backslash path traversal blocked.".to_string());
    }

    let path = PathBuf::from(raw);
    if !path.is_absolute() {
        return Err("Target path must be absolute.".to_string());
    }
    let mut lexical = PathBuf::new();
    for comp in path.components() {
        match comp {
            Component::RootDir => lexical.push("/"),
            Component::Normal(seg) => lexical.push(seg),
            Component::CurDir => {}
            Component::ParentDir | Component::Prefix(_) => {
                return Err("Parent-directory traversal component blocked.".to_string());
            }
        }
    }

    if is_sensitive_or_protected_path(&lexical) {
        return Err("Target path is protected/sensitive and blocked by zci-daemon.".to_string());
    }

    let sym_meta = fs::symlink_metadata(&lexical)
        .map_err(|_| "Target path does not exist or is inaccessible.".to_string())?;
    if sym_meta.file_type().is_symlink() {
        return Err("Direct symlink targets are rejected by zci-daemon.".to_string());
    }

    let canonical = fs::canonicalize(&lexical)
        .map_err(|_| "Target path canonicalization failed.".to_string())?;
    if is_sensitive_or_protected_path(&canonical) {
        return Err("Canonical target path resolves to a protected/sensitive location.".to_string());
    }

    // Prevent cross-user read privilege escalation when daemon runs with elevated UID.
    if caller_uid != 0 && caller_uid != u32::MAX {
        let meta = fs::metadata(&canonical)
            .map_err(|_| "Failed to inspect target metadata.".to_string())?;
        let world_readable = (meta.mode() & 0o004) != 0;
        if meta.uid() != caller_uid && !world_readable {
            return Err("Caller UID does not have read ownership of target path.".to_string());
        }
    }

    Ok(canonical)
}

pub fn execute_bounded_file_read(
    raw_path: &str,
    caller_uid: u32,
    max_bytes: u64,
) -> Result<(String, u64), String> {
    let canonical = validate_safe_read_path(raw_path, caller_uid)?;
    let meta = fs::metadata(&canonical)
        .map_err(|_| "Target file metadata unavailable.".to_string())?;
    if !meta.is_file() {
        return Err("Target path is not a regular file.".to_string());
    }
    if meta.len() > max_bytes {
        return Err(format!(
            "Target file exceeds zci-daemon read limit ({max_bytes} bytes)."
        ));
    }

    let file = File::open(&canonical).map_err(|e| format!("Failed to open target file: {e}"))?;
    let mut buf = Vec::new();
    file.take(max_bytes + 1)
        .read_to_end(&mut buf)
        .map_err(|e| format!("Failed to read target file: {e}"))?;
    if buf.len() as u64 > max_bytes {
        return Err(format!(
            "Target file grew past zci-daemon read limit ({max_bytes} bytes)."
        ));
    }
    if buf.contains(&0) {
        return Err("Binary NUL bytes rejected in text file read gateway.".to_string());
    }
    let text = String::from_utf8_lossy(&buf).into_owned();
    Ok((text, buf.len() as u64))
}

pub fn execute_bounded_project_analysis(
    raw_dir: &str,
    caller_uid: u32,
    max_depth: usize,
    max_entries: usize,
) -> Result<String, String> {
    let root = validate_safe_read_path(raw_dir, caller_uid)?;
    let root_meta = fs::metadata(&root).map_err(|_| "Directory metadata unavailable.".to_string())?;
    if !root_meta.is_dir() {
        return Err("Project analysis target must be a directory.".to_string());
    }

    const SKIP_DIRS: &[&str] = &[".git", "target", "build", "node_modules", "__pycache__", ".venv"];
    const MANIFEST_NAMES: &[&str] = &[
        "Cargo.toml",
        "CMakeLists.txt",
        "PKGBUILD",
        "pyproject.toml",
        "package.json",
        "Makefile",
    ];

    let mut queue: VecDeque<(PathBuf, usize)> = VecDeque::new();
    queue.push_back((root.clone(), 0));

    let mut file_count = 0usize;
    let mut dir_count = 0usize;
    let mut truncated = false;
    let mut extensions: BTreeMap<String, usize> = BTreeMap::new();
    let mut manifests: Vec<String> = Vec::new();
    let mut sample_files: Vec<String> = Vec::new();

    while let Some((dir, depth)) = queue.pop_front() {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        let mut sorted: Vec<PathBuf> = entries.flatten().map(|e| e.path()).collect();
        sorted.sort();

        for entry_path in sorted {
            if file_count + dir_count >= max_entries {
                truncated = true;
                break;
            }
            let Ok(sym) = fs::symlink_metadata(&entry_path) else {
                continue;
            };
            if sym.file_type().is_symlink() || is_sensitive_or_protected_path(&entry_path) {
                continue;
            }
            let name = entry_path
                .file_name()
                .and_then(|n| n.to_str())
                .unwrap_or_default();
            if name.is_empty() {
                continue;
            }
            let rel = entry_path
                .strip_prefix(&root)
                .map(|p| p.display().to_string())
                .unwrap_or_else(|_| name.to_string());

            if sym.is_dir() {
                if SKIP_DIRS.contains(&name) {
                    continue;
                }
                dir_count += 1;
                if depth + 1 < max_depth {
                    queue.push_back((entry_path, depth + 1));
                }
            } else if sym.is_file() {
                file_count += 1;
                if MANIFEST_NAMES.contains(&name) {
                    manifests.push(rel.clone());
                }
                let ext = entry_path
                    .extension()
                    .and_then(|e| e.to_str())
                    .map(|e| e.to_ascii_lowercase())
                    .unwrap_or_else(|| "(none)".to_string());
                *extensions.entry(ext).or_insert(0) += 1;
                if sample_files.len() < 40 {
                    sample_files.push(rel);
                }
            }
        }
        if truncated {
            break;
        }
    }

    let summary = json!({
        "root_name": root.file_name().and_then(|n| n.to_str()).unwrap_or("workspace"),
        "file_count": file_count,
        "dir_count": dir_count,
        "truncated": truncated,
        "manifests": manifests,
        "extensions": extensions,
        "sample_files": sample_files,
    });
    serde_json::to_string(&summary).map_err(|e| format!("Failed to serialize project analysis: {e}"))
}

fn zvalue_to_json(val: &ZValue<'_>, depth: usize) -> JsonValue {
    if depth >= 8 {
        return JsonValue::String("<depth-limit>".to_string());
    }
    match val {
        ZValue::Bool(b) => JsonValue::Bool(*b),
        ZValue::U8(n) => JsonValue::from(*n),
        ZValue::I16(n) => JsonValue::from(*n),
        ZValue::U16(n) => JsonValue::from(*n),
        ZValue::I32(n) => JsonValue::from(*n),
        ZValue::U32(n) => JsonValue::from(*n),
        ZValue::I64(n) => JsonValue::from(*n),
        ZValue::U64(n) => JsonValue::from(*n),
        ZValue::F64(n) => {
            if n.is_finite() {
                JsonValue::from(*n)
            } else {
                JsonValue::Null
            }
        }
        ZValue::Str(s) => JsonValue::String(s.to_string()),
        ZValue::Value(inner) => zvalue_to_json(inner, depth + 1),
        ZValue::Array(arr) => {
            let items: Vec<JsonValue> = arr
                .iter()
                .take(128)
                .map(|item| zvalue_to_json(item, depth + 1))
                .collect();
            JsonValue::Array(items)
        }
        ZValue::Dict(dict) => {
            let mut obj = Map::new();
            for (k, v) in dict.iter().take(128) {
                let key = match k {
                    ZValue::Str(s) => s.to_string(),
                    other => format!("{other:?}"),
                };
                obj.insert(key, zvalue_to_json(v, depth + 1));
            }
            JsonValue::Object(obj)
        }
        ZValue::Structure(st) => {
            let items: Vec<JsonValue> = st
                .fields()
                .iter()
                .take(64)
                .map(|f| zvalue_to_json(f, depth + 1))
                .collect();
            JsonValue::Array(items)
        }
        _ => JsonValue::String("<unsupported-dbus-value>".to_string()),
    }
}

pub async fn execute_readonly_dbus_diagnostic(
    connection: &zbus::Connection,
    target: DiagnosticTarget,
    timeout: Duration,
) -> Result<String, String> {
    let proxy: zbus::Proxy<'_> = tokio::time::timeout(
        timeout,
        zbus::proxy::Builder::new(connection)
            .destination(target.destination)
            .map_err(|e| format!("Invalid diagnostic destination: {e}"))?
            .path(target.path)
            .map_err(|e| format!("Invalid diagnostic path: {e}"))?
            .interface(target.interface)
            .map_err(|e| format!("Invalid diagnostic interface: {e}"))?
            .cache_properties(CacheProperties::No)
            .build(),
    )
    .await
    .map_err(|_| "Timed out building diagnostic D-Bus proxy.".to_string())?
    .map_err(|e| format!("Diagnostic proxy build error: {e}"))?;

    let raw_map: HashMap<String, OwnedValue> =
        tokio::time::timeout(timeout, proxy.call(target.method, &()))
            .await
            .map_err(|_| {
                format!(
                    "Timed out invoking read-only diagnostic {}.{}",
                    target.interface, target.method
                )
            })?
            .map_err(|e| {
                format!(
                    "Read-only diagnostic {}.{} failed: {e}",
                    target.interface, target.method
                )
            })?;

    let mut obj = Map::new();
    for (k, v) in raw_map {
        obj.insert(k, zvalue_to_json(&v, 0));
    }
    serde_json::to_string(&JsonValue::Object(obj))
        .map_err(|e| format!("Failed to serialize diagnostic response: {e}"))
}

#[derive(Debug, Clone)]
pub enum ModelQueryOutcome {
    Responded { status_code: u16, body: String },
    PassiveStandby { gateway_addr: String, detail: String },
}

/// Strictly loopback-only (`127.0.0.1` / `::1`) bounded HTTP query to local passive Gemma 4B gateway.
pub async fn execute_loopback_model_query(
    cfg: &DaemonConfig,
    agent_id: &str,
    wrapped_prompt: &str,
    timeout: Duration,
) -> Result<ModelQueryOutcome, String> {
    let addr: SocketAddr = cfg.gateway_addr_for_agent(agent_id);
    if !addr.ip().is_loopback() || addr.port() < 1024 {
        return Err("Refusing non-loopback or privileged local model gateway.".to_string());
    }

    let model_alias = if agent_id == "zekans-code" {
        "zekans-gemma4-e4b"
    } else {
        "zci-chat-gemma4-e4b"
    };

    let mut stream = match tokio::time::timeout(timeout, TcpStream::connect(addr)).await {
        Ok(Ok(s)) => s,
        Ok(Err(err)) if err.kind() == std::io::ErrorKind::ConnectionRefused => {
            return Ok(ModelQueryOutcome::PassiveStandby {
                gateway_addr: addr.to_string(),
                detail: "Local Gemma 4B gateway is currently in passive standby (not listening on loopback)."
                    .to_string(),
            });
        }
        Ok(Err(err)) => {
            return Err(format!("Local model loopback connection error: {err}"));
        }
        Err(_) => {
            return Err("Timed out connecting to local model gateway.".to_string());
        }
    };

    let body_json = json!({
        "model": model_alias,
        "stream": false,
        "max_tokens": 512,
        "messages": [
            { "role": "user", "content": wrapped_prompt }
        ]
    });
    let body_bytes =
        serde_json::to_vec(&body_json).map_err(|e| format!("Failed to encode model query: {e}"))?;

    let req = format!(
        "POST /v1/chat/completions HTTP/1.1\r\nHost: {addr}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body_bytes.len()
    );

    tokio::time::timeout(timeout, async {
        stream
            .write_all(req.as_bytes())
            .await
            .map_err(|e| format!("Failed writing model HTTP request: {e}"))?;
        stream
            .write_all(&body_bytes)
            .await
            .map_err(|e| format!("Failed writing model HTTP body: {e}"))?;
        stream
            .flush()
            .await
            .map_err(|e| format!("Failed flushing model HTTP request: {e}"))?;

        let mut raw = Vec::new();
        let mut chunk = [0u8; 4096];
        loop {
            let n = stream
                .read(&mut chunk)
                .await
                .map_err(|e| format!("Failed reading model HTTP response: {e}"))?;
            if n == 0 {
                break;
            }
            if raw.len() + n > MAX_MODEL_RESPONSE_BYTES {
                return Err("Local model gateway response exceeded 256 KiB cap.".to_string());
            }
            raw.extend_from_slice(&chunk[..n]);
        }

        let text = String::from_utf8_lossy(&raw);
        let (head, resp_body) = text
            .split_once("\r\n\r\n")
            .ok_or_else(|| "Malformed HTTP response from local model gateway.".to_string())?;
        let status_code = head
            .lines()
            .next()
            .and_then(|line| line.split_whitespace().nth(1))
            .and_then(|s| s.parse::<u16>().ok())
            .unwrap_or(502);

        Ok(ModelQueryOutcome::Responded {
            status_code,
            body: resp_body.to_string(),
        })
    })
    .await
    .map_err(|_| "Local model query timed out.".to_string())?
}
