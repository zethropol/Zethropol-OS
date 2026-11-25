use std::fs;
use std::path::Path;
use std::thread;
use std::time::{Duration, Instant};

pub struct NetworkInfo {
    pub interface: Option<String>,
    pub operstate: Option<String>,
    pub download_mbps: Option<f64>,
    pub upload_mbps: Option<f64>,
    pub ping_ms: Option<f64>,
}

/// Default inventory read. Deliberately performs zero external ICMP ping or
/// outbound network traffic; `ping_ms` remains `None` unless an explicit
/// user-initiated connectivity test exists in a future stage.
pub fn read() -> NetworkInfo {
    let interface = default_interface();
    let name = interface.as_deref().unwrap_or("");
    let operstate = read_operstate(name);
    let link_up = is_interface_link_up(operstate.as_deref());

    let (download_mbps, upload_mbps) = if !name.is_empty() && link_up {
        let start = Instant::now();
        let before = read_counters(name);
        thread::sleep(Duration::from_millis(200));
        let after = read_counters(name);
        let seconds = start.elapsed().as_secs_f64();
        match (before, after) {
            (Some((rx1, tx1)), Some((rx2, tx2))) => (
                compute_mbps(rx1, rx2, seconds),
                compute_mbps(tx1, tx2, seconds),
            ),
            _ => (None, None),
        }
    } else {
        (None, None)
    };

    NetworkInfo {
        interface,
        operstate,
        download_mbps,
        upload_mbps,
        ping_ms: None,
    }
}

pub(crate) fn default_interface() -> Option<String> {
    default_interface_at(
        Path::new("/proc/net/route"),
        Path::new("/proc/net/ipv6_route"),
    )
}

pub(crate) fn default_interface_at(ipv4_route: &Path, ipv6_route: &Path) -> Option<String> {
    if let Ok(content) = fs::read_to_string(ipv4_route)
        && let Some(iface) = parse_default_interface_ipv4(&content)
    {
        return Some(iface);
    }

    if let Ok(content) = fs::read_to_string(ipv6_route)
        && let Some(iface) = parse_default_interface_ipv6(&content)
    {
        return Some(iface);
    }

    None
}

pub(crate) fn parse_default_interface_ipv4(content: &str) -> Option<String> {
    let mut best: Option<(u32, String)> = None;
    for line in content.lines().skip(1) {
        let fields: Vec<&str> = line.split_whitespace().collect();
        if fields.len() < 8 {
            continue;
        }
        let iface = fields[0];
        let destination = fields[1];
        let Ok(flags) = u32::from_str_radix(fields[3], 16) else {
            continue;
        };
        let Ok(metric) = fields[6].parse::<u32>() else {
            continue;
        };
        let mask = fields[7];

        // RTF_UP = 0x1; default route has 0.0.0.0 destination and 0.0.0.0 mask.
        if iface.is_empty()
            || iface == "lo"
            || destination != "00000000"
            || mask != "00000000"
            || (flags & 0x1) == 0
        {
            continue;
        }

        let candidate = (metric, iface.to_string());
        if best.as_ref().is_none_or(|current| candidate < *current) {
            best = Some(candidate);
        }
    }
    best.map(|(_, iface)| iface)
}

pub(crate) fn parse_default_interface_ipv6(content: &str) -> Option<String> {
    let mut best: Option<(u32, String)> = None;
    for line in content.lines() {
        let fields: Vec<&str> = line.split_whitespace().collect();
        if fields.len() < 10 {
            continue;
        }
        let destination = fields[0];
        let dest_prefix = fields[1];
        let metric = match u32::from_str_radix(fields[5], 16) {
            Ok(v) => v,
            Err(_) => continue,
        };
        let flags = match u32::from_str_radix(fields[8], 16) {
            Ok(v) => v,
            Err(_) => continue,
        };
        let iface = fields[9];

        if iface.is_empty()
            || iface == "lo"
            || destination != "00000000000000000000000000000000"
            || dest_prefix != "00"
            || (flags & 0x1) == 0
        {
            continue;
        }

        let candidate = (metric, iface.to_string());
        if best.as_ref().is_none_or(|current| candidate < *current) {
            best = Some(candidate);
        }
    }
    best.map(|(_, iface)| iface)
}

pub(crate) fn is_interface_link_up(operstate: Option<&str>) -> bool {
    match operstate {
        Some(state) => !matches!(
            state.trim().to_ascii_lowercase().as_str(),
            "down" | "notpresent" | "lowerlayerdown" | "dormant"
        ),
        None => false,
    }
}

pub(crate) fn read_operstate(interface: &str) -> Option<String> {
    read_operstate_at(Path::new("/sys/class/net"), interface)
}

pub(crate) fn read_operstate_at(net_root: &Path, interface: &str) -> Option<String> {
    if interface.is_empty() {
        return None;
    }
    fs::read_to_string(net_root.join(interface).join("operstate"))
        .ok()
        .map(|v| v.trim().to_string())
        .filter(|v| !v.is_empty())
}

pub(crate) fn read_counters(interface: &str) -> Option<(u64, u64)> {
    read_counters_at(Path::new("/sys/class/net"), interface)
}

pub(crate) fn read_counters_at(net_root: &Path, interface: &str) -> Option<(u64, u64)> {
    if interface.is_empty() {
        return None;
    }
    let rx = read_bytes_at(net_root, interface, "rx_bytes")?;
    let tx = read_bytes_at(net_root, interface, "tx_bytes")?;
    Some((rx, tx))
}

fn read_bytes_at(net_root: &Path, interface: &str, counter: &str) -> Option<u64> {
    fs::read_to_string(net_root.join(interface).join("statistics").join(counter))
        .ok()?
        .trim()
        .parse()
        .ok()
}

pub(crate) fn compute_mbps(before: u64, after: u64, elapsed_secs: f64) -> Option<f64> {
    if !elapsed_secs.is_finite() || elapsed_secs <= 0.0 {
        return None;
    }
    let delta_bytes = after.checked_sub(before)?;
    let mbps = (delta_bytes as f64) * 8.0 / elapsed_secs / 1_000_000.0;
    if !mbps.is_finite() || mbps < 0.0 {
        return None;
    }
    Some(if mbps == 0.0 { 0.0 } else { mbps })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ipv4_route_selects_lowest_metric_up_default_interface() {
        let route = "\
Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT
wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0
enp34s0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0
eth1\t00000000\t0101A8C0\t0000\t0\t0\t10\t00000000\t0\t0\t0
enp34s0\t0001A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0
";
        assert_eq!(parse_default_interface_ipv4(route), Some("enp34s0".to_string()));
    }

    #[test]
    fn ipv6_route_fallback_works_when_no_ipv4_default_route() {
        let f = crate::hardware::test_support::Fixture::new();
        f.write(
            "route",
            "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n",
        );
        f.write(
            "ipv6_route",
            "00000000000000000000000000000000 00 00000000000000000000000000000000 00 fe800000000000000000000000000001 00000064 00000001 00000000 00000003 wlan0\n",
        );
        assert_eq!(
            default_interface_at(&f.path("route"), &f.path("ipv6_route")),
            Some("wlan0".to_string())
        );
    }

    #[test]
    fn mbps_calculation_accuracy_real_zero_and_counter_reset() {
        // 125_000 bytes in 1.0s = 1_000_000 bits/s = 1.0 Mbps (decimal)
        assert_eq!(compute_mbps(1000, 126_000, 1.0), Some(1.0));
        // 1_250_000 bytes in 0.5s = 20.0 Mbps
        assert_eq!(compute_mbps(0, 1_250_000, 0.5), Some(20.0));
        // Real zero traffic on active link
        assert_eq!(compute_mbps(5000, 5000, 1.0), Some(0.0));
        // Counter reset / wrap -> None
        assert_eq!(compute_mbps(5000, 100, 1.0), None);
        // Zero / invalid elapsed -> None
        assert_eq!(compute_mbps(0, 1000, 0.0), None);
        assert_eq!(compute_mbps(0, 1000, f64::NAN), None);
    }

    #[test]
    fn ipv4_route_skips_malformed_lines_and_ipv6_selects_lowest_metric() {
        let route_with_bad_row = "\
Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT
bad0\t00000000\t00000000\tZZZZ\t0\t0\t10\t00000000\t0\t0\t0
wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0
enp34s0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0
";
        assert_eq!(
            parse_default_interface_ipv4(route_with_bad_row),
            Some("enp34s0".to_string())
        );

        let ipv6_multi = "\
00000000000000000000000000000000 00 00000000000000000000000000000000 00 fe800000000000000000000000000001 00000258 00000001 00000000 00000003 wlan0
00000000000000000000000000000000 00 00000000000000000000000000000000 00 fe800000000000000000000000000001 00000064 00000001 00000000 00000003 enp34s0
00000000000000000000000000000000 00 00000000000000000000000000000000 00 fe800000000000000000000000000001 0000000a 00000001 00000000 00000000 eth1
";
        assert_eq!(
            parse_default_interface_ipv6(ipv6_multi),
            Some("enp34s0".to_string())
        );
        assert_eq!(parse_default_interface_ipv4("Iface\tDestination\n"), None);
        assert_eq!(parse_default_interface_ipv6(""), None);
    }

    #[test]
    fn default_read_never_populates_external_ping() {
        let info = read();
        assert_eq!(info.ping_ms, None);
    }
}
