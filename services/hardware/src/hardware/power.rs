// Dynamic power information from Linux sysfs.

use std::fs;
use std::path::Path;

#[derive(Debug, Clone)]
pub struct PowerInfo {
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

impl PowerInfo {
    pub fn read() -> Self {
        let power_supply = Path::new("/sys/class/power_supply");
        let mut battery_present = false;
        let mut battery_percent = None;
        let mut battery_status = None;
        let mut charging = None;
        let mut ac_online = None;
        let mut power_w = None;
        let mut energy_now_wh = None;
        let mut energy_full_wh = None;

        if let Ok(entries) = fs::read_dir(power_supply) {
            for entry in entries.flatten() {
                let path = entry.path();
                let kind = read_string(&path.join("type"));

                match kind.as_deref() {
                    Some("Battery") => {
                        battery_present = true;
                        battery_percent = read_f64(&path.join("capacity"));
                        battery_status = read_string(&path.join("status"));
                        charging = battery_status.as_deref().map(|status| {
                            status.eq_ignore_ascii_case("Charging")
                        });
                        energy_now_wh = read_energy_wh(&path);
                        energy_full_wh = read_energy_full_wh(&path);
                        power_w = read_power_w(&path);
                    }
                    Some("Mains") => {
                        ac_online = read_bool_numeric(&path.join("online"));
                    }
                    _ => {}
                }
            }
        }

        let profile = read_first_available(&[
            "/sys/firmware/acpi/platform_profile", 
            "/sys/firmware/acpi/platform_profile_choices",
        ]);

        let governor = read_string(Path::new("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"));

        Self {
            available: battery_present || ac_online.is_some() || profile.is_some() || governor.is_some(),
            battery_present,
            battery_percent,
            battery_status,
            charging,
            ac_online,
            power_w,
            energy_now_wh,
            energy_full_wh,
            profile,
            governor,
        }
    }
}

fn read_string(path: &Path) -> Option<String> {
    fs::read_to_string(path).ok().map(|value| value.trim().to_string()).filter(|value| !value.is_empty())
}

fn read_f64(path: &Path) -> Option<f64> {
    read_string(path)?.parse::<f64>().ok()
}

fn read_bool_numeric(path: &Path) -> Option<bool> {
    match read_string(path)?.as_str() {
        "1" => Some(true),
        "0" => Some(false),
        _ => None,
    }
}


fn read_energy_full_wh(path: &Path) -> Option<f64> {
    read_f64(&path.join("energy_full")).map(|v| v / 1_000_000.0)
        .or_else(|| read_f64(&path.join("charge_full")).map(|v| v / 1_000_000.0))
}

fn read_energy_wh(path: &Path) -> Option<f64> {
    read_f64(&path.join("energy_now")).map(|v| v / 1_000_000.0)
        .or_else(|| read_f64(&path.join("charge_now")).map(|v| v / 1_000_000.0))
}

fn read_power_w(path: &Path) -> Option<f64> {
    read_f64(&path.join("power_now")).map(|v| v / 1_000_000.0)
        .or_else(|| read_f64(&path.join("current_now")).map(|v| v / 1_000_000.0))
}

fn read_first_available(paths: &[&str]) -> Option<String> {
    for path in paths {
        if let Some(value) = read_string(Path::new(path)) {
            return Some(value);
        }
    }
    None
}
