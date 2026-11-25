use std::collections::HashMap;
use std::sync::Arc;

use crate::hardware::cpu::read as read_cpu;
use crate::hardware::diagnostics::assess_storage;
use crate::hardware::gpu::read_all as read_gpus;
use crate::hardware::intelligence::{assess, assess_hardware_change, inventory_fingerprint};
use crate::hardware::memory::read as read_memory;
use crate::hardware::network::read as read_network;
use crate::hardware::power::PowerInfo;
use crate::hardware::state::NormalizedHardwareState;
use crate::hardware::storage::read as read_storage;
use crate::hardware::system::read as read_system;
use crate::hardware::telemetry::{TelemetryConfig, TelemetryEngine};
use serde_json::Value;
use zbus::interface;
use zbus::zvariant::{OwnedValue, Str};

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
        Value::Array(values) => {
            let values: Vec<OwnedValue> = values.into_iter().filter_map(value_to_owned).collect();
            Some(OwnedValue::try_from(zbus::zvariant::Array::from(values)).ok()?)
        }
    }
}

pub fn collect_state() -> NormalizedHardwareState {
    let cpu = read_cpu();
    let gpus = read_gpus();
    let gpu = gpus.first();
    let memory = read_memory();
    let storage = read_storage();
    let network = read_network();
    let power = PowerInfo::read();
    let system = read_system();

    let assessment = assess(&cpu, gpu, &memory, storage.as_ref());

    let fingerprint = inventory_fingerprint(&cpu, &gpus, &memory, storage.as_ref());

    let hardware_change_status = assess_hardware_change(&fingerprint);
    let storage_diagnostics = assess_storage(storage.as_ref());

    let mut state = NormalizedHardwareState::from_assessment(
        &cpu,
        &memory,
        &assessment,
        gpu,
        storage.as_ref(),
        &network,
        &storage_diagnostics,
        &power,
        hardware_change_status,
        &system,
    );
    state.gpus = gpus
        .iter()
        .map(|gpu| {
            crate::hardware::state::NormalizedGpuState::from_gpu(
                Some(gpu),
                "unknown: per-device graphics runtime unverified",
            )
        })
        .collect();
    state
}

fn ok_envelope(data: OwnedValue) -> HashMap<String, OwnedValue> {
    let mut result = HashMap::with_capacity(5);
    result.insert("success".to_string(), OwnedValue::from(true));
    result.insert("code".to_string(), OwnedValue::from(Str::from("ok")));
    result.insert("message".to_string(), OwnedValue::from(Str::from("")));
    result.insert("operationId".to_string(), OwnedValue::from(Str::from("")));
    result.insert("data".to_string(), data);
    result
}

pub struct HardwareService {
    telemetry: Arc<TelemetryEngine>,
}

impl HardwareService {
    pub fn new() -> Self {
        Self {
            telemetry: Arc::new(TelemetryEngine::new(TelemetryConfig::default())),
        }
    }

    #[cfg(test)]
    pub fn with_telemetry(telemetry: Arc<TelemetryEngine>) -> Self {
        Self { telemetry }
    }
}

#[interface(name = "org.zethropol.Hardware.v1")]
impl HardwareService {
    async fn get_state(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let state = tokio::task::spawn_blocking(collect_state)
            .await
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        let json = serde_json::to_value(&state)
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        let data = value_to_owned(json).ok_or_else(|| {
            zbus::fdo::Error::Failed("failed to encode hardware state".to_string())
        })?;

        Ok(ok_envelope(data))
    }

    async fn get_telemetry(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let telemetry = self
            .telemetry
            .sample()
            .await
            .map_err(zbus::fdo::Error::Failed)?;

        let json = serde_json::to_value(&telemetry)
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        let data = value_to_owned(json).ok_or_else(|| {
            zbus::fdo::Error::Failed("failed to encode hardware telemetry".to_string())
        })?;

        Ok(ok_envelope(data))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{BufRead, BufReader};
    use std::process::{Child, Command, Stdio};
    use std::time::{Duration, Instant};
    use zbus::proxy::CacheProperties;

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
                .expect("failed to spawn private dbus-daemon");
            let stdout = child.stdout.take().expect("missing stdout");
            let mut reader = BufReader::new(stdout);
            let mut address = String::new();
            reader.read_line(&mut address).unwrap();
            let address = address.trim().to_string();
            assert!(!address.is_empty());
            Self { child, address }
        }
    }

    impl Drop for PrivateBus {
        fn drop(&mut self) {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }

    #[test]
    fn inventory_arrays_are_encoded_and_null_sensors_are_omitted() {
        let value = value_to_owned(serde_json::json!({"gpus":[{"model":"Intel", "temperature_c":null}, {"model":"NVIDIA"}]})).unwrap();
        let map: HashMap<String, OwnedValue> = value.try_into().unwrap();
        let array = zbus::zvariant::Array::try_from(map["gpus"].try_clone().unwrap()).unwrap();
        assert_eq!(array.len(), 2);
        assert_eq!(array.signature().to_string(), "av");
        assert!(value_to_owned(Value::Null).is_none());
    }

    #[tokio::test]
    async fn private_bus_get_telemetry_contract_coalescing_and_no_heavy_inventory() {
        let bus = PrivateBus::start();
        let engine = Arc::new(TelemetryEngine::new(TelemetryConfig {
            coalesce_ttl: Duration::from_millis(300),
            ..TelemetryConfig::default()
        }));
        let service = HardwareService::with_telemetry(Arc::clone(&engine));

        let _server = zbus::connection::Builder::address(bus.address.as_str())
            .unwrap()
            .name("org.zethropol.Hardware")
            .unwrap()
            .serve_at("/org/zethropol/Hardware", service)
            .unwrap()
            .build()
            .await
            .unwrap();

        let client_conn = zbus::connection::Builder::address(bus.address.as_str())
            .unwrap()
            .build()
            .await
            .unwrap();
        let proxy = zbus::proxy::Builder::<zbus::Proxy<'static>>::new(&client_conn)
            .destination("org.zethropol.Hardware")
            .unwrap()
            .path("/org/zethropol/Hardware")
            .unwrap()
            .interface("org.zethropol.Hardware.v1")
            .unwrap()
            .cache_properties(CacheProperties::No)
            .build()
            .await
            .unwrap();

        // First call establishes CPU/Network baselines without blocking sleep
        let t0 = Instant::now();
        let first: HashMap<String, OwnedValue> = proxy.call("GetTelemetry", &()).await.unwrap();
        let elapsed_first = t0.elapsed();
        assert!(
            elapsed_first < Duration::from_millis(200),
            "GetTelemetry must not perform blocking sleeps, took {:?}",
            elapsed_first
        );

        assert_eq!(bool::try_from(&first["success"]).unwrap(), true);
        assert_eq!(
            String::try_from(first["code"].try_clone().unwrap()).unwrap(),
            "ok"
        );
        let data: HashMap<String, OwnedValue> =
            first["data"].try_clone().unwrap().try_into().unwrap();

        // Expected 5 telemetry sections present
        for section in ["cpu", "gpu", "memory", "normalized_storage", "network"] {
            assert!(data.contains_key(section), "missing section {section}");
        }
        // Heavy inventory sections MUST NOT leak into GetTelemetry
        for heavy_section in [
            "firmware",
            "motherboard",
            "diagnostics",
            "capabilities",
            "health",
            "changes",
            "system",
            "power",
            "gpus",
        ] {
            assert!(
                !data.contains_key(heavy_section),
                "heavy inventory section {heavy_section} leaked into GetTelemetry"
            );
        }

        // First call must not fake CPU usage or external ping
        let cpu_map: HashMap<String, OwnedValue> =
            data["cpu"].try_clone().unwrap().try_into().unwrap();
        assert!(!cpu_map.contains_key("usage_percent"));
        let net_map: HashMap<String, OwnedValue> =
            data["network"].try_clone().unwrap().try_into().unwrap();
        assert!(!net_map.contains_key("ping_ms"));

        // Storage filesystem metrics exist on Linux root mount
        let storage_map: HashMap<String, OwnedValue> =
            data["normalized_storage"].try_clone().unwrap().try_into().unwrap();
        assert!(storage_map.contains_key("filesystem_total_gb"));
        assert!(storage_map.contains_key("filesystem_used_gb"));
        assert!(storage_map.contains_key("filesystem_free_gb"));
        assert!(storage_map.contains_key("filesystem_available_gb"));

        // 6 concurrent calls within coalesce_ttl must reuse the single cached scan
        let scans_before = engine.scan_count();
        assert_eq!(scans_before, 1);
        let mut handles = Vec::new();
        for _ in 0..6 {
            let p = proxy.clone();
            handles.push(tokio::spawn(async move {
                let env: HashMap<String, OwnedValue> = p.call("GetTelemetry", &()).await.unwrap();
                assert_eq!(bool::try_from(&env["success"]).unwrap(), true);
            }));
        }
        for h in handles {
            h.await.unwrap();
        }
        assert_eq!(
            engine.scan_count(),
            scans_before,
            "concurrent calls within coalesce window must share a single hardware scan"
        );

        // After coalesce_ttl expires, a fresh scan runs and computes real CPU usage delta
        std::thread::sleep(Duration::from_millis(320));
        let second: HashMap<String, OwnedValue> = proxy.call("GetTelemetry", &()).await.unwrap();
        assert_eq!(engine.scan_count(), scans_before + 1);
        let data2: HashMap<String, OwnedValue> =
            second["data"].try_clone().unwrap().try_into().unwrap();
        let cpu_map2: HashMap<String, OwnedValue> =
            data2["cpu"].try_clone().unwrap().try_into().unwrap();
        let cpu_usage = f64::try_from(&cpu_map2["usage_percent"]).unwrap();
        assert!((0.0..=100.0).contains(&cpu_usage));

        // GetState remains backwards compatible on the same interface
        let full_state: HashMap<String, OwnedValue> = proxy.call("GetState", &()).await.unwrap();
        assert_eq!(bool::try_from(&full_state["success"]).unwrap(), true);
        let state_data: HashMap<String, OwnedValue> =
            full_state["data"].try_clone().unwrap().try_into().unwrap();
        for expected in ["cpu", "gpu", "gpus", "motherboard", "memory", "network", "firmware", "diagnostics"] {
            assert!(state_data.contains_key(expected), "GetState missing {expected}");
        }
        let state_net: HashMap<String, OwnedValue> =
            state_data["network"].try_clone().unwrap().try_into().unwrap();
        assert!(!state_net.contains_key("ping_ms"), "GetState must not emit external ping");

        // Concurrent GetState (slow blocking inventory) must not stall GetTelemetry
        let p_state = proxy.clone();
        let p_tele = proxy.clone();
        let state_handle = tokio::spawn(async move {
            let _: HashMap<String, OwnedValue> = p_state.call("GetState", &()).await.unwrap();
        });
        let t_tele = Instant::now();
        let tele_during_state: HashMap<String, OwnedValue> =
            p_tele.call("GetTelemetry", &()).await.unwrap();
        let tele_latency = t_tele.elapsed();
        assert_eq!(bool::try_from(&tele_during_state["success"]).unwrap(), true);
        assert!(
            tele_latency < Duration::from_millis(150),
            "GetTelemetry was unexpectedly delayed by concurrent GetState: {:?}",
            tele_latency
        );
        state_handle.await.unwrap();
    }
}
