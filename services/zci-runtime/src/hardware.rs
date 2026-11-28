use serde::{Deserialize, Serialize};
use std::fs;
use std::path::Path;

/// 11.03 — Hardware capability profile (Development PC vs. ANCHA Production PC).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum HardwareProfile {
    DevBaseline,
    AnchaProduction,
}

impl HardwareProfile {
    pub fn as_str(&self) -> &'static str {
        match self {
            HardwareProfile::DevBaseline => "dev-baseline",
            HardwareProfile::AnchaProduction => "ancha-production",
        }
    }

    pub fn from_label(raw: &str) -> Option<Self> {
        match raw.trim().to_ascii_lowercase().as_str() {
            "dev-baseline" => Some(HardwareProfile::DevBaseline),
            "ancha-production" => Some(HardwareProfile::AnchaProduction),
            _ => None,
        }
    }

    pub fn satisfies_min_profile(&self, min_profile: HardwareProfile) -> bool {
        match (self, min_profile) {
            (HardwareProfile::AnchaProduction, _) => true,
            (HardwareProfile::DevBaseline, HardwareProfile::DevBaseline) => true,
            (HardwareProfile::DevBaseline, HardwareProfile::AnchaProduction) => false,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProfileLimits {
    pub profile_id: String,
    pub passive_standby_mode: String,
    pub max_context_tokens: u32,
    pub max_output_tokens: u32,
    pub max_concurrent_requests: u32,
    pub vram_budget_mb: u64,
    pub default_active_vram_estimate_mb: u64,
    pub hybrid_cpu_ram_gpu: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HardwareSnapshot {
    pub cpu_cores: u32,
    pub ram_total_mb: u64,
    pub ram_available_mb: u64,
    pub gpu_vendor: String,
    pub gpu_device: String,
    pub gpu_vram_total_mb: u64,
    pub gpu_vram_used_mb: u64,
    pub profile: HardwareProfile,
    pub profile_reason: String,
    pub limits: ProfileLimits,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HardwareOverrideFile {
    pub cpu_cores: u32,
    pub ram_total_mb: u64,
    pub ram_available_mb: u64,
    pub gpu_vendor: Option<String>,
    pub gpu_device: Option<String>,
    pub gpu_vram_total_mb: u64,
    pub gpu_vram_used_mb: Option<u64>,
}

/// Deterministic hardware profile classification:
/// - `ancha-production`: dedicated high-VRAM production machine
///   (>= 12 GiB VRAM & >= 48 GiB RAM & >= 12 cores, OR >= 16 GiB VRAM & >= 32 GiB RAM & >= 8 cores).
/// - `dev-baseline`: development workstation (e.g., Ryzen 5 3600 / 32 GB RAM / RX 560D 4 GB VRAM)
///   using passive standby hybrid CPU+RAM+GPU execution so idle VRAM stays zero.
pub fn select_hardware_profile(
    cpu_cores: u32,
    ram_total_mb: u64,
    gpu_vram_total_mb: u64,
) -> (HardwareProfile, String, ProfileLimits) {
    let meets_ancha_balanced =
        gpu_vram_total_mb >= 12_288 && ram_total_mb >= 49_152 && cpu_cores >= 12;
    let meets_ancha_high_vram =
        gpu_vram_total_mb >= 16_384 && ram_total_mb >= 32_768 && cpu_cores >= 8;

    if meets_ancha_balanced || meets_ancha_high_vram {
        let vram_budget = (gpu_vram_total_mb.saturating_mul(80) / 100).max(8_192);
        let reason = format!(
            "Detected ANCHA production-class hardware ({cpu_cores} cores, {ram_total_mb} MiB RAM, {gpu_vram_total_mb} MiB VRAM)."
        );
        let limits = ProfileLimits {
            profile_id: HardwareProfile::AnchaProduction.as_str().to_string(),
            passive_standby_mode: "passive-standby-ancha-high-vram".to_string(),
            max_context_tokens: 32_768,
            max_output_tokens: 4_096,
            max_concurrent_requests: 4,
            vram_budget_mb: vram_budget,
            default_active_vram_estimate_mb: 6_144.min(vram_budget),
            hybrid_cpu_ram_gpu: true,
        };
        (HardwareProfile::AnchaProduction, reason, limits)
    } else {
        let vram_budget = if gpu_vram_total_mb > 0 {
            (gpu_vram_total_mb / 2).clamp(512, 3_072)
        } else {
            0
        };
        let reason = format!(
            "Selected dev-baseline passive hybrid CPU+RAM+GPU profile ({cpu_cores} cores, {ram_total_mb} MiB RAM, {gpu_vram_total_mb} MiB VRAM)."
        );
        let limits = ProfileLimits {
            profile_id: HardwareProfile::DevBaseline.as_str().to_string(),
            passive_standby_mode: "passive-standby-hybrid-cpu-ram-gpu".to_string(),
            max_context_tokens: 8_192,
            max_output_tokens: 768,
            max_concurrent_requests: 1,
            vram_budget_mb: vram_budget,
            default_active_vram_estimate_mb: if vram_budget > 0 {
                1_536.min(vram_budget)
            } else {
                0
            },
            hybrid_cpu_ram_gpu: true,
        };
        (HardwareProfile::DevBaseline, reason, limits)
    }
}

pub fn build_hardware_snapshot(
    cpu_cores: u32,
    ram_total_mb: u64,
    ram_available_mb: u64,
    gpu_vendor: String,
    gpu_device: String,
    gpu_vram_total_mb: u64,
    gpu_vram_used_mb: u64,
) -> HardwareSnapshot {
    let (profile, profile_reason, limits) =
        select_hardware_profile(cpu_cores, ram_total_mb, gpu_vram_total_mb);
    HardwareSnapshot {
        cpu_cores,
        ram_total_mb,
        ram_available_mb,
        gpu_vendor,
        gpu_device,
        gpu_vram_total_mb,
        gpu_vram_used_mb,
        profile,
        profile_reason,
        limits,
    }
}

fn read_proc_meminfo_mb() -> (u64, u64) {
    let Ok(raw) = fs::read_to_string("/proc/meminfo") else {
        return (0, 0);
    };
    let mut total_kb = 0u64;
    let mut avail_kb = 0u64;
    for line in raw.lines() {
        if let Some(rest) = line.strip_prefix("MemTotal:") {
            total_kb = rest
                .split_whitespace()
                .next()
                .and_then(|s| s.parse::<u64>().ok())
                .unwrap_or(0);
        } else if let Some(rest) = line.strip_prefix("MemAvailable:") {
            avail_kb = rest
                .split_whitespace()
                .next()
                .and_then(|s| s.parse::<u64>().ok())
                .unwrap_or(0);
        }
    }
    (total_kb / 1024, avail_kb / 1024)
}

fn vendor_label(hex_id: &str) -> String {
    match hex_id.trim().to_ascii_lowercase().as_str() {
        "0x1002" => "AMD".to_string(),
        "0x10de" => "NVIDIA".to_string(),
        "0x8086" => "Intel".to_string(),
        other if !other.is_empty() => other.to_string(),
        _ => "unknown".to_string(),
    }
}

fn read_sysfs_u64(path: &Path) -> Option<u64> {
    fs::read_to_string(path).ok()?.trim().parse::<u64>().ok()
}

fn read_primary_gpu_sysfs() -> (String, String, u64, u64) {
    let drm_dir = Path::new("/sys/class/drm");
    let Ok(entries) = fs::read_dir(drm_dir) else {
        return ("none".to_string(), "none".to_string(), 0, 0);
    };

    let mut best: Option<(String, String, u64, u64)> = None;
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        if !name.starts_with("card") || name.contains('-') {
            continue;
        }
        let dev_dir = entry.path().join("device");
        let vram_bytes = read_sysfs_u64(&dev_dir.join("mem_info_vram_total"))
            .or_else(|| read_sysfs_u64(&dev_dir.join("lmem_total_bytes")))
            .unwrap_or(0);
        let used_bytes = read_sysfs_u64(&dev_dir.join("mem_info_vram_used")).unwrap_or(0);
        let vendor_raw = fs::read_to_string(dev_dir.join("vendor")).unwrap_or_default();
        let device_raw = fs::read_to_string(dev_dir.join("device")).unwrap_or_default();

        let vram_mb = vram_bytes / (1024 * 1024);
        let used_mb = used_bytes / (1024 * 1024);
        let candidate = (
            vendor_label(&vendor_raw),
            device_raw.trim().to_string(),
            vram_mb,
            used_mb,
        );

        let replace = match &best {
            None => true,
            Some(prev) => candidate.2 > prev.2,
        };
        if replace {
            best = Some(candidate);
        }
    }

    best.unwrap_or_else(|| ("none".to_string(), "none".to_string(), 0, 0))
}

pub fn detect_host_hardware(override_file: Option<&Path>) -> HardwareSnapshot {
    if let Some(path) = override_file {
        if let Ok(raw) = fs::read_to_string(path) {
            if let Ok(ov) = serde_json::from_str::<HardwareOverrideFile>(&raw) {
                return build_hardware_snapshot(
                    ov.cpu_cores.max(1),
                    ov.ram_total_mb,
                    ov.ram_available_mb,
                    ov.gpu_vendor.unwrap_or_else(|| "synthetic".to_string()),
                    ov.gpu_device.unwrap_or_else(|| "override".to_string()),
                    ov.gpu_vram_total_mb,
                    ov.gpu_vram_used_mb.unwrap_or(0),
                );
            }
        }
    }

    let cpu_cores = std::thread::available_parallelism()
        .map(|n| n.get() as u32)
        .unwrap_or(1);
    let (ram_total_mb, ram_available_mb) = read_proc_meminfo_mb();
    let (gpu_vendor, gpu_device, gpu_vram_total_mb, gpu_vram_used_mb) = read_primary_gpu_sysfs();

    build_hardware_snapshot(
        cpu_cores,
        ram_total_mb,
        ram_available_mb,
        gpu_vendor,
        gpu_device,
        gpu_vram_total_mb,
        gpu_vram_used_mb,
    )
}
