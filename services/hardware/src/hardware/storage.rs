use std::fs;
use std::path::Path;
use std::process::Command;

pub struct StorageInfo {
    pub device_path: String,
    pub model: String,
    pub firmware: Option<String>,
    pub capacity_gb: f64,
    pub used_gb: Option<f64>,
    pub available_gb: Option<f64>,
    pub temperature_c: Option<f64>,
}

pub fn read() -> Option<StorageInfo> {
    let source = output("findmnt", &["-nro", "SOURCE", "/"])?;
    let partition = source.split('[').next()?;
    if !partition.starts_with("/dev/") {
        return None;
    }
    // Inverse dependencies also cover whole-disk filesystems and device mapper.
    // An ambiguous multi-disk root is not attributed to an arbitrary disk.
    let devices = output(
        "lsblk",
        &[
            "--inverse",
            "--json",
            "--bytes",
            "--output",
            "PATH,TYPE,MODEL,REV,SIZE",
            partition,
        ],
    )?;
    let value: serde_json::Value = serde_json::from_str(&devices).ok()?;
    let mut disks = std::collections::BTreeMap::new();
    collect_disks(&value["blockdevices"], &mut disks);
    if disks.len() != 1 {
        return None;
    }
    let (device_path, disk) = disks.into_iter().next()?;
    let name = Path::new(&device_path).file_name()?.to_str()?;
    let block = Path::new("/sys/class/block").join(name);
    let sectors = fs::read_to_string(block.join("size"))
        .ok()?
        .trim()
        .parse::<u64>()
        .ok()?;
    let capacity_gb = capacity_gb(sectors);
    let model = disk["model"]
        .as_str()
        .map(str::trim)
        .filter(|v| !v.is_empty())
        .unwrap_or(name)
        .to_string();
    let firmware = disk["rev"]
        .as_str()
        .map(str::trim)
        .filter(|v| !v.is_empty())
        .map(str::to_string);
    let (used_gb, available_gb) = output("df", &["-B1", "--output=used,avail", "/"])
        .and_then(|text| parse_df(&text))
        .map(|(used, available)| (Some(used), Some(available)))
        .unwrap_or((None, None));
    // Read only sensors attached to this disk, never the first unrelated NVMe.
    let device = block.join("device");
    let temperature_c = temperature_for_device(&device, Path::new("/sys/class/hwmon"));

    Some(StorageInfo {
        device_path,
        model,
        firmware,
        capacity_gb,
        used_gb,
        available_gb,
        temperature_c,
    })
}

fn output(program: &str, args: &[&str]) -> Option<String> {
    let result = Command::new(program)
        .env("LC_ALL", "C")
        .args(args)
        .output()
        .ok()?;
    if !result.status.success() {
        return None;
    }
    Some(String::from_utf8(result.stdout).ok()?.trim().to_string())
}

fn collect_disks(
    value: &serde_json::Value,
    disks: &mut std::collections::BTreeMap<String, serde_json::Value>,
) {
    if let Some(items) = value.as_array() {
        for item in items {
            if item["type"] == "disk" {
                if let Some(path) = item["path"].as_str() {
                    disks.insert(path.to_string(), item.clone());
                }
            }
            collect_disks(&item["children"], disks);
        }
    }
}

// Linux block/size is always expressed in 512-byte sectors, including 4Kn disks.
fn capacity_gb(sectors: u64) -> f64 {
    sectors as f64 * 512.0 / 1_000_000_000.0
}

fn parse_df(text: &str) -> Option<(f64, f64)> {
    let mut fields = text.lines().nth(1)?.split_whitespace();
    let used = fields.next()?.parse::<u64>().ok()?;
    let available = fields.next()?.parse::<u64>().ok()?;
    Some((
        used as f64 / 1_000_000_000.0,
        available as f64 / 1_000_000_000.0,
    ))
}

fn temperature_for_device(device: &Path, hwmon: &Path) -> Option<f64> {
    let device = fs::canonicalize(device).ok()?;
    fs::read_dir(hwmon).ok()?.flatten().find_map(|entry| {
        // NVMe exposes hwmonN directly under its controller; other drivers use
        // a hwmon/ subdirectory. Match the owning device instead of its layout.
        if fs::canonicalize(entry.path().join("device")).ok()? != device {
            return None;
        }
        let value = fs::read_to_string(entry.path().join("temp1_input"))
            .ok()?
            .trim()
            .parse::<f64>()
            .ok()?;
        (value.is_finite() && (-100_000.0..=250_000.0).contains(&value)).then_some(value / 1000.0)
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn temperature_belongs_to_the_selected_disk_only() {
        let f = crate::hardware::test_support::Fixture::new();
        f.write("disk0/model", "First disk");
        f.write("disk1/model", "Root disk");
        f.write("hwmon/hwmon0/temp1_input", "99000");
        f.link(&f.path("disk0"), "hwmon/hwmon0/device");
        f.write("hwmon/hwmon1/temp1_input", "42000");
        f.link(&f.path("disk1"), "hwmon/hwmon1/device");
        assert_eq!(
            temperature_for_device(&f.path("disk1"), &f.path("hwmon")),
            Some(42.0)
        );
        assert_eq!(
            temperature_for_device(&f.path("missing"), &f.path("hwmon")),
            None
        );
    }
    #[test]
    fn capacity_uses_kernel_sectors_not_logical_block_size() {
        assert_eq!(capacity_gb(1_953_125), 1.0);
    }
    #[test]
    fn filesystem_usage_can_be_missing_without_becoming_zero() {
        assert_eq!(
            parse_df(" Used Avail\n1000000000 2000000000"),
            Some((1.0, 2.0))
        );
        assert_eq!(parse_df(" Used Avail\n- -"), None);
    }
    #[test]
    fn mapper_parents_are_deduplicated_and_ambiguous_disks_preserved() {
        let value = serde_json::json!([{"type":"lvm", "children":[
            {"type":"disk","path":"/dev/sda"}, {"type":"disk","path":"/dev/sda"},
            {"type":"disk","path":"/dev/sdb"}]}]);
        let mut disks = std::collections::BTreeMap::new();
        collect_disks(&value, &mut disks);
        assert_eq!(disks.len(), 2);
    }
}
