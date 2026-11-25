use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, Instant};

use tokio::sync::{Mutex, Notify, RwLock};
use tokio::time;
use zbus::interface;
use zbus::object_server::SignalEmitter;
use zbus::proxy::CacheProperties;

use crate::telemetry::{
    ExtractedTelemetry, MonitoringMap, SampleError, SampleErrorKind, TelemetryCache,
    extract_monitoring_metrics, unwrap_hardware_envelope,
};

pub const MONITORING_SERVICE: &str = "org.zethropol.Monitoring";
pub const MONITORING_PATH: &str = "/org/zethropol/Monitoring";
pub const HARDWARE_SERVICE: &str = "org.zethropol.Hardware";
pub const HARDWARE_PATH: &str = "/org/zethropol/Hardware";
pub const HARDWARE_INTERFACE: &str = "org.zethropol.Hardware.v1";
pub const HARDWARE_TELEMETRY_METHOD: &str = "GetTelemetry";

pub type SharedCache = Arc<RwLock<TelemetryCache>>;

#[derive(Debug, Clone)]
pub struct SamplerConfig {
    pub sample_interval: Duration,
    pub retry_interval: Duration,
    /// Longer backoff when Hardware does not implement `GetTelemetry` (`UnknownMethod`)
    /// so version incompatibility never produces uncontrolled retry or log spam.
    pub incompatible_retry_interval: Duration,
    pub sample_timeout: Duration,
    pub max_sample_age: Duration,
}

impl Default for SamplerConfig {
    fn default() -> Self {
        Self {
            sample_interval: Duration::from_secs(1),
            retry_interval: Duration::from_secs(1),
            incompatible_retry_interval: Duration::from_secs(30),
            sample_timeout: Duration::from_millis(3500),
            max_sample_age: Duration::from_secs(6),
        }
    }
}

#[derive(Debug, Clone)]
pub enum BusTarget {
    System,
    #[cfg_attr(not(test), allow(dead_code))]
    Address(String),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SampleOutcome {
    Success,
    Failed(SampleErrorKind),
}

/// Reusable D-Bus client for `org.zethropol.Hardware.v1.GetTelemetry`.
///
/// Maintains a single persistent connection and an uncached proxy
/// (`CacheProperties::No`) across sampling cycles, resetting them cleanly on
/// error or disconnect. Never falls back to heavy `Hardware.v1.GetState`.
pub struct DbusHardwareClient {
    target: BusTarget,
    connection: Option<zbus::Connection>,
    proxy: Option<zbus::Proxy<'static>>,
    connect_count: Arc<AtomicUsize>,
    proxy_build_count: Arc<AtomicUsize>,
}

impl DbusHardwareClient {
    pub fn from_shared_connection(connection: zbus::Connection, target: BusTarget) -> Self {
        Self {
            target,
            connection: Some(connection),
            proxy: None,
            connect_count: Arc::new(AtomicUsize::new(1)),
            proxy_build_count: Arc::new(AtomicUsize::new(0)),
        }
    }

    #[cfg(test)]
    pub fn new(target: BusTarget) -> Self {
        Self {
            target,
            connection: None,
            proxy: None,
            connect_count: Arc::new(AtomicUsize::new(0)),
            proxy_build_count: Arc::new(AtomicUsize::new(0)),
        }
    }

    #[cfg(test)]
    pub fn connect_count(&self) -> usize {
        self.connect_count.load(Ordering::SeqCst)
    }

    #[cfg(test)]
    pub fn proxy_build_count(&self) -> usize {
        self.proxy_build_count.load(Ordering::SeqCst)
    }

    async fn ensure_proxy(&mut self) -> Result<zbus::Proxy<'static>, SampleError> {
        if let Some(existing_conn) = &self.connection
            && existing_conn.is_closed()
        {
            self.invalidate(true);
        }

        if let Some(proxy) = &self.proxy {
            return Ok(proxy.clone());
        }

        let conn = match &self.connection {
            Some(existing) => existing.clone(),
            None => {
                let built = match &self.target {
                    BusTarget::System => zbus::Connection::system().await,
                    BusTarget::Address(addr) => {
                        match zbus::connection::Builder::address(addr.as_str()) {
                            Ok(builder) => builder.build().await,
                            Err(error) => Err(error),
                        }
                    }
                }
                .map_err(|error| {
                    let (mut kind, _) = classify_zbus_error(&error);
                    if kind == SampleErrorKind::Failed && !is_auth_zbus_error(&error) {
                        kind = SampleErrorKind::Unavailable;
                    }
                    SampleError::new(
                        kind,
                        format!("Failed to connect to Hardware D-Bus bus: {error}"),
                    )
                })?;

                self.connect_count.fetch_add(1, Ordering::SeqCst);
                self.connection = Some(built.clone());
                built
            }
        };

        let proxy = zbus::proxy::Builder::<zbus::Proxy<'static>>::new(&conn)
            .destination(HARDWARE_SERVICE)
            .and_then(|b| b.path(HARDWARE_PATH))
            .and_then(|b| b.interface(HARDWARE_INTERFACE))
            .map(|b| b.cache_properties(CacheProperties::No))
            .map_err(|error| {
                SampleError::new(
                    SampleErrorKind::Failed,
                    format!("Failed to configure Hardware D-Bus proxy: {error}"),
                )
            })?
            .build()
            .await
            .map_err(|error| {
                let (kind, reset_conn) = classify_zbus_error(&error);
                if reset_conn {
                    self.connection = None;
                }
                SampleError::new(
                    kind,
                    format!("Failed to build Hardware D-Bus proxy: {error}"),
                )
            })?;

        self.proxy_build_count.fetch_add(1, Ordering::SeqCst);
        self.proxy = Some(proxy.clone());
        Ok(proxy)
    }

    pub fn invalidate(&mut self, reset_connection: bool) {
        self.proxy = None;
        if reset_connection {
            self.connection = None;
        }
    }

    pub async fn fetch_telemetry(
        &mut self,
        timeout_duration: Duration,
    ) -> Result<ExtractedTelemetry, SampleError> {
        let proxy = match time::timeout(timeout_duration, self.ensure_proxy()).await {
            Ok(Ok(proxy)) => proxy,
            Ok(Err(error)) => {
                self.invalidate(matches!(error.kind, SampleErrorKind::Unavailable));
                return Err(error);
            }
            Err(_) => {
                self.invalidate(true);
                return Err(SampleError::new(
                    SampleErrorKind::Timeout,
                    "Timed out establishing Hardware D-Bus proxy.",
                ));
            }
        };

        let call_result = time::timeout(
            timeout_duration,
            proxy.call::<_, _, MonitoringMap>(HARDWARE_TELEMETRY_METHOD, &()),
        )
        .await;

        let envelope = match call_result {
            Ok(Ok(envelope)) => envelope,
            Ok(Err(error)) => {
                let (kind, reset_conn) = classify_zbus_error(&error);
                self.invalidate(reset_conn);
                let message = if kind == SampleErrorKind::Incompatible {
                    format!(
                        "Hardware service does not support {HARDWARE_INTERFACE}.{HARDWARE_TELEMETRY_METHOD}; incompatible zethropol-hardware version."
                    )
                } else {
                    format!("Hardware D-Bus {HARDWARE_TELEMETRY_METHOD} failed: {error}")
                };
                return Err(SampleError::new(kind, message));
            }
            Err(_) => {
                self.invalidate(false);
                return Err(SampleError::new(
                    SampleErrorKind::Timeout,
                    format!("Hardware D-Bus {HARDWARE_TELEMETRY_METHOD} timed out."),
                ));
            }
        };

        let hardware_data = unwrap_hardware_envelope(envelope)?;
        extract_monitoring_metrics(&hardware_data)
    }
}

fn is_auth_zbus_error(error: &zbus::Error) -> bool {
    match error {
        zbus::Error::MethodError(name, _, _) => matches!(
            name.as_str(),
            "org.freedesktop.DBus.Error.AccessDenied" | "org.freedesktop.DBus.Error.AuthFailed"
        ),
        zbus::Error::FDO(fdo_err) => matches!(
            fdo_err.as_ref(),
            zbus::fdo::Error::AccessDenied(_) | zbus::fdo::Error::AuthFailed(_)
        ),
        _ => false,
    }
}

fn classify_zbus_error(error: &zbus::Error) -> (SampleErrorKind, bool) {
    match error {
        zbus::Error::InputOutput(_)
        | zbus::Error::Handshake(_)
        | zbus::Error::InvalidReply
        | zbus::Error::InvalidGUID
        | zbus::Error::Address(_) => (SampleErrorKind::Unavailable, true),
        zbus::Error::Failure(msg)
            if msg.contains("Failed to connect")
                || msg.contains("Connection refused")
                || msg.contains("No such file or directory")
                || msg.contains("Disconnected") =>
        {
            (SampleErrorKind::Unavailable, true)
        }
        zbus::Error::MethodError(name, _, _) => {
            let err_name = name.as_str();
            if err_name == "org.freedesktop.DBus.Error.Disconnected" {
                (SampleErrorKind::Unavailable, true)
            } else if err_name == "org.freedesktop.DBus.Error.ServiceUnknown"
                || err_name == "org.freedesktop.DBus.Error.NameHasNoOwner"
            {
                (SampleErrorKind::Unavailable, false)
            } else if err_name == "org.freedesktop.DBus.Error.UnknownMethod" {
                (SampleErrorKind::Incompatible, false)
            } else if err_name == "org.freedesktop.DBus.Error.NoReply"
                || err_name == "org.freedesktop.DBus.Error.TimedOut"
                || err_name == "org.freedesktop.DBus.Error.Timeout"
            {
                (SampleErrorKind::Timeout, false)
            } else {
                (SampleErrorKind::Failed, false)
            }
        }
        zbus::Error::FDO(fdo_err) => match fdo_err.as_ref() {
            zbus::fdo::Error::Disconnected(_) => (SampleErrorKind::Unavailable, true),
            zbus::fdo::Error::ServiceUnknown(_) | zbus::fdo::Error::NameHasNoOwner(_) => {
                (SampleErrorKind::Unavailable, false)
            }
            zbus::fdo::Error::UnknownMethod(_) => (SampleErrorKind::Incompatible, false),
            zbus::fdo::Error::NoReply(_)
            | zbus::fdo::Error::TimedOut(_)
            | zbus::fdo::Error::Timeout(_) => (SampleErrorKind::Timeout, false),
            _ => (SampleErrorKind::Failed, false),
        },
        _ => (SampleErrorKind::Failed, false),
    }
}

#[derive(Default)]
pub struct ErrorLogDeduplicator {
    last_logged: Option<(SampleErrorKind, String)>,
}

impl ErrorLogDeduplicator {
    pub fn log_error(&mut self, error: &SampleError) {
        let key = (error.kind, error.message.clone());
        if self.last_logged.as_ref() != Some(&key) {
            eprintln!(
                "Monitoring sampler error ({}): {}",
                error.kind.result_code(),
                error.message
            );
            self.last_logged = Some(key);
        }
    }

    pub fn clear(&mut self) {
        self.last_logged = None;
    }
}

/// Runs one serialized sampling cycle against `client`, updates `cache`, wakes
/// the staleness watchdog on fresh samples, and emits `StateChanged` only when
/// telemetry or service readiness meaningfully changed.
pub async fn sample_once(
    client: &mut DbusHardwareClient,
    cache: &SharedCache,
    timeout_duration: Duration,
    emitter: Option<&SignalEmitter<'_>>,
    staleness_notify: Option<&Notify>,
    log_dedup: &mut ErrorLogDeduplicator,
) -> SampleOutcome {
    let outcome = client.fetch_telemetry(timeout_duration).await;
    let now = Instant::now();

    let (result, signal_payload) = match outcome {
        Ok(telemetry) => {
            log_dedup.clear();
            let mut guard = cache.write().await;
            let payload = guard.record_success(telemetry, now);
            (SampleOutcome::Success, payload)
        }
        Err(error) => {
            let kind = error.kind;
            log_dedup.log_error(&error);
            let mut guard = cache.write().await;
            let payload = guard.record_failure(error);
            (SampleOutcome::Failed(kind), payload)
        }
    };

    if let Some(notify) = staleness_notify {
        notify.notify_one();
    }

    if let (Some(emitter), Some(envelope)) = (emitter, signal_payload)
        && let Err(error) = MonitoringService::state_changed(emitter, envelope).await
    {
        eprintln!("Monitoring StateChanged signal error: {error}");
    }

    result
}

/// Event-driven staleness watchdog that sleeps cleanly until the active sample's
/// `max_sample_age` deadline and proactively emits `StateChanged` if telemetry
/// expires before a fresh sample arrives.
pub async fn run_staleness_watchdog(
    cache: SharedCache,
    staleness_notify: Arc<Notify>,
    connection: zbus::Connection,
) {
    loop {
        if connection.is_closed() {
            break;
        }

        let deadline = cache.read().await.next_staleness_deadline();
        match deadline {
            None => {
                tokio::select! {
                    _ = staleness_notify.notified() => {}
                    _ = connection.closed() => break,
                }
            }
            Some(std_deadline) => {
                // Add 1ms margin so `now.saturating_duration_since(collected_at) > max_sample_age`
                // holds deterministically upon timer wake-up.
                let wake_at =
                    time::Instant::from_std(std_deadline + Duration::from_millis(1));
                tokio::select! {
                    _ = time::sleep_until(wake_at) => {
                        let stale_envelope = cache.write().await.expire_if_stale(Instant::now());
                        if let Some(envelope) = stale_envelope
                            && let Ok(iface_ref) = connection
                                .object_server()
                                .interface::<_, MonitoringService>(MONITORING_PATH)
                                .await
                            && let Err(error) =
                                MonitoringService::state_changed(iface_ref.signal_emitter(), envelope)
                                    .await
                        {
                            eprintln!("Monitoring stale StateChanged signal error: {error}");
                        }
                    }
                    _ = staleness_notify.notified() => {}
                    _ = connection.closed() => break,
                }
            }
        }
    }
}

/// Single shared non-overlapping background sampling loop paired with the
/// proactive staleness watchdog.
pub async fn run_sampler(
    client: DbusHardwareClient,
    cache: SharedCache,
    config: SamplerConfig,
    connection: zbus::Connection,
) {
    let client = Mutex::new(client);
    let staleness_notify = Arc::new(Notify::new());
    let watchdog = tokio::spawn(run_staleness_watchdog(
        Arc::clone(&cache),
        Arc::clone(&staleness_notify),
        connection.clone(),
    ));
    let mut log_dedup = ErrorLogDeduplicator::default();

    loop {
        if connection.is_closed() {
            break;
        }

        let emitter_ref = connection
            .object_server()
            .interface::<_, MonitoringService>(MONITORING_PATH)
            .await
            .ok();

        let outcome = {
            let mut guard = client.lock().await;
            let emitter = emitter_ref.as_ref().map(|iface| iface.signal_emitter());
            sample_once(
                &mut guard,
                &cache,
                config.sample_timeout,
                emitter,
                Some(&staleness_notify),
                &mut log_dedup,
            )
            .await
        };

        let delay = match outcome {
            SampleOutcome::Success => config.sample_interval,
            SampleOutcome::Failed(SampleErrorKind::Incompatible) => {
                config.incompatible_retry_interval
            }
            SampleOutcome::Failed(_) => config.retry_interval,
        };

        tokio::select! {
            _ = time::sleep(delay) => {}
            _ = connection.closed() => break,
        }
    }

    watchdog.abort();
}

pub struct MonitoringService {
    pub cache: SharedCache,
}

#[interface(name = "org.zethropol.Monitoring.v1")]
impl MonitoringService {
    #[zbus(signal)]
    pub async fn state_changed(
        emitter: &SignalEmitter<'_>,
        state: MonitoringMap,
    ) -> zbus::Result<()>;

    async fn get_state(&self) -> zbus::fdo::Result<MonitoringMap> {
        let guard = self.cache.read().await;
        Ok(guard.get_state_envelope(Instant::now()))
    }
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;
    use std::io::{BufRead, BufReader};
    use std::process::{Child, Command, Stdio};
    use std::sync::Arc;
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
    use std::time::{Duration, Instant};

    use tokio::sync::{Notify, RwLock};
    use zbus::export::ordered_stream::OrderedStreamExt;
    use zbus::zvariant::{OwnedValue, Str};

    use super::*;
    use crate::telemetry::METRIC_SPECS;

    struct PrivateBus {
        child: Child,
        address: String,
    }

    impl PrivateBus {
        fn start() -> Self {
            let mut child = Command::new("dbus-daemon")
                .args(["--session", "--nofork", "--nopidfile", "--print-address=1"])
                .stdout(Stdio::piped())
                .stderr(Stdio::null())
                .spawn()
                .expect("failed to spawn private dbus-daemon for tests");

            let stdout = child.stdout.take().expect("missing dbus-daemon stdout");
            let mut reader = BufReader::new(stdout);
            let mut address = String::new();
            reader
                .read_line(&mut address)
                .expect("failed to read private dbus-daemon address");
            let address = address.trim().to_string();
            assert!(!address.is_empty(), "dbus-daemon returned empty address");

            Self { child, address }
        }
    }

    impl Drop for PrivateBus {
        fn drop(&mut self) {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }

    fn map_value(entries: Vec<(&str, OwnedValue)>) -> OwnedValue {
        let mut map = HashMap::new();
        for (k, v) in entries {
            map.insert(k.to_string(), v);
        }
        OwnedValue::from(map)
    }

    fn full_telemetry_data(cpu_usage: f64, include_ping: bool) -> MonitoringMap {
        let mut hw = HashMap::new();
        hw.insert(
            "cpu".to_string(),
            map_value(vec![
                ("usage_percent", OwnedValue::from(cpu_usage)),
                ("temperature_c", OwnedValue::from(42.5_f64)),
                ("frequency_ghz", OwnedValue::from(3.85_f64)),
            ]),
        );
        hw.insert(
            "gpu".to_string(),
            map_value(vec![
                ("detected", OwnedValue::from(true)),
                ("usage_percent", OwnedValue::from(0.0_f64)),
                ("memory_usage_percent", OwnedValue::from(2.0_f64)),
                ("core_clock_mhz", OwnedValue::from(1149.0_f64)),
                ("vram_used_gb", OwnedValue::from(0.42_f64)),
                ("vram_total_gb", OwnedValue::from(4.0_f64)),
                ("temperature_c", OwnedValue::from(29.0_f64)),
                ("memory_clock_mhz", OwnedValue::from(300.0_f64)),
            ]),
        );
        hw.insert(
            "memory".to_string(),
            map_value(vec![
                ("used_gb", OwnedValue::from(4.46_f64)),
                ("total_gb", OwnedValue::from(31.26_f64)),
                ("available_gb", OwnedValue::from(26.80_f64)),
            ]),
        );
        hw.insert(
            "normalized_storage".to_string(),
            map_value(vec![
                ("detected", OwnedValue::from(true)),
                ("capacity_gb", OwnedValue::from(500.11_f64)),
                ("physical_capacity_gb", OwnedValue::from(500.11_f64)),
                ("used_gb", OwnedValue::from(255.84_f64)),
                ("available_gb", OwnedValue::from(237.79_f64)),
                ("filesystem_total_gb", OwnedValue::from(495.81_f64)),
                ("filesystem_used_gb", OwnedValue::from(255.84_f64)),
                ("filesystem_free_gb", OwnedValue::from(239.98_f64)),
                ("filesystem_available_gb", OwnedValue::from(237.79_f64)),
                ("filesystem_usage_percent", OwnedValue::from(51.83_f64)),
                ("filesystem_type", OwnedValue::from(Str::from("btrfs"))),
                ("filesystem_subvolume", OwnedValue::from(Str::from("/@"))),
                ("temperature_c", OwnedValue::from(32.9_f64)),
            ]),
        );
        let mut net_entries = vec![
            ("interface", OwnedValue::from(Str::from("enp34s0"))),
            ("operstate", OwnedValue::from(Str::from("up"))),
            ("download_mbps", OwnedValue::from(0.0_f64)),
            ("upload_mbps", OwnedValue::from(1.25_f64)),
        ];
        if include_ping {
            net_entries.push(("ping_ms", OwnedValue::from(6.1_f64)));
        }
        hw.insert("network".to_string(), map_value(net_entries));
        hw
    }

    fn first_sample_warming_data() -> MonitoringMap {
        let mut hw = full_telemetry_data(15.0, false);
        // First GetTelemetry() omits CPU usage and active-link network rates while counters warm up
        hw.insert(
            "cpu".to_string(),
            map_value(vec![
                ("temperature_c", OwnedValue::from(42.5_f64)),
                ("frequency_ghz", OwnedValue::from(3.85_f64)),
            ]),
        );
        hw.insert(
            "network".to_string(),
            map_value(vec![
                ("interface", OwnedValue::from(Str::from("enp34s0"))),
                ("operstate", OwnedValue::from(Str::from("up"))),
            ]),
        );
        hw
    }

    fn hardware_ok_envelope(data: MonitoringMap) -> MonitoringMap {
        let mut envelope = HashMap::new();
        envelope.insert("success".to_string(), OwnedValue::from(true));
        envelope.insert("code".to_string(), OwnedValue::from(Str::from("ok")));
        envelope.insert("message".to_string(), OwnedValue::from(Str::from("")));
        envelope.insert("operationId".to_string(), OwnedValue::from(Str::from("")));
        envelope.insert("data".to_string(), OwnedValue::from(data));
        envelope
    }

    fn get_bool(map: &MonitoringMap, key: &str) -> bool {
        bool::try_from(&map[key]).unwrap()
    }

    fn get_string(map: &MonitoringMap, key: &str) -> String {
        String::try_from(map[key].try_clone().unwrap()).unwrap()
    }

    fn get_data(map: &MonitoringMap) -> MonitoringMap {
        map["data"].try_clone().unwrap().try_into().unwrap()
    }

    struct MockHardwareControl {
        payload: RwLock<MonitoringMap>,
        delay_ms: AtomicUsize,
        fail_envelope: AtomicBool,
        telemetry_calls: AtomicUsize,
        state_calls: AtomicUsize,
        in_flight: AtomicUsize,
        max_concurrent: AtomicUsize,
    }

    impl MockHardwareControl {
        fn new(data: MonitoringMap) -> Arc<Self> {
            Arc::new(Self {
                payload: RwLock::new(data),
                delay_ms: AtomicUsize::new(0),
                fail_envelope: AtomicBool::new(false),
                telemetry_calls: AtomicUsize::new(0),
                state_calls: AtomicUsize::new(0),
                in_flight: AtomicUsize::new(0),
                max_concurrent: AtomicUsize::new(0),
            })
        }
    }

    struct MockHardwareService {
        ctrl: Arc<MockHardwareControl>,
    }

    #[interface(name = "org.zethropol.Hardware.v1")]
    impl MockHardwareService {
        async fn get_state(&self) -> zbus::fdo::Result<MonitoringMap> {
            self.ctrl.state_calls.fetch_add(1, Ordering::SeqCst);
            let data = self.ctrl.payload.read().await.clone();
            Ok(hardware_ok_envelope(data))
        }

        async fn get_telemetry(&self) -> zbus::fdo::Result<MonitoringMap> {
            self.ctrl.telemetry_calls.fetch_add(1, Ordering::SeqCst);
            let active = self.ctrl.in_flight.fetch_add(1, Ordering::SeqCst) + 1;
            self.ctrl
                .max_concurrent
                .fetch_max(active, Ordering::SeqCst);

            let delay = self.ctrl.delay_ms.load(Ordering::SeqCst);
            if delay > 0 {
                time::sleep(Duration::from_millis(delay as u64)).await;
            }

            let response = if self.ctrl.fail_envelope.load(Ordering::SeqCst) {
                let mut err = HashMap::new();
                err.insert("success".to_string(), OwnedValue::from(false));
                err.insert("code".to_string(), OwnedValue::from(Str::from("failed")));
                err.insert(
                    "message".to_string(),
                    OwnedValue::from(Str::from("sensor backend unavailable")),
                );
                err.insert("operationId".to_string(), OwnedValue::from(Str::from("")));
                err.insert("data".to_string(), OwnedValue::from(MonitoringMap::new()));
                err
            } else {
                let data = self.ctrl.payload.read().await.clone();
                hardware_ok_envelope(data)
            };

            self.ctrl.in_flight.fetch_sub(1, Ordering::SeqCst);
            Ok(response)
        }
    }

    /// Legacy Hardware mock exposing ONLY `GetState` (no `GetTelemetry`) to test
    /// `UnknownMethod` version incompatibility and verify zero fallback to `GetState`.
    struct LegacyHardwareOnlyGetState {
        state_calls: Arc<AtomicUsize>,
    }

    #[interface(name = "org.zethropol.Hardware.v1")]
    impl LegacyHardwareOnlyGetState {
        async fn get_state(&self) -> zbus::fdo::Result<MonitoringMap> {
            self.state_calls.fetch_add(1, Ordering::SeqCst);
            Ok(hardware_ok_envelope(full_telemetry_data(10.0, false)))
        }
    }

    async fn connect_private(addr: &str) -> zbus::Connection {
        zbus::connection::Builder::address(addr)
            .expect("valid private address")
            .build()
            .await
            .expect("connect to private bus")
    }

    async fn serve_mock_hardware(
        addr: &str,
        ctrl: Arc<MockHardwareControl>,
    ) -> zbus::Connection {
        zbus::connection::Builder::address(addr)
            .expect("valid private address")
            .name(HARDWARE_SERVICE)
            .expect("acquire Hardware name")
            .serve_at(HARDWARE_PATH, MockHardwareService { ctrl })
            .expect("serve mock Hardware")
            .build()
            .await
            .expect("build mock Hardware connection")
    }

    async fn serve_monitoring(addr: &str, cache: SharedCache) -> zbus::Connection {
        zbus::connection::Builder::address(addr)
            .expect("valid private address")
            .name(MONITORING_SERVICE)
            .expect("acquire Monitoring name")
            .serve_at(MONITORING_PATH, MonitoringService { cache })
            .expect("serve Monitoring")
            .build()
            .await
            .expect("build Monitoring connection")
    }

    async fn monitoring_proxy(conn: &zbus::Connection) -> zbus::Proxy<'static> {
        zbus::proxy::Builder::<zbus::Proxy<'static>>::new(conn)
            .destination(MONITORING_SERVICE)
            .unwrap()
            .path(MONITORING_PATH)
            .unwrap()
            .interface("org.zethropol.Monitoring.v1")
            .unwrap()
            .cache_properties(CacheProperties::No)
            .build()
            .await
            .unwrap()
    }

    // Scenarios 4, 5, 6: All 25 metrics (19 canonical + 6 storage), GB/GiB semantics,
    // and verified filesystem usage percent passed directly without recomputing from physical disk.
    #[test]
    fn full_25_metric_contract_preserves_19_canonical_and_adds_6_storage_metrics() {
        assert_eq!(METRIC_SPECS.len(), 25);
        let extracted = extract_monitoring_metrics(&full_telemetry_data(17.5, true)).unwrap();
        assert_eq!(extracted.metrics.len(), 25);

        for spec in METRIC_SPECS {
            assert!(
                extracted.metrics.contains_key(spec.output_key),
                "missing expected metric {}",
                spec.output_key
            );
        }

        // Verify the 6 new Storage metrics & that Hardware filesystem_usage_percent is NOT recomputed
        // from physical_capacity_gb (255.84 / 500.11 = 51.16% != 51.83% filesystem occupancy).
        assert_eq!(extracted.metrics["storage_physical_capacity_gb"], 500.11);
        assert_eq!(extracted.metrics["storage_capacity_gb"], 500.11);
        assert_eq!(extracted.metrics["storage_filesystem_total_gb"], 495.81);
        assert_eq!(extracted.metrics["storage_filesystem_used_gb"], 255.84);
        assert_eq!(extracted.metrics["storage_filesystem_free_gb"], 239.98);
        assert_eq!(extracted.metrics["storage_filesystem_available_gb"], 237.79);
        assert_eq!(extracted.metrics["storage_filesystem_usage_percent"], 51.83);

        // Verify legacy GiB memory/VRAM fields remain untouched while storage uses decimal GB
        assert_eq!(extracted.metrics["memory_total_gb"], 31.26);
        assert_eq!(extracted.metrics["gpu_vram_total_gb"], 4.0);

        let mut cache = TelemetryCache::new(Duration::from_secs(6));
        let now = Instant::now();
        assert!(cache.record_success(extracted, now).is_some());
        let env = cache.get_state_envelope(now);
        assert!(get_bool(&env, "success"));
        assert_eq!(get_string(&env, "code"), "ok");
        assert_eq!(get_string(&env, "readiness"), "ready");
        let data = get_data(&env);
        assert_eq!(data.len(), 25);
        for spec in METRIC_SPECS {
            assert!(
                f64::try_from(&data[spec.output_key]).is_ok(),
                "metric {} is not D-Bus f64",
                spec.output_key
            );
        }
    }

    // Scenarios 7, 8, 9: First CPU/Network counter sample is Degraded -> 2nd sample transitions to Ready
    // even with default ping disabled (network_ping_ms absent).
    #[test]
    fn first_counter_sample_is_degraded_and_second_sample_without_ping_becomes_ready() {
        let mut cache = TelemetryCache::new(Duration::from_secs(6));
        let t0 = Instant::now();

        // Initializing before first sample
        let init = cache.get_state_envelope(t0);
        assert!(!get_bool(&init, "success"));
        assert_eq!(get_string(&init, "readiness"), "initializing");
        assert!(get_data(&init).is_empty());

        // Sample 1: CPU usage & active-link network throughput warming up -> Degraded (NOT Ready)
        let first = extract_monitoring_metrics(&first_sample_warming_data()).unwrap();
        assert!(!first.metrics.contains_key("cpu_usage_percent"));
        assert!(!first.metrics.contains_key("network_download_mbps"));
        assert!(!first.metrics.contains_key("network_ping_ms"));
        let sig1 = cache.record_success(first, t0).unwrap();
        assert!(get_bool(&sig1, "success"));
        assert_eq!(get_string(&sig1, "readiness"), "degraded");

        // Sample 2: real CPU & Network deltas arrived, ping still absent by default -> Ready
        let second = extract_monitoring_metrics(&full_telemetry_data(12.0, false)).unwrap();
        assert!(!second.metrics.contains_key("network_ping_ms"));
        let sig2 = cache
            .record_success(second, t0 + Duration::from_secs(1))
            .unwrap();
        assert!(get_bool(&sig2, "success"));
        assert_eq!(get_string(&sig2, "readiness"), "ready");
        assert!(!get_data(&sig2).contains_key("network_ping_ms"));
    }

    // Scenarios 10, 11, 12: GPU-less system, offline network, and unsupported optional sensors
    // remain Ready; transient dropout of a previously supported sensor marks Degraded until recovery.
    #[test]
    fn gpu_less_offline_and_unsupported_sensors_are_ready_while_transient_dropout_is_degraded() {
        let mut cache = TelemetryCache::new(Duration::from_secs(6));
        let t0 = Instant::now();

        // Headless/VM system: no GPU (detected=false), offline network (operstate=down),
        // no cpu_temperature_c, no storage_temperature_c, no ping_ms -> MUST be Ready!
        let mut headless = HashMap::new();
        headless.insert(
            "cpu".to_string(),
            map_value(vec![
                ("usage_percent", OwnedValue::from(8.5_f64)),
                ("frequency_ghz", OwnedValue::from(2.8_f64)),
            ]),
        );
        headless.insert(
            "gpu".to_string(),
            map_value(vec![("detected", OwnedValue::from(false))]),
        );
        headless.insert(
            "memory".to_string(),
            map_value(vec![
                ("used_gb", OwnedValue::from(2.0_f64)),
                ("total_gb", OwnedValue::from(16.0_f64)),
            ]),
        );
        headless.insert(
            "normalized_storage".to_string(),
            map_value(vec![
                ("detected", OwnedValue::from(true)),
                ("used_gb", OwnedValue::from(20.0_f64)),
                ("available_gb", OwnedValue::from(80.0_f64)),
                ("filesystem_total_gb", OwnedValue::from(100.0_f64)),
                ("filesystem_used_gb", OwnedValue::from(20.0_f64)),
                ("filesystem_free_gb", OwnedValue::from(80.0_f64)),
                ("filesystem_available_gb", OwnedValue::from(80.0_f64)),
                ("filesystem_usage_percent", OwnedValue::from(20.0_f64)),
            ]),
        );
        headless.insert(
            "network".to_string(),
            map_value(vec![
                ("interface", OwnedValue::from(Str::from("enp34s0"))),
                ("operstate", OwnedValue::from(Str::from("down"))),
            ]),
        );

        let extracted = extract_monitoring_metrics(&headless).unwrap();
        assert!(!extracted.capabilities.gpu_detected);
        assert!(!extracted.capabilities.network_link_up);
        let env = cache.record_success(extracted, t0).unwrap();
        assert_eq!(get_string(&env, "readiness"), "ready");
        let data = get_data(&env);
        assert!(!data.contains_key("gpu_usage_percent"));
        assert!(!data.contains_key("network_download_mbps"));
        assert!(!data.contains_key("cpu_temperature_c"));

        // Transient sensor dropout: cpu_frequency_ghz was previously observed, now fails to read -> Degraded
        headless.insert(
            "cpu".to_string(),
            map_value(vec![("usage_percent", OwnedValue::from(9.0_f64))]),
        );
        let dropped = extract_monitoring_metrics(&headless).unwrap();
        let env_degraded = cache
            .record_success(dropped, t0 + Duration::from_secs(1))
            .unwrap();
        assert_eq!(get_string(&env_degraded, "readiness"), "degraded");

        // Sensor returns -> recovers cleanly to Ready
        headless.insert(
            "cpu".to_string(),
            map_value(vec![
                ("usage_percent", OwnedValue::from(9.0_f64)),
                ("frequency_ghz", OwnedValue::from(2.9_f64)),
            ]),
        );
        let recovered = extract_monitoring_metrics(&headless).unwrap();
        let env_ready = cache
            .record_success(recovered, t0 + Duration::from_secs(2))
            .unwrap();
        assert_eq!(get_string(&env_ready, "readiness"), "ready");
    }

    // Scenarios 11 & 13: Invalid values (NaN, +/-Inf, out-of-range, non-numeric) rejected;
    // genuine zero values preserved accurately across all domains.
    #[test]
    fn invalid_values_rejected_and_real_zeros_preserved() {
        let mut hw = HashMap::new();
        hw.insert(
            "cpu".to_string(),
            map_value(vec![
                ("usage_percent", OwnedValue::from(0.0_f64)),
                ("temperature_c", OwnedValue::from(-0.0_f64)),
                ("frequency_ghz", OwnedValue::from(f64::NEG_INFINITY)),
            ]),
        );
        hw.insert(
            "gpu".to_string(),
            map_value(vec![
                ("detected", OwnedValue::from(true)),
                ("usage_percent", OwnedValue::from(145.0_f64)),
                ("memory_usage_percent", OwnedValue::from(f64::NAN)),
                ("core_clock_mhz", OwnedValue::from(Str::from("1200 MHz"))),
                ("vram_used_gb", OwnedValue::from(0.0_f64)),
                ("temperature_c", OwnedValue::from(400.0_f64)),
            ]),
        );
        hw.insert(
            "memory".to_string(),
            map_value(vec![
                ("used_gb", OwnedValue::from(true)),
                ("total_gb", OwnedValue::from(32_u64)),
            ]),
        );
        hw.insert(
            "normalized_storage".to_string(),
            map_value(vec![
                ("detected", OwnedValue::from(true)),
                ("filesystem_usage_percent", OwnedValue::from(0.0_f64)),
                ("filesystem_used_gb", OwnedValue::from(0.0_f64)),
                ("filesystem_total_gb", OwnedValue::from(500.0_f64)),
                ("filesystem_available_gb", OwnedValue::from(500.0_f64)),
                ("filesystem_free_gb", OwnedValue::from(500.0_f64)),
                ("physical_capacity_gb", OwnedValue::from(-10.0_f64)),
            ]),
        );
        hw.insert(
            "network".to_string(),
            map_value(vec![
                ("interface", OwnedValue::from(Str::from("enp34s0"))),
                ("operstate", OwnedValue::from(Str::from("up"))),
                ("download_mbps", OwnedValue::from(0.0_f64)),
                ("upload_mbps", OwnedValue::from(0.0_f64)),
                ("ping_ms", OwnedValue::from(f64::INFINITY)),
            ]),
        );

        let extracted = extract_monitoring_metrics(&hw).unwrap();
        assert_eq!(extracted.metrics["cpu_usage_percent"], 0.0);
        assert_eq!(
            extracted.metrics["cpu_temperature_c"].to_bits(),
            0.0_f64.to_bits()
        );
        assert_eq!(extracted.metrics["gpu_vram_used_gb"], 0.0);
        assert_eq!(extracted.metrics["memory_total_gb"], 32.0);
        assert_eq!(extracted.metrics["storage_filesystem_usage_percent"], 0.0);
        assert_eq!(extracted.metrics["storage_filesystem_used_gb"], 0.0);
        assert_eq!(extracted.metrics["network_download_mbps"], 0.0);
        assert_eq!(extracted.metrics["network_upload_mbps"], 0.0);

        for rejected in [
            "cpu_frequency_ghz",
            "gpu_usage_percent",
            "gpu_memory_usage_percent",
            "gpu_core_clock_mhz",
            "gpu_temperature_c",
            "memory_used_gb",
            "storage_physical_capacity_gb",
            "network_ping_ms",
        ] {
            assert!(
                !extracted.metrics.contains_key(rejected),
                "invalid metric {rejected} should have been omitted"
            );
        }
    }

    #[test]
    fn inconsistent_capacities_and_zero_totals_are_rejected_while_zero_used_survives() {
        let mut hw = full_telemetry_data(22.0, false);
        hw.insert(
            "memory".to_string(),
            map_value(vec![
                ("used_gb", OwnedValue::from(40.0_f64)),
                ("total_gb", OwnedValue::from(32.0_f64)),
            ]),
        );
        hw.insert(
            "gpu".to_string(),
            map_value(vec![
                ("detected", OwnedValue::from(true)),
                ("usage_percent", OwnedValue::from(12.0_f64)),
                ("vram_used_gb", OwnedValue::from(6.0_f64)),
                ("vram_total_gb", OwnedValue::from(4.0_f64)),
            ]),
        );
        hw.insert(
            "normalized_storage".to_string(),
            map_value(vec![
                ("detected", OwnedValue::from(true)),
                ("capacity_gb", OwnedValue::from(0.0_f64)),
                ("physical_capacity_gb", OwnedValue::from(0.0_f64)),
                ("filesystem_total_gb", OwnedValue::from(500.0_f64)),
                ("filesystem_used_gb", OwnedValue::from(600.0_f64)),
                ("filesystem_usage_percent", OwnedValue::from(99.0_f64)),
            ]),
        );

        let extracted = extract_monitoring_metrics(&hw).unwrap();
        for omitted in [
            "memory_used_gb",
            "memory_total_gb",
            "gpu_vram_used_gb",
            "gpu_vram_total_gb",
            "storage_capacity_gb",
            "storage_physical_capacity_gb",
            "storage_filesystem_used_gb",
            "storage_filesystem_total_gb",
            "storage_filesystem_usage_percent",
        ] {
            assert!(
                !extracted.metrics.contains_key(omitted),
                "inconsistent capacity metric {omitted} must be rejected"
            );
        }
        assert!(!extracted.has_mandatory_metrics());

        let mut cache = TelemetryCache::new(Duration::from_secs(6));
        let env = cache.record_success(extracted, Instant::now()).unwrap();
        assert_eq!(get_string(&env, "readiness"), "degraded");

        // Zero total capacities must also be rejected, while genuine 0.0 used with >0 total survives
        let mut zero_case = full_telemetry_data(15.0, false);
        zero_case.insert(
            "memory".to_string(),
            map_value(vec![
                ("used_gb", OwnedValue::from(0.0_f64)),
                ("total_gb", OwnedValue::from(0.0_f64)),
            ]),
        );
        zero_case.insert(
            "gpu".to_string(),
            map_value(vec![
                ("detected", OwnedValue::from(true)),
                ("vram_used_gb", OwnedValue::from(0.0_f64)),
                ("vram_total_gb", OwnedValue::from(4.0_f64)),
            ]),
        );
        let z_ext = extract_monitoring_metrics(&zero_case).unwrap();
        assert!(!z_ext.metrics.contains_key("memory_total_gb"));
        assert_eq!(z_ext.metrics["gpu_vram_used_gb"], 0.0);
        assert_eq!(z_ext.metrics["gpu_vram_total_gb"], 4.0);
    }

    // Scenarios 1, 2, 3, 18: Uses GetTelemetry exclusively; UnknownMethod returns structured
    // unsupported error and NEVER falls back to heavy GetState.
    #[tokio::test]
    async fn private_bus_unknown_method_incompatibility_never_falls_back_to_get_state() {
        let bus = PrivateBus::start();
        let legacy_state_calls = Arc::new(AtomicUsize::new(0));
        let _legacy_hw = zbus::connection::Builder::address(bus.address.as_str())
            .unwrap()
            .name(HARDWARE_SERVICE)
            .unwrap()
            .serve_at(
                HARDWARE_PATH,
                LegacyHardwareOnlyGetState {
                    state_calls: Arc::clone(&legacy_state_calls),
                },
            )
            .unwrap()
            .build()
            .await
            .unwrap();

        let cache = Arc::new(RwLock::new(TelemetryCache::new(Duration::from_secs(6))));
        let mon_conn = serve_monitoring(&bus.address, Arc::clone(&cache)).await;
        let iface_ref = mon_conn
            .object_server()
            .interface::<_, MonitoringService>(MONITORING_PATH)
            .await
            .unwrap();

        let client_conn = connect_private(&bus.address).await;
        let proxy = monitoring_proxy(&client_conn).await;
        let mut signals = proxy.receive_signal("StateChanged").await.unwrap();

        let mut hw_client = DbusHardwareClient::from_shared_connection(
            mon_conn.clone(),
            BusTarget::Address(bus.address.clone()),
        );
        let mut log_dedup = ErrorLogDeduplicator::default();

        let outcome1 = sample_once(
            &mut hw_client,
            &cache,
            Duration::from_millis(500),
            Some(iface_ref.signal_emitter()),
            None,
            &mut log_dedup,
        )
        .await;
        assert_eq!(outcome1, SampleOutcome::Failed(SampleErrorKind::Incompatible));
        assert_eq!(
            legacy_state_calls.load(Ordering::SeqCst),
            0,
            "Monitoring must NEVER fall back to Hardware.v1.GetState"
        );

        let reply: MonitoringMap = proxy.call("GetState", &()).await.unwrap();
        assert!(!get_bool(&reply, "success"));
        assert_eq!(get_string(&reply, "code"), "unsupported");
        assert_eq!(get_string(&reply, "readiness"), "dependency-failed");
        assert!(get_data(&reply).is_empty());

        // First incompatibility emits one StateChanged; repeated attempt emits no duplicate signal
        let msg = time::timeout(Duration::from_secs(2), signals.next())
            .await
            .expect("expected unsupported StateChanged")
            .unwrap();
        let sig_env: MonitoringMap = msg.body().deserialize().unwrap();
        assert_eq!(get_string(&sig_env, "code"), "unsupported");

        let outcome2 = sample_once(
            &mut hw_client,
            &cache,
            Duration::from_millis(500),
            Some(iface_ref.signal_emitter()),
            None,
            &mut log_dedup,
        )
        .await;
        assert_eq!(outcome2, SampleOutcome::Failed(SampleErrorKind::Incompatible));
        assert_eq!(legacy_state_calls.load(Ordering::SeqCst), 0);
        assert!(
            time::timeout(Duration::from_millis(100), signals.next())
                .await
                .is_err(),
            "repeated UnknownMethod must not spam StateChanged"
        );
    }

    // Scenarios 14 & 15: StateChanged deduplication AND proactive staleness notification
    // to passive StateChanged subscribers (without polling GetState).
    #[tokio::test]
    async fn private_bus_state_changed_deduplication_and_proactive_staleness_notification() {
        let bus = PrivateBus::start();
        let ctrl = MockHardwareControl::new(first_sample_warming_data());
        let _hw_conn = serve_mock_hardware(&bus.address, Arc::clone(&ctrl)).await;

        let max_age = Duration::from_millis(180);
        let cache = Arc::new(RwLock::new(TelemetryCache::new(max_age)));
        let mon_conn = serve_monitoring(&bus.address, Arc::clone(&cache)).await;
        let iface_ref = mon_conn
            .object_server()
            .interface::<_, MonitoringService>(MONITORING_PATH)
            .await
            .unwrap();

        let notify = Arc::new(Notify::new());
        let watchdog = tokio::spawn(run_staleness_watchdog(
            Arc::clone(&cache),
            Arc::clone(&notify),
            mon_conn.clone(),
        ));

        let sub_conn = connect_private(&bus.address).await;
        let sub_proxy = monitoring_proxy(&sub_conn).await;
        let mut signals = sub_proxy.receive_signal("StateChanged").await.unwrap();

        let mut hw_client = DbusHardwareClient::from_shared_connection(
            mon_conn.clone(),
            BusTarget::Address(bus.address.clone()),
        );
        let mut log_dedup = ErrorLogDeduplicator::default();

        // Sample 1 (warming up): emits Degraded
        assert_eq!(
            sample_once(
                &mut hw_client,
                &cache,
                Duration::from_millis(500),
                Some(iface_ref.signal_emitter()),
                Some(&notify),
                &mut log_dedup,
            )
            .await,
            SampleOutcome::Success
        );
        let env1: MonitoringMap = time::timeout(Duration::from_secs(2), signals.next())
            .await
            .unwrap()
            .unwrap()
            .body()
            .deserialize()
            .unwrap();
        assert!(get_bool(&env1, "success"));
        assert_eq!(get_string(&env1, "readiness"), "degraded");

        // Sample 2 (deltas ready): transitions Degraded -> Ready and emits StateChanged
        *ctrl.payload.write().await = full_telemetry_data(23.5, false);
        assert_eq!(
            sample_once(
                &mut hw_client,
                &cache,
                Duration::from_millis(500),
                Some(iface_ref.signal_emitter()),
                Some(&notify),
                &mut log_dedup,
            )
            .await,
            SampleOutcome::Success
        );
        let env2: MonitoringMap = time::timeout(Duration::from_secs(2), signals.next())
            .await
            .unwrap()
            .unwrap()
            .body()
            .deserialize()
            .unwrap();
        assert!(get_bool(&env2, "success"));
        assert_eq!(get_string(&env2, "readiness"), "ready");

        // Sample 3 (identical telemetry, only timestamp changes): NO duplicate signal
        assert_eq!(
            sample_once(
                &mut hw_client,
                &cache,
                Duration::from_millis(500),
                Some(iface_ref.signal_emitter()),
                Some(&notify),
                &mut log_dedup,
            )
            .await,
            SampleOutcome::Success
        );
        assert!(
            time::timeout(Duration::from_millis(80), signals.next())
                .await
                .is_err(),
            "timestamp-only refresh must not emit StateChanged"
        );

        // Now stop sampling and wait past max_sample_age (180ms) WITHOUT calling GetState:
        // passive subscriber MUST receive proactive stale StateChanged notification!
        let stale_msg = time::timeout(Duration::from_secs(2), signals.next())
            .await
            .expect("passive subscriber must receive proactive staleness StateChanged")
            .unwrap();
        let stale_env: MonitoringMap = stale_msg.body().deserialize().unwrap();
        assert!(!get_bool(&stale_env, "success"));
        assert_eq!(get_string(&stale_env, "code"), "not-available");
        assert_eq!(get_string(&stale_env, "readiness"), "dependency-failed");
        assert!(get_data(&stale_env).is_empty());

        // Subsequent GetState also refuses stale data
        let polled: MonitoringMap = sub_proxy.call("GetState", &()).await.unwrap();
        assert!(!get_bool(&polled, "success"));
        assert_eq!(get_string(&polled, "code"), "not-available");
        assert!(get_data(&polled).is_empty());

        assert_eq!(ctrl.state_calls.load(Ordering::SeqCst), 0);
        watchdog.abort();
    }

    // Scenarios 16 & transport disconnect: Hardware restart keeps Monitoring bus name/interface intact
    // and auto-recovers; real D-Bus daemon drop sets `is_closed()` / resolves `closed()` and
    // standalone client reconnects cleanly.
    #[tokio::test]
    async fn private_bus_hardware_restart_timeout_and_transport_drop_behavior() {
        let bus = PrivateBus::start();
        let cache = Arc::new(RwLock::new(TelemetryCache::new(Duration::from_secs(6))));
        let mon_conn = serve_monitoring(&bus.address, Arc::clone(&cache)).await;
        let iface_ref = mon_conn
            .object_server()
            .interface::<_, MonitoringService>(MONITORING_PATH)
            .await
            .unwrap();

        let client_conn = connect_private(&bus.address).await;
        let proxy = monitoring_proxy(&client_conn).await;

        let mut hw_client = DbusHardwareClient::new(BusTarget::Address(bus.address.clone()));
        let mut log_dedup = ErrorLogDeduplicator::default();

        // Hardware unavailable
        assert_eq!(
            sample_once(
                &mut hw_client,
                &cache,
                Duration::from_millis(300),
                Some(iface_ref.signal_emitter()),
                None,
                &mut log_dedup,
            )
            .await,
            SampleOutcome::Failed(SampleErrorKind::Unavailable)
        );
        let unavail: MonitoringMap = proxy.call("GetState", &()).await.unwrap();
        assert!(!get_bool(&unavail, "success"));
        assert_eq!(get_string(&unavail, "code"), "not-available");
        assert!(get_data(&unavail).is_empty());

        // Hardware starts -> Ready, reuses 1 connection + 1 healthy proxy
        let ctrl1 = MockHardwareControl::new(full_telemetry_data(18.0, false));
        let hw_conn1 = serve_mock_hardware(&bus.address, Arc::clone(&ctrl1)).await;
        assert_eq!(
            sample_once(
                &mut hw_client,
                &cache,
                Duration::from_millis(500),
                Some(iface_ref.signal_emitter()),
                None,
                &mut log_dedup,
            )
            .await,
            SampleOutcome::Success
        );
        let c1 = hw_client.connect_count();
        let p1 = hw_client.proxy_build_count();
        assert_eq!(c1, 1);
        for _ in 0..2 {
            assert_eq!(
                sample_once(
                    &mut hw_client,
                    &cache,
                    Duration::from_millis(500),
                    Some(iface_ref.signal_emitter()),
                    None,
                    &mut log_dedup,
                )
                .await,
                SampleOutcome::Success
            );
        }
        assert_eq!(hw_client.connect_count(), c1);
        assert_eq!(hw_client.proxy_build_count(), p1);

        // Hardware timeout -> structured timeout error, empty data
        ctrl1.delay_ms.store(350, Ordering::SeqCst);
        assert_eq!(
            sample_once(
                &mut hw_client,
                &cache,
                Duration::from_millis(100),
                Some(iface_ref.signal_emitter()),
                None,
                &mut log_dedup,
            )
            .await,
            SampleOutcome::Failed(SampleErrorKind::Timeout)
        );
        let timed: MonitoringMap = proxy.call("GetState", &()).await.unwrap();
        assert!(!get_bool(&timed, "success"));
        assert_eq!(get_string(&timed, "code"), "timeout");
        assert!(get_data(&timed).is_empty());

        // Hardware stops -> Monitoring server connection & bus name remain alive
        ctrl1.delay_ms.store(0, Ordering::SeqCst);
        hw_conn1.close().await.unwrap();
        assert!(!mon_conn.is_closed());
        assert_eq!(
            sample_once(
                &mut hw_client,
                &cache,
                Duration::from_millis(300),
                Some(iface_ref.signal_emitter()),
                None,
                &mut log_dedup,
            )
            .await,
            SampleOutcome::Failed(SampleErrorKind::Unavailable)
        );
        assert!(!mon_conn.is_closed());

        // Hardware restarts -> auto-recovery with fresh data and zero GetState calls
        let ctrl2 = MockHardwareControl::new(full_telemetry_data(44.0, false));
        let hw_conn2 = serve_mock_hardware(&bus.address, Arc::clone(&ctrl2)).await;
        assert_eq!(
            sample_once(
                &mut hw_client,
                &cache,
                Duration::from_millis(500),
                Some(iface_ref.signal_emitter()),
                None,
                &mut log_dedup,
            )
            .await,
            SampleOutcome::Success
        );
        let rec: MonitoringMap = proxy.call("GetState", &()).await.unwrap();
        assert!(get_bool(&rec, "success"));
        assert_eq!(
            f64::try_from(&get_data(&rec)["cpu_usage_percent"]).unwrap(),
            44.0
        );
        assert_eq!(ctrl1.state_calls.load(Ordering::SeqCst), 0);
        assert_eq!(ctrl2.state_calls.load(Ordering::SeqCst), 0);

        // Real D-Bus transport daemon drop -> server detects closed() and standalone client reconnects
        drop(hw_conn2);
        drop(bus);
        time::timeout(Duration::from_secs(2), mon_conn.closed())
            .await
            .expect("server connection must detect bus transport loss");
        assert!(mon_conn.is_closed());

        assert_eq!(
            sample_once(
                &mut hw_client,
                &cache,
                Duration::from_millis(300),
                None,
                None,
                &mut log_dedup,
            )
            .await,
            SampleOutcome::Failed(SampleErrorKind::Unavailable)
        );

        let bus2 = PrivateBus::start();
        let ctrl3 = MockHardwareControl::new(full_telemetry_data(61.0, false));
        let _hw_conn3 = serve_mock_hardware(&bus2.address, Arc::clone(&ctrl3)).await;
        hw_client.target = BusTarget::Address(bus2.address.clone());
        assert_eq!(
            sample_once(
                &mut hw_client,
                &cache,
                Duration::from_millis(500),
                None,
                None,
                &mut log_dedup,
            )
            .await,
            SampleOutcome::Success
        );
        assert_eq!(hw_client.connect_count(), 2);
        let after_reconnect = cache.read().await.get_state_envelope(Instant::now());
        assert!(get_bool(&after_reconnect, "success"));
        assert_eq!(
            f64::try_from(&get_data(&after_reconnect)["cpu_usage_percent"]).unwrap(),
            61.0
        );
    }

    // Scenarios 17 & 18: Multiple concurrent clients share ONE non-overlapping GetTelemetry loop,
    // with zero GetState calls.
    #[tokio::test]
    async fn private_bus_multiple_concurrent_clients_share_single_get_telemetry_loop() {
        let bus = PrivateBus::start();
        let ctrl = MockHardwareControl::new(full_telemetry_data(35.0, false));
        ctrl.delay_ms.store(30, Ordering::SeqCst);
        let _hw_conn = serve_mock_hardware(&bus.address, Arc::clone(&ctrl)).await;

        let config = SamplerConfig {
            sample_interval: Duration::from_millis(120),
            retry_interval: Duration::from_millis(120),
            incompatible_retry_interval: Duration::from_secs(5),
            sample_timeout: Duration::from_millis(500),
            max_sample_age: Duration::from_secs(2),
        };
        let cache = Arc::new(RwLock::new(TelemetryCache::new(config.max_sample_age)));
        let mon_conn = serve_monitoring(&bus.address, Arc::clone(&cache)).await;

        let hw_client = DbusHardwareClient::from_shared_connection(
            mon_conn.clone(),
            BusTarget::Address(bus.address.clone()),
        );
        let sampler = tokio::spawn(run_sampler(
            hw_client,
            Arc::clone(&cache),
            config,
            mon_conn.clone(),
        ));

        let probe_conn = connect_private(&bus.address).await;
        let probe_proxy = monitoring_proxy(&probe_conn).await;
        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            let env: MonitoringMap = probe_proxy.call("GetState", &()).await.unwrap();
            if get_bool(&env, "success") {
                break;
            }
            assert!(Instant::now() < deadline, "sampler never became ready");
            time::sleep(Duration::from_millis(20)).await;
        }

        // 8 concurrent clients * 6 calls = 48 GetState calls
        let mut handles = Vec::new();
        for _ in 0..8 {
            let addr = bus.address.clone();
            handles.push(tokio::spawn(async move {
                let conn = connect_private(&addr).await;
                let proxy = monitoring_proxy(&conn).await;
                for _ in 0..6 {
                    let env: MonitoringMap = proxy.call("GetState", &()).await.unwrap();
                    assert!(get_bool(&env, "success"));
                    assert_eq!(get_string(&env, "readiness"), "ready");
                    let data = get_data(&env);
                    assert_eq!(data.len(), 24); // 25 minus default-absent ping_ms
                    assert_eq!(f64::try_from(&data["cpu_usage_percent"]).unwrap(), 35.0);
                    time::sleep(Duration::from_millis(15)).await;
                }
            }));
        }

        for h in handles {
            h.await.unwrap();
        }
        sampler.abort();

        assert_eq!(ctrl.max_concurrent.load(Ordering::SeqCst), 1);
        assert_eq!(ctrl.state_calls.load(Ordering::SeqCst), 0);
        let tele_calls = ctrl.telemetry_calls.load(Ordering::SeqCst);
        assert!(
            (1..=6).contains(&tele_calls),
            "expected single shared loop (<=6 GetTelemetry calls), got {tele_calls}"
        );
    }

    // Scenario 19: Control Center & Diagnostics backwards compatibility + D-Bus error classification
    #[tokio::test]
    async fn private_bus_control_center_diagnostics_and_dbus_error_classification() {
        let auth_err = zbus::Error::FDO(Box::new(zbus::fdo::Error::AccessDenied(
            "policy rejected".to_string(),
        )));
        assert_eq!(
            classify_zbus_error(&auth_err),
            (SampleErrorKind::Failed, false)
        );
        let unknown_method = zbus::Error::FDO(Box::new(zbus::fdo::Error::UnknownMethod(
            "no GetTelemetry".to_string(),
        )));
        assert_eq!(
            classify_zbus_error(&unknown_method),
            (SampleErrorKind::Incompatible, false)
        );

        let bus = PrivateBus::start();
        let ctrl = MockHardwareControl::new(full_telemetry_data(19.2, false));
        let _hw_conn = serve_mock_hardware(&bus.address, Arc::clone(&ctrl)).await;

        let cache = Arc::new(RwLock::new(TelemetryCache::new(Duration::from_secs(6))));
        let mon_conn = serve_monitoring(&bus.address, Arc::clone(&cache)).await;
        let iface_ref = mon_conn
            .object_server()
            .interface::<_, MonitoringService>(MONITORING_PATH)
            .await
            .unwrap();

        let mut hw_client = DbusHardwareClient::from_shared_connection(
            mon_conn.clone(),
            BusTarget::Address(bus.address.clone()),
        );
        let mut log_dedup = ErrorLogDeduplicator::default();
        assert_eq!(
            sample_once(
                &mut hw_client,
                &cache,
                Duration::from_millis(500),
                Some(iface_ref.signal_emitter()),
                None,
                &mut log_dedup,
            )
            .await,
            SampleOutcome::Success
        );

        let cc_conn = connect_private(&bus.address).await;
        let proxy = monitoring_proxy(&cc_conn).await;
        let result: MonitoringMap = proxy.call("GetState", &()).await.unwrap();

        let mut fields: Vec<String> = result.keys().cloned().collect();
        fields.sort();
        for required_key in ["success", "code", "message", "operationId", "data", "readiness"] {
            assert!(fields.iter().any(|k| k == required_key));
        }
        let diag_check = serde_json::json!({
            "service": MONITORING_SERVICE,
            "reachable": true,
            "stateAvailable": true,
            "fields": fields,
        });
        assert_eq!(diag_check["reachable"], true);

        assert!(get_bool(&result, "success"));
        assert_eq!(get_string(&result, "code"), "ok");
        assert_eq!(get_string(&result, "readiness"), "ready");

        let data = get_data(&result);
        // Existing Control Center Performance tab metrics (ping_ms intentionally absent by default)
        for cc_key in [
            "cpu_usage_percent",
            "cpu_temperature_c",
            "cpu_frequency_ghz",
            "gpu_usage_percent",
            "gpu_temperature_c",
            "gpu_vram_used_gb",
            "gpu_vram_total_gb",
            "memory_used_gb",
            "memory_total_gb",
            "storage_used_gb",
            "storage_capacity_gb",
            "storage_temperature_c",
            "network_download_mbps",
            "network_upload_mbps",
        ] {
            let value = f64::try_from(&data[cc_key]).expect("Control Center metric must be f64");
            assert!(value.is_finite());
        }
        assert!(!data.contains_key("network_ping_ms"));
        assert_eq!(ctrl.state_calls.load(Ordering::SeqCst), 0);
    }
}
