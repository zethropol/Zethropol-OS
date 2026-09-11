use std::fs;

pub struct StorageInfo {
    pub model: String,
    pub firmware: String,
    pub capacity_gb: f64,
    pub used_gb: f64,
    pub available_gb: f64,
    pub temperature_c: Option<f64>,
}

pub fn read() -> Option<StorageInfo> {
    let model = fs::read_to_string("/sys/class/nvme/nvme0/model")
        .ok()?
        .trim()
        .to_string();

    let firmware = fs::read_to_string("/sys/class/nvme/nvme0/firmware_rev")
        .ok()?
        .trim()
        .to_string();

    let sectors = fs::read_to_string("/sys/class/block/nvme0n1/size")
        .ok()?
        .trim()
        .parse::<u64>()
        .ok()?;

    let block_size = fs::read_to_string("/sys/class/block/nvme0n1/queue/logical_block_size")
        .ok()?
        .trim()
        .parse::<u64>()
        .ok()?;

    let capacity_gb = sectors as f64 * block_size as f64 / 1_000_000_000.0;

    let filesystem = fs::read_to_string("/proc/mounts").ok()
        .and_then(|mounts| mounts.lines().find(|line| line.ends_with(" / btrfs rw,relatime,ssd,discard=async,space_cache=v2,subvolid=256,subvol=/@ 0 0"))
        .map(|_| "/".to_string()))
        .unwrap_or_else(|| "/".to_string());

    let df_output = std::process::Command::new("df")
        .args(["-B1", &filesystem])
        .output()
        .ok();

    let (used_gb, available_gb) = df_output
        .and_then(|output| String::from_utf8(output.stdout).ok())
        .and_then(|output| output.lines().nth(1).map(str::to_string))
        .and_then(|line| {
            let fields: Vec<&str> = line.split_whitespace().collect();
            if fields.len() < 5 {
                return None;
            }
            let used = fields[2].parse::<f64>().ok()?;
            let available = fields[3].parse::<f64>().ok()?;
            Some((used / 1_000_000_000.0, available / 1_000_000_000.0))
        })
        .unwrap_or((0.0, 0.0));

    let temperature_c = fs::read_to_string("/sys/class/hwmon/hwmon0/temp1_input")
        .ok()
        .and_then(|value| value.trim().parse::<f64>().ok())
        .map(|millidegrees| millidegrees / 1000.0);

    Some(StorageInfo {
        model,
        firmware,
        capacity_gb,
        used_gb,
        available_gb,
        temperature_c,
    })
}
