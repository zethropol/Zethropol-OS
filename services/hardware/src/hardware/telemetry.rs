use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, Instant};

use serde::Serialize;

use crate::hardware::{cpu, gpu, memory, network, storage};

#[derive(Debug, Clone)]
pub struct TelemetryConfig {
    /// Short coalescing window so parallel `GetTelemetry()` calls share one scan.
    pub coalesce_ttl: Duration,
    /// Temperature sensor cache window; cleared immediately if sensor read fails.
    pub temp_cache_ttl: Duration,
    /// Infrequent topology discovery cache window (primary GPU DRM path, root disk).
    pub topology_cache_ttl: Duration,
    /// Minimum monotonic interval for valid CPU/Network counter deltas.
    pub min_delta: Duration,
    /// Maximum age for a counter baseline before resetting (prevents stale deltas).
    pub max_delta_age: Duration,
}

impl Default for TelemetryConfig {
    fn default() -> Self {
        Self {
            coalesce_ttl: Duration::from_millis(250),
            temp_cache_ttl: Duration::from_secs(2),
            topology_cache_ttl: Duration::from_secs(30),
            min_delta: Duration::from_millis(1),
            max_delta_age: Duration::from_secs(30),
        }
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct CpuTelemetry {
    pub usage_percent: Option<f64>,
    pub temperature_c: Option<f64>,
    pub frequency_ghz: Option<f64>,
}

#[derive(Debug, Clone, Serialize)]
pub struct GpuTelemetry {
    pub detected: bool,
    pub usage_percent: Option<f64>,
    pub memory_usage_percent: Option<f64>,
    pub core_clock_mhz: Option<f64>,
    pub vram_used_gb: Option<f64>,
    pub vram_total_gb: Option<f64>,
    pub temperature_c: Option<f64>,
    pub memory_clock_mhz: Option<f64>,
}

#[derive(Debug, Clone, Serialize)]
pub struct MemoryTelemetry {
    pub used_gb: Option<f64>,
    pub total_gb: Option<f64>,
    pub available_gb: Option<f64>,
}

#[derive(Debug, Clone, Serialize)]
pub struct StorageTelemetry {
    pub detected: bool,
    /// Preserves legacy `normalized_storage.capacity_gb` physical disk capacity (decimal GB).
    pub capacity_gb: Option<f64>,
    /// Explicit physical root disk capacity (decimal GB).
    pub physical_capacity_gb: Option<f64>,
    /// Preserves legacy `normalized_storage.used_gb` root filesystem used (decimal GB).
    pub used_gb: Option<f64>,
    /// Preserves legacy `normalized_storage.available_gb` root filesystem available (decimal GB).
    pub available_gb: Option<f64>,
    /// Explicit root filesystem total size (decimal GB).
    pub filesystem_total_gb: Option<f64>,
    /// Explicit root filesystem used size (`total - free`, decimal GB).
    pub filesystem_used_gb: Option<f64>,
    /// Explicit root filesystem raw free size (including Btrfs/ext4 reserved space, decimal GB).
    pub filesystem_free_gb: Option<f64>,
    /// Explicit root filesystem user-available size (decimal GB).
    pub filesystem_available_gb: Option<f64>,
    /// User-accessible occupancy percentage: `used / (used + available) * 100`.
    pub filesystem_usage_percent: Option<f64>,
    /// Root filesystem type (e.g. `btrfs`, `ext4`).
    pub filesystem_type: Option<String>,
    /// Active Btrfs subvolume when applicable (e.g. `/@`).
    pub filesystem_subvolume: Option<String>,
    pub temperature_c: Option<f64>,
}

#[derive(Debug, Clone, Serialize)]
pub struct NetworkTelemetry {
    pub interface: Option<String>,
    pub operstate: Option<String>,
    pub download_mbps: Option<f64>,
    pub upload_mbps: Option<f64>,
    /// Always `None` in default telemetry (zero outbound ping/network traffic).
    pub ping_ms: Option<f64>,
}

#[derive(Debug, Clone, Serialize)]
pub struct NormalizedTelemetryState {
    pub cpu: CpuTelemetry,
    pub gpu: GpuTelemetry,
    pub memory: MemoryTelemetry,
    pub normalized_storage: StorageTelemetry,
    pub network: NetworkTelemetry,
}

#[derive(Debug, Default)]
pub struct CpuCounterTracker {
    previous: Option<((u64, u64), Instant)>,
}

impl CpuCounterTracker {
    pub fn update(
        &mut self,
        current: Option<(u64, u64)>,
        now: Instant,
        min_delta: Duration,
        max_delta_age: Duration,
    ) -> Option<f64> {
        let current = match current {
            Some(c) => c,
            None => {
                self.previous = None;
                return None;
            }
        };

        let result = match self.previous {
            None => None,
            Some((prev, prev_at)) => {
                let elapsed = now.checked_duration_since(prev_at);
                match elapsed {
                    Some(dt) if dt >= min_delta && dt <= max_delta_age => {
                        cpu::compute_usage_percent_delta(prev, current)
                    }
                    _ => None,
                }
            }
        };

        self.previous = Some((current, now));
        result
    }
}

#[derive(Debug, Clone)]
pub struct RawNetworkSample {
    pub interface: Option<String>,
    pub operstate: Option<String>,
    pub counters: Option<(u64, u64)>,
}

#[derive(Debug)]
struct PrevNetCounters {
    interface: String,
    rx_bytes: u64,
    tx_bytes: u64,
    captured_at: Instant,
}

#[derive(Debug, Default)]
pub struct NetworkCounterTracker {
    previous: Option<PrevNetCounters>,
}

impl NetworkCounterTracker {
    pub fn update(
        &mut self,
        sample: &RawNetworkSample,
        now: Instant,
        min_delta: Duration,
        max_delta_age: Duration,
    ) -> (Option<f64>, Option<f64>) {
        let Some(interface) = sample.interface.as_deref() else {
            self.previous = None;
            return (None, None);
        };

        if !network::is_interface_link_up(sample.operstate.as_deref()) {
            self.previous = None;
            return (None, None);
        }

        let Some((rx_now, tx_now)) = sample.counters else {
            self.previous = None;
            return (None, None);
        };

        let rates = match &self.previous {
            Some(prev) if prev.interface == interface => match now.checked_duration_since(prev.captured_at) {
                Some(dt) if dt >= min_delta && dt <= max_delta_age => {
                    let secs = dt.as_secs_f64();
                    (
                        network::compute_mbps(prev.rx_bytes, rx_now, secs),
                        network::compute_mbps(prev.tx_bytes, tx_now, secs),
                    )
                }
                _ => (None, None),
            },
            _ => (None, None),
        };

        self.previous = Some(PrevNetCounters {
            interface: interface.to_string(),
            rx_bytes: rx_now,
            tx_bytes: tx_now,
            captured_at: now,
        });

        rates
    }
}

#[derive(Debug, Default)]
pub struct TemperatureCache {
    value: Option<(f64, Instant)>,
}

impl TemperatureCache {
    pub fn read_or_refresh<F>(&mut self, now: Instant, ttl: Duration, reader: F) -> Option<f64>
    where
        F: FnOnce() -> Option<f64>,
    {
        if let Some((cached, captured_at)) = self.value
            && now.checked_duration_since(captured_at).is_some_and(|dt| dt <= ttl)
        {
            return Some(cached);
        }

        match reader().filter(|v| v.is_finite()) {
            Some(fresh) => {
                let normalized = if fresh == 0.0 { 0.0 } else { fresh };
                self.value = Some((normalized, now));
                Some(normalized)
            }
            None => {
                // Never return a stale temperature when a sensor becomes unavailable.
                self.value = None;
                None
            }
        }
    }

    pub fn invalidate(&mut self) {
        self.value = None;
    }
}

#[derive(Debug, Clone)]
struct DiscoveredTopology {
    gpu_device_path: Option<PathBuf>,
    gpu_temp_path: Option<PathBuf>,
    root_mount: Option<storage::RootMountInfo>,
    root_disk: Option<storage::RootDiskTopology>,
    discovered_at: Instant,
}

#[derive(Debug, Default)]
struct EngineState {
    cpu_tracker: CpuCounterTracker,
    network_tracker: NetworkCounterTracker,
    cpu_temp_cache: TemperatureCache,
    gpu_temp_cache: TemperatureCache,
    storage_temp_cache: TemperatureCache,
    topology: Option<DiscoveredTopology>,
    last_snapshot: Option<(NormalizedTelemetryState, Instant)>,
}

impl EngineState {
    fn ensure_topology(&mut self, now: Instant, ttl: Duration) -> &DiscoveredTopology {
        let needs_refresh = match &self.topology {
            None => true,
            Some(topo) => now
                .checked_duration_since(topo.discovered_at)
                .is_none_or(|dt| dt > ttl),
        };

        if needs_refresh {
            let gpu_device_path = gpu::discover_primary_gpu_sysfs();
            let gpu_temp_path = gpu_device_path
                .as_deref()
                .and_then(gpu::find_temperature_sensor_path);
            let root_mount = storage::read_root_mountinfo();
            let root_disk = root_mount
                .as_ref()
                .and_then(|m| storage::discover_root_disk_topology(&m.source));

            self.topology = Some(DiscoveredTopology {
                gpu_device_path,
                gpu_temp_path,
                root_mount,
                root_disk,
                discovered_at: now,
            });
        }

        self.topology.as_ref().expect("topology initialized")
    }

    fn collect_live(&mut self, config: &TelemetryConfig, now: Instant) -> NormalizedTelemetryState {
        if let Some((snapshot, captured_at)) = &self.last_snapshot
            && now
                .checked_duration_since(*captured_at)
                .is_some_and(|dt| dt <= config.coalesce_ttl)
        {
            return snapshot.clone();
        }

        let topo = self.ensure_topology(now, config.topology_cache_ttl).clone();

        // CPU (zero sleep, zero external processes)
        let cpu_times = cpu::read_cpu_times();
        let cpu_usage = self
            .cpu_tracker
            .update(cpu_times, now, config.min_delta, config.max_delta_age);
        let cpu_temp = self
            .cpu_temp_cache
            .read_or_refresh(now, config.temp_cache_ttl, cpu::read_temperature);
        let cpu_freq = cpu::read_average_frequency_ghz();

        // GPU (zero pci.ids parsing, cached DRM sysfs path; auto re-probe if path disappeared)
        let mut gpu_path = topo.gpu_device_path.clone();
        let mut gpu_temp_path = topo.gpu_temp_path.clone();
        let gpu_sample = match gpu_path.as_deref().and_then(gpu::read_telemetry_at) {
            Some(sample) => Some(sample),
            None => {
                // Re-probe if GPU appeared or device path changed.
                let fresh_path = gpu::discover_primary_gpu_sysfs();
                let fresh_temp = fresh_path
                    .as_deref()
                    .and_then(gpu::find_temperature_sensor_path);
                if fresh_path != gpu_path {
                    self.gpu_temp_cache.invalidate();
                    if let Some(ref mut cached_topo) = self.topology {
                        cached_topo.gpu_device_path = fresh_path.clone();
                        cached_topo.gpu_temp_path = fresh_temp.clone();
                    }
                }
                gpu_path = fresh_path;
                gpu_temp_path = fresh_temp;
                gpu_path.as_deref().and_then(gpu::read_telemetry_at)
            }
        };

        let gpu_temp = match gpu_temp_path.as_deref() {
            Some(path) => self
                .gpu_temp_cache
                .read_or_refresh(now, config.temp_cache_ttl, || gpu::read_temperature_file(path)),
            None => {
                self.gpu_temp_cache.invalidate();
                None
            }
        };

        let gpu_telemetry = match gpu_sample {
            Some(sample) => GpuTelemetry {
                detected: true,
                usage_percent: sample.usage_percent,
                memory_usage_percent: sample.memory_usage_percent,
                core_clock_mhz: sample.core_clock_mhz,
                vram_used_gb: sample.vram_used_gb,
                vram_total_gb: sample.vram_total_gb,
                temperature_c: gpu_temp,
                memory_clock_mhz: sample.memory_clock_mhz,
            },
            None => GpuTelemetry {
                detected: false,
                usage_percent: None,
                memory_usage_percent: None,
                core_clock_mhz: None,
                vram_used_gb: None,
                vram_total_gb: None,
                temperature_c: None,
                memory_clock_mhz: None,
            },
        };

        // Memory (zero external processes, /proc/meminfo)
        let mem = memory::read();
        let memory_telemetry = MemoryTelemetry {
            used_gb: mem.used_gb,
            total_gb: mem.total_gb,
            available_gb: mem.available_gb,
        };

        // Storage (zero external processes: statvfs("/") + cached sysfs physical disk & hwmon temp)
        let fs_usage = storage::read_root_filesystem_usage();
        let disk_temp_path = topo
            .root_disk
            .as_ref()
            .and_then(|disk| disk.hwmon_temp_path.as_deref());
        let storage_temp = match disk_temp_path {
            Some(path) => self.storage_temp_cache.read_or_refresh(
                now,
                config.temp_cache_ttl,
                || storage::read_temperature_file(path),
            ),
            None => {
                self.storage_temp_cache.invalidate();
                None
            }
        };

        let physical_capacity_gb = topo
            .root_disk
            .as_ref()
            .and_then(|disk| disk.physical_capacity_gb);
        let storage_detected = topo.root_disk.is_some() || fs_usage.is_some();

        let storage_telemetry = StorageTelemetry {
            detected: storage_detected,
            capacity_gb: physical_capacity_gb,
            physical_capacity_gb,
            used_gb: fs_usage.map(|u| u.used_gb()),
            available_gb: fs_usage.map(|u| u.available_gb()),
            filesystem_total_gb: fs_usage.map(|u| u.total_gb()),
            filesystem_used_gb: fs_usage.map(|u| u.used_gb()),
            filesystem_free_gb: fs_usage.map(|u| u.free_gb()),
            filesystem_available_gb: fs_usage.map(|u| u.available_gb()),
            filesystem_usage_percent: fs_usage.and_then(|u| u.usage_percent()),
            filesystem_type: topo.root_mount.as_ref().map(|m| m.fs_type.clone()),
            filesystem_subvolume: topo.root_mount.as_ref().and_then(|m| m.subvolume.clone()),
            temperature_c: storage_temp,
        };

        // Network (zero sleep, zero external processes, zero outbound ping)
        let iface = network::default_interface();
        let operstate = iface.as_deref().and_then(network::read_operstate);
        let counters = iface.as_deref().and_then(network::read_counters);
        let raw_net = RawNetworkSample {
            interface: iface.clone(),
            operstate: operstate.clone(),
            counters,
        };
        let (download_mbps, upload_mbps) =
            self.network_tracker
                .update(&raw_net, now, config.min_delta, config.max_delta_age);
        let network_telemetry = NetworkTelemetry {
            interface: iface,
            operstate,
            download_mbps,
            upload_mbps,
            ping_ms: None,
        };

        let snapshot = NormalizedTelemetryState {
            cpu: CpuTelemetry {
                usage_percent: cpu_usage,
                temperature_c: cpu_temp,
                frequency_ghz: cpu_freq,
            },
            gpu: gpu_telemetry,
            memory: memory_telemetry,
            normalized_storage: storage_telemetry,
            network: network_telemetry,
        };

        self.last_snapshot = Some((snapshot.clone(), now));
        snapshot
    }
}

pub struct TelemetryEngine {
    config: TelemetryConfig,
    state: Arc<tokio::sync::Mutex<EngineState>>,
    scan_count: Arc<AtomicUsize>,
}

impl TelemetryEngine {
    pub fn new(config: TelemetryConfig) -> Self {
        Self {
            config,
            state: Arc::new(tokio::sync::Mutex::new(EngineState::default())),
            scan_count: Arc::new(AtomicUsize::new(0)),
        }
    }

    #[cfg(test)]
    pub fn scan_count(&self) -> usize {
        self.scan_count.load(Ordering::SeqCst)
    }

    /// Collects or returns a freshly coalesced lightweight telemetry snapshot.
    /// Serialized under an async mutex so concurrent `GetTelemetry()` calls
    /// never trigger duplicate hardware scans, while remaining completely
    /// independent from slow `GetState()` inventory reads.
    pub async fn sample(&self) -> Result<NormalizedTelemetryState, String> {
        let mut guard = self.state.lock().await;
        let now = Instant::now();

        if let Some((snapshot, captured_at)) = &guard.last_snapshot
            && now
                .checked_duration_since(*captured_at)
                .is_some_and(|dt| dt <= self.config.coalesce_ttl)
        {
            return Ok(snapshot.clone());
        }

        // Perform the fast sysfs/procfs collection while holding the engine
        // mutex so concurrent callers wait and reuse the single fresh snapshot.
        let snapshot = guard.collect_live(&self.config, now);
        self.scan_count.fetch_add(1, Ordering::SeqCst);
        Ok(snapshot)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    #[test]
    fn cpu_tracker_first_sample_is_none_then_computes_delta_real_zero_and_reset() {
        let mut tracker = CpuCounterTracker::default();
        let t0 = Instant::now();
        let min_d = Duration::from_millis(1);
        let max_d = Duration::from_secs(30);

        // First sample must not fake 0.0%
        assert_eq!(tracker.update(Some((1000, 2000)), t0, min_d, max_d), None);

        // Normal 25% busy delta
        let t1 = t0 + Duration::from_millis(500);
        let pct = tracker.update(Some((1750, 3000)), t1, min_d, max_d).unwrap();
        assert!((pct - 25.0).abs() < 1e-9);

        // Real 0.0% busy delta (100% idle over valid interval)
        let t2 = t1 + Duration::from_millis(500);
        assert_eq!(tracker.update(Some((2750, 4000)), t2, min_d, max_d), Some(0.0));

        // Counter reset -> None (re-baselines cleanly)
        let t3 = t2 + Duration::from_millis(500);
        assert_eq!(tracker.update(Some((100, 200)), t3, min_d, max_d), None);

        // Zero / sub-min elapsed time -> None
        assert_eq!(tracker.update(Some((150, 300)), t3, min_d, max_d), None);

        // Stale delta (> max_delta_age) -> None
        let t4 = t3 + Duration::from_secs(45);
        assert_eq!(tracker.update(Some((200, 400)), t4, min_d, max_d), None);

        // Recovery right after re-baseline -> valid 50%
        let t5 = t4 + Duration::from_secs(1);
        assert_eq!(tracker.update(Some((250, 500)), t5, min_d, max_d), Some(50.0));
    }

    #[test]
    fn network_tracker_handles_first_sample_mbps_zero_switch_disconnect_and_reset() {
        let mut tracker = NetworkCounterTracker::default();
        let t0 = Instant::now();
        let min_d = Duration::from_millis(1);
        let max_d = Duration::from_secs(30);

        let sample = |iface: &str, state: &str, rx: u64, tx: u64| RawNetworkSample {
            interface: Some(iface.to_string()),
            operstate: Some(state.to_string()),
            counters: Some((rx, tx)),
        };

        // First sample -> (None, None)
        assert_eq!(
            tracker.update(&sample("enp34s0", "up", 10_000, 5_000), t0, min_d, max_d),
            (None, None)
        );

        // Normal 1.0s delta: rx +1_250_000 B = 10.0 Mbps, tx +0 B = 0.0 Mbps (real zero preserved)
        let t1 = t0 + Duration::from_secs(1);
        let (dl, ul) = tracker.update(
            &sample("enp34s0", "up", 1_260_000, 5_000),
            t1,
            min_d,
            max_d,
        );
        assert!((dl.unwrap() - 10.0).abs() < 1e-9);
        assert_eq!(ul, Some(0.0));

        // Interface switch -> resets baseline, returns (None, None)
        let t2 = t1 + Duration::from_secs(1);
        assert_eq!(
            tracker.update(&sample("wlan0", "up", 100, 200), t2, min_d, max_d),
            (None, None)
        );

        // Counter reset on wlan0 -> None for reset counter
        let t3 = t2 + Duration::from_secs(1);
        assert_eq!(
            tracker.update(&sample("wlan0", "up", 50, 125_200), t3, min_d, max_d),
            (None, Some(1.0))
        );

        // Link disconnect -> clears baseline and returns (None, None)
        let t4 = t3 + Duration::from_secs(1);
        assert_eq!(
            tracker.update(&sample("wlan0", "down", 50, 125_200), t4, min_d, max_d),
            (None, None)
        );

        // Reconnect after disconnect -> first sample again (None, None)
        let t5 = t4 + Duration::from_secs(1);
        assert_eq!(
            tracker.update(&sample("wlan0", "up", 50, 125_200), t5, min_d, max_d),
            (None, None)
        );
    }

    #[test]
    fn temperature_cache_reuses_within_ttl_refreshes_after_ttl_and_clears_on_missing_sensor() {
        let mut cache = TemperatureCache::default();
        let ttl = Duration::from_secs(2);
        let t0 = Instant::now();
        let reads = Cell::new(0usize);

        let v1 = cache.read_or_refresh(t0, ttl, || {
            reads.set(reads.get() + 1);
            Some(42.0)
        });
        assert_eq!(v1, Some(42.0));
        assert_eq!(reads.get(), 1);

        // Within TTL -> returns cached value without invoking reader
        let v2 = cache.read_or_refresh(t0 + Duration::from_secs(1), ttl, || {
            reads.set(reads.get() + 1);
            Some(50.0)
        });
        assert_eq!(v2, Some(42.0));
        assert_eq!(reads.get(), 1);

        // Past TTL -> refreshes value
        let v3 = cache.read_or_refresh(t0 + Duration::from_secs(3), ttl, || {
            reads.set(reads.get() + 1);
            Some(48.5)
        });
        assert_eq!(v3, Some(48.5));
        assert_eq!(reads.get(), 2);

        // Sensor becomes unavailable after TTL -> clears cache immediately (no stale temp!)
        let v4 = cache.read_or_refresh(t0 + Duration::from_secs(6), ttl, || {
            reads.set(reads.get() + 1);
            None
        });
        assert_eq!(v4, None);
        assert_eq!(reads.get(), 3);

        // Real 0.0 °C sensor value is preserved
        let v5 = cache.read_or_refresh(t0 + Duration::from_secs(7), ttl, || {
            reads.set(reads.get() + 1);
            Some(-0.0)
        });
        assert_eq!(v5.unwrap().to_bits(), 0.0_f64.to_bits());
    }
}
