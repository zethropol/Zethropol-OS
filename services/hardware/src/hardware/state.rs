use crate::hardware::cpu::CpuInfo;
use crate::hardware::memory::MemoryInfo;
use crate::hardware::gpu::GpuInfo;
use crate::hardware::storage::StorageInfo;
use crate::hardware::network::NetworkInfo;
use crate::hardware::intelligence::HardwareAssessment;

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

pub struct NormalizedMemoryState {
    pub total_gb: f64,
    pub used_gb: f64,
    pub available_gb: f64,
}

pub struct NormalizedGpuState {
    pub detected: bool,
    pub card: Option<String>,
    pub vendor: Option<String>,
    pub device: Option<String>,
    pub driver: Option<String>,
    pub capability_status: String,
}

pub struct NormalizedNetworkState {
    pub download_mbps: f64,
    pub upload_mbps: f64,
    pub ping_ms: Option<f64>,
}

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

pub struct NormalizedHardwareState {
    pub cpu: NormalizedCpuState,
    pub gpu: NormalizedGpuState,
    pub normalized_storage: NormalizedStorageState,
    pub memory: NormalizedMemoryState,
    pub storage: String,
    pub network: NormalizedNetworkState,
    pub firmware: String,
    pub capabilities: String,
    pub health: String,
    pub changes: String,
}

impl NormalizedHardwareState {
    pub fn from_assessment(
        cpu: &CpuInfo,
        memory: &MemoryInfo,
        assessment: &HardwareAssessment,
        gpu: Option<&GpuInfo>,
        storage: Option<&StorageInfo>,
        network: &NetworkInfo,
        changes: String,
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

        let normalized_network = NormalizedNetworkState { download_mbps: network.download_mbps, upload_mbps: network.upload_mbps, ping_ms: network.ping_ms };
        let normalized_gpu = NormalizedGpuState {
            detected: gpu.is_some(),
            card: gpu.map(|value| value.card.clone()),
            vendor: gpu.map(|value| value.vendor.clone()),
            device: gpu.map(|value| value.device.clone()),
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

        Self {
            cpu: normalized_cpu,
            gpu: normalized_gpu,
            memory: normalized_memory,
            normalized_storage,
            storage: assessment.storage_status.clone(),
            network: normalized_network,
            firmware: assessment.firmware_status.clone(),
            capabilities: assessment.performance_power_status.clone(),
            health: assessment.overall_status.clone(),
            changes,
        }
    }
}
