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
        vram_used_gb: read_number(&path.join("mem_info_vram_used")).map(|v| v / 1_073_741_824.0),
        vram_total_gb: read_number(&path.join("mem_info_vram_total"))
            .filter(|v| *v > 0.0)
            .map(|v| v / 1_073_741_824.0),
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
                if millidegrees.is_finite() && (-100_000.0..=250_000.0).contains(&millidegrees) {
                    return Some(millidegrees / 1000.0);
                }
            }
        }
    }

    None
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
}
