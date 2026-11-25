use std::fs;
use std::path::Path;
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

    let cpu_root = Path::new("/sys/devices/system/cpu");
    let frequency_ghz = average_frequency_ghz_at(cpu_root);
    let frequency_min_ghz = read_first_cpufreq_khz_at(cpu_root, "scaling_min_freq")
        .map(|khz| khz as f64 / 1_000_000.0);
    let frequency_max_ghz = read_first_cpufreq_khz_at(cpu_root, "scaling_max_freq")
        .map(|khz| khz as f64 / 1_000_000.0);
    let governor = read_first_cpufreq_string_at(cpu_root, "scaling_governor");

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

pub(crate) fn parse_cpu_list(text: &str) -> Vec<usize> {
    let mut cpus = Vec::new();
    for part in text.trim().split(',') {
        let part = part.trim();
        if part.is_empty() {
            continue;
        }
        if let Some((start, end)) = part.split_once('-') {
            if let (Ok(s), Ok(e)) = (start.trim().parse::<usize>(), end.trim().parse::<usize>()) {
                if s <= e && e.saturating_sub(s) <= 4096 {
                    cpus.extend(s..=e);
                }
            }
        } else if let Ok(index) = part.parse::<usize>() {
            cpus.push(index);
        }
    }
    cpus.sort_unstable();
    cpus.dedup();
    cpus
}

fn online_cpu_indices(cpu_root: &Path) -> Vec<usize> {
    if let Ok(online_text) = fs::read_to_string(cpu_root.join("online")) {
        let parsed = parse_cpu_list(&online_text);
        if !parsed.is_empty() {
            return parsed;
        }
    }

    let mut indices = Vec::new();
    for entry in fs::read_dir(cpu_root).into_iter().flatten().flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        let Some(suffix) = name.strip_prefix("cpu") else {
            continue;
        };
        let Ok(index) = suffix.parse::<usize>() else {
            continue;
        };
        // Skip cores explicitly marked offline (cpu0 normally has no online file).
        if let Ok(online_flag) = fs::read_to_string(entry.path().join("online"))
            && online_flag.trim() == "0"
        {
            continue;
        }
        indices.push(index);
    }
    indices.sort_unstable();
    indices
}

pub(crate) fn read_average_frequency_ghz() -> Option<f64> {
    average_frequency_ghz_at(Path::new("/sys/devices/system/cpu"))
}

pub(crate) fn average_frequency_ghz_at(cpu_root: &Path) -> Option<f64> {
    let mut sum_khz = 0.0_f64;
    let mut count = 0usize;

    for index in online_cpu_indices(cpu_root) {
        let freq_path = cpu_root
            .join(format!("cpu{index}"))
            .join("cpufreq/scaling_cur_freq");
        if let Some(khz) = fs::read_to_string(freq_path)
            .ok()
            .and_then(|value| value.trim().parse::<f64>().ok())
            .filter(|v| v.is_finite() && *v > 0.0)
        {
            sum_khz += khz;
            count += 1;
        }
    }

    if count == 0 {
        None
    } else {
        Some((sum_khz / count as f64) / 1_000_000.0)
    }
}

fn read_first_cpufreq_khz_at(cpu_root: &Path, filename: &str) -> Option<u64> {
    for index in online_cpu_indices(cpu_root) {
        let path = cpu_root
            .join(format!("cpu{index}"))
            .join("cpufreq")
            .join(filename);
        if let Some(khz) = fs::read_to_string(path)
            .ok()
            .and_then(|v| v.trim().parse::<u64>().ok())
            .filter(|v| *v > 0)
        {
            return Some(khz);
        }
    }
    None
}

fn read_first_cpufreq_string_at(cpu_root: &Path, filename: &str) -> Option<String> {
    for index in online_cpu_indices(cpu_root) {
        let path = cpu_root
            .join(format!("cpu{index}"))
            .join("cpufreq")
            .join(filename);
        if let Some(value) = fs::read_to_string(path)
            .ok()
            .map(|v| v.trim().to_string())
            .filter(|v| !v.is_empty())
        {
            return Some(value);
        }
    }
    None
}

pub(crate) fn read_temperature() -> Option<f64> {
    temperature_at(Path::new("/sys/class/hwmon"))
}

pub(crate) fn temperature_at(root: &Path) -> Option<f64> {
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
            compute_usage_percent_delta((idle1, total1), (idle2, total2))
        }
        _ => None,
    }
}

pub(crate) fn compute_usage_percent_delta(
    first: (u64, u64),
    second: (u64, u64),
) -> Option<f64> {
    let (idle1, total1) = first;
    let (idle2, total2) = second;

    if total2 <= total1 || idle2 < idle1 {
        return None;
    }

    let total_delta = total2 - total1;
    let idle_delta = idle2 - idle1;
    if idle_delta > total_delta {
        return None;
    }

    let busy = total_delta - idle_delta;
    let pct = (busy as f64 / total_delta as f64 * 100.0).clamp(0.0, 100.0);
    Some(if pct == 0.0 { 0.0 } else { pct })
}

pub(crate) fn read_cpu_times() -> Option<(u64, u64)> {
    let stat = fs::read_to_string("/proc/stat").ok()?;
    parse_cpu_times(&stat)
}

pub(crate) fn parse_cpu_times(stat: &str) -> Option<(u64, u64)> {
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
    #[test]
    fn average_frequency_uses_all_measurable_online_cores() {
        let f = crate::hardware::test_support::Fixture::new();
        f.write("online", "0-1,3\n");
        f.write("cpu0/cpufreq/scaling_cur_freq", "2000000\n");
        f.write("cpu1/cpufreq/scaling_cur_freq", "4000000\n");
        // cpu2 is offline and must not affect the average
        f.write("cpu2/cpufreq/scaling_cur_freq", "9000000\n");
        // cpu3 has an invalid sensor reading and must be skipped cleanly
        f.write("cpu3/cpufreq/scaling_cur_freq", "NaN\n");
        let avg = average_frequency_ghz_at(&f.0).unwrap();
        assert!((avg - 3.0).abs() < 1e-9);
    }
    #[test]
    fn average_frequency_falls_back_to_directory_scan_and_ignores_offline_cores() {
        let f = crate::hardware::test_support::Fixture::new();
        f.write("cpu1/cpufreq/scaling_cur_freq", "3600000\n");
        f.write("cpu2/online", "0\n");
        f.write("cpu2/cpufreq/scaling_cur_freq", "1000000\n");
        f.write("cpu3/online", "1\n");
        f.write("cpu3/cpufreq/scaling_cur_freq", "4400000\n");
        let avg = average_frequency_ghz_at(&f.0).unwrap();
        assert!((avg - 4.0).abs() < 1e-9);
    }
    #[test]
    fn cpu_delta_distinguishes_real_zero_from_invalid_or_reset_counters() {
        assert_eq!(compute_usage_percent_delta((100, 200), (200, 300)), Some(0.0));
        assert_eq!(compute_usage_percent_delta((100, 200), (150, 300)), Some(50.0));
        assert_eq!(compute_usage_percent_delta((100, 200), (100, 200)), None);
        assert_eq!(compute_usage_percent_delta((100, 200), (90, 250)), None);
        assert_eq!(compute_usage_percent_delta((100, 200), (100, 150)), None);
        assert_eq!(compute_usage_percent_delta((100, 200), (250, 300)), None);
    }
}
