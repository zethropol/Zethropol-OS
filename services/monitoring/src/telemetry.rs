use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::time::{Duration, Instant};

use zbus::zvariant::{OwnedValue, Str};

pub type MonitoringMap = HashMap<String, OwnedValue>;

pub const STALE_TELEMETRY_MESSAGE: &str =
    "Monitoring telemetry is stale; waiting for a fresh hardware sample.";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MetricKind {
    Percent,
    TemperatureC,
    NonNegative,
    Positive,
}

#[derive(Debug, Clone, Copy)]
pub struct MetricSpec {
    pub section: &'static str,
    pub source_key: &'static str,
    pub output_key: &'static str,
    pub kind: MetricKind,
}

/// 25 supported normalized monitoring metrics (19 canonical v1 metrics +
/// 6 explicit filesystem/physical storage metrics from `Hardware.v1.GetTelemetry`).
///
/// Unit contract:
/// - CPU frequency: `GHz`
/// - Memory (`memory_*_gb`) and GPU VRAM (`gpu_vram_*_gb`): binary `GiB` (legacy `_gb` keys preserved)
/// - Storage (`storage_*_gb`): decimal `GB` (`10^9` bytes)
/// - Network throughput: decimal `Mbps`
/// - Percentages: `0.0..=100.0` `%`
/// - Temperatures: `-100.0..=250.0` `°C`
pub const METRIC_SPECS: [MetricSpec; 25] = [
    // CPU (3)
    MetricSpec {
        section: "cpu",
        source_key: "usage_percent",
        output_key: "cpu_usage_percent",
        kind: MetricKind::Percent,
    },
    MetricSpec {
        section: "cpu",
        source_key: "temperature_c",
        output_key: "cpu_temperature_c",
        kind: MetricKind::TemperatureC,
    },
    MetricSpec {
        section: "cpu",
        source_key: "frequency_ghz",
        output_key: "cpu_frequency_ghz",
        kind: MetricKind::NonNegative,
    },
    // GPU (7)
    MetricSpec {
        section: "gpu",
        source_key: "usage_percent",
        output_key: "gpu_usage_percent",
        kind: MetricKind::Percent,
    },
    MetricSpec {
        section: "gpu",
        source_key: "memory_usage_percent",
        output_key: "gpu_memory_usage_percent",
        kind: MetricKind::Percent,
    },
    MetricSpec {
        section: "gpu",
        source_key: "core_clock_mhz",
        output_key: "gpu_core_clock_mhz",
        kind: MetricKind::NonNegative,
    },
    MetricSpec {
        section: "gpu",
        source_key: "vram_used_gb",
        output_key: "gpu_vram_used_gb",
        kind: MetricKind::NonNegative,
    },
    MetricSpec {
        section: "gpu",
        source_key: "vram_total_gb",
        output_key: "gpu_vram_total_gb",
        kind: MetricKind::Positive,
    },
    MetricSpec {
        section: "gpu",
        source_key: "temperature_c",
        output_key: "gpu_temperature_c",
        kind: MetricKind::TemperatureC,
    },
    MetricSpec {
        section: "gpu",
        source_key: "memory_clock_mhz",
        output_key: "gpu_memory_clock_mhz",
        kind: MetricKind::NonNegative,
    },
    // Memory (2) — GiB values under legacy `_gb` keys
    MetricSpec {
        section: "memory",
        source_key: "used_gb",
        output_key: "memory_used_gb",
        kind: MetricKind::NonNegative,
    },
    MetricSpec {
        section: "memory",
        source_key: "total_gb",
        output_key: "memory_total_gb",
        kind: MetricKind::Positive,
    },
    // Storage (4 canonical + 6 explicit filesystem/physical metrics = 10, decimal GB & %)
    MetricSpec {
        section: "normalized_storage",
        source_key: "used_gb",
        output_key: "storage_used_gb",
        kind: MetricKind::NonNegative,
    },
    MetricSpec {
        section: "normalized_storage",
        source_key: "capacity_gb",
        output_key: "storage_capacity_gb",
        kind: MetricKind::Positive,
    },
    MetricSpec {
        section: "normalized_storage",
        source_key: "available_gb",
        output_key: "storage_available_gb",
        kind: MetricKind::NonNegative,
    },
    MetricSpec {
        section: "normalized_storage",
        source_key: "temperature_c",
        output_key: "storage_temperature_c",
        kind: MetricKind::TemperatureC,
    },
    MetricSpec {
        section: "normalized_storage",
        source_key: "filesystem_total_gb",
        output_key: "storage_filesystem_total_gb",
        kind: MetricKind::Positive,
    },
    MetricSpec {
        section: "normalized_storage",
        source_key: "filesystem_used_gb",
        output_key: "storage_filesystem_used_gb",
        kind: MetricKind::NonNegative,
    },
    MetricSpec {
        section: "normalized_storage",
        source_key: "filesystem_free_gb",
        output_key: "storage_filesystem_free_gb",
        kind: MetricKind::NonNegative,
    },
    MetricSpec {
        section: "normalized_storage",
        source_key: "filesystem_available_gb",
        output_key: "storage_filesystem_available_gb",
        kind: MetricKind::NonNegative,
    },
    MetricSpec {
        section: "normalized_storage",
        source_key: "filesystem_usage_percent",
        output_key: "storage_filesystem_usage_percent",
        kind: MetricKind::Percent,
    },
    MetricSpec {
        section: "normalized_storage",
        source_key: "physical_capacity_gb",
        output_key: "storage_physical_capacity_gb",
        kind: MetricKind::Positive,
    },
    // Network (3) — decimal Mbps & ms
    MetricSpec {
        section: "network",
        source_key: "download_mbps",
        output_key: "network_download_mbps",
        kind: MetricKind::NonNegative,
    },
    MetricSpec {
        section: "network",
        source_key: "upload_mbps",
        output_key: "network_upload_mbps",
        kind: MetricKind::NonNegative,
    },
    MetricSpec {
        section: "network",
        source_key: "ping_ms",
        output_key: "network_ping_ms",
        kind: MetricKind::NonNegative,
    },
];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReadinessState {
    Initializing,
    Ready,
    Degraded,
    DependencyFailed,
}

impl ReadinessState {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Initializing => "initializing",
            Self::Ready => "ready",
            Self::Degraded => "degraded",
            Self::DependencyFailed => "dependency-failed",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SampleErrorKind {
    Unavailable,
    Timeout,
    Incompatible,
    Failed,
}

impl SampleErrorKind {
    pub const fn result_code(self) -> &'static str {
        match self {
            Self::Unavailable => "not-available",
            Self::Timeout => "timeout",
            Self::Incompatible => "unsupported",
            Self::Failed => "failed",
        }
    }
}

/// Hardware presence and link capability signals parsed from `Hardware.v1.GetTelemetry`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HardwareCapabilities {
    pub gpu_detected: bool,
    pub storage_detected: bool,
    pub network_link_up: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ExtractedTelemetry {
    pub metrics: BTreeMap<String, f64>,
    pub capabilities: HardwareCapabilities,
}

impl ExtractedTelemetry {
    /// Returns true if all mandatory measurements for currently present/active
    /// subsystems exist in this sample.
    ///
    /// - CPU: `cpu_usage_percent` is mandatory (first sample before `/proc/stat`
    ///   delta warms up is therefore not `Ready`).
    /// - Memory: `memory_used_gb` and `memory_total_gb` are mandatory.
    /// - Storage (when `storage_detected`): root filesystem usage
    ///   (`storage_filesystem_used_gb` + `storage_filesystem_total_gb`, or legacy
    ///   `storage_used_gb` + `storage_available_gb`) is mandatory.
    /// - Network (when `network_link_up`): `network_download_mbps` and
    ///   `network_upload_mbps` are mandatory (first sample after link up warms up
    ///   as `Degraded`). When offline (`!network_link_up`), absent rates are normal.
    /// - Default absent `network_ping_ms`, GPU-less systems (`!gpu_detected`), and
    ///   unsupported optional sensors never prevent `Ready`.
    pub fn has_mandatory_metrics(&self) -> bool {
        let has_cpu = self.metrics.contains_key("cpu_usage_percent");
        let has_memory = self.metrics.contains_key("memory_used_gb")
            && self.metrics.contains_key("memory_total_gb");

        let has_storage = if self.capabilities.storage_detected {
            (self.metrics.contains_key("storage_filesystem_used_gb")
                && self.metrics.contains_key("storage_filesystem_total_gb"))
                || (self.metrics.contains_key("storage_used_gb")
                    && self.metrics.contains_key("storage_available_gb"))
        } else {
            true
        };

        let has_network = if self.capabilities.network_link_up {
            self.metrics.contains_key("network_download_mbps")
                && self.metrics.contains_key("network_upload_mbps")
        } else {
            true
        };

        has_cpu && has_memory && has_storage && has_network
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SampleError {
    pub kind: SampleErrorKind,
    pub message: String,
}

impl SampleError {
    pub fn new(kind: SampleErrorKind, message: impl Into<String>) -> Self {
        Self {
            kind,
            message: message.into(),
        }
    }
}

fn validate_metric(raw: f64, kind: MetricKind) -> Option<f64> {
    if !raw.is_finite() {
        return None;
    }

    let valid = match kind {
        MetricKind::Percent => (0.0..=100.0).contains(&raw),
        MetricKind::TemperatureC => (-100.0..=250.0).contains(&raw),
        MetricKind::NonNegative => raw >= 0.0,
        MetricKind::Positive => raw > 0.0,
    };

    if !valid {
        return None;
    }

    // Normalize -0.0 to 0.0 while preserving genuine zero measurements.
    Some(if raw == 0.0 { 0.0 } else { raw })
}

fn extract_numeric_f64(value: &OwnedValue) -> Option<f64> {
    if let Ok(v) = f64::try_from(value) {
        return Some(v);
    }
    if let Ok(v) = u64::try_from(value) {
        return Some(v as f64);
    }
    if let Ok(v) = i64::try_from(value) {
        return Some(v as f64);
    }
    if let Ok(v) = u32::try_from(value) {
        return Some(v as f64);
    }
    if let Ok(v) = i32::try_from(value) {
        return Some(v as f64);
    }
    if let Ok(v) = u16::try_from(value) {
        return Some(v as f64);
    }
    if let Ok(v) = i16::try_from(value) {
        return Some(v as f64);
    }
    if let Ok(v) = u8::try_from(value) {
        return Some(v as f64);
    }
    None
}

fn extract_section(hardware: &MonitoringMap, name: &str) -> Option<MonitoringMap> {
    hardware
        .get(name)
        .and_then(|value| value.try_clone().ok())
        .and_then(|value| HashMap::try_from(value).ok())
}

fn extract_bool(section: &MonitoringMap, key: &str) -> Option<bool> {
    section.get(key).and_then(|v| bool::try_from(v).ok())
}

fn extract_string(section: &MonitoringMap, key: &str) -> Option<String> {
    section
        .get(key)
        .and_then(|v| v.try_clone().ok())
        .and_then(|v| String::try_from(v).ok())
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
}

fn is_operstate_up(operstate: &str) -> bool {
    !matches!(
        operstate.to_ascii_lowercase().as_str(),
        "down" | "notpresent" | "lowerlayerdown" | "dormant"
    )
}

/// Validates the structured envelope returned by `org.zethropol.Hardware.v1.GetTelemetry`
/// and extracts its inner `data` map.
pub fn unwrap_hardware_envelope(envelope: MonitoringMap) -> Result<MonitoringMap, SampleError> {
    if let Some(success_val) = envelope.get("success") {
        let ok = bool::try_from(success_val).map_err(|_| {
            SampleError::new(
                SampleErrorKind::Failed,
                "Hardware service returned malformed success field.",
            )
        })?;
        if !ok {
            let detail = envelope
                .get("message")
                .and_then(|v| v.try_clone().ok())
                .and_then(|v| String::try_from(v).ok())
                .filter(|s| !s.trim().is_empty())
                .unwrap_or_else(|| "Hardware service reported failure.".to_string());
            return Err(SampleError::new(SampleErrorKind::Failed, detail));
        }
    }

    if let Some(code_val) = envelope.get("code") {
        let code = code_val
            .try_clone()
            .ok()
            .and_then(|v| String::try_from(v).ok())
            .unwrap_or_default();
        if !code.is_empty() && code != "ok" {
            let detail = envelope
                .get("message")
                .and_then(|v| v.try_clone().ok())
                .and_then(|v| String::try_from(v).ok())
                .filter(|s| !s.trim().is_empty())
                .unwrap_or_else(|| format!("Hardware service returned status code '{code}'."));
            return Err(SampleError::new(SampleErrorKind::Failed, detail));
        }
    }

    let data = envelope.get("data").ok_or_else(|| {
        SampleError::new(
            SampleErrorKind::Failed,
            "Hardware service telemetry has no data payload.",
        )
    })?;

    data.try_clone()
        .and_then(HashMap::try_from)
        .map_err(|_| {
            SampleError::new(
                SampleErrorKind::Failed,
                "Hardware service telemetry data payload is not a dictionary.",
            )
        })
}

/// Extracts and validates the 25 supported monitoring metrics plus hardware
/// capability flags from a `Hardware.v1.GetTelemetry` `data` dictionary.
/// Missing or invalid metrics are omitted (never masked as 0.0).
/// Genuine 0.0 measurements are preserved.
pub fn extract_monitoring_metrics(
    hardware: &MonitoringMap,
) -> Result<ExtractedTelemetry, SampleError> {
    let cpu_section = extract_section(hardware, "cpu");
    let gpu_section = extract_section(hardware, "gpu");
    let memory_section = extract_section(hardware, "memory");
    let storage_section = extract_section(hardware, "normalized_storage");
    let network_section = extract_section(hardware, "network");

    let sections: [(&str, Option<&MonitoringMap>); 5] = [
        ("cpu", cpu_section.as_ref()),
        ("gpu", gpu_section.as_ref()),
        ("memory", memory_section.as_ref()),
        ("normalized_storage", storage_section.as_ref()),
        ("network", network_section.as_ref()),
    ];

    let mut metrics = BTreeMap::new();

    for spec in METRIC_SPECS {
        let Some((_, Some(section))) = sections.iter().find(|(name, _)| *name == spec.section)
        else {
            continue;
        };

        let Some(raw_value) = section.get(spec.source_key) else {
            continue;
        };

        let Some(numeric) = extract_numeric_f64(raw_value) else {
            continue;
        };

        if let Some(valid) = validate_metric(numeric, spec.kind) {
            metrics.insert(spec.output_key.to_string(), valid);
        }
    }

    // Cross-field capacity consistency guards: reject used > total so inconsistent
    // upstream telemetry is never forwarded as valid across D-Bus layers.
    if let (Some(&used), Some(&total)) = (
        metrics.get("memory_used_gb"),
        metrics.get("memory_total_gb"),
    ) && used > total
    {
        metrics.remove("memory_used_gb");
        metrics.remove("memory_total_gb");
    }
    if let (Some(&used), Some(&total)) = (
        metrics.get("gpu_vram_used_gb"),
        metrics.get("gpu_vram_total_gb"),
    ) && used > total
    {
        metrics.remove("gpu_vram_used_gb");
        metrics.remove("gpu_vram_total_gb");
    }
    if let Some(&fs_total) = metrics.get("storage_filesystem_total_gb") {
        if metrics
            .get("storage_filesystem_used_gb")
            .is_some_and(|&used| used > fs_total)
        {
            metrics.remove("storage_filesystem_used_gb");
            metrics.remove("storage_filesystem_total_gb");
            metrics.remove("storage_filesystem_usage_percent");
        } else {
            if metrics
                .get("storage_filesystem_free_gb")
                .is_some_and(|&free| free > fs_total)
            {
                metrics.remove("storage_filesystem_free_gb");
            }
            if metrics
                .get("storage_filesystem_available_gb")
                .is_some_and(|&avail| avail > fs_total)
            {
                metrics.remove("storage_filesystem_available_gb");
            }
        }
    }

    if metrics.is_empty() {
        return Err(SampleError::new(
            SampleErrorKind::Failed,
            "Hardware telemetry contained no valid monitoring metrics.",
        ));
    }

    let gpu_has_metrics = metrics.keys().any(|k| k.starts_with("gpu_"));
    let gpu_detected = gpu_section
        .as_ref()
        .and_then(|s| extract_bool(s, "detected"))
        .unwrap_or(gpu_has_metrics);

    let storage_has_metrics = metrics.keys().any(|k| k.starts_with("storage_"));
    let storage_detected = storage_section
        .as_ref()
        .and_then(|s| extract_bool(s, "detected"))
        .unwrap_or(storage_has_metrics || storage_section.is_some());

    let network_has_rates = metrics.contains_key("network_download_mbps")
        || metrics.contains_key("network_upload_mbps");
    let network_link_up = match network_section.as_ref() {
        Some(net) => {
            let iface = extract_string(net, "interface");
            let operstate = extract_string(net, "operstate");
            match (iface, operstate) {
                (Some(_), Some(state)) => is_operstate_up(&state),
                (Some(_), None) => true,
                (None, Some(state)) => is_operstate_up(&state),
                (None, None) => network_has_rates,
            }
        }
        None => false,
    };

    Ok(ExtractedTelemetry {
        metrics,
        capabilities: HardwareCapabilities {
            gpu_detected,
            storage_detected,
            network_link_up,
        },
    })
}

fn metrics_to_dbus_map(metrics: &BTreeMap<String, f64>) -> MonitoringMap {
    let mut map = HashMap::with_capacity(metrics.len());
    for (key, value) in metrics {
        map.insert(key.clone(), OwnedValue::from(*value));
    }
    map
}

pub fn build_envelope(
    success: bool,
    code: &str,
    message: &str,
    readiness: ReadinessState,
    data: MonitoringMap,
) -> MonitoringMap {
    let mut result = HashMap::with_capacity(6);
    result.insert("success".to_string(), OwnedValue::from(success));
    result.insert("code".to_string(), OwnedValue::from(Str::from(code.to_string())));
    result.insert(
        "message".to_string(),
        OwnedValue::from(Str::from(message.to_string())),
    );
    result.insert("operationId".to_string(), OwnedValue::from(Str::from("")));
    result.insert(
        "readiness".to_string(),
        OwnedValue::from(Str::from(readiness.as_str().to_string())),
    );
    result.insert("data".to_string(), OwnedValue::from(data));
    result
}

#[derive(Debug, Clone, PartialEq)]
enum PublishedSignature {
    Available {
        readiness: ReadinessState,
        metrics: BTreeMap<String, f64>,
    },
    Unavailable {
        readiness: ReadinessState,
        code: &'static str,
        message: String,
    },
}

#[derive(Debug, Clone)]
enum CacheEntry {
    Initializing,
    Available {
        telemetry: ExtractedTelemetry,
        readiness: ReadinessState,
        collected_at: Instant,
    },
    Stale,
    Failed {
        error: SampleError,
    },
}

#[derive(Debug)]
pub struct TelemetryCache {
    max_sample_age: Duration,
    entry: CacheEntry,
    last_published: Option<PublishedSignature>,
    /// Tracks optional/hardware sensors observed as supported on currently present
    /// hardware so transient sensor read failures are distinguished from sensors
    /// that do not exist on this machine.
    observed_sensors: BTreeSet<String>,
}

impl TelemetryCache {
    pub fn new(max_sample_age: Duration) -> Self {
        Self {
            max_sample_age,
            entry: CacheEntry::Initializing,
            last_published: None,
            observed_sensors: BTreeSet::new(),
        }
    }

    fn evaluate_readiness(&mut self, telemetry: &ExtractedTelemetry) -> ReadinessState {
        // If an optional subsystem is no longer present or link went offline,
        // drop its prior sensor expectations so natural absence stays Ready.
        if !telemetry.capabilities.gpu_detected {
            self.observed_sensors.retain(|k| !k.starts_with("gpu_"));
        }
        if !telemetry.capabilities.storage_detected {
            self.observed_sensors.retain(|k| !k.starts_with("storage_"));
        }
        if !telemetry.capabilities.network_link_up {
            self.observed_sensors.retain(|k| !k.starts_with("network_"));
        }

        // Transient dropout check: a previously observed sensor on a still-present
        // subsystem disappeared (excluding optional ping_ms).
        let transient_sensor_dropout = self
            .observed_sensors
            .iter()
            .any(|key| !telemetry.metrics.contains_key(key));

        for key in telemetry.metrics.keys() {
            if key != "network_ping_ms" {
                self.observed_sensors.insert(key.clone());
            }
        }

        if telemetry.has_mandatory_metrics() && !transient_sensor_dropout {
            ReadinessState::Ready
        } else {
            ReadinessState::Degraded
        }
    }

    /// Records a valid fresh telemetry sample. Returns `Some(envelope)` if
    /// `StateChanged` should be emitted (meaningful metric or readiness change;
    /// timestamp-only refresh returns `None`).
    pub fn record_success(
        &mut self,
        telemetry: ExtractedTelemetry,
        now: Instant,
    ) -> Option<MonitoringMap> {
        let readiness = self.evaluate_readiness(&telemetry);
        let signature = PublishedSignature::Available {
            readiness,
            metrics: telemetry.metrics.clone(),
        };

        let should_emit = self.last_published.as_ref() != Some(&signature);
        let data = if should_emit {
            Some(metrics_to_dbus_map(&telemetry.metrics))
        } else {
            None
        };

        self.entry = CacheEntry::Available {
            telemetry,
            readiness,
            collected_at: now,
        };

        if should_emit {
            self.last_published = Some(signature);
            Some(build_envelope(
                true,
                "ok",
                "",
                readiness,
                data.unwrap_or_default(),
            ))
        } else {
            None
        }
    }

    /// Records an upstream sampling failure and immediately invalidates any
    /// previous cached metrics so stale telemetry is never served as current.
    /// Returns `Some(envelope)` only when the unavailable status/code/message
    /// changes compared to the last emitted signal.
    pub fn record_failure(&mut self, error: SampleError) -> Option<MonitoringMap> {
        let code = error.kind.result_code();
        let signature = PublishedSignature::Unavailable {
            readiness: ReadinessState::DependencyFailed,
            code,
            message: error.message.clone(),
        };

        let should_emit = self.last_published.as_ref() != Some(&signature);
        let message = error.message.clone();
        self.entry = CacheEntry::Failed { error };

        if should_emit {
            self.last_published = Some(signature);
            Some(build_envelope(
                false,
                code,
                &message,
                ReadinessState::DependencyFailed,
                HashMap::new(),
            ))
        } else {
            None
        }
    }

    /// Returns the instant at which the currently cached `Available` sample
    /// becomes stale, or `None` if no live sample is cached.
    pub fn next_staleness_deadline(&self) -> Option<Instant> {
        match &self.entry {
            CacheEntry::Available { collected_at, .. } => {
                collected_at.checked_add(self.max_sample_age)
            }
            _ => None,
        }
    }

    /// Transitions an expired `Available` sample to `Stale` and returns the
    /// `StateChanged` envelope once (deduplicated) so passive signal subscribers
    /// are proactively notified of staleness.
    pub fn expire_if_stale(&mut self, now: Instant) -> Option<MonitoringMap> {
        let is_expired = match &self.entry {
            CacheEntry::Available { collected_at, .. } => {
                now.saturating_duration_since(*collected_at) > self.max_sample_age
            }
            _ => false,
        };

        if !is_expired {
            return None;
        }

        self.entry = CacheEntry::Stale;
        let signature = PublishedSignature::Unavailable {
            readiness: ReadinessState::DependencyFailed,
            code: "not-available",
            message: STALE_TELEMETRY_MESSAGE.to_string(),
        };

        if self.last_published.as_ref() != Some(&signature) {
            self.last_published = Some(signature);
            Some(build_envelope(
                false,
                "not-available",
                STALE_TELEMETRY_MESSAGE,
                ReadinessState::DependencyFailed,
                HashMap::new(),
            ))
        } else {
            None
        }
    }

    /// Builds the `GetState()` response envelope at timestamp `now`.
    /// Enforces `max_sample_age` so expired samples are never presented as current.
    pub fn get_state_envelope(&self, now: Instant) -> MonitoringMap {
        match &self.entry {
            CacheEntry::Initializing => build_envelope(
                false,
                "not-available",
                "Monitoring telemetry is initializing; no sample collected yet.",
                ReadinessState::Initializing,
                HashMap::new(),
            ),
            CacheEntry::Available {
                telemetry,
                readiness,
                collected_at,
            } => {
                if now.saturating_duration_since(*collected_at) > self.max_sample_age {
                    build_envelope(
                        false,
                        "not-available",
                        STALE_TELEMETRY_MESSAGE,
                        ReadinessState::DependencyFailed,
                        HashMap::new(),
                    )
                } else {
                    build_envelope(
                        true,
                        "ok",
                        "",
                        *readiness,
                        metrics_to_dbus_map(&telemetry.metrics),
                    )
                }
            }
            CacheEntry::Stale => build_envelope(
                false,
                "not-available",
                STALE_TELEMETRY_MESSAGE,
                ReadinessState::DependencyFailed,
                HashMap::new(),
            ),
            CacheEntry::Failed { error } => build_envelope(
                false,
                error.kind.result_code(),
                &error.message,
                ReadinessState::DependencyFailed,
                HashMap::new(),
            ),
        }
    }
}
