use std::collections::HashMap;
use std::sync::Arc;

use serde_json::Value;
use tokio::sync::RwLock;
use tokio::time::{self, Duration};
use zbus::interface;
use zbus::zvariant::{OwnedValue, Str};

const HARDWARE_SERVICE: &str = "org.zethropol.Hardware";
const HARDWARE_PATH: &str = "/org/zethropol/Hardware";
const HARDWARE_INTERFACE: &str = "org.zethropol.Hardware.v1";

pub type MonitoringState = HashMap<String, OwnedValue>;
pub type SharedState = Arc<RwLock<Option<MonitoringState>>>;

async fn read_hardware_state() -> zbus::fdo::Result<MonitoringState> {
    let connection = zbus::Connection::system()
        .await
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

    let proxy = zbus::Proxy::new(
        &connection,
        HARDWARE_SERVICE,
        HARDWARE_PATH,
        HARDWARE_INTERFACE,
    )
    .await
    .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

    let result: HashMap<String, OwnedValue> = proxy
        .call("GetState", &())
        .await
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

    let data = result
        .get("data")
        .ok_or_else(|| zbus::fdo::Error::Failed("hardware state has no data".to_string()))?;

    data.try_clone()
        .and_then(HashMap::try_from)
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))
}

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

fn extract_monitoring_state(hardware: MonitoringState) -> zbus::fdo::Result<MonitoringState> {
    let section = |name: &str| -> Option<HashMap<String, OwnedValue>> {
        hardware
            .get(name)
            .and_then(|value| value.try_clone().ok())
            .and_then(|value| HashMap::try_from(value).ok())
    };

    let number = |section: &HashMap<String, OwnedValue>, name: &str| -> Option<f64> {
        section.get(name).and_then(|value| f64::try_from(value).ok())
    };

    let mut monitoring = serde_json::Map::new();

    if let Some(cpu) = section("cpu") {
        if let Some(value) = number(&cpu, "usage_percent") { monitoring.insert("cpu_usage_percent".into(), Value::from(value)); }
        if let Some(value) = number(&cpu, "temperature_c") { monitoring.insert("cpu_temperature_c".into(), Value::from(value)); }
        if let Some(value) = number(&cpu, "frequency_ghz") { monitoring.insert("cpu_frequency_ghz".into(), Value::from(value)); }
    }

    if let Some(gpu) = section("gpu") {
        if let Some(value) = number(&gpu, "usage_percent") { monitoring.insert("gpu_usage_percent".into(), Value::from(value)); }
        if let Some(value) = number(&gpu, "memory_usage_percent") { monitoring.insert("gpu_memory_usage_percent".into(), Value::from(value)); }
        if let Some(value) = number(&gpu, "core_clock_mhz") { monitoring.insert("gpu_core_clock_mhz".into(), Value::from(value)); }
        if let Some(value) = number(&gpu, "vram_used_gb") { monitoring.insert("gpu_vram_used_gb".into(), Value::from(value)); }
        if let Some(value) = number(&gpu, "vram_total_gb") { monitoring.insert("gpu_vram_total_gb".into(), Value::from(value)); }
        if let Some(value) = number(&gpu, "temperature_c") { monitoring.insert("gpu_temperature_c".into(), Value::from(value)); }
        if let Some(value) = number(&gpu, "memory_clock_mhz") { monitoring.insert("gpu_memory_clock_mhz".into(), Value::from(value)); }
    }

    if let Some(memory) = section("memory") {
        if let Some(value) = number(&memory, "used_gb") { monitoring.insert("memory_used_gb".into(), Value::from(value)); }
        if let Some(value) = number(&memory, "total_gb") { monitoring.insert("memory_total_gb".into(), Value::from(value)); }
    }

    if let Some(storage) = section("normalized_storage") {
        if let Some(value) = number(&storage, "used_gb") { monitoring.insert("storage_used_gb".into(), Value::from(value)); }
        if let Some(value) = number(&storage, "capacity_gb") { monitoring.insert("storage_capacity_gb".into(), Value::from(value)); }
        if let Some(value) = number(&storage, "available_gb") { monitoring.insert("storage_available_gb".into(), Value::from(value)); }
        if let Some(value) = number(&storage, "temperature_c") { monitoring.insert("storage_temperature_c".into(), Value::from(value)); }
    }

    if let Some(network) = section("network") {
        if let Some(value) = number(&network, "download_mbps") { monitoring.insert("network_download_mbps".into(), Value::from(value)); }
        if let Some(value) = number(&network, "upload_mbps") { monitoring.insert("network_upload_mbps".into(), Value::from(value)); }
        if let Some(value) = number(&network, "ping_ms") { monitoring.insert("network_ping_ms".into(), Value::from(value)); }
    }

    let json = Value::Object(monitoring);
    let data = value_to_owned(json)
        .ok_or_else(|| zbus::fdo::Error::Failed("failed to encode monitoring state".to_string()))?;

    data.try_clone()
        .and_then(HashMap::try_from)
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))
}

pub async fn run_sampler(state: SharedState) {
    let mut interval = time::interval(Duration::from_secs(1));

    loop {
        interval.tick().await;

        match read_hardware_state().await.and_then(extract_monitoring_state) {
            Ok(snapshot) => {
                let mut current = state.write().await;
                *current = Some(snapshot);
            }
            Err(error) => {
                eprintln!("Monitoring sampler error: {error}");
            }
        }
    }
}

pub struct MonitoringService {
    pub state: SharedState,
}

#[interface(name = "org.zethropol.Monitoring.v1")]
impl MonitoringService {
    async fn get_state(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let snapshot = self.state.read().await;

        let data = snapshot
            .as_ref()
            .ok_or_else(|| zbus::fdo::Error::Failed("monitoring state is not ready".to_string()))?;

        let mut result = HashMap::new();
        result.insert("success".to_string(), OwnedValue::from(true));
        result.insert("code".to_string(), OwnedValue::from(Str::from("ok")));
        result.insert("message".to_string(), OwnedValue::from(Str::from("")));
        result.insert("operationId".to_string(), OwnedValue::from(Str::from("")));
        result.insert("data".to_string(), OwnedValue::from(data.clone()));

        Ok(result)
    }
}
