use std::fs;

pub struct MemoryInfo {
    pub total_gb: f64,
    pub used_gb: f64,
    pub available_gb: f64,
}

pub fn read() -> MemoryInfo {
    let meminfo = fs::read_to_string("/proc/meminfo").expect("Failed to read /proc/meminfo");

    let memory_value = |name: &str| -> u64 {
        meminfo
            .lines()
            .find(|line| line.starts_with(name))
            .and_then(|line| line.split_once(":"))
            .and_then(|(_, value)| value.split_whitespace().next())
            .and_then(|value| value.parse::<u64>().ok())
            .unwrap_or(0)
    };

    let total_gb = memory_value("MemTotal") as f64 / 1_048_576.0;
    let available_gb = memory_value("MemAvailable") as f64 / 1_048_576.0;

    MemoryInfo {
        total_gb,
        used_gb: total_gb - available_gb,
        available_gb,
    }
}
