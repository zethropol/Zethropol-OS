use std::fs;
use std::path::Path;

#[derive(Clone)]

pub struct GpuInfo {
    pub card: String,
    pub vendor: String,
    pub device: String,
    pub subsystem_vendor: String,
    pub subsystem_device: String,
    pub model: String,
    pub family: String,
    pub driver: String,
    pub usage_percent: Option<f64>,
    pub memory_usage_percent: Option<f64>,
    pub vram_used_gb: Option<f64>,
    pub vram_total_gb: Option<f64>,
    pub temperature_c: Option<f64>,
    pub core_clock_mhz: Option<f64>,
    pub memory_clock_mhz: Option<f64>,
}

// Keep the legacy primary GPU for existing consumers, and expose the complete
// inventory separately. Card order is deterministic; an inaccessible card never
// prevents the remaining adapters from being discovered.
pub fn read() -> Option<GpuInfo> {
    read_all().into_iter().next()
}

pub fn read_all() -> Vec<GpuInfo> {
    read_inventory(
        Path::new("/sys/class/drm"),
        Path::new("/sys/bus/pci/devices"),
    )
}

fn read_inventory(drm: &Path, pci: &Path) -> Vec<GpuInfo> {
    let mut devices = Vec::new();
    let mut seen = std::collections::HashSet::new();
    let mut cards: Vec<_> = fs::read_dir(drm).into_iter().flatten().flatten().collect();
    cards.sort_by_key(|entry| {
        entry
            .file_name()
            .to_string_lossy()
            .strip_prefix("card")
            .and_then(|n| n.parse::<u32>().ok())
            .unwrap_or(u32::MAX)
    });
    for entry in cards {
        let name = entry.file_name().to_string_lossy().into_owned();
        if !name
            .strip_prefix("card")
            .is_some_and(|n| !n.is_empty() && n.chars().all(|c| c.is_ascii_digit()))
        {
            continue;
        }
        let path = entry.path().join("device");
        if let Some(gpu) = read_device(name, &path) {
            if let Ok(real) = fs::canonicalize(&path) {
                seen.insert(real);
            }
            devices.push(gpu);
        }
    }
    // Unbound PCI GPUs (including a dGPU without a loaded DRM driver) still
    // belong in the inventory. No driver/sensor availability is inferred.
    let mut pci_devices: Vec<_> = fs::read_dir(pci).into_iter().flatten().flatten().collect();
    pci_devices.sort_by_key(|entry| entry.file_name());
    for entry in pci_devices {
        let path = entry.path();
        let is_display = read_id(&path.join("class")).is_some_and(|class| class.starts_with("03"));
        if !is_display || fs::canonicalize(&path).is_ok_and(|real| seen.contains(&real)) {
            continue;
        }
        if let Some(gpu) = read_device(entry.file_name().to_string_lossy().into_owned(), &path) {
            devices.push(gpu);
        }
    }
    devices
}

fn read_device(card: String, path: &Path) -> Option<GpuInfo> {
    let vendor = read_id(&path.join("vendor"))?;
    let device = read_id(&path.join("device"))?;
    let subsystem_vendor = read_id(&path.join("subsystem_vendor")).unwrap_or_default();
    let subsystem_device = read_id(&path.join("subsystem_device")).unwrap_or_default();
    let driver = fs::read_link(path.join("driver"))
        .ok()
        .and_then(|p| p.file_name().map(|n| n.to_string_lossy().into_owned()))
        .unwrap_or_default();
    let (model, family) = resolve_identity(&vendor, &device, &subsystem_vendor, &subsystem_device);
    let (vram_used_gb, vram_total_gb) = read_vram_gb_pair(path);
    Some(GpuInfo {
        card,
        vendor,
        device,
        subsystem_vendor,
        subsystem_device,
        model,
        family,
        driver,
        usage_percent: read_number(&path.join("gpu_busy_percent"))
            .filter(|v| (0.0..=100.0).contains(v)),
        memory_usage_percent: read_number(&path.join("mem_busy_percent"))
            .filter(|v| (0.0..=100.0).contains(v)),
        vram_used_gb,
        vram_total_gb,
        temperature_c: read_temperature(path),
        core_clock_mhz: read_active_clock(&path.join("pp_dpm_sclk")),
        memory_clock_mhz: read_active_clock(&path.join("pp_dpm_mclk")),
    })
}

fn read_id(path: &std::path::Path) -> Option<String> {
    fs::read_to_string(path)
        .ok()
        .map(|value| value.trim().trim_start_matches("0x").to_ascii_lowercase())
        .filter(|value| !value.is_empty() && value.chars().all(|c| c.is_ascii_hexdigit()))
}

fn resolve_identity(
    vendor: &str,
    device: &str,
    subsystem_vendor: &str,
    subsystem_device: &str,
) -> (String, String) {
    // Use the installed PCI ID database rather than guessing a retail model or
    // VRAM size from a chipset/subsystem ID.
    let _ = (subsystem_vendor, subsystem_device);
    let ids = fs::read_to_string("/usr/share/hwdata/pci.ids")
        .or_else(|_| fs::read_to_string("/usr/share/misc/pci.ids"))
        .unwrap_or_default();
    pci_identity(&ids, vendor, device)
}

fn pci_identity(ids: &str, vendor: &str, device: &str) -> (String, String) {
    let mut vendor_name = None;
    let mut device_name = None;
    let mut active = false;
    for line in ids
        .lines()
        .filter(|l| !l.starts_with('#') && !l.trim().is_empty())
    {
        if !line.starts_with(char::is_whitespace) {
            active = line.get(..4) == Some(vendor);
            if active {
                vendor_name = line.get(4..).map(str::trim);
            }
        } else if active
            && line.starts_with('\t')
            && !line.starts_with("\t\t")
            && line.get(1..5) == Some(device)
        {
            device_name = line.get(5..).map(str::trim);
        }
    }
    let model = match (vendor_name, device_name) {
        (Some(v), Some(d)) => format!("{v} {d}"),
        _ => format!("PCI {vendor}:{device}"),
    };
    (model, device_name.unwrap_or("Unknown").to_string())
}

fn read_number(path: &std::path::Path) -> Option<f64> {
    fs::read_to_string(path)
        .ok()
        .and_then(|value| value.trim().parse::<f64>().ok())
        .filter(|v| v.is_finite() && *v >= 0.0)
}

fn read_vram_gb_pair(device_path: &Path) -> (Option<f64>, Option<f64>) {
    let raw_total = read_number(&device_path.join("mem_info_vram_total"));
    let total_bytes = raw_total.filter(|v| *v > 0.0);
    let raw_used = read_number(&device_path.join("mem_info_vram_used"));

    // Reject inconsistent VRAM measurements where total is explicitly 0 or used > total.
    let used_bytes = match (raw_used, raw_total) {
        (Some(used), Some(total)) if total > 0.0 && used <= total => Some(used),
        (Some(_), Some(_)) => None,
        (Some(used), None) => Some(used),
        (None, _) => None,
    };

    (
        used_bytes.map(|v| v / 1_073_741_824.0),
        total_bytes.map(|v| v / 1_073_741_824.0),
    )
}

fn read_active_clock(path: &std::path::Path) -> Option<f64> {
    let contents = fs::read_to_string(path).ok()?;

    contents.lines().find_map(|line| {
        if !line.contains("*") {
            return None;
        }

        line.split_whitespace()
            .nth(1)
            .and_then(|value| value.strip_suffix("Mhz").or_else(|| value.strip_suffix("MHz")))
            .and_then(|value| value.parse::<f64>().ok())
            .filter(|v| v.is_finite() && *v >= 0.0)
            .map(|v| if v == 0.0 { 0.0 } else { v })
    })
}

pub struct GpuTelemetrySample {
    pub usage_percent: Option<f64>,
    pub memory_usage_percent: Option<f64>,
    pub vram_used_gb: Option<f64>,
    pub vram_total_gb: Option<f64>,
    pub core_clock_mhz: Option<f64>,
    pub memory_clock_mhz: Option<f64>,
}

/// Discovers the primary DRM GPU device sysfs path without reading `pci.ids`
/// or scanning unbound PCI devices. Respects numeric card ordering and skips
/// connector entries or inaccessible cards.
pub(crate) fn discover_primary_gpu_sysfs() -> Option<std::path::PathBuf> {
    discover_primary_gpu_sysfs_at(Path::new("/sys/class/drm"))
}

pub(crate) fn discover_primary_gpu_sysfs_at(drm: &Path) -> Option<std::path::PathBuf> {
    let mut cards: Vec<_> = fs::read_dir(drm).into_iter().flatten().flatten().collect();
    cards.sort_by_key(|entry| {
        entry
            .file_name()
            .to_string_lossy()
            .strip_prefix("card")
            .and_then(|n| n.parse::<u32>().ok())
            .unwrap_or(u32::MAX)
    });
    for entry in cards {
        let name = entry.file_name().to_string_lossy().into_owned();
        if !name
            .strip_prefix("card")
            .is_some_and(|n| !n.is_empty() && n.chars().all(|c| c.is_ascii_digit()))
        {
            continue;
        }
        let device_path = entry.path().join("device");
        if read_id(&device_path.join("vendor")).is_some()
            && read_id(&device_path.join("device")).is_some()
        {
            return Some(device_path);
        }
    }
    None
}

pub(crate) fn find_temperature_sensor_path(device_path: &Path) -> Option<std::path::PathBuf> {
    let hwmon = device_path.join("hwmon");
    let mut entries: Vec<_> = fs::read_dir(hwmon).ok()?.flatten().collect();
    entries.sort_by_key(|e| e.file_name());
    for entry in entries {
        let input = entry.path().join("temp1_input");
        if input.exists() {
            return Some(input);
        }
    }
    None
}

pub(crate) fn read_temperature_file(path: &Path) -> Option<f64> {
    let value = fs::read_to_string(path).ok()?;
    let millidegrees = value.trim().parse::<f64>().ok()?;
    if millidegrees.is_finite() && (-100_000.0..=250_000.0).contains(&millidegrees) {
        Some(millidegrees / 1000.0)
    } else {
        None
    }
}

pub(crate) fn read_telemetry_at(device_path: &Path) -> Option<GpuTelemetrySample> {
    if !device_path.exists() {
        return None;
    }
    let (vram_used_gb, vram_total_gb) = read_vram_gb_pair(device_path);
    Some(GpuTelemetrySample {
        usage_percent: read_number(&device_path.join("gpu_busy_percent"))
            .filter(|v| (0.0..=100.0).contains(v)),
        memory_usage_percent: read_number(&device_path.join("mem_busy_percent"))
            .filter(|v| (0.0..=100.0).contains(v)),
        vram_used_gb,
        vram_total_gb,
        core_clock_mhz: read_active_clock(&device_path.join("pp_dpm_sclk")),
        memory_clock_mhz: read_active_clock(&device_path.join("pp_dpm_mclk")),
    })
}

fn read_temperature(device_path: &std::path::Path) -> Option<f64> {
    let path = find_temperature_sensor_path(device_path)?;
    read_temperature_file(&path)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hardware::test_support::Fixture;

    #[test]
    fn hybrid_inventory_survives_inaccessible_card_and_unbound_gpu() {
        let f = Fixture::new();
        f.write("drm/card0/device/vendor", "0x8086\n");
        f.write("drm/card0/device/device", "0x1234\n");
        f.write("drm/card0/device/gpu_busy_percent", "nan");
        f.write("drm/card0/device/hwmon/hwmon0/temp1_input", "NaN");
        f.write("drm/card1/device/vendor", "0x1002\n"); // unreadable/missing identity
        f.write("pci/0000:01:00.0/class", "0x030200\n");
        f.write("pci/0000:01:00.0/vendor", "0x10de\n");
        f.write("pci/0000:01:00.0/device", "0x5678\n");
        f.link(&f.path("drm/card0/device"), "pci/0000:00:02.0");
        f.write("drm/card0/device/class", "0x030000\n");
        let gpus = read_inventory(&f.path("drm"), &f.path("pci"));
        assert_eq!(gpus.len(), 2);
        assert_eq!(gpus[0].vendor, "8086");
        assert_eq!(gpus[1].vendor, "10de");
        assert!(gpus.iter().all(|g| g.driver.is_empty()));
        assert_eq!(gpus[0].temperature_c, None);
        assert_eq!(gpus[0].usage_percent, None);
        assert_eq!(gpus[1].vram_total_gb, None);
    }
    #[test]
    fn card_order_is_numeric_and_connectors_are_excluded() {
        let f = Fixture::new();
        for card in ["card10", "card2", "card2-HDMI-A-1"] {
            f.write(&format!("drm/{card}/device/vendor"), "0x1002");
            f.write(&format!("drm/{card}/device/device"), "0x67ef");
        }
        let gpus = read_inventory(&f.path("drm"), &f.path("pci"));
        assert_eq!(gpus.len(), 2);
        assert_eq!(gpus[0].card, "card2");
        assert_eq!(gpus[1].card, "card10");
    }
    #[test]
    fn pci_identity_never_guesses_vram_or_retail_variant() {
        let ids = "1002  AMD\n\t67ef  Polaris chipset\n\t\t1043 054f  Subsystem\n8086  Intel\n\t67ef  Different device\n";
        assert_eq!(pci_identity(ids, "1002", "67ef").0, "AMD Polaris chipset");
        assert_eq!(pci_identity(ids, "1234", "abcd").0, "PCI 1234:abcd");
    }
    #[test]
    fn lightweight_gpu_telemetry_does_not_hardcode_card0_or_hwmon0() {
        let f = Fixture::new();
        // card0 is incomplete/unreadable; card1 is valid with hwmon2
        f.write("drm/card0/device/vendor", "0x8086\n");
        f.write("drm/card1/device/vendor", "0x1002\n");
        f.write("drm/card1/device/device", "0x67ef\n");
        f.write("drm/card1/device/gpu_busy_percent", "0\n");
        f.write("drm/card1/device/mem_busy_percent", "4\n");
        f.write("drm/card1/device/mem_info_vram_used", "0\n");
        f.write("drm/card1/device/mem_info_vram_total", "4294967296\n");
        f.write("drm/card1/device/pp_dpm_sclk", "0: 300Mhz \n1: 1149Mhz *\n");
        f.write("drm/card1/device/pp_dpm_mclk", "0: 300Mhz *\n");
        f.write("drm/card1/device/hwmon/hwmon2/temp1_input", "31500\n");

        let dev = discover_primary_gpu_sysfs_at(&f.path("drm")).unwrap();
        assert!(dev.ends_with("card1/device"));
        let sample = read_telemetry_at(&dev).unwrap();
        assert_eq!(sample.usage_percent, Some(0.0));
        assert_eq!(sample.memory_usage_percent, Some(4.0));
        assert_eq!(sample.vram_used_gb, Some(0.0));
        assert_eq!(sample.vram_total_gb, Some(4.0));
        assert_eq!(sample.core_clock_mhz, Some(1149.0));
        assert_eq!(sample.memory_clock_mhz, Some(300.0));
        let temp_path = find_temperature_sensor_path(&dev).unwrap();
        assert_eq!(read_temperature_file(&temp_path), Some(31.5));
    }
    #[test]
    fn gpu_telemetry_handles_gpu_less_multi_gpu_high_load_and_inconsistent_vram_or_clocks() {
        // GPU-less DRM directory returns None cleanly
        let empty = Fixture::new();
        empty.write("drm/version", "drm\n");
        assert!(discover_primary_gpu_sysfs_at(&empty.path("drm")).is_none());

        // Multi-GPU deterministic order selects lower numeric card (card1 over card2)
        // and synthetic high load (99.0%, 3.75/4.0 GiB) is converted accurately.
        let multi = Fixture::new();
        multi.write("drm/card2/device/vendor", "0x10de\n");
        multi.write("drm/card2/device/device", "0x2204\n");
        multi.write("drm/card2/device/gpu_busy_percent", "10\n");
        multi.write("drm/card1/device/vendor", "0x1002\n");
        multi.write("drm/card1/device/device", "0x67ef\n");
        multi.write("drm/card1/device/gpu_busy_percent", "99\n");
        multi.write("drm/card1/device/mem_info_vram_used", "4026531840\n"); // 3.75 GiB
        multi.write("drm/card1/device/mem_info_vram_total", "4294967296\n"); // 4.0 GiB
        multi.write("drm/card1/device/pp_dpm_sclk", "7: 1187Mhz *\n");
        multi.write("drm/card1/device/pp_dpm_mclk", "2: 1500Mhz *\n");
        let dev = discover_primary_gpu_sysfs_at(&multi.path("drm")).unwrap();
        assert!(dev.ends_with("card1/device"));
        let high = read_telemetry_at(&dev).unwrap();
        assert_eq!(high.usage_percent, Some(99.0));
        assert!((high.vram_used_gb.unwrap() - 3.75).abs() < 1e-9);
        assert_eq!(high.vram_total_gb, Some(4.0));
        assert_eq!(high.core_clock_mhz, Some(1187.0));
        assert_eq!(high.memory_clock_mhz, Some(1500.0));

        // Inconsistent VRAM (used > total), zero total, and malformed/negative clocks are omitted
        let bad = Fixture::new();
        bad.write("drm/card0/device/vendor", "0x1002\n");
        bad.write("drm/card0/device/device", "0x67ef\n");
        bad.write("drm/card0/device/mem_info_vram_used", "5368709120\n"); // 5 GiB > 4 GiB
        bad.write("drm/card0/device/mem_info_vram_total", "4294967296\n");
        bad.write("drm/card0/device/pp_dpm_sclk", "0: -300Mhz *\n");
        bad.write("drm/card0/device/pp_dpm_mclk", "0: NaNMhz *\n");
        let s_bad = read_telemetry_at(&bad.path("drm/card0/device")).unwrap();
        assert_eq!(s_bad.vram_used_gb, None);
        assert_eq!(s_bad.vram_total_gb, Some(4.0));
        assert_eq!(s_bad.core_clock_mhz, None);
        assert_eq!(s_bad.memory_clock_mhz, None);

        // Zero VRAM total also suppresses vram_used_gb
        bad.write("drm/card0/device/mem_info_vram_total", "0\n");
        bad.write("drm/card0/device/mem_info_vram_used", "0\n");
        let s_zero_total = read_telemetry_at(&bad.path("drm/card0/device")).unwrap();
        assert_eq!(s_zero_total.vram_total_gb, None);
        assert_eq!(s_zero_total.vram_used_gb, None);
    }
}
