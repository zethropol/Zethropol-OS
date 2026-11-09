use std::fs;
use std::process::Command;
use std::thread;
use std::time::{Duration, Instant};

pub struct NetworkInfo {
    pub interface: Option<String>,
    pub operstate: Option<String>,
    pub download_mbps: Option<f64>,
    pub upload_mbps: Option<f64>,
    pub ping_ms: Option<f64>,
}

pub fn read() -> NetworkInfo {
    let interface = default_interface();
    let name = interface.as_deref().unwrap_or("");
    let operstate = fs::read_to_string(format!("/sys/class/net/{name}/operstate"))
        .ok()
        .map(|v| v.trim().to_string());
    let start = Instant::now();
    let rx1 = read_bytes(name, "rx_bytes");
    let tx1 = read_bytes(name, "tx_bytes");
    thread::sleep(Duration::from_millis(200));
    let rx2 = read_bytes(name, "rx_bytes");
    let tx2 = read_bytes(name, "tx_bytes");
    let seconds = start.elapsed().as_secs_f64();
    let rate = |before: Option<u64>, after: Option<u64>| {
        before
            .zip(after)
            .and_then(|(b, a)| a.checked_sub(b))
            .map(|bytes| bytes as f64 * 8.0 / seconds / 1_000_000.0)
    };
    let ping_ms = interface.as_ref().and_then(|_| read_ping());
    NetworkInfo {
        interface,
        operstate,
        download_mbps: rate(rx1, rx2),
        upload_mbps: rate(tx1, tx2),
        ping_ms,
    }
}

fn default_interface() -> Option<String> {
    let output = Command::new("ip")
        .args(["route", "show", "default"])
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    let text = String::from_utf8(output.stdout).ok()?;
    text.lines().find_map(|line| {
        let fields: Vec<&str> = line.split_whitespace().collect();
        fields
            .iter()
            .position(|field| *field == "dev")
            .and_then(|index| fields.get(index + 1))
            .map(|v| v.to_string())
    })
}

fn read_bytes(interface: &str, counter: &str) -> Option<u64> {
    if interface.is_empty() {
        return None;
    }
    fs::read_to_string(format!("/sys/class/net/{interface}/statistics/{counter}"))
        .ok()?
        .trim()
        .parse()
        .ok()
}

fn read_ping() -> Option<f64> {
    let output = Command::new("ping")
        .env("LC_ALL", "C")
        .args(["-c", "1", "-W", "0.2", "1.1.1.1"])
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&output.stdout);
    text.split("time=")
        .nth(1)?
        .split_whitespace()
        .next()?
        .parse::<f64>()
        .ok()
        .filter(|v| v.is_finite() && *v >= 0.0)
}
