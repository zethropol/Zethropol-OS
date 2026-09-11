mod hardware;
use hardware::network::NetworkInfo;

use hardware::cpu::CpuInfo;
use hardware::gpu::GpuInfo;
use hardware::memory::MemoryInfo;
use hardware::storage::StorageInfo;

struct HardwareInfo {
    cpu: CpuInfo,
    gpu: Option<GpuInfo>,
    memory: MemoryInfo,
    storage: Option<StorageInfo>,
    network: NetworkInfo,
}

fn main() {
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
        gpu.and_then(|value| value.memory_usage_percent).unwrap_or(0.0),
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
}
