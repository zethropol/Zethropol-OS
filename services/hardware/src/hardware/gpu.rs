use std::fs;

pub struct GpuInfo {
    pub card: String,
    pub vendor: String,
    pub device: String,
    pub driver: String,
    pub usage_percent: Option<f64>,
    pub memory_usage_percent: Option<f64>,
    pub vram_used_gb: Option<f64>,
    pub vram_total_gb: Option<f64>,
    pub temperature_c: Option<f64>,
    pub core_clock_mhz: Option<f64>,
    pub memory_clock_mhz: Option<f64>,
}

pub fn read() -> Option<GpuInfo> {
    let drm = fs::read_dir("/sys/class/drm").ok()?;

    for entry in drm.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();

        if !name.starts_with("card") || name.contains("-") {
            continue;
        }

        let device_path = entry.path().join("device");
        let vendor_path = device_path.join("vendor");
        let device_id_path = device_path.join("device");
        let driver_path = device_path.join("driver");

        let vendor = match fs::read_to_string(&vendor_path) {
            Ok(value) => value.trim().to_string(),
            Err(_) => continue,
        };

        let device = match fs::read_to_string(&device_id_path) {
            Ok(value) => value.trim().to_string(),
            Err(_) => continue,
        };

        let driver = match fs::read_link(&driver_path) {
            Ok(path) => match path.file_name() {
                Some(name) => name.to_string_lossy().into_owned(),
                None => continue,
            },
            Err(_) => continue,
        };

        let usage_percent = read_number(&device_path.join("gpu_busy_percent"));
        let memory_usage_percent = read_number(&device_path.join("mem_busy_percent"));

        let vram_used_gb = read_number(&device_path.join("mem_info_vram_used"))
            .map(|bytes| bytes / 1_073_741_824.0);

        let vram_total_gb = read_number(&device_path.join("mem_info_vram_total"))
            .map(|bytes| bytes / 1_073_741_824.0);

        let temperature_c = read_temperature(&device_path);
        let core_clock_mhz = read_active_clock(&device_path.join("pp_dpm_sclk"));
        let memory_clock_mhz = read_active_clock(&device_path.join("pp_dpm_mclk"));

        return Some(GpuInfo {
            card: name,
            vendor,
            device,
            driver,
            usage_percent,
            memory_usage_percent,
            vram_used_gb,
            vram_total_gb,
            temperature_c,
            core_clock_mhz,
            memory_clock_mhz,
        });
    }

    None
}

fn read_number(path: &std::path::Path) -> Option<f64> {
    fs::read_to_string(path)
        .ok()
        .and_then(|value| value.trim().parse::<f64>().ok())
}

fn read_active_clock(path: &std::path::Path) -> Option<f64> {
    let contents = fs::read_to_string(path).ok()?;

    contents.lines().find_map(|line| {
        if !line.contains("*") {
            return None;
        }

        line.split_whitespace()
            .nth(1)
            .and_then(|value| value.strip_suffix("Mhz"))
            .and_then(|value| value.parse::<f64>().ok())
    })
}

fn read_temperature(device_path: &std::path::Path) -> Option<f64> {
    let hwmon = device_path.join("hwmon");

    for entry in fs::read_dir(hwmon).ok()?.flatten() {
        let input = entry.path().join("temp1_input");

        if let Ok(value) = fs::read_to_string(input) {
            if let Ok(millidegrees) = value.trim().parse::<f64>() {
                return Some(millidegrees / 1000.0);
            }
        }
    }

    None
}
