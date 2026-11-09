use std::fs;
use std::thread;
use std::time::Duration;

pub struct CpuInfo {
    pub model: String,
    pub cores: usize,
    pub threads: usize,
    pub frequency_ghz: Option<f64>,
    pub frequency_min_ghz: Option<f64>,
    pub frequency_max_ghz: Option<f64>,
    pub governor: Option<String>,
    pub temperature_c: Option<f64>,
    pub usage_percent: Option<f64>,
}

pub fn read() -> CpuInfo {
    let cpuinfo = fs::read_to_string("/proc/cpuinfo").unwrap_or_default();

    let model = cpuinfo
        .lines()
        .find(|line| line.starts_with("model name"))
        .and_then(|line| line.split_once(":"))
        .map(|(_, value)| value.trim().to_string())
        .unwrap_or_else(|| "Unknown".to_string());

    let threads = cpuinfo
        .lines()
        .filter(|line| line.starts_with("processor"))
        .count();

    let cores = physical_cores(&cpuinfo);

    let frequency_ghz = fs::read_to_string("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq")
        .ok()
        .and_then(|value| value.trim().parse::<u64>().ok())
        .map(|khz| khz as f64 / 1_000_000.0);

    let frequency_min_ghz =
        fs::read_to_string("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq")
            .ok()
            .and_then(|value| value.trim().parse::<u64>().ok())
            .map(|khz| khz as f64 / 1_000_000.0);

    let frequency_max_ghz =
        fs::read_to_string("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq")
            .ok()
            .and_then(|value| value.trim().parse::<u64>().ok())
            .map(|khz| khz as f64 / 1_000_000.0);

    let governor = fs::read_to_string("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
        .ok()
        .map(|value| value.trim().to_string());

    let temperature_c = read_temperature();

    let usage_percent = read_usage_percent();

    CpuInfo {
        model,
        cores,
        threads,
        frequency_ghz,
        frequency_min_ghz,
        frequency_max_ghz,
        governor,
        temperature_c,
        usage_percent,
    }
}

fn physical_cores(cpuinfo: &str) -> usize {
    let mut cores = std::collections::HashSet::new();
    for block in cpuinfo.split("\n\n") {
        let field = |name| {
            block
                .lines()
                .filter_map(|l| l.split_once(':'))
                .find(|(key, _)| key.trim() == name)
                .map(|(_, value)| value.trim())
        };
        if let (Some(socket), Some(core)) = (field("physical id"), field("core id")) {
            cores.insert((socket, core));
        }
    }
    cores.len()
}

fn read_temperature() -> Option<f64> {
    temperature_at(std::path::Path::new("/sys/class/hwmon"))
}

fn temperature_at(root: &std::path::Path) -> Option<f64> {
    let mut temperatures = Vec::new();
    for entry in fs::read_dir(root).ok()?.flatten() {
        let path = entry.path();
        let name = fs::read_to_string(path.join("name")).unwrap_or_default();
        if !matches!(
            name.trim(),
            "k10temp" | "coretemp" | "zenpower" | "cpu_thermal"
        ) {
            continue;
        }
        for sensor in fs::read_dir(&path).into_iter().flatten().flatten() {
            let name = sensor.file_name().to_string_lossy().into_owned();
            if !name.starts_with("temp") || !name.ends_with("_input") {
                continue;
            }
            if let Some(value) = fs::read_to_string(sensor.path())
                .ok()
                .and_then(|text| text.trim().parse::<f64>().ok())
                .filter(|v| v.is_finite() && (-100_000.0..=250_000.0).contains(v))
            {
                temperatures.push(value / 1000.0);
            }
        }
    }
    temperatures.into_iter().reduce(f64::max)
}

fn read_usage_percent() -> Option<f64> {
    let first = read_cpu_times();
    thread::sleep(Duration::from_millis(100));
    let second = read_cpu_times();

    match (first, second) {
        (Some((idle1, total1)), Some((idle2, total2))) => {
            let total_delta = total2.saturating_sub(total1);
            let idle_delta = idle2.saturating_sub(idle1);

            if total_delta == 0 {
                return None;
            }

            Some(
                (total_delta.saturating_sub(idle_delta) as f64 / total_delta as f64 * 100.0)
                    .clamp(0.0, 100.0),
            )
        }
        _ => None,
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
    // guest and guest_nice are already included in user/nice.
    let total = values.iter().take(8).sum();
    Some((idle, total))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn missing_hwmon_name_does_not_hide_intel_cpu_sensor() {
        let f = crate::hardware::test_support::Fixture::new();
        f.write("hwmon0/temp1_input", "1000");
        f.write("hwmon1/name", "coretemp");
        f.write("hwmon1/temp1_input", "NaN");
        f.write("hwmon1/temp2_input", "47000");
        f.write("hwmon2/name", "nvme");
        f.write("hwmon2/temp1_input", "99000");
        assert_eq!(temperature_at(&f.0), Some(47.0));
    }
    #[test]
    fn cores_count_socket_and_core_pairs() {
        let data = "physical id : 0\ncore id : 0\n\nphysical id : 0\ncore id : 0\n\nphysical id : 1\ncore id : 0\n\nphysical id : 1\ncore id : 1\n";
        assert_eq!(physical_cores(data), 3);
        assert_eq!(physical_cores("processor : 0"), 0);
    }
}
