use crate::hardware::cpu::CpuInfo;
use crate::hardware::memory::MemoryInfo;
use crate::hardware::gpu::GpuInfo;
use crate::hardware::storage::StorageInfo;
use crate::hardware::network::NetworkInfo;
use crate::hardware::power::PowerInfo;
use crate::hardware::intelligence::HardwareAssessment;
use crate::hardware::diagnostics::StorageDiagnostics;
use serde::Serialize;

#[derive(Serialize)]
pub struct NormalizedCpuState {
    pub model: String,
    pub cores: usize,
    pub threads: usize,
    pub frequency_ghz: Option<f64>,
    pub frequency_min_ghz: Option<f64>,
    pub frequency_max_ghz: Option<f64>,
    pub governor: Option<String>,
    pub temperature_c: Option<f64>,
    pub usage_percent: f64,
}

#[derive(Serialize)]
pub struct NormalizedMemoryState {
    pub total_gb: f64,
    pub used_gb: f64,
    pub available_gb: f64,
}

#[derive(Serialize)]
pub struct NormalizedGpuState {
    pub detected: bool,
    pub card: Option<String>,
    pub vendor: Option<String>,
    pub device: Option<String>,
    pub subsystem_vendor: Option<String>,
    pub subsystem_device: Option<String>,
    pub model: Option<String>,
    pub family: Option<String>,
    pub driver: Option<String>,
    pub capability_status: String,
}

#[derive(Serialize)]
pub struct NormalizedHealthState {
    pub overall_status: String,
    pub cpu_status: String,
    pub gpu_status: String,
    pub driver_status: String,
    pub memory_status: String,
    pub storage_status: String,
}

#[derive(Serialize)]
pub struct NormalizedCapabilitiesState {
    pub cpu_frequency_ghz: Option<f64>,
    pub cpu_frequency_min_ghz: Option<f64>,
    pub cpu_frequency_max_ghz: Option<f64>,
    pub cpu_governor: Option<String>,
    pub thread_capacity: String,
    pub gpu_acceleration: bool,
    pub gpu_vram_gb: Option<f64>,
    pub storage_power_management: bool,
}

#[derive(Serialize)]
pub struct NormalizedFirmwareState {
    pub status: String,
    pub update_available: bool,
    pub capsule_updates_available: bool,
}

#[derive(Serialize)]
pub struct NormalizedNetworkState {
    pub download_mbps: f64,
    pub upload_mbps: f64,
    pub ping_ms: Option<f64>,
}

#[derive(Serialize)]
pub struct NormalizedStorageState {
    pub detected: bool,
    pub device_path: Option<String>,
    pub model: Option<String>,
    pub firmware: Option<String>,
    pub capacity_gb: Option<f64>,
    pub used_gb: Option<f64>,
    pub available_gb: Option<f64>,
    pub temperature_c: Option<f64>,
}

#[derive(Serialize)]
pub struct NormalizedDiagnosticsState {
    pub health_status: String,
    pub temperature_c: Option<f64>,
    pub percentage_used: Option<f64>,
    pub available_spare_percent: Option<f64>,
    pub media_data_integrity_errors: Option<u64>,
    pub unsafe_shutdowns: Option<u64>,
    pub error_log_entries: Option<u64>,
    pub unsafe_shutdowns_status: String,
    pub error_log_status: String,
    pub self_test_status: String,
}

#[derive(Serialize)]
pub struct NormalizedPowerState {
    pub available: bool,
    pub battery_present: bool,
    pub battery_percent: Option<f64>,
    pub battery_status: Option<String>,
    pub charging: Option<bool>,
    pub ac_online: Option<bool>,
    pub power_w: Option<f64>,
    pub energy_now_wh: Option<f64>,
    pub energy_full_wh: Option<f64>,
    pub profile: Option<String>,
    pub governor: Option<String>,
}

#[derive(Serialize)]
pub struct NormalizedSystemState {
    pub os_name: String,
    pub os_version: String,
    pub base_system: String,
    pub architecture: String,
    pub kernel_version: String,
    pub desktop_name: String,
    pub desktop_version: String,
    pub session_type: String,
    pub hostname: String,
    pub uptime_seconds: Option<f64>,
}

#[derive(Serialize)]
pub struct NormalizedChangesState {
    pub status: String,
    pub changed: bool,
    pub message: String,
}

#[derive(Serialize)]
pub struct NormalizedHardwareState {
    pub cpu: NormalizedCpuState,
    pub gpu: NormalizedGpuState,
    pub normalized_storage: NormalizedStorageState,
    pub memory: NormalizedMemoryState,
    pub storage: String,
    pub network: NormalizedNetworkState,
    pub firmware: NormalizedFirmwareState,
    pub capabilities: NormalizedCapabilitiesState,
    pub health: NormalizedHealthState,
    pub diagnostics: NormalizedDiagnosticsState,
    pub changes: NormalizedChangesState,
    pub power: NormalizedPowerState,
    pub system: NormalizedSystemState,
}

impl NormalizedHardwareState {
    pub fn from_assessment(
        cpu: &CpuInfo,
        memory: &MemoryInfo,
        assessment: &HardwareAssessment,
        gpu: Option<&GpuInfo>,
        storage: Option<&StorageInfo>,
        network: &NetworkInfo,
        diagnostics: &StorageDiagnostics,
        power: &PowerInfo,
        changes: String,
        system: &crate::hardware::system::SystemInfo,
    ) -> Self {
        let normalized_cpu = NormalizedCpuState {
            model: cpu.model.clone(),
            cores: cpu.cores,
            threads: cpu.threads,
            frequency_ghz: cpu.frequency_ghz,
            frequency_min_ghz: cpu.frequency_min_ghz,
            frequency_max_ghz: cpu.frequency_max_ghz,
            governor: cpu.governor.clone(),
            temperature_c: cpu.temperature_c,
            usage_percent: cpu.usage_percent,
        };

        let normalized_memory = NormalizedMemoryState {
            total_gb: memory.total_gb,
            used_gb: memory.used_gb,
            available_gb: memory.available_gb,
        };

        let normalized_health = NormalizedHealthState {
            overall_status: assessment.overall_status.clone(),
            cpu_status: assessment.cpu_status.clone(),
            gpu_status: assessment.gpu_status.clone(),
            driver_status: assessment.driver_status.clone(),
            memory_status: assessment.memory_status.clone(),
            storage_status: assessment.storage_status.clone(),
        };

        let normalized_firmware = NormalizedFirmwareState {
            status: assessment.firmware_status.clone(),
            update_available: assessment
                .firmware_status
                .contains("firmware update available"),
            capsule_updates_available: !assessment
                .firmware_status
                .contains("UEFI capsule updates unavailable or disabled"),
        };

        let normalized_network = NormalizedNetworkState {
            download_mbps: network.download_mbps,
            upload_mbps: network.upload_mbps,
            ping_ms: network.ping_ms,
        };

        let normalized_gpu = NormalizedGpuState {
            detected: gpu.is_some(),
            card: gpu.map(|value| value.card.clone()),
            vendor: gpu.map(|value| value.vendor.clone()),
            device: gpu.map(|value| value.device.clone()),
            subsystem_vendor: gpu.map(|value| value.subsystem_vendor.clone()),
            subsystem_device: gpu.map(|value| value.subsystem_device.clone()),
            model: gpu.map(|value| value.model.clone()),
            family: gpu.map(|value| value.family.clone()),
            driver: gpu.map(|value| value.driver.clone()),
            capability_status: assessment.gpu_capability_status.clone(),
        };

        let normalized_storage = NormalizedStorageState {
            detected: storage.is_some(),
            device_path: storage.map(|value| value.device_path.clone()),
            model: storage.map(|value| value.model.clone()),
            firmware: storage.map(|value| value.firmware.clone()),
            capacity_gb: storage.map(|value| value.capacity_gb),
            used_gb: storage.map(|value| value.used_gb),
            available_gb: storage.map(|value| value.available_gb),
            temperature_c: storage.and_then(|value| value.temperature_c),
        };

        let normalized_capabilities = NormalizedCapabilitiesState {
            cpu_frequency_ghz: cpu.frequency_ghz,
            cpu_frequency_min_ghz: cpu.frequency_min_ghz,
            cpu_frequency_max_ghz: cpu.frequency_max_ghz,
            cpu_governor: cpu.governor.clone(),
            thread_capacity: if cpu.threads >= 12 {
                "high".to_string()
            } else if cpu.threads >= 8 {
                "multi".to_string()
            } else {
                "standard".to_string()
            },
            gpu_acceleration: gpu.is_some(),
            gpu_vram_gb: gpu.and_then(|value| value.vram_total_gb),
            storage_power_management: storage.is_some(),
        };

        let normalized_diagnostics = NormalizedDiagnosticsState {
            health_status: diagnostics.health_status.clone(),
            temperature_c: diagnostics.temperature_c,
            percentage_used: diagnostics.percentage_used,
            available_spare_percent: diagnostics.available_spare_percent,
            media_data_integrity_errors: diagnostics.media_data_integrity_errors,
            unsafe_shutdowns: diagnostics.unsafe_shutdowns,
            error_log_entries: diagnostics.error_log_entries,
            unsafe_shutdowns_status: diagnostics.unsafe_shutdowns_status.clone(),
            error_log_status: diagnostics.error_log_status.clone(),
            self_test_status: diagnostics.self_test_status.clone(),
        };

        let normalized_power = NormalizedPowerState {
            available: power.available,
            battery_present: power.battery_present,
            battery_percent: power.battery_percent,
            battery_status: power.battery_status.clone(),
            charging: power.charging,
            ac_online: power.ac_online,
            power_w: power.power_w,
            energy_now_wh: power.energy_now_wh,
            energy_full_wh: power.energy_full_wh,
            profile: power.profile.clone(),
            governor: power.governor.clone(),
        };

        let normalized_system = NormalizedSystemState {
            os_name: system.os_name.clone(),
            os_version: system.os_version.clone(),
            base_system: system.base_system.clone(),
            architecture: system.architecture.clone(),
            kernel_version: system.kernel_version.clone(),
            desktop_name: system.desktop_name.clone(),
            desktop_version: system.desktop_version.clone(),
            session_type: system.session_type.clone(),
            hostname: system.hostname.clone(),
            uptime_seconds: system.uptime_seconds,
        };

        let normalized_changes = NormalizedChangesState {
            status: if changes.starts_with("ok:") {
                "ok".to_string()
            } else if changes.starts_with("attention:") {
                "attention".to_string()
            } else if changes.starts_with("unknown:") {
                "unknown".to_string()
            } else {
                "unknown".to_string()
            },
            changed: changes.starts_with("attention:"),
            message: changes.clone(),
        };

        Self {
            cpu: normalized_cpu,
            gpu: normalized_gpu,
            memory: normalized_memory,
            normalized_storage,
            storage: assessment.storage_status.clone(),
            network: normalized_network,
            firmware: normalized_firmware,
            capabilities: normalized_capabilities,
            health: normalized_health,
            diagnostics: normalized_diagnostics,
            changes: normalized_changes,
            power: normalized_power,
            system: normalized_system,
        }
    }
}
