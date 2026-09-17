mod hardware;
use hardware::network::NetworkInfo;

use hardware::cpu::CpuInfo;
use hardware::gpu::GpuInfo;
use hardware::intelligence::{assess, assess_hardware_change, hardware_fingerprint};
use hardware::diagnostics::assess_storage;
use hardware::memory::MemoryInfo;
use hardware::storage::StorageInfo;
use hardware::state::NormalizedHardwareState;
use std::io::{self, Write};

struct HardwareInfo {
    cpu: CpuInfo,
    gpu: Option<GpuInfo>,
    memory: MemoryInfo,
    storage: Option<StorageInfo>,
    network: NetworkInfo,
}

fn main() {
    if std::env::args().any(|arg| arg == "--intelligence") {
        print_intelligence();
        return;
    }

    if std::env::args().any(|arg| arg == "--monitor") {
        print_monitor_data();
        return;
    }

    let hardware = HardwareInfo {
        cpu: hardware::cpu::read(),
        gpu: hardware::gpu::read(),
        memory: hardware::memory::read(),
        storage: hardware::storage::read(),
        network: hardware::network::read(),
    };

    println!("Zethropol Hardware Service");
    println!("CPU: {}", hardware.cpu.model);
    println!("Cores: {}", hardware.cpu.cores);
    println!("Threads: {}", hardware.cpu.threads);
    println!("CPU Usage: {:.1}%", hardware.cpu.usage_percent);

    match hardware.cpu.temperature_c {
        Some(value) => println!("CPU Temperature: {value:.1} °C"),
        None => println!("CPU Temperature: Unavailable"),
    }

    match hardware.cpu.frequency_ghz {
        Some(ghz) => println!("Frequency: {ghz:.2} GHz"),
        None => println!("Frequency: Unavailable"),
    }

    match hardware.gpu {
        Some(gpu) => {
            println!("GPU: {}", gpu.card);
            println!("GPU Vendor: {}", gpu.vendor);
            println!("GPU Device: {}", gpu.device);
            println!("GPU Driver: {}", gpu.driver);

            match gpu.usage_percent {
                Some(value) => println!("GPU Load: {value:.0}%"),
                None => println!("GPU Load: Unavailable"),
            }

            match gpu.memory_usage_percent {
                Some(value) => println!("GPU Memory Load: {value:.0}%"),
                None => println!("GPU Memory Load: Unavailable"),
            }

            match gpu.core_clock_mhz {
                Some(value) => println!("GPU Core Clock: {value:.0} MHz"),
                None => println!("GPU Core Clock: Unavailable"),
            }

            match gpu.memory_clock_mhz {
                Some(value) => println!("GPU Memory Clock: {value:.0} MHz"),
                None => println!("GPU Memory Clock: Unavailable"),
            }

            match (gpu.vram_used_gb, gpu.vram_total_gb) {
                (Some(used), Some(total)) => {
                    println!("VRAM: {used:.2} / {total:.2} GB");
                }
                _ => println!("VRAM: Unavailable"),
            }

            match gpu.temperature_c {
                Some(value) => println!("GPU Temperature: {value:.1} °C"),
                None => println!("GPU Temperature: Unavailable"),
            }
        }
        None => println!("GPU: Unavailable"),
    }

    match hardware.storage {
        Some(storage) => {
            println!("Storage: {}", storage.model);
            println!("Storage Firmware: {}", storage.firmware);
            println!("Storage Capacity: {:.0} GB", storage.capacity_gb);
            println!("Storage Used: {:.2} GB", storage.used_gb);
            println!("Storage Available: {:.2} GB", storage.available_gb);

            match storage.temperature_c {
                Some(value) => println!("Storage Temperature: {value:.1} °C"),
                None => println!("Storage Temperature: Unavailable"),
            }
        }
        None => println!("Storage: Unavailable"),
    }

    println!(
        "Network Download: {:.2} MB/s",
        hardware.network.download_mbps
    );
    println!("Network Upload: {:.2} MB/s", hardware.network.upload_mbps);
    match hardware.network.ping_ms {
        Some(value) => println!("Network Ping: {value:.1} ms"),
        None => println!("Network Ping: Unavailable"),
    }
    fn print_intelligence() {
        let hardware = HardwareInfo {
            cpu: hardware::cpu::read(),
            gpu: hardware::gpu::read(),
            memory: hardware::memory::read(),
            storage: hardware::storage::read(),
            network: hardware::network::read(),
        };

        let assessment = assess(
            &hardware.cpu,
            hardware.gpu.as_ref(),
            &hardware.memory,
            hardware.storage.as_ref(),
        );

        let fingerprint = hardware_fingerprint(
            &hardware.cpu,
            hardware.gpu.as_ref(),
            &hardware.memory,
            hardware.storage.as_ref(),
        );
        let hardware_change_status = assess_hardware_change(&fingerprint);
        let storage_diagnostics = assess_storage(hardware.storage.as_ref());
        let normalized_state = NormalizedHardwareState::from_assessment(
            &hardware.cpu,
            &hardware.memory,
            &assessment,
            hardware.gpu.as_ref(),
            hardware.storage.as_ref(),
            &hardware.network,
            hardware_change_status.clone(),
        );

        println!("Zethropol Hardware Intelligence");
        println!("Hardware Fingerprint: {}", fingerprint);
        println!("Hardware Change: {}", hardware_change_status);
        println!("Normalized CPU: model={}, cores={}, threads={}, frequency={:?} GHz, range={:?}-{:?} GHz, governor={:?}, temperature={:?} °C, usage={:.1}%", normalized_state.cpu.model, normalized_state.cpu.cores, normalized_state.cpu.threads, normalized_state.cpu.frequency_ghz, normalized_state.cpu.frequency_min_ghz, normalized_state.cpu.frequency_max_ghz, normalized_state.cpu.governor, normalized_state.cpu.temperature_c, normalized_state.cpu.usage_percent);
        println!("Normalized GPU: detected={}, card={:?}, vendor={:?}, device={:?}, driver={:?}, capability={}", normalized_state.gpu.detected, normalized_state.gpu.card, normalized_state.gpu.vendor, normalized_state.gpu.device, normalized_state.gpu.driver, normalized_state.gpu.capability_status);
        println!("Normalized Storage: detected={}, device={:?}, model={:?}, firmware={:?}, capacity={:?} GB, used={:?} GB, available={:?} GB, temperature={:?} °C", normalized_state.normalized_storage.detected, normalized_state.normalized_storage.device_path, normalized_state.normalized_storage.model, normalized_state.normalized_storage.firmware, normalized_state.normalized_storage.capacity_gb, normalized_state.normalized_storage.used_gb, normalized_state.normalized_storage.available_gb, normalized_state.normalized_storage.temperature_c);
        println!("Normalized Memory: total={:.2} GB, used={:.2} GB, available={:.2} GB", normalized_state.memory.total_gb, normalized_state.memory.used_gb, normalized_state.memory.available_gb);
        println!("Normalized Storage: {}", normalized_state.storage);
        println!("Normalized Network: download={:.2} Mbps, upload={:.2} Mbps, ping={:?} ms", normalized_state.network.download_mbps, normalized_state.network.upload_mbps, normalized_state.network.ping_ms);
        println!("Normalized Firmware: status={}, update_available={}, capsule_updates_available={}", normalized_state.firmware.status, normalized_state.firmware.update_available, normalized_state.firmware.capsule_updates_available);
        println!("Normalized Capabilities: cpu_frequency={:?} GHz, cpu_range={:?}-{:?} GHz, governor={:?}, thread_capacity={}, gpu_acceleration={}, gpu_vram={:?} GiB, storage_power_management={}", normalized_state.capabilities.cpu_frequency_ghz, normalized_state.capabilities.cpu_frequency_min_ghz, normalized_state.capabilities.cpu_frequency_max_ghz, normalized_state.capabilities.cpu_governor, normalized_state.capabilities.thread_capacity, normalized_state.capabilities.gpu_acceleration, normalized_state.capabilities.gpu_vram_gb, normalized_state.capabilities.storage_power_management);
        println!("Normalized Health: overall={}, cpu={}, gpu={}, driver={}, memory={}, storage={}", normalized_state.health.overall_status, normalized_state.health.cpu_status, normalized_state.health.gpu_status, normalized_state.health.driver_status, normalized_state.health.memory_status, normalized_state.health.storage_status);
        println!("Normalized Changes: status={}, changed={}, message={}", normalized_state.changes.status, normalized_state.changes.changed, normalized_state.changes.message);
        println!("Overall: {}", assessment.overall_status);
        println!("CPU: {}", assessment.cpu_status);
        println!("GPU: {}", assessment.gpu_status);
    println!("GPU Capability: {}", assessment.gpu_capability_status);
        println!("Driver: {}", assessment.driver_status);
        println!("Firmware: {}", assessment.firmware_status);
        println!("Memory: {}", assessment.memory_status);
        println!("Storage: {}", assessment.storage_status);
        println!("Storage Health: {}", storage_diagnostics.health_status);
        match storage_diagnostics.temperature_c {
            Some(value) => println!("Storage Diagnostic Temperature: {value:.1} °C"),
            None => println!("Storage Diagnostic Temperature: Unavailable"),
        }
        match storage_diagnostics.percentage_used {
            Some(value) => println!("Storage Wear: {value:.0}%"),
            None => println!("Storage Wear: Unavailable"),
        }
        match storage_diagnostics.available_spare_percent {
            Some(value) => println!("Storage Spare: {value:.0}%"),
            None => println!("Storage Spare: Unavailable"),
        }
        match storage_diagnostics.media_data_integrity_errors {
            Some(value) => println!("Storage Media Errors: {value}"),
            None => println!("Storage Media Errors: Unavailable"),
        }
        match storage_diagnostics.unsafe_shutdowns {
            Some(value) => println!("Storage Unsafe Shutdowns: {value}"),
            None => println!("Storage Unsafe Shutdowns: Unavailable"),
        }
        match storage_diagnostics.error_log_entries {
            Some(value) => println!("Storage Error Log Entries: {value}"),
            None => println!("Storage Error Log Entries: Unavailable"),
        }
        println!("Storage Unsafe Shutdowns Status: {}", storage_diagnostics.unsafe_shutdowns_status);
        println!("Storage Error Log Status: {}", storage_diagnostics.error_log_status);
        println!("Storage Self-Test: {}", storage_diagnostics.self_test_status);
    println!("Performance / Power: {}", assessment.performance_power_status);
    for recommendation in &assessment.recommendations {
        println!("Recommendation: {}", recommendation);
    }
    }

    println!("RAM Total: {:.2} GB", hardware.memory.total_gb);
    println!("RAM Used: {:.2} GB", hardware.memory.used_gb);
    println!("RAM Available: {:.2} GB", hardware.memory.available_gb);
}

fn print_monitor_data() {
    let hardware = HardwareInfo {
        cpu: hardware::cpu::read(),
        gpu: hardware::gpu::read(),
        memory: hardware::memory::read(),
        storage: hardware::storage::read(),
        network: hardware::network::read(),
    };

    let gpu = hardware.gpu.as_ref();
    let storage = hardware.storage.as_ref();

    println!(
        "{:.1} {:.1} {:.0} {:.1} {:.0} {:.0} {:.2} {:.2} {:.1} {:.0} {:.2} {:.2} {:.2} {:.2} {:.2} {:.1} {:.2} {:.2} {:.1}",
        hardware.cpu.usage_percent,
        hardware.cpu.temperature_c.unwrap_or(0.0),
        hardware.cpu.frequency_ghz.unwrap_or(0.0) * 1000.0,
        gpu.and_then(|value| value.usage_percent).unwrap_or(0.0),
        gpu.and_then(|value| value.memory_usage_percent)
            .unwrap_or(0.0),
        gpu.and_then(|value| value.core_clock_mhz).unwrap_or(0.0),
        gpu.and_then(|value| value.vram_used_gb).unwrap_or(0.0),
        gpu.and_then(|value| value.vram_total_gb).unwrap_or(0.0),
        gpu.and_then(|value| value.temperature_c).unwrap_or(0.0),
        gpu.and_then(|value| value.memory_clock_mhz).unwrap_or(0.0),
        hardware.memory.used_gb,
        hardware.memory.total_gb,
        storage.map(|value| value.used_gb).unwrap_or(0.0),
        storage.map(|value| value.capacity_gb).unwrap_or(0.0),
        storage.map(|value| value.available_gb).unwrap_or(0.0),
        storage.and_then(|value| value.temperature_c).unwrap_or(0.0),
        hardware.network.download_mbps,
        hardware.network.upload_mbps,
        hardware.network.ping_ms.unwrap_or(0.0),
    );
    io::stdout().flush().ok();
}
