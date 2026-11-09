use crate::hardware::cpu::CpuInfo;
use crate::hardware::gpu::GpuInfo;
use crate::hardware::memory::MemoryInfo;
use crate::hardware::storage::StorageInfo;
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::process::Command;

#[derive(Deserialize)]
struct FwupdRoot {
    #[serde(rename = "Devices")]
    devices: Vec<FwupdDevice>,
}

#[derive(Deserialize)]
struct FwupdDevice {
    #[serde(rename = "Name")]
    name: Option<String>,
    #[serde(rename = "Flags", default)]
    flags: Vec<String>,
}

pub struct HardwareAssessment {
    pub cpu_status: String,
    pub gpu_status: String,
    pub gpu_capability_status: String,
    pub driver_status: String,
    pub firmware_status: String,
    pub memory_status: String,
    pub storage_status: String,
    pub performance_power_status: String,
    pub recommendations: Vec<String>,
    pub overall_status: String,
}

pub fn assess(
    cpu: &CpuInfo,
    gpu: Option<&GpuInfo>,
    memory: &MemoryInfo,
    storage: Option<&StorageInfo>,
) -> HardwareAssessment {
    let cpu_status = match cpu.temperature_c {
        Some(temp) if temp >= 90.0 => "warning: high CPU temperature".to_string(),
        Some(temp) if temp >= 80.0 => "attention: elevated CPU temperature".to_string(),
        Some(_) => "ok: CPU temperature".to_string(),
        None => "unknown: CPU temperature unavailable".to_string(),
    };

    let gpu_status = match gpu {
        Some(_) => "ok: GPU detected".to_string(),
        None => "warning: GPU unavailable".to_string(),
    };

    let gpu_capability_status = assess_gpu_capabilities(gpu);
    let driver_status = assess_driver(gpu);
    let firmware_status = assess_firmware();

    let memory_status = match memory
        .used_gb
        .zip(memory.total_gb)
        .filter(|(_, total)| *total > 0.0)
        .map(|(used, total)| used / total * 100.0)
    {
        Some(usage) if usage >= 95.0 => "warning: critical memory usage".to_string(),
        Some(usage) if usage >= 85.0 => "attention: high memory usage".to_string(),
        Some(usage) => format!("ok: memory usage {usage:.1}%"),
        None => "unknown: memory usage unavailable".to_string(),
    };

    let storage_status = match storage {
        Some(storage) if storage.firmware.is_none() => {
            "attention: storage firmware unavailable".to_string()
        }
        Some(storage) if storage.temperature_c.is_some_and(|temp| temp >= 70.0) => {
            "warning: high storage temperature".to_string()
        }
        Some(_) => "ok: storage detected".to_string(),
        None => "warning: storage unavailable".to_string(),
    };

    let performance_power_status = assess_performance_power(cpu, gpu, storage);
    let recommendations = build_recommendations(cpu, &driver_status, &firmware_status, storage);

    let overall_status = aggregate_health(&[
        &cpu_status,
        &gpu_status,
        &driver_status,
        &memory_status,
        &storage_status,
    ])
    .to_string();

    HardwareAssessment {
        cpu_status,
        gpu_status,
        gpu_capability_status,
        driver_status,
        firmware_status,
        memory_status,
        storage_status,
        performance_power_status,
        recommendations,
        overall_status,
    }
}

fn aggregate_health(statuses: &[&str]) -> &'static str {
    if statuses.iter().any(|s| s.starts_with("warning")) {
        "WARNING"
    } else if statuses.iter().any(|s| s.starts_with("unknown")) {
        "UNKNOWN"
    } else if statuses.iter().any(|s| s.starts_with("attention")) {
        "ATTENTION"
    } else {
        "OK"
    }
}

fn build_recommendations(
    cpu: &CpuInfo,
    driver_status: &str,
    firmware_status: &str,
    _storage: Option<&StorageInfo>,
) -> Vec<String> {
    let mut recommendations = Vec::new();

    if driver_status.starts_with("warning") {
        recommendations
            .push("Review the stale GPU driver profile before making driver changes.".to_string());
    }

    if firmware_status.contains("firmware update available") {
        recommendations.push(
            "Review the available SSD firmware update with fwupdmgr before applying it."
                .to_string(),
        );
    }

    if let Some(temp) = cpu.temperature_c {
        if temp >= 90.0 {
            recommendations.push(
                "Inspect CPU cooling and airflow before sustained high-load use.".to_string(),
            );
        } else if temp >= 80.0 {
            recommendations.push("Monitor CPU cooling under sustained load.".to_string());
        }
    }

    if recommendations.is_empty() {
        recommendations.push("No immediate configuration recommendations.".to_string());
    }

    recommendations
}

fn assess_firmware() -> String {
    let output = Command::new("fwupdmgr")
        .args(["get-devices", "--json"])
        .output();

    let output = match output {
        Ok(output) => output,
        Err(_) => return "unknown: fwupd device state unavailable".to_string(),
    };

    if !output.status.success() {
        return "unknown: fwupd device state unavailable".to_string();
    }

    let root: FwupdRoot = match serde_json::from_slice(&output.stdout) {
        Ok(root) => root,
        Err(_) => return "unknown: fwupd JSON unavailable".to_string(),
    };

    let updatable = root
        .devices
        .iter()
        .filter(|device| device.flags.iter().any(|flag| flag == "updatable"))
        .map(|device| device.name.as_deref().unwrap_or("unknown device"))
        .collect::<Vec<_>>();
    // "updatable" describes a capability, not the presence of a newer release.
    format!(
        "unknown: firmware updates not checked; {} updatable devices detected",
        updatable.len()
    )
}

fn assess_driver(gpu: Option<&GpuInfo>) -> String {
    let gpu = match gpu {
        Some(gpu) => gpu,
        None => return "warning: GPU unavailable".to_string(),
    };

    assess_active_driver(&gpu.vendor, &gpu.driver)
}

fn assess_active_driver(vendor: &str, driver: &str) -> String {
    let vendor = vendor.trim().trim_start_matches("0x").to_ascii_lowercase();
    let driver = driver.trim().to_ascii_lowercase();

    if driver.is_empty() {
        return "unknown: GPU driver unavailable".to_string();
    }

    let (vendor_name, expected_drivers): (&str, &[&str]) = match vendor.as_str() {
        "1002" => ("AMD", &["amdgpu", "radeon"]),
        "8086" => ("Intel", &["i915", "xe"]),
        "10de" => ("NVIDIA", &["nvidia", "nouveau"]),
        _ => return format!("unknown: unsupported GPU vendor {}", vendor),
    };

    if expected_drivers.contains(&driver.as_str()) {
        format!("ok: active driver {} matches {} GPU", driver, vendor_name)
    } else {
        format!(
            "warning: active driver {} is unexpected for {} GPU",
            driver, vendor_name
        )
    }
}

fn assess_gpu_capabilities(gpu: Option<&GpuInfo>) -> String {
    if gpu.is_none() {
        return "unknown: GPU unavailable".to_string();
    }

    "unknown: per-device graphics runtime unverified".to_string()
}

pub fn hardware_fingerprint(
    cpu: &CpuInfo,
    gpu: Option<&GpuInfo>,
    memory: &MemoryInfo,
    storage: Option<&StorageInfo>,
) -> String {
    let gpu_identity = gpu
        .map(|gpu| format!("{}:{}:{}", gpu.card, gpu.vendor, gpu.device))
        .unwrap_or_else(|| "none".to_string());

    let storage_identity = storage
        .map(|storage| format!("{}:{}", storage.model, storage.capacity_gb))
        .unwrap_or_else(|| "none".to_string());

    let identity = format!(
        "cpu:{}:{}:{}|gpu:{}|memory:{}|storage:{}",
        cpu.model,
        cpu.cores,
        cpu.threads,
        gpu_identity,
        memory
            .total_gb
            .map(|v| v.to_string())
            .unwrap_or_else(|| "unknown".to_string()),
        storage_identity
    );

    let digest = Sha256::digest(identity.as_bytes());
    format!("{:x}", digest)
}

pub fn inventory_fingerprint(
    cpu: &CpuInfo,
    gpus: &[GpuInfo],
    memory: &MemoryInfo,
    storage: Option<&StorageInfo>,
) -> String {
    let primary = hardware_fingerprint(cpu, gpus.first(), memory, storage);
    if gpus.len() <= 1 {
        return primary;
    }
    let mut identities: Vec<_> = gpus
        .iter()
        .map(|gpu| {
            format!(
                "{}:{}:{}:{}:{}",
                gpu.card, gpu.vendor, gpu.device, gpu.subsystem_vendor, gpu.subsystem_device
            )
        })
        .collect();
    identities.sort();
    format!(
        "{:x}",
        Sha256::digest(format!("{primary}|gpus:{}", identities.join("|")))
    )
}

pub fn assess_hardware_change(current_fingerprint: &str) -> String {
    let path = std::env::var_os("STATE_DIRECTORY")
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("XDG_STATE_HOME")
                .map(|p| std::path::PathBuf::from(p).join("zethropol"))
        })
        .or_else(|| {
            std::env::var_os("HOME")
                .map(|p| std::path::PathBuf::from(p).join(".local/state/zethropol"))
        })
        .map(|p| p.join("hardware-fingerprint"));
    let Some(path) = path else {
        return "unknown: no hardware baseline".to_string();
    };

    let previous = match std::fs::read_to_string(path) {
        Ok(value) => value.trim().to_string(),
        Err(_) => return "unknown: no hardware baseline".to_string(),
    };

    if previous == current_fingerprint {
        "ok: hardware unchanged".to_string()
    } else {
        "attention: hardware configuration changed".to_string()
    }
}

fn assess_performance_power(
    cpu: &CpuInfo,
    gpu: Option<&GpuInfo>,
    storage: Option<&StorageInfo>,
) -> String {
    let mut capabilities = Vec::new();

    match cpu.frequency_ghz {
        Some(freq) => capabilities.push(format!("CPU frequency {:.1} GHz", freq)),
        None => capabilities.push("CPU frequency unavailable".to_string()),
    }

    if let (Some(min), Some(max)) = (cpu.frequency_min_ghz, cpu.frequency_max_ghz) {
        capabilities.push(format!("CPU range {:.1}-{:.1} GHz", min, max));
    }

    if let Some(governor) = &cpu.governor {
        capabilities.push(format!("CPU governor {}", governor));
    }

    if cpu.threads >= 12 {
        capabilities.push("high thread capacity".to_string());
    } else if cpu.threads >= 8 {
        capabilities.push("multi-thread capacity".to_string());
    }

    if gpu.is_some() {
        capabilities.push("GPU detected; acceleration unverified".to_string());
    }

    if let Some(gpu) = gpu {
        if let Some(total) = gpu.vram_total_gb {
            if total > 0.0 {
                capabilities.push(format!("GPU VRAM {:.1} GiB", total));
            }
        }
    }

    if storage.is_some() {
        capabilities.push("storage detected; power management unverified".to_string());
    }

    if capabilities.is_empty() {
        "unknown: performance and power capabilities unavailable".to_string()
    } else {
        format!("ok: {}", capabilities.join("; "))
    }
}

#[cfg(test)]
mod driver_assessment_tests {
    use super::assess_active_driver;

    #[test]
    fn unknown_health_is_not_reported_as_ok() {
        assert_eq!(
            super::aggregate_health(&["ok: memory", "unknown: CPU sensor"]),
            "UNKNOWN"
        );
        assert_eq!(
            super::aggregate_health(&["unknown: CPU sensor", "warning: storage"]),
            "WARNING"
        );
        assert!(assess_active_driver("1002", "").starts_with("unknown:"));
    }
    #[test]
    fn amd_accepts_amdgpu_with_normalized_vendor_id() {
        assert!(assess_active_driver("0x1002", "amdgpu").starts_with("ok:"));
    }

    #[test]
    fn amd_accepts_legacy_radeon_driver() {
        assert!(assess_active_driver("1002", "radeon").starts_with("ok:"));
    }

    #[test]
    fn nvidia_accepts_nouveau_driver() {
        assert!(assess_active_driver("10de", "nouveau").starts_with("ok:"));
    }

    #[test]
    fn intel_accepts_xe_driver() {
        assert!(assess_active_driver("8086", "xe").starts_with("ok:"));
    }

    #[test]
    fn amd_rejects_nouveau_as_active_driver() {
        assert!(assess_active_driver("1002", "nouveau").starts_with("warning:"));
    }
}
