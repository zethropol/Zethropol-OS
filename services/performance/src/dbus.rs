use std::collections::HashMap;

use serde_json::Value;
use zbus::interface;
use zbus::proxy::CacheProperties;
use zbus::zvariant::{OwnedValue, Str};

const HARDWARE_SERVICE: &str = "org.zethropol.Hardware";
const HARDWARE_PATH: &str = "/org/zethropol/Hardware";
const HARDWARE_INTERFACE: &str = "org.zethropol.Hardware.v1";
const HARDWARE_TELEMETRY_METHOD: &str = "GetTelemetry";

fn value_to_owned(value: Value) -> Option<OwnedValue> {
    match value {
        Value::Null => None,
        Value::Bool(value) => Some(OwnedValue::from(value)),
        Value::Number(value) => {
            if let Some(value) = value.as_u64() {
                Some(OwnedValue::from(value))
            } else if let Some(value) = value.as_i64() {
                Some(OwnedValue::from(value))
            } else {
                value.as_f64().map(OwnedValue::from)
            }
        }
        Value::String(value) => Some(OwnedValue::from(Str::from(value))), 
        Value::Object(value) => {
            let mut result = HashMap::new();
            for (key, value) in value {
                if let Some(value) = value_to_owned(value) {
                    result.insert(key, value);
                }
            }
            Some(OwnedValue::from(result))
        }
        Value::Array(_) => None,
    }
}

async fn read_hardware_telemetry(
    connection: &zbus::Connection,
) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
    let proxy: zbus::Proxy<'_> = zbus::proxy::Builder::new(connection)
        .destination(HARDWARE_SERVICE)
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?
        .path(HARDWARE_PATH)
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?
        .interface(HARDWARE_INTERFACE)
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?
        .cache_properties(CacheProperties::No)
        .build()
        .await
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

    proxy
        .call(HARDWARE_TELEMETRY_METHOD, &())
        .await
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))
}

fn unwrap_hardware_envelope(
    hardware_result: &HashMap<String, OwnedValue>,
) -> zbus::fdo::Result<&OwnedValue> {
    let success = hardware_result
        .get("success")
        .and_then(|value| bool::try_from(value).ok())
        .unwrap_or(false);
    if !success {
        let code = hardware_result
            .get("code")
            .and_then(|value| value.try_clone().ok())
            .and_then(|value| String::try_from(value).ok())
            .unwrap_or_else(|| "unknown".to_string());
        let message = hardware_result
            .get("message")
            .and_then(|value| value.try_clone().ok())
            .and_then(|value| String::try_from(value).ok())
            .unwrap_or_default();
        let detail = if message.is_empty() {
            format!("hardware telemetry failed (code={code})")
        } else {
            format!("hardware telemetry failed (code={code}): {message}")
        };
        return Err(zbus::fdo::Error::Failed(detail));
    }

    hardware_result
        .get("data")
        .ok_or_else(|| zbus::fdo::Error::Failed("hardware telemetry has no data".to_string()))
}

fn extract_number(value: &OwnedValue) -> Option<f64> {
    let parsed = f64::try_from(value)
        .ok()
        .or_else(|| u64::try_from(value).ok().map(|v| v as f64))
        .or_else(|| i64::try_from(value).ok().map(|v| v as f64))
        .or_else(|| u32::try_from(value).ok().map(|v| v as f64))
        .or_else(|| i32::try_from(value).ok().map(|v| v as f64))?;
    parsed.is_finite().then_some(parsed)
}

fn extract_performance(value: &OwnedValue) -> zbus::fdo::Result<Value> {
    let root: HashMap<String, OwnedValue> = value
        .try_clone()
        .and_then(HashMap::try_from)
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

    let section = |name: &str| -> Option<HashMap<String, OwnedValue>> {
        root.get(name)
            .and_then(|value| value.try_clone().ok())
            .and_then(|value| HashMap::try_from(value).ok())
    };

    let number = |section: &HashMap<String, OwnedValue>, name: &str| -> Option<f64> {
        section.get(name).and_then(extract_number)
    };

    let mut performance = serde_json::Map::new();

    if let Some(cpu) = section("cpu") {
        if let Some(value) = number(&cpu, "usage_percent") {
            performance.insert("cpu_usage_percent".into(), Value::from(value));
        }
        if let Some(value) = number(&cpu, "temperature_c") {
            performance.insert("cpu_temperature_c".into(), Value::from(value));
        }
        if let Some(value) = number(&cpu, "frequency_ghz") {
            performance.insert("cpu_frequency_ghz".into(), Value::from(value));
        }
    }

    if let Some(gpu) = section("gpu") {
        if let Some(value) = number(&gpu, "usage_percent") {
            performance.insert("gpu_usage_percent".into(), Value::from(value));
        }
        if let Some(value) = number(&gpu, "memory_usage_percent") {
            performance.insert("gpu_memory_usage_percent".into(), Value::from(value));
        }
        if let Some(value) = number(&gpu, "core_clock_mhz") {
            performance.insert("gpu_core_clock_mhz".into(), Value::from(value));
        }
        if let Some(value) = number(&gpu, "vram_used_gb") {
            performance.insert("gpu_vram_used_gb".into(), Value::from(value));
        }
        if let Some(value) = number(&gpu, "vram_total_gb") {
            performance.insert("gpu_vram_total_gb".into(), Value::from(value));
        }
        if let Some(value) = number(&gpu, "temperature_c") {
            performance.insert("gpu_temperature_c".into(), Value::from(value));
        }
        if let Some(value) = number(&gpu, "memory_clock_mhz") {
            performance.insert("gpu_memory_clock_mhz".into(), Value::from(value));
        }
    }

    if let Some(memory) = section("memory") {
        if let Some(value) = number(&memory, "used_gb") {
            performance.insert("memory_used_gb".into(), Value::from(value));
        }
        if let Some(value) = number(&memory, "total_gb") {
            performance.insert("memory_total_gb".into(), Value::from(value));
        }
    }

    if let Some(storage) = section("normalized_storage") {
        if let Some(value) = number(&storage, "used_gb") {
            performance.insert("storage_used_gb".into(), Value::from(value));
        }
        if let Some(value) = number(&storage, "capacity_gb") {
            performance.insert("storage_capacity_gb".into(), Value::from(value));
        }
        if let Some(value) = number(&storage, "available_gb") {
            performance.insert("storage_available_gb".into(), Value::from(value));
        }
        if let Some(value) = number(&storage, "temperature_c") {
            performance.insert("storage_temperature_c".into(), Value::from(value));
        }
    }

    if let Some(network) = section("network") {
        if let Some(value) = number(&network, "download_mbps") {
            performance.insert("network_download_mbps".into(), Value::from(value));
        }
        if let Some(value) = number(&network, "upload_mbps") {
            performance.insert("network_upload_mbps".into(), Value::from(value));
        }
        if let Some(value) = number(&network, "ping_ms") {
            performance.insert("network_ping_ms".into(), Value::from(value));
        }
    }

    Ok(Value::Object(performance))
}
pub struct PerformanceService;

#[interface(name = "org.zethropol.Performance.v1")]
impl PerformanceService {
    async fn get_state(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let hardware_result = read_hardware_telemetry(connection).await?;
        let hardware_data = unwrap_hardware_envelope(&hardware_result)?;

        let performance = extract_performance(hardware_data)?;
        let json = serde_json::to_value(performance)
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        let data = value_to_owned(json)
            .ok_or_else(|| zbus::fdo::Error::Failed("failed to encode performance state".to_string()))?;

        let mut result = HashMap::new();
        result.insert("success".to_string(), OwnedValue::from(true));
        result.insert("code".to_string(), OwnedValue::from(Str::from("ok")));
        result.insert("message".to_string(), OwnedValue::from(Str::from("")));
        result.insert("operationId".to_string(), OwnedValue::from(Str::from("")));
        result.insert("data".to_string(), data);

        Ok(result)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    fn owned_object(entries: Vec<(&str, OwnedValue)>) -> OwnedValue {
        let mut map = HashMap::new();
        for (key, value) in entries {
            map.insert(key.to_string(), value);
        }
        OwnedValue::from(map)
    }

    #[test]
    fn extract_performance_projects_all_five_sections_and_19_metrics() {
        let root = owned_object(vec![
            (
                "cpu",
                owned_object(vec![
                    ("usage_percent", OwnedValue::from(42.5_f64)),
                    ("temperature_c", OwnedValue::from(61.0_f64)),
                    ("frequency_ghz", OwnedValue::from(3.85_f64)),
                    ("model", OwnedValue::from(Str::from("Ignored CPU"))),
                ]),
            ),
            (
                "gpu",
                owned_object(vec![
                    ("usage_percent", OwnedValue::from(55.0_f64)),
                    ("memory_usage_percent", OwnedValue::from(33.5_f64)),
                    ("core_clock_mhz", OwnedValue::from(1850.0_f64)),
                    ("vram_used_gb", OwnedValue::from(2.7_f64)),
                    ("vram_total_gb", OwnedValue::from(8.0_f64)),
                    ("temperature_c", OwnedValue::from(64.0_f64)),
                    ("memory_clock_mhz", OwnedValue::from(7000.0_f64)),
                ]),
            ),
            (
                "memory",
                owned_object(vec![
                    ("used_gb", OwnedValue::from(9.2_f64)),
                    ("total_gb", OwnedValue::from(32.0_f64)),
                ]),
            ),
            (
                "normalized_storage",
                owned_object(vec![
                    ("used_gb", OwnedValue::from(210.5_f64)),
                    ("capacity_gb", OwnedValue::from(1000.0_f64)),
                    ("available_gb", OwnedValue::from(789.5_f64)),
                    ("temperature_c", OwnedValue::from(39.0_f64)),
                ]),
            ),
            (
                "network",
                owned_object(vec![
                    ("download_mbps", OwnedValue::from(125.4_f64)),
                    ("upload_mbps", OwnedValue::from(18.6_f64)),
                    ("ping_ms", OwnedValue::from(11.2_f64)),
                ]),
            ),
            (
                "unrelated",
                owned_object(vec![("ignored", OwnedValue::from(99.0_f64))]),
            ),
        ]);

        let extracted = extract_performance(&root).expect("valid hardware map should extract");
        let obj = extracted.as_object().expect("result should be JSON object");

        assert_eq!(obj.len(), 19);
        assert_eq!(obj["cpu_usage_percent"], Value::from(42.5));
        assert_eq!(obj["cpu_temperature_c"], Value::from(61.0));
        assert_eq!(obj["cpu_frequency_ghz"], Value::from(3.85));
        assert_eq!(obj["gpu_usage_percent"], Value::from(55.0));
        assert_eq!(obj["gpu_memory_usage_percent"], Value::from(33.5));
        assert_eq!(obj["gpu_core_clock_mhz"], Value::from(1850.0));
        assert_eq!(obj["gpu_vram_used_gb"], Value::from(2.7));
        assert_eq!(obj["gpu_vram_total_gb"], Value::from(8.0));
        assert_eq!(obj["gpu_temperature_c"], Value::from(64.0));
        assert_eq!(obj["gpu_memory_clock_mhz"], Value::from(7000.0));
        assert_eq!(obj["memory_used_gb"], Value::from(9.2));
        assert_eq!(obj["memory_total_gb"], Value::from(32.0));
        assert_eq!(obj["storage_used_gb"], Value::from(210.5));
        assert_eq!(obj["storage_capacity_gb"], Value::from(1000.0));
        assert_eq!(obj["storage_available_gb"], Value::from(789.5));
        assert_eq!(obj["storage_temperature_c"], Value::from(39.0));
        assert_eq!(obj["network_download_mbps"], Value::from(125.4));
        assert_eq!(obj["network_upload_mbps"], Value::from(18.6));
        assert_eq!(obj["network_ping_ms"], Value::from(11.2));
    }

    #[test]
    fn extract_performance_skips_missing_sections_and_non_numeric_values() {
        let root = owned_object(vec![(
            "cpu",
            owned_object(vec![
                ("usage_percent", OwnedValue::from(Str::from("not-a-number"))),
                ("temperature_c", OwnedValue::from(52.5_f64)),
            ]),
        )]);

        let extracted = extract_performance(&root).expect("partial map should succeed");
        let obj = extracted.as_object().expect("result should be JSON object");

        assert_eq!(obj.len(), 1);
        assert_eq!(obj["cpu_temperature_c"], Value::from(52.5));
    }

    #[test]
    fn extract_performance_rejects_non_map_root() {
        let invalid = OwnedValue::from(Str::from("not-a-map"));
        assert!(extract_performance(&invalid).is_err());
    }

    #[test]
    fn value_to_owned_converts_scalars_nested_objects_and_filters_null_and_arrays() {
        assert!(value_to_owned(Value::Null).is_none());
        assert!(value_to_owned(serde_json::json!([1, 2, 3])).is_none());

        let owned = value_to_owned(serde_json::json!({
            "flag": true,
            "unsigned": 7_u64,
            "signed": -3_i64,
            "float": 1.5_f64,
            "label": "ok",
            "dropped_null": null,
            "dropped_array": [1, 2]
        }))
        .expect("object should encode");

        let decoded: HashMap<String, OwnedValue> =
            HashMap::try_from(owned).expect("decoded value should be a map");

        assert_eq!(decoded.len(), 5);
        assert_eq!(bool::try_from(&decoded["flag"]).unwrap(), true);
        assert_eq!(u64::try_from(&decoded["unsigned"]).unwrap(), 7);
        assert_eq!(i64::try_from(&decoded["signed"]).unwrap(), -3);
        assert_eq!(f64::try_from(&decoded["float"]).unwrap(), 1.5);
        assert_eq!(String::try_from(decoded["label"].try_clone().unwrap()).unwrap(), "ok");
        assert!(!decoded.contains_key("dropped_null"));
        assert!(!decoded.contains_key("dropped_array"));
    }

    #[test]
    fn deployment_dbus_policy_restricts_ownership_to_root() {
        let policy_path =
            Path::new(env!("CARGO_MANIFEST_DIR")).join("deployment/dbus/org.zethropol.Performance.conf");
        let policy = std::fs::read_to_string(&policy_path)
            .expect("D-Bus policy file should exist in deployment/dbus");

        let root_start = policy
            .find("<policy user=\"root\">")
            .expect("root policy block must exist");
        let root_end = policy[root_start..]
            .find("</policy>")
            .expect("root policy block must close")
            + root_start;
        let root_block = &policy[root_start..root_end];

        let default_start = policy
            .find("<policy context=\"default\">")
            .expect("default policy block must exist");
        let default_end = policy[default_start..]
            .find("</policy>")
            .expect("default policy block must close")
            + default_start;
        let default_block = &policy[default_start..default_end];

        assert!(root_block.contains("<allow own=\"org.zethropol.Performance\"/>"));
        assert!(!default_block.contains("allow own="));
        assert!(default_block.contains("<allow send_destination=\"org.zethropol.Performance\"/>"));
    }

    #[test]
    fn deployment_systemd_unit_retains_read_only_hardening_directives() {
        let unit_path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("deployment/systemd/zethropol-performance.service");
        let unit = std::fs::read_to_string(&unit_path)
            .expect("systemd unit file should exist in deployment/systemd");

        for required in [
            "Type=dbus",
            "BusName=org.zethropol.Performance",
            "PrivateTmp=true",
            "ProtectSystem=strict",
            "ProtectHome=true",
            "PrivateDevices=true",
            "ProtectKernelTunables=true",
            "ProtectKernelModules=true",
            "ProtectControlGroups=true",
            "NoNewPrivileges=true",
            "CapabilityBoundingSet=",
            "AmbientCapabilities=",
            "RestrictAddressFamilies=AF_UNIX",
            "RestrictNamespaces=true",
            "RestrictRealtime=true",
            "RestrictSUIDSGID=true",
            "LockPersonality=true",
            "MemoryDenyWriteExecute=true",
        ] {
            assert!(
                unit.lines().any(|line| line.trim() == required),
                "missing required systemd hardening directive: {required}"
            );
        }
        assert!(
            !unit.contains("ReadWritePaths="),
            "read-only performance service should not declare ReadWritePaths"
        );
    }

    #[test]
    fn hardware_client_uses_get_telemetry_only() {
        assert_eq!(HARDWARE_TELEMETRY_METHOD, "GetTelemetry");
    }

    #[test]
    fn unwrap_hardware_envelope_rejects_unsuccessful_or_dataless_replies() {
        let mut failed = HashMap::new();
        failed.insert("success".to_string(), OwnedValue::from(false));
        failed.insert("code".to_string(), OwnedValue::from(Str::from("hardware_error")));
        failed.insert("message".to_string(), OwnedValue::from(Str::from("sensor offline")));
        let err = unwrap_hardware_envelope(&failed).expect_err("success=false must fail");
        assert!(err.to_string().contains("hardware_error"));
        assert!(err.to_string().contains("sensor offline"));

        let mut missing_data = HashMap::new();
        missing_data.insert("success".to_string(), OwnedValue::from(true));
        missing_data.insert("code".to_string(), OwnedValue::from(Str::from("ok")));
        assert!(unwrap_hardware_envelope(&missing_data).is_err());

        let mut valid = HashMap::new();
        valid.insert("success".to_string(), OwnedValue::from(true));
        valid.insert("code".to_string(), OwnedValue::from(Str::from("ok")));
        valid.insert("data".to_string(), owned_object(vec![]));
        assert!(unwrap_hardware_envelope(&valid).is_ok());
    }

    #[test]
    fn extract_performance_coerces_integers_and_skips_non_finite_values() {
        let root = owned_object(vec![
            (
                "cpu",
                owned_object(vec![
                    ("usage_percent", OwnedValue::from(25_u64)),
                    ("temperature_c", OwnedValue::from(f64::NAN)),
                    ("frequency_ghz", OwnedValue::from(f64::INFINITY)),
                ]),
            ),
            (
                "gpu",
                owned_object(vec![
                    ("core_clock_mhz", OwnedValue::from(1200_i64)),
                    ("memory_clock_mhz", OwnedValue::from(1750_u32)),
                    ("temperature_c", OwnedValue::from(48_i32)),
                ]),
            ),
        ]);

        let extracted = extract_performance(&root).expect("mixed integer/float map should extract");
        let obj = extracted.as_object().expect("result should be JSON object");

        assert_eq!(obj.len(), 4);
        assert_eq!(obj["cpu_usage_percent"], Value::from(25.0));
        assert!(!obj.contains_key("cpu_temperature_c"));
        assert!(!obj.contains_key("cpu_frequency_ghz"));
        assert_eq!(obj["gpu_core_clock_mhz"], Value::from(1200.0));
        assert_eq!(obj["gpu_memory_clock_mhz"], Value::from(1750.0));
        assert_eq!(obj["gpu_temperature_c"], Value::from(48.0));
    }

    struct PrivateDbusDaemon {
        child: std::process::Child,
        address: String,
    }

    impl PrivateDbusDaemon {
        fn start() -> Self {
            use std::io::{BufRead, BufReader};
            use std::process::{Command, Stdio};

            let mut child = Command::new("/usr/bin/dbus-daemon")
                .args(["--session", "--nofork", "--nopidfile", "--print-address=1"])
                .stdin(Stdio::null())
                .stdout(Stdio::piped())
                .stderr(Stdio::null())
                .spawn()
                .expect("failed to spawn private dbus-daemon for test");

            let stdout = child.stdout.take().expect("dbus-daemon stdout should be piped");
            let mut reader = BufReader::new(stdout);
            let mut address = String::new();
            reader
                .read_line(&mut address)
                .expect("failed to read private dbus-daemon address");
            let address = address.trim().to_string();
            assert!(!address.is_empty(), "private dbus-daemon returned empty address");
            Self { child, address }
        }
    }

    impl Drop for PrivateDbusDaemon {
        fn drop(&mut self) {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }

    struct FakeHardwareService {
        get_telemetry_calls: std::sync::Arc<std::sync::atomic::AtomicUsize>,
        get_state_calls: std::sync::Arc<std::sync::atomic::AtomicUsize>,
    }

    #[interface(name = "org.zethropol.Hardware.v1")]
    impl FakeHardwareService {
        async fn get_telemetry(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
            self.get_telemetry_calls
                .fetch_add(1, std::sync::atomic::Ordering::SeqCst);

            let data = owned_object(vec![
                (
                    "cpu",
                    owned_object(vec![
                        ("usage_percent", OwnedValue::from(18.5_f64)),
                        ("temperature_c", OwnedValue::from(49.0_f64)),
                        ("frequency_ghz", OwnedValue::from(3.6_f64)),
                    ]),
                ),
                (
                    "memory",
                    owned_object(vec![
                        ("used_gb", OwnedValue::from(8.0_f64)),
                        ("total_gb", OwnedValue::from(32.0_f64)),
                    ]),
                ),
                (
                    "normalized_storage",
                    owned_object(vec![
                        ("used_gb", OwnedValue::from(120_u64)),
                        ("capacity_gb", OwnedValue::from(500_u64)),
                        ("available_gb", OwnedValue::from(380_u64)),
                    ]),
                ),
            ]);

            let mut envelope = HashMap::new();
            envelope.insert("success".to_string(), OwnedValue::from(true));
            envelope.insert("code".to_string(), OwnedValue::from(Str::from("ok")));
            envelope.insert("message".to_string(), OwnedValue::from(Str::from("")));
            envelope.insert("operationId".to_string(), OwnedValue::from(Str::from("")));
            envelope.insert("data".to_string(), data);
            Ok(envelope)
        }

        async fn get_state(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
            self.get_state_calls
                .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            Err(zbus::fdo::Error::Failed(
                "heavy Hardware.v1.GetState must never be called by Performance".to_string(),
            ))
        }
    }

    #[tokio::test]
    async fn private_bus_get_state_uses_hardware_get_telemetry_only() {
        use std::sync::Arc;
        use std::sync::atomic::{AtomicUsize, Ordering};

        let daemon = PrivateDbusDaemon::start();
        let get_telemetry_calls = Arc::new(AtomicUsize::new(0));
        let get_state_calls = Arc::new(AtomicUsize::new(0));

        let fake_hw = FakeHardwareService {
            get_telemetry_calls: Arc::clone(&get_telemetry_calls),
            get_state_calls: Arc::clone(&get_state_calls),
        };

        let _hw_conn = zbus::connection::Builder::address(daemon.address.as_str())
            .expect("valid private bus address")
            .name(HARDWARE_SERVICE)
            .expect("valid hardware service name")
            .serve_at(HARDWARE_PATH, fake_hw)
            .expect("register fake hardware service")
            .build()
            .await
            .expect("connect fake hardware service");

        let _perf_conn = zbus::connection::Builder::address(daemon.address.as_str())
            .expect("valid private bus address")
            .name("org.zethropol.Performance")
            .expect("valid performance service name")
            .serve_at("/org/zethropol/Performance", PerformanceService)
            .expect("register performance service")
            .build()
            .await
            .expect("connect performance service");

        let client_conn = zbus::connection::Builder::address(daemon.address.as_str())
            .expect("valid private bus address")
            .build()
            .await
            .expect("connect test client");

        let proxy: zbus::Proxy<'_> = zbus::proxy::Builder::new(&client_conn)
            .destination("org.zethropol.Performance")
            .expect("valid destination")
            .path("/org/zethropol/Performance")
            .expect("valid path")
            .interface("org.zethropol.Performance.v1")
            .expect("valid interface")
            .cache_properties(CacheProperties::No)
            .build()
            .await
            .expect("build performance client proxy");

        let reply: HashMap<String, OwnedValue> = proxy
            .call("GetState", &())
            .await
            .expect("Performance.v1.GetState should succeed via Hardware.v1.GetTelemetry");

        assert_eq!(get_telemetry_calls.load(Ordering::SeqCst), 1);
        assert_eq!(get_state_calls.load(Ordering::SeqCst), 0);
        assert_eq!(bool::try_from(&reply["success"]).unwrap(), true);
        assert_eq!(
            String::try_from(reply["code"].try_clone().unwrap()).unwrap(),
            "ok"
        );

        let data: HashMap<String, OwnedValue> =
            HashMap::try_from(reply["data"].try_clone().unwrap()).unwrap();
        assert_eq!(f64::try_from(&data["cpu_usage_percent"]).unwrap(), 18.5);
        assert_eq!(f64::try_from(&data["memory_total_gb"]).unwrap(), 32.0);
        assert_eq!(f64::try_from(&data["storage_used_gb"]).unwrap(), 120.0);
        assert_eq!(f64::try_from(&data["storage_capacity_gb"]).unwrap(), 500.0);
        assert_eq!(f64::try_from(&data["storage_available_gb"]).unwrap(), 380.0);
    }
}
