use crate::crypto_util::{constant_time_eq_hex, is_valid_sha256_hex, sha256_file_streaming};
use crate::hardware::{HardwareProfile, HardwareSnapshot};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::Read;
use std::path::{Component, Path, PathBuf};

const MAX_MANIFEST_BYTES: u64 = 1_048_576;
const MAX_MODELS_IN_MANIFEST: usize = 64;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModelManifestFile {
    pub schema_version: u32,
    pub models: Vec<ModelManifestEntry>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModelManifestEntry {
    pub model_id: String,
    pub display_name: String,
    pub format: String,
    pub path: String,
    pub size_bytes: u64,
    pub sha256: String,
    pub license: String,
    pub min_profile: String,
    #[serde(default)]
    pub required_ram_mb: u64,
    #[serde(default)]
    pub required_vram_mb: u64,
    #[serde(default)]
    pub hybrid_vram_estimate_mb: u64,
    #[serde(default = "default_max_context")]
    pub max_context_tokens: u32,
    #[serde(default)]
    pub trusted: bool,
    #[serde(default)]
    pub signature_status: String,
}

fn default_max_context() -> u32 {
    8_192
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VerifiedModelRecord {
    pub model_id: String,
    pub display_name: String,
    pub format: String,
    pub path: String,
    pub expected_size_bytes: u64,
    pub actual_size_bytes: u64,
    pub expected_sha256: String,
    pub actual_sha256: String,
    pub license: String,
    pub min_profile: String,
    pub required_ram_mb: u64,
    pub required_vram_mb: u64,
    pub hybrid_vram_estimate_mb: u64,
    pub max_context_tokens: u32,
    pub signature_status: String,
    pub verified: bool,
    pub rejection_code: String,
    pub rejection_reason: String,
}

#[derive(Debug, Clone, Default)]
pub struct ManifestCatalog {
    pub manifest_path: PathBuf,
    pub manifest_present: bool,
    pub manifest_error: String,
    pub models: BTreeMap<String, VerifiedModelRecord>,
}

fn is_valid_model_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 128
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-'))
}

fn is_denied_model_path(path: &Path) -> bool {
    let Some(s) = path.to_str() else {
        return true;
    };
    let lower = s.to_ascii_lowercase();
    const DENIED_PREFIXES: &[&str] = &[
        "/etc/",
        "/boot/",
        "/root/",
        "/proc/",
        "/sys/",
        "/dev/",
        "/var/lib/zethropol-security",
        "/var/log/zethropol-security",
    ];
    DENIED_PREFIXES.iter().any(|p| lower.starts_with(p))
        || lower.ends_with("/agents.md")
        || lower.contains("services/recovery/src/recovery_service.rs")
}

fn validate_model_path(raw: &str, format: &str) -> Result<PathBuf, (String, String)> {
    if raw.is_empty()
        || raw.len() > 2048
        || raw.contains('\\')
        || raw.chars().any(|c| c == '\0' || c.is_control())
    {
        return Err((
            "invalid-model-path".to_string(),
            "Model path contains invalid characters or syntax.".to_string(),
        ));
    }
    let path = PathBuf::from(raw);
    if !path.is_absolute() {
        return Err((
            "invalid-model-path".to_string(),
            "Model path must be absolute.".to_string(),
        ));
    }
    let mut clean = PathBuf::new();
    for comp in path.components() {
        match comp {
            Component::RootDir => clean.push("/"),
            Component::Normal(seg) => clean.push(seg),
            Component::CurDir => {}
            Component::ParentDir | Component::Prefix(_) => {
                return Err((
                    "invalid-model-path".to_string(),
                    "Path traversal ('..') forbidden in model manifest.".to_string(),
                ));
            }
        }
    }
    if is_denied_model_path(&clean) {
        return Err((
            "forbidden-model-path".to_string(),
            "Model path points to a protected system location.".to_string(),
        ));
    }

    let expected_ext = match format {
        "gguf" => "gguf",
        "safetensors" => "safetensors",
        _ => {
            return Err((
                "unsupported-format".to_string(),
                format!("Unsupported model format '{format}'; only gguf and safetensors are allowed."),
            ))
        }
    };
    let actual_ext = clean
        .extension()
        .and_then(|e| e.to_str())
        .map(|s| s.to_ascii_lowercase())
        .unwrap_or_default();
    if actual_ext != expected_ext {
        return Err((
            "format-extension-mismatch".to_string(),
            format!("Model file extension '.{actual_ext}' does not match declared format '{format}'."),
        ));
    }

    let sym = fs::symlink_metadata(&clean).map_err(|_| {
        (
            "model-file-missing".to_string(),
            format!("Model file does not exist or is unreadable: {}", clean.display()),
        )
    })?;
    if sym.file_type().is_symlink() || !sym.is_file() {
        return Err((
            "invalid-model-file-type".to_string(),
            "Model path must be a regular non-symlink file.".to_string(),
        ));
    }
    let canonical = fs::canonicalize(&clean).map_err(|_| {
        (
            "model-canonicalize-failed".to_string(),
            "Failed to canonicalize model file path.".to_string(),
        )
    })?;
    if is_denied_model_path(&canonical) {
        return Err((
            "forbidden-model-path".to_string(),
            "Canonical model path resolves to a protected location.".to_string(),
        ));
    }
    Ok(canonical)
}

fn validate_model_header_magic(path: &Path, format: &str, file_size: u64) -> Result<(), (String, String)> {
    let mut file = File::open(path).map_err(|e| {
        (
            "model-read-failed".to_string(),
            format!("Failed opening model file for header check: {e}"),
        )
    })?;
    match format {
        "gguf" => {
            let mut magic = [0u8; 4];
            file.read_exact(&mut magic).map_err(|_| {
                (
                    "corrupt-model-header".to_string(),
                    "GGUF model file is truncated (< 4 bytes).".to_string(),
                )
            })?;
            if &magic != b"GGUF" {
                return Err((
                    "corrupt-model-header".to_string(),
                    "GGUF magic header ('GGUF') verification failed.".to_string(),
                ));
            }
            Ok(())
        }
        "safetensors" => {
            let mut hdr_len_bytes = [0u8; 8];
            file.read_exact(&mut hdr_len_bytes).map_err(|_| {
                (
                    "corrupt-model-header".to_string(),
                    "Safetensors file is truncated (< 8 bytes).".to_string(),
                )
            })?;
            let header_len = u64::from_le_bytes(hdr_len_bytes);
            if header_len < 2 || header_len > 100_000_000 || header_len.saturating_add(8) > file_size {
                return Err((
                    "corrupt-model-header".to_string(),
                    "Safetensors header length is invalid or exceeds file size.".to_string(),
                ));
            }
            let mut first = [0u8; 1];
            file.read_exact(&mut first).map_err(|_| {
                (
                    "corrupt-model-header".to_string(),
                    "Safetensors JSON header missing.".to_string(),
                )
            })?;
            if first[0] != b'{' {
                return Err((
                    "corrupt-model-header".to_string(),
                    "Safetensors header does not begin with a JSON object.".to_string(),
                ));
            }
            Ok(())
        }
        _ => Err((
            "unsupported-format".to_string(),
            format!("Unsupported model format '{format}'."),
        )),
    }
}

fn is_trusted_signature_status(status: &str) -> bool {
    matches!(
        status.trim().to_ascii_lowercase().as_str(),
        "verified-manifest-pin" | "signed-ed25519" | "signed-zethropol-v1"
    )
}

/// 11.04 — Fail-closed verification of a single model manifest entry.
pub fn verify_model_entry(
    entry: &ModelManifestEntry,
    hw: &HardwareSnapshot,
) -> VerifiedModelRecord {
    let format = entry.format.trim().to_ascii_lowercase();
    let expected_sha = entry.sha256.trim().to_ascii_lowercase();

    let mut rec = VerifiedModelRecord {
        model_id: entry.model_id.trim().to_string(),
        display_name: entry.display_name.trim().to_string(),
        format: format.clone(),
        path: entry.path.trim().to_string(),
        expected_size_bytes: entry.size_bytes,
        actual_size_bytes: 0,
        expected_sha256: expected_sha.clone(),
        actual_sha256: String::new(),
        license: entry.license.trim().to_string(),
        min_profile: entry.min_profile.trim().to_ascii_lowercase(),
        required_ram_mb: entry.required_ram_mb,
        required_vram_mb: entry.required_vram_mb,
        hybrid_vram_estimate_mb: entry.hybrid_vram_estimate_mb,
        max_context_tokens: entry.max_context_tokens.clamp(512, 131_072),
        signature_status: entry.signature_status.trim().to_string(),
        verified: false,
        rejection_code: String::new(),
        rejection_reason: String::new(),
    };

    let reject = |mut r: VerifiedModelRecord, code: &str, reason: String| -> VerifiedModelRecord {
        r.verified = false;
        r.rejection_code = code.to_string();
        r.rejection_reason = reason;
        r
    };

    if !is_valid_model_id(&rec.model_id) {
        return reject(rec, "invalid-model-id", "Model ID contains invalid characters.".to_string());
    }
    if !matches!(format.as_str(), "gguf" | "safetensors") {
        return reject(
            rec,
            "unsupported-format",
            format!("Unsupported model format '{format}'; expected 'gguf' or 'safetensors'."),
        );
    }
    if !entry.trusted || !is_trusted_signature_status(&entry.signature_status) {
        return reject(
            rec,
            "unsigned-or-unverified-model",
            "Fail-closed rejection: model entry is unsigned or not marked trusted in manifest."
                .to_string(),
        );
    }
    if !is_valid_sha256_hex(&expected_sha) {
        return reject(
            rec,
            "invalid-manifest-sha256",
            "Manifest entry must declare a valid 64-character lowercase SHA-256 hex digest."
                .to_string(),
        );
    }
    if entry.size_bytes == 0 {
        return reject(
            rec,
            "invalid-manifest-size",
            "Manifest entry must declare a non-zero model byte size.".to_string(),
        );
    }

    let Some(min_profile) = HardwareProfile::from_label(&entry.min_profile) else {
        return reject(
            rec,
            "invalid-min-profile",
            format!(
                "Invalid min_profile '{}'; expected 'dev-baseline' or 'ancha-production'.",
                entry.min_profile
            ),
        );
    };

    if !hw.profile.satisfies_min_profile(min_profile) {
        let reason = format!(
            "Model '{}' requires '{}' profile, but host is classified as '{}'.",
            rec.model_id,
            min_profile.as_str(),
            hw.profile.as_str()
        );
        return reject(rec, "profile-incompatible", reason);
    }
    if entry.required_vram_mb > hw.gpu_vram_total_mb {
        let reason = format!(
            "Model '{}' requires {} MiB VRAM, host has {} MiB VRAM.",
            rec.model_id, entry.required_vram_mb, hw.gpu_vram_total_mb
        );
        return reject(rec, "insufficient-vram", reason);
    }
    if entry.required_ram_mb > hw.ram_total_mb && hw.ram_total_mb > 0 {
        let reason = format!(
            "Model '{}' requires {} MiB RAM, host has {} MiB RAM.",
            rec.model_id, entry.required_ram_mb, hw.ram_total_mb
        );
        return reject(rec, "insufficient-ram", reason);
    }

    let canonical = match validate_model_path(&entry.path, &format) {
        Ok(p) => p,
        Err((code, reason)) => return reject(rec, &code, reason),
    };

    let meta = match fs::metadata(&canonical) {
        Ok(m) => m,
        Err(e) => {
            return reject(
                rec,
                "model-stat-failed",
                format!("Failed reading model file metadata: {e}"),
            );
        }
    };
    rec.actual_size_bytes = meta.len();
    if meta.len() != entry.size_bytes {
        return reject(
            rec,
            "size-mismatch",
            format!(
                "Model size mismatch: manifest expects {} bytes, disk file has {} bytes.",
                entry.size_bytes,
                meta.len()
            ),
        );
    }

    if let Err((code, reason)) = validate_model_header_magic(&canonical, &format, meta.len()) {
        return reject(rec, &code, reason);
    }

    let (actual_sha, hashed_bytes) = match sha256_file_streaming(&canonical) {
        Ok(pair) => pair,
        Err(err) => return reject(rec, "sha256-read-failed", err),
    };
    rec.actual_size_bytes = hashed_bytes;
    rec.actual_sha256 = actual_sha.clone();

    if hashed_bytes != entry.size_bytes {
        return reject(
            rec,
            "size-mismatch",
            format!(
                "Model byte count changed during SHA-256 verification ({hashed_bytes} != {}).",
                entry.size_bytes
            ),
        );
    }
    if !constant_time_eq_hex(&actual_sha, &expected_sha) {
        let reason = format!(
            "SHA-256 integrity mismatch for '{}': expected {}, computed {}.",
            rec.model_id, expected_sha, actual_sha
        );
        return reject(rec, "sha256-mismatch", reason);
    }

    rec.verified = true;
    rec
}

pub fn load_and_verify_manifest(path: &Path, hw: &HardwareSnapshot) -> ManifestCatalog {
    let mut catalog = ManifestCatalog {
        manifest_path: path.to_path_buf(),
        manifest_present: false,
        manifest_error: String::new(),
        models: BTreeMap::new(),
    };

    if !path.exists() {
        catalog.manifest_error = format!(
            "Model manifest '{}' not found (fail-closed empty catalog).",
            path.display()
        );
        return catalog;
    }
    catalog.manifest_present = true;

    let meta = match fs::symlink_metadata(path) {
        Ok(m) => m,
        Err(e) => {
            catalog.manifest_error = format!("Failed to stat model manifest: {e}");
            return catalog;
        }
    };
    if meta.file_type().is_symlink() || !meta.is_file() {
        catalog.manifest_error = "Model manifest must be a regular non-symlink file.".to_string();
        return catalog;
    }
    if meta.len() > MAX_MANIFEST_BYTES {
        catalog.manifest_error = "Model manifest exceeds 1 MiB safety limit.".to_string();
        return catalog;
    }

    let raw = match fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => {
            catalog.manifest_error = format!("Failed to read model manifest: {e}");
            return catalog;
        }
    };
    let parsed: ModelManifestFile = match serde_json::from_str(&raw) {
        Ok(m) => m,
        Err(e) => {
            catalog.manifest_error = format!("Malformed model manifest JSON: {e}");
            return catalog;
        }
    };
    if parsed.schema_version != 1 {
        catalog.manifest_error = format!(
            "Unsupported model manifest schema_version {}; expected 1.",
            parsed.schema_version
        );
        return catalog;
    }
    if parsed.models.len() > MAX_MODELS_IN_MANIFEST {
        catalog.manifest_error = format!(
            "Model manifest exceeds maximum entry limit ({MAX_MODELS_IN_MANIFEST})."
        );
        return catalog;
    }

    for entry in &parsed.models {
        let rec = verify_model_entry(entry, hw);
        if !rec.model_id.is_empty() {
            catalog.models.insert(rec.model_id.clone(), rec);
        }
    }

    catalog
}
