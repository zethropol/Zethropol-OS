use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
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

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FilesystemUsage {
    pub total_bytes: u64,
    pub used_bytes: u64,
    pub free_bytes: u64,
    pub available_bytes: u64,
}

impl FilesystemUsage {
    pub fn from_statvfs_blocks(
        frsize: u64,
        bsize: u64,
        blocks: u64,
        bfree: u64,
        bavail: u64,
    ) -> Option<Self> {
        let unit = if frsize > 0 { frsize } else { bsize };
        if unit == 0 || blocks == 0 || bfree > blocks || bavail > blocks {
            return None;
        }
        let total_bytes = u64::try_from(u128::from(blocks).checked_mul(u128::from(unit))?).ok()?;
        let free_bytes = u64::try_from(u128::from(bfree).checked_mul(u128::from(unit))?).ok()?;
        let available_bytes =
            u64::try_from(u128::from(bavail).checked_mul(u128::from(unit))?).ok()?;
        let used_bytes = total_bytes.checked_sub(free_bytes)?;

        Some(Self {
            total_bytes,
            used_bytes,
            free_bytes,
            available_bytes,
        })
    }

    /// Decimal GB (10^9 bytes), matching existing Zethropol storage fields.
    pub fn total_gb(&self) -> f64 {
        bytes_to_decimal_gb(self.total_bytes)
    }

    pub fn used_gb(&self) -> f64 {
        bytes_to_decimal_gb(self.used_bytes)
    }

    pub fn free_gb(&self) -> f64 {
        bytes_to_decimal_gb(self.free_bytes)
    }

    pub fn available_gb(&self) -> f64 {
        bytes_to_decimal_gb(self.available_bytes)
    }

    /// User-accessible occupancy percentage: `used / (used + available) * 100`.
    /// Matches GNU `df` semantics on Btrfs/ext4 where metadata/reserved blocks
    /// make `used + available < total`.
    pub fn usage_percent(&self) -> Option<f64> {
        let denom = u128::from(self.used_bytes).checked_add(u128::from(self.available_bytes))?;
        if denom == 0 {
            return None;
        }
        let pct = (self.used_bytes as f64 / denom as f64 * 100.0).clamp(0.0, 100.0);
        if !pct.is_finite() {
            return None;
        }
        Some(if pct == 0.0 { 0.0 } else { pct })
    }
}

pub(crate) fn bytes_to_decimal_gb(bytes: u64) -> f64 {
    bytes as f64 / 1_000_000_000.0
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RootMountInfo {
    pub mount_root: String,
    pub fs_type: String,
    pub source: String,
    pub subvolume: Option<String>,
}

#[derive(Debug, Clone)]
pub struct RootDiskTopology {
    #[cfg_attr(not(test), allow(dead_code))]
    pub disk_name: String,
    pub physical_capacity_gb: Option<f64>,
    pub hwmon_temp_path: Option<PathBuf>,
}

#[cfg(target_os = "linux")]
mod statvfs_ffi {
    use super::FilesystemUsage;
    use std::ffi::CString;
    use std::mem::MaybeUninit;

    #[cfg(all(target_arch = "x86_64", target_env = "gnu"))]
    const _: () = {
        assert!(std::mem::size_of::<libc::statvfs>() == 112);
        assert!(std::mem::align_of::<libc::statvfs>() == 8);
    };

    pub fn read_filesystem_usage(mount_path: &str) -> Option<FilesystemUsage> {
        let c_path = CString::new(mount_path).ok()?;
        let mut buf = MaybeUninit::<libc::statvfs>::zeroed();
        // SAFETY: `c_path` is a valid NUL-terminated C string and `buf` points
        // to valid writable memory matching the target Linux `libc::statvfs` ABI.
        let rc = unsafe { libc::statvfs(c_path.as_ptr(), buf.as_mut_ptr()) };
        if rc != 0 {
            return None;
        }
        // SAFETY: `libc::statvfs` returned 0 and initialized `buf`.
        let stat = unsafe { buf.assume_init() };
        // Preserve POSIX/GNU df semantics: block counts are in `f_frsize` units,
        // falling back to `f_bsize` only when `f_frsize == 0`.
        FilesystemUsage::from_statvfs_blocks(
            u64::try_from(stat.f_frsize).ok()?,
            u64::try_from(stat.f_bsize).ok()?,
            u64::try_from(stat.f_blocks).ok()?,
            u64::try_from(stat.f_bfree).ok()?,
            u64::try_from(stat.f_bavail).ok()?,
        )
    }
}

pub(crate) fn read_filesystem_usage(mount_path: &str) -> Option<FilesystemUsage> {
    #[cfg(target_os = "linux")]
    {
        statvfs_ffi::read_filesystem_usage(mount_path)
    }
    #[cfg(not(target_os = "linux"))]
    {
        let text = output("df", &["-B1", "--output=size,used,avail", mount_path])?;
        let mut fields = text.lines().nth(1)?.split_whitespace();
        let total = fields.next()?.parse::<u64>().ok()?;
        let used = fields.next()?.parse::<u64>().ok()?;
        let available = fields.next()?.parse::<u64>().ok()?;
        if total == 0 || used > total {
            return None;
        }
        Some(FilesystemUsage {
            total_bytes: total,
            used_bytes: used,
            free_bytes: total.saturating_sub(used),
            available_bytes: available,
        })
    }
}

pub(crate) fn read_root_filesystem_usage() -> Option<FilesystemUsage> {
    read_filesystem_usage("/")
}

fn unescape_mountinfo(input: &str) -> String {
    let mut out = String::with_capacity(input.len());
    let bytes = input.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'\\' && i + 3 < bytes.len() {
            let oct = &input[i + 1..i + 4];
            if oct.bytes().all(|b| (b'0'..=b'7').contains(&b))
                && let Ok(val) = u8::from_str_radix(oct, 8)
            {
                out.push(val as char);
                i += 4;
                continue;
            }
        }
        out.push(bytes[i] as char);
        i += 1;
    }
    out
}

pub(crate) fn parse_root_mountinfo(content: &str) -> Option<RootMountInfo> {
    let mut result = None;
    for line in content.lines() {
        let Some((left, right)) = line.split_once(" - ") else {
            continue;
        };
        let left_fields: Vec<&str> = left.split_whitespace().collect();
        let right_fields: Vec<&str> = right.split_whitespace().collect();
        if left_fields.len() < 5 || right_fields.len() < 3 {
            continue;
        }
        let mount_point = unescape_mountinfo(left_fields[4]);
        if mount_point != "/" {
            continue;
        }
        let mount_root = unescape_mountinfo(left_fields[3]);
        let fs_type = right_fields[0].to_string();
        let source = unescape_mountinfo(right_fields[1]);
        let super_opts = right_fields[2];

        let subvolume = if fs_type == "btrfs" {
            super_opts
                .split(',')
                .find_map(|opt| opt.strip_prefix("subvol="))
                .map(unescape_mountinfo)
                .filter(|v| !v.is_empty())
                .or_else(|| (!mount_root.is_empty()).then(|| mount_root.clone()))
        } else {
            None
        };

        result = Some(RootMountInfo {
            mount_root,
            fs_type,
            source,
            subvolume,
        });
    }
    result
}

pub(crate) fn read_root_mountinfo() -> Option<RootMountInfo> {
    let content = fs::read_to_string("/proc/self/mountinfo").ok()?;
    parse_root_mountinfo(&content)
}

fn collect_whole_disks_sysfs(
    dev_name: &str,
    block_root: &Path,
    disks: &mut BTreeSet<String>,
    depth: usize,
) {
    if depth > 16 || dev_name.is_empty() {
        return;
    }

    let entry = block_root.join(dev_name);
    if !entry.exists() {
        return;
    }

    // Virtual/mapper/RAID devices expose underlying block devices under slaves/.
    let slaves_dir = entry.join("slaves");
    let mut slaves = Vec::new();
    if let Ok(read_dir) = fs::read_dir(&slaves_dir) {
        for slave in read_dir.flatten() {
            let name = slave.file_name().to_string_lossy().into_owned();
            if !name.is_empty() {
                slaves.push(name);
            }
        }
    }
    if !slaves.is_empty() {
        for slave in slaves {
            collect_whole_disks_sysfs(&slave, block_root, disks, depth + 1);
        }
        return;
    }

    // Partitions carry a `partition` attribute and sit directly beneath the whole disk.
    if entry.join("partition").exists() {
        if let Ok(canonical) = fs::canonicalize(&entry)
            && let Some(parent_name) = canonical
                .parent()
                .and_then(|p| p.file_name())
                .and_then(|n| n.to_str())
            && block_root.join(parent_name).exists()
        {
            collect_whole_disks_sysfs(parent_name, block_root, disks, depth + 1);
            return;
        }
    }

    disks.insert(dev_name.to_string());
}

pub(crate) fn resolve_root_disk_topology_at(
    mount_source: &str,
    block_root: &Path,
    hwmon_root: &Path,
) -> Option<RootDiskTopology> {
    let device_node = mount_source.split('[').next()?;
    if !device_node.starts_with("/dev/") {
        return None;
    }

    let resolved = fs::canonicalize(device_node).unwrap_or_else(|_| PathBuf::from(device_node));
    let dev_name = resolved.file_name()?.to_str()?;

    let mut disks = BTreeSet::new();
    collect_whole_disks_sysfs(dev_name, block_root, &mut disks, 0);
    if disks.len() != 1 {
        return None;
    }
    let disk_name = disks.into_iter().next()?;
    let block = block_root.join(&disk_name);
    let physical_capacity_gb = fs::read_to_string(block.join("size"))
        .ok()
        .and_then(|s| s.trim().parse::<u64>().ok())
        .filter(|sectors| *sectors > 0)
        .map(capacity_gb);
    let hwmon_temp_path = find_temperature_path_for_device(&block.join("device"), hwmon_root);

    Some(RootDiskTopology {
        disk_name,
        physical_capacity_gb,
        hwmon_temp_path,
    })
}

pub(crate) fn discover_root_disk_topology(mount_source: &str) -> Option<RootDiskTopology> {
    resolve_root_disk_topology_at(
        mount_source,
        Path::new("/sys/class/block"),
        Path::new("/sys/class/hwmon"),
    )
}

pub(crate) fn find_temperature_path_for_device(device: &Path, hwmon: &Path) -> Option<PathBuf> {
    let device = fs::canonicalize(device).ok()?;
    let mut entries: Vec<_> = fs::read_dir(hwmon).ok()?.flatten().collect();
    entries.sort_by_key(|e| e.file_name());
    entries.into_iter().find_map(|entry| {
        if fs::canonicalize(entry.path().join("device")).ok()? != device {
            return None;
        }
        let temp_path = entry.path().join("temp1_input");
        temp_path.exists().then_some(temp_path)
    })
}

pub(crate) fn read_temperature_file(path: &Path) -> Option<f64> {
    let value = fs::read_to_string(path)
        .ok()?
        .trim()
        .parse::<f64>()
        .ok()?;
    (value.is_finite() && (-100_000.0..=250_000.0).contains(&value)).then_some(value / 1000.0)
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
    let mut disks = BTreeMap::new();
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
    let (used_gb, available_gb) = read_root_filesystem_usage()
        .map(|fs_usage| (Some(fs_usage.used_gb()), Some(fs_usage.available_gb())))
        .unwrap_or_else(|| {
            output("df", &["-B1", "--output=used,avail", "/"])
                .and_then(|text| parse_df(&text))
                .map(|(used, available)| (Some(used), Some(available)))
                .unwrap_or((None, None))
        });
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

fn collect_disks(value: &serde_json::Value, disks: &mut BTreeMap<String, serde_json::Value>) {
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
    let path = find_temperature_path_for_device(device, hwmon)?;
    read_temperature_file(&path)
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
        let mut disks = BTreeMap::new();
        collect_disks(&value, &mut disks);
        assert_eq!(disks.len(), 2);
    }
    #[test]
    fn btrfs_filesystem_usage_separates_total_used_free_available_and_matches_df_ratio() {
        // Real host Btrfs block counts (4096-byte fundamental block size):
        // total=495.810760704 GB, used=255.835602944 GB, free=239.975157760 GB, avail=237.786742784 GB
        let usage = FilesystemUsage::from_statvfs_blocks(
            4096,
            4096,
            121_047_549,
            58_587_685,
            58_053_404,
        )
        .unwrap();
        assert_eq!(usage.total_bytes, 495_810_760_704);
        assert_eq!(usage.used_bytes, 255_835_602_944);
        assert_eq!(usage.free_bytes, 239_975_157_760);
        assert_eq!(usage.available_bytes, 237_786_742_784);
        // Decimal GB verification
        assert!((usage.total_gb() - 495.810760704).abs() < 1e-6);
        assert!((usage.used_gb() - 255.835602944).abs() < 1e-6);
        assert!((usage.free_gb() - 239.975157760).abs() < 1e-6);
        assert!((usage.available_gb() - 237.786742784).abs() < 1e-6);
        // Free (raw) > Available (user-accessible) because of Btrfs metadata reserve
        assert!(usage.free_bytes > usage.available_bytes);
        // Formula: used / (used + available) * 100
        let expected_pct = 255_835_602_944.0 / (255_835_602_944.0 + 237_786_742_784.0) * 100.0;
        assert!((usage.usage_percent().unwrap() - expected_pct).abs() < 1e-9);
    }
    #[test]
    fn filesystem_usage_guards_zero_denominator_and_invalid_blocks() {
        assert_eq!(
            FilesystemUsage::from_statvfs_blocks(4096, 4096, 0, 0, 0),
            None
        );
        assert_eq!(
            FilesystemUsage::from_statvfs_blocks(0, 0, 100, 50, 50),
            None
        );
        // bfree > blocks is invalid
        assert_eq!(
            FilesystemUsage::from_statvfs_blocks(4096, 4096, 100, 101, 50),
            None
        );
        // 0 used + 0 available (100% reserved) -> zero denominator returns None
        let zero_denom = FilesystemUsage {
            total_bytes: 1_000_000_000,
            used_bytes: 0,
            free_bytes: 1_000_000_000,
            available_bytes: 0,
        };
        assert_eq!(zero_denom.usage_percent(), None);
        // Genuine 0% used with available > 0 returns Some(0.0)
        let empty_fs = FilesystemUsage::from_statvfs_blocks(4096, 4096, 1000, 1000, 1000).unwrap();
        assert_eq!(empty_fs.used_bytes, 0);
        assert_eq!(empty_fs.usage_percent(), Some(0.0));
    }
    #[test]
    fn statvfs_frsize_precedence_and_bsize_fallback_preserve_semantics() {
        // POSIX/glibc statvfs: block counts use `f_frsize` (fundamental fragment size),
        // NOT `f_bsize` (preferred I/O transfer block size), when `f_frsize > 0`.
        let distinct = FilesystemUsage::from_statvfs_blocks(4096, 65536, 1000, 400, 350).unwrap();
        assert_eq!(distinct.total_bytes, 4_096_000);
        assert_eq!(distinct.free_bytes, 1_638_400);
        assert_eq!(distinct.used_bytes, 2_457_600);
        assert_eq!(distinct.available_bytes, 1_433_600);
        assert!((distinct.total_gb() - 0.004096).abs() < 1e-12);
        let expected_pct = 2_457_600.0 / (2_457_600.0 + 1_433_600.0) * 100.0;
        assert!((distinct.usage_percent().unwrap() - expected_pct).abs() < 1e-9);

        // Fallback to `f_bsize` only when `f_frsize == 0`.
        let fallback = FilesystemUsage::from_statvfs_blocks(0, 4096, 1000, 400, 350).unwrap();
        assert_eq!(fallback, distinct);
    }
    #[test]
    fn statvfs_root_matches_live_df_output() {
        let usage = read_root_filesystem_usage().expect("statvfs(/) should succeed on Linux");
        assert!(usage.total_bytes > 0);
        assert!(usage.used_bytes <= usage.total_bytes);
        assert!(usage.available_bytes <= usage.total_bytes);
        if let Some(df_out) = output("df", &["-B1", "--output=size,used,avail", "/"]) {
            let mut fields = df_out.lines().nth(1).unwrap().split_whitespace();
            let df_total: u64 = fields.next().unwrap().parse().unwrap();
            let df_used: u64 = fields.next().unwrap().parse().unwrap();
            let df_avail: u64 = fields.next().unwrap().parse().unwrap();
            assert_eq!(usage.total_bytes, df_total);
            // Allow small live filesystem drift during test execution (<= 1 GiB),
            // confirming Btrfs/ext4 block-unit scaling matches GNU `df -B1`.
            assert!(usage.used_bytes.abs_diff(df_used) <= 1_073_741_824);
            assert!(usage.available_bytes.abs_diff(df_avail) <= 1_073_741_824);
            let df_pct = df_used as f64 / (df_used as f64 + df_avail as f64) * 100.0;
            assert!((usage.usage_percent().unwrap() - df_pct).abs() <= 1.0);
        }
        // Invalid/missing mount path returns None cleanly without panicking or spawning processes.
        assert_eq!(read_filesystem_usage("/nonexistent-zethropol-mount-check"), None);
    }
    #[test]
    fn mountinfo_parses_btrfs_root_subvolume_and_takes_last_root_mount() {
        let content = "\
22 1 0:20 / / rw,relatime - tmpfs rootfs rw
42 22 0:35 /@ / rw,noatime shared:1 - btrfs /dev/nvme0n1p2 rw,compress=zstd:1,subvolid=539,subvol=/@
43 42 0:35 /@home /home rw,noatime - btrfs /dev/nvme0n1p2 rw,subvol=/@home
";
        let root = parse_root_mountinfo(content).unwrap();
        assert_eq!(root.fs_type, "btrfs");
        assert_eq!(root.source, "/dev/nvme0n1p2");
        assert_eq!(root.mount_root, "/@");
        assert_eq!(root.subvolume.as_deref(), Some("/@"));
    }
    #[test]
    fn sysfs_disk_resolution_handles_partitions_dm_slaves_and_multi_disk_ambiguity() {
        let f = crate::hardware::test_support::Fixture::new();
        // Whole disk nvme0n1 with partition nvme0n1p2 and attached hwmon
        f.write("devices/nvme0/nvme0n1/size", "976773168\n"); // ~500.107 GB
        f.write("devices/nvme0/nvme0n1/nvme0n1p2/partition", "2\n");
        f.link(&f.path("devices/nvme0"), "devices/nvme0/nvme0n1/device");
        f.link(&f.path("devices/nvme0/nvme0n1"), "block/nvme0n1");
        f.link(&f.path("devices/nvme0/nvme0n1/nvme0n1p2"), "block/nvme0n1p2");
        f.write("hwmon/hwmon3/temp1_input", "34500\n");
        f.link(&f.path("devices/nvme0"), "hwmon/hwmon3/device");

        let topo = resolve_root_disk_topology_at(
            "/dev/nvme0n1p2",
            &f.path("block"),
            &f.path("hwmon"),
        )
        .unwrap();
        assert_eq!(topo.disk_name, "nvme0n1");
        assert!((topo.physical_capacity_gb.unwrap() - 500.107862016).abs() < 1e-6);
        assert_eq!(
            read_temperature_file(topo.hwmon_temp_path.as_ref().unwrap()),
            Some(34.5)
        );

        // Multi-disk mapper dm-0 -> sda + sdb must be rejected as ambiguous (None)
        f.write("block/sda/size", "1000\n");
        f.write("block/sdb/size", "1000\n");
        f.link(&f.path("block/sda"), "block/dm-0/slaves/sda");
        f.link(&f.path("block/sdb"), "block/dm-0/slaves/sdb");
        assert!(resolve_root_disk_topology_at("/dev/dm-0", &f.path("block"), &f.path("hwmon")).is_none());
    }
}
