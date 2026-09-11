use std::fs;
use std::thread;
use std::time::Duration;

pub struct CpuInfo {
    pub model: String,
    pub cores: usize,
    pub threads: usize,
    pub frequency_ghz: Option<f64>,
    pub temperature_c: Option<f64>,
    pub usage_percent: f64,
}

pub fn read() -> CpuInfo {
    let cpuinfo = fs::read_to_string("/proc/cpuinfo").expect("Failed to read /proc/cpuinfo");

    let model = cpuinfo
        .lines()
        .find(|line| line.starts_with("model name"))
        .and_then(|line| line.split_once(":"))
        .map(|(_, value)| value.trim().to_string())
        .unwrap_or_else(|| "Unknown".to_string());

    let threads = cpuinfo.lines().filter(|line| line.starts_with("processor")).count();

    let cores = cpuinfo
        .lines()
        .find(|line| line.starts_with("cpu cores"))
        .and_then(|line| line.split_once(":"))
        .and_then(|(_, value)| value.trim().parse::<usize>().ok())
        .unwrap_or(0);

    let frequency_ghz = fs::read_to_string("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq")
        .ok()
        .and_then(|value| value.trim().parse::<u64>().ok())
        .map(|khz| khz as f64 / 1_000_000.0);

    let temperature_c = fs::read_to_string("/sys/class/hwmon/hwmon2/temp1_input")
        .ok()
        .and_then(|value| value.trim().parse::<f64>().ok())
        .map(|millidegrees| millidegrees / 1000.0);

    let usage_percent = read_usage_percent();

    CpuInfo {
        model,
        cores,
        threads,
        frequency_ghz,
        temperature_c,
        usage_percent,
    }
}

fn read_usage_percent() -> f64 {
    let first = read_cpu_times();
    thread::sleep(Duration::from_millis(100));
    let second = read_cpu_times();

    match (first, second) {
        (Some((idle1, total1)), Some((idle2, total2))) => {
            let total_delta = total2.saturating_sub(total1);
            let idle_delta = idle2.saturating_sub(idle1);

            if total_delta == 0 {
                return 0.0;
            }

            ((total_delta - idle_delta) as f64 / total_delta as f64 * 100.0)
                .clamp(0.0, 100.0)
        }
        _ => 0.0,
    }
}

fn read_cpu_times() -> Option<(u64, u64)> {
    let stat = fs::read_to_string("/proc/stat").ok()?;
    let line = stat.lines().find(|line| line.starts_with("cpu "))?;
    let values: Vec<u64> = line
        .split_whitespace()
        .skip(1)
        .filter_map(|value| value.parse::<u64>().ok())
        .collect();

    if values.len() < 4 {
        return None;
    }

    let idle = values[3].saturating_add(*values.get(4).unwrap_or(&0));
    let total = values.iter().sum();
    Some((idle, total))
}
