use std::fs;

pub struct MemoryInfo {
    pub total_gb: Option<f64>,
    pub used_gb: Option<f64>,
    pub available_gb: Option<f64>,
}

pub fn read() -> MemoryInfo {
    parse(&fs::read_to_string("/proc/meminfo").unwrap_or_default())
}

fn parse(meminfo: &str) -> MemoryInfo {
    let value = |name: &str| -> Option<u64> {
        let text = meminfo.lines().find_map(|l| {
            let (key, value) = l.split_once(':')?;
            (key == name).then_some(value)
        })?;
        text.split_whitespace().next()?.parse().ok()
    };
    let total = value("MemTotal").filter(|v| *v > 0);
    let available = value("MemAvailable").filter(|v| total.is_some_and(|total| *v <= total));
    MemoryInfo {
        total_gb: total.map(|v| v as f64 / 1_048_576.0),
        used_gb: total
            .zip(available)
            .map(|(t, a)| (t - a) as f64 / 1_048_576.0),
        available_gb: available.map(|v| v as f64 / 1_048_576.0),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn missing_available_is_not_reported_as_fully_used() {
        let info = parse("MemTotal: 1048576 kB");
        assert_eq!(info.total_gb, Some(1.0));
        assert_eq!(info.used_gb, None);
        assert_eq!(parse("").total_gb, None);
        assert_eq!(parse("MemTotal: 1 kB\nMemAvailable: 2 kB").used_gb, None);
        assert_eq!(
            parse("MemTotal: 1048576 kB\nMemAvailable: 524288 kB").used_gb,
            Some(0.5)
        );
    }
}
