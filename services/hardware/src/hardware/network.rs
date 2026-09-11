use std::fs;
use std::process::Command;
use std::thread;
use std::time::Duration;

pub struct NetworkInfo {
    pub download_mbps: f64,
    pub upload_mbps: f64,
    pub ping_ms: Option<f64>,
}

pub fn read() -> NetworkInfo {
    let interface = default_interface().unwrap_or_default();
    let rx1 = read_bytes(&interface, "rx_bytes");
    let tx1 = read_bytes(&interface, "tx_bytes");

    thread::sleep(Duration::from_secs(1));

    let rx2 = read_bytes(&interface, "rx_bytes");
    let tx2 = read_bytes(&interface, "tx_bytes");

    let download_mbps = (rx2.saturating_sub(rx1)) as f64 / 1_048_576.0;
    let upload_mbps = (tx2.saturating_sub(tx1)) as f64 / 1_048_576.0;
    let ping_ms = read_ping();

    NetworkInfo {
        download_mbps,
        upload_mbps,
        ping_ms,
    }
}

fn default_interface() -> Option<String> {
    let output = Command::new("ip").args(["route", "show", "default"]).output().ok()?;
    let text = String::from_utf8(output.stdout).ok()?;
    text.lines().find_map(|line| {
        let fields: Vec<&str> = line.split_whitespace().collect();
        fields.iter().position(|field| *field == "dev").and_then(|index| fields.get(index + 1)).map(|value| value.to_string())
    })
}

fn read_bytes(interface: &str, counter: &str) -> u64 {
    if interface.is_empty() {
        return 0;
    }
    fs::read_to_string(format!("/sys/class/net/{interface}/statistics/{counter}"))
        .ok()
        .and_then(|value| value.trim().parse::<u64>().ok())
        .unwrap_or(0)
}

fn read_ping() -> Option<f64> {
    let output = Command::new("ping")
        .env("LC_ALL", "C")
        .args(["-c", "1", "-W", "2", "1.1.1.1"])
        .output()
        .ok()?;
    let text = String::from_utf8_lossy(&output.stdout);
    let value = text.split("time=").nth(1)?.split_whitespace().next()?;
    value.parse::<f64>().ok()
}
