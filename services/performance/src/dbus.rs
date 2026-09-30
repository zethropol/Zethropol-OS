use std::collections::HashMap;

use serde_json::Value;
use zbus::interface;
use zbus::zvariant::{OwnedValue, Str};

const HARDWARE_SERVICE: &str = "org.zethropol.Hardware";
const HARDWARE_PATH: &str = "/org/zethropol/Hardware";
const HARDWARE_INTERFACE: &str = "org.zethropol.Hardware.v1";

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

async fn read_hardware_state() -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
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

    proxy
        .call("GetState", &())
        .await
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))
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
        section
            .get(name)
            .and_then(|value| f64::try_from(value).ok())
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
    async fn get_state(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let hardware_result = read_hardware_state().await?;

        let hardware_data = hardware_result
            .get("data")
            .ok_or_else(|| zbus::fdo::Error::Failed("hardware state has no data".to_string()))?;

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
