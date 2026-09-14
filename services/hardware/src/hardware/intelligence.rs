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

    let memory_usage = if memory.total_gb > 0.0 {
        memory.used_gb / memory.total_gb * 100.0
    } else {
        0.0
    };

    let memory_status = if memory_usage >= 95.0 {
        "warning: critical memory usage".to_string()
    } else if memory_usage >= 85.0 {
        "attention: high memory usage".to_string()
    } else {
        format!("ok: memory usage {:.1}%", memory_usage)
    };

    let storage_status = match storage {
        Some(storage) if storage.firmware.trim().is_empty() => {
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

    let warning_count = [
        cpu_status.starts_with("warning"),
        gpu_status.starts_with("warning"),
        driver_status.starts_with("warning"),
        firmware_status.starts_with("warning"),
        memory_status.starts_with("warning"),
        storage_status.starts_with("warning"),
    ]
    .into_iter()
    .filter(|value| *value)
    .count();

    let overall_status = if warning_count > 0 {
        "WARNING".to_string()
    } else {
        "OK".to_string()
    };

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

fn build_recommendations(cpu: &CpuInfo, driver_status: &str, firmware_status: &str, _storage: Option<&StorageInfo>) -> Vec<String> {
    let mut recommendations = Vec::new();

    if driver_status.starts_with("warning") {
        recommendations.push("Review the stale GPU driver profile before making driver changes.".to_string());
    }

    if firmware_status.contains("firmware update available") {
        recommendations.push("Review the available SSD firmware update with fwupdmgr before applying it.".to_string());
    }

    if let Some(temp) = cpu.temperature_c {
        if temp >= 90.0 {
            recommendations.push("Inspect CPU cooling and airflow before sustained high-load use.".to_string());
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

    let updatable: Vec<String> = root
        .devices
        .iter()
        .filter(|device| device.flags.iter().any(|flag| flag == "updatable"))
        .map(|device| {
            device
                .name
                .clone()
                .unwrap_or_else(|| "unknown device".to_string())
                .trim_start_matches("-")
                .trim()
                .to_string()
        })
        .collect();

    let capsule_warning = Command::new("fwupdmgr")
        .arg("get-devices")
        .output()
        .map(|output| {
            String::from_utf8_lossy(&output.stderr)
                .contains("UEFI capsule updates not available or enabled")
        })
        .unwrap_or(false);

    let mut statuses = Vec::new();

    if !updatable.is_empty() {
        statuses.push(format!(
            "firmware update available for {}",
            updatable.join(", ")
        ));
    }

    if capsule_warning {
        statuses.push("UEFI capsule updates unavailable or disabled".to_string());
    }

    if statuses.is_empty() {
        "ok: no firmware updates available".to_string()
    } else {
        format!("attention: {}", statuses.join("; "))
    }
}

fn assess_driver(gpu: Option<&GpuInfo>) -> String {
    let gpu = match gpu {
        Some(gpu) => gpu,
        None => return "warning: GPU unavailable".to_string(),
    };

    let expected_profile = match gpu.vendor.to_lowercase().as_str() {
        "0x1002" => "amd",
        "0x8086" => "intel",
        "0x10de" => "nvidia",
        _ => return format!("unknown: unsupported GPU vendor {}", gpu.vendor),
    };

    let output = Command::new("chwd").arg("--list-installed").output();
    let installed = match output {
        Ok(output) if output.status.success() => {
            String::from_utf8_lossy(&output.stdout).to_lowercase()
        }
        _ => {
            return format!(
                "unknown: chwd profile state unavailable; active driver {}",
                gpu.driver
            );
        }
    };

    if installed.contains(expected_profile) {
        format!(
            "ok: active driver {} matches chwd profile {}",
            gpu.driver, expected_profile
        )
    } else if installed.contains("nouveau") && expected_profile == "amd" {
        format!(
            "warning: active driver {} is AMD but chwd has stale nouveau profile",
            gpu.driver
        )
    } else {
        format!(
            "attention: active driver {} differs from chwd profile {}",
            gpu.driver, expected_profile
        )
    }
}

fn assess_gpu_capabilities(gpu: Option<&GpuInfo>) -> String {
    if gpu.is_none() {
        return "unknown: GPU unavailable".to_string();
    }

    let vulkan = Command::new("vulkaninfo")
        .args(["--summary"])
        .output()
        .map(|output| output.status.success())
        .unwrap_or(false);

    let opengl = Command::new("glxinfo")
        .args(["-B"])
        .output()
        .map(|output| output.status.success())
        .unwrap_or(false);

    match (vulkan, opengl) {
        (true, true) => "ok: Vulkan and OpenGL available".to_string(),
        (true, false) => "attention: Vulkan available, OpenGL unavailable".to_string(),
        (false, true) => "attention: OpenGL available, Vulkan unavailable".to_string(),
        (false, false) => "warning: Vulkan and OpenGL unavailable".to_string(),
    }
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
        memory.total_gb,
        storage_identity
    );

    let digest = Sha256::digest(identity.as_bytes());
    format!("{:x}", digest)
}

pub fn assess_hardware_change(current_fingerprint: &str) -> String {
    let path = std::path::Path::new("/home/Zevor/.local/state/zethropol/hardware-fingerprint");

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

fn assess_performance_power(cpu: &CpuInfo, gpu: Option<&GpuInfo>, storage: Option<&StorageInfo>) -> String {
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
        capabilities.push("GPU acceleration available".to_string());
    }

    if let Some(gpu) = gpu {
        if let Some(total) = gpu.vram_total_gb {
            if total > 0.0 {
                capabilities.push(format!("GPU VRAM {:.1} GiB", total));
            }
        }
    }

    if storage.is_some() {
        capabilities.push("NVMe storage power management available".to_string());
    }

    if capabilities.is_empty() {
        "unknown: performance and power capabilities unavailable".to_string()
    } else {
        format!("ok: {}", capabilities.join("; "))
    }
}
