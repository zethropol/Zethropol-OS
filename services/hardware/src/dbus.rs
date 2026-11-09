use std::collections::HashMap;

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

pub struct HardwareService;

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
    #[test]
    fn inventory_arrays_are_encoded_and_null_sensors_are_omitted() {
        let value = value_to_owned(serde_json::json!({"gpus":[{"model":"Intel", "temperature_c":null}, {"model":"NVIDIA"}]})).unwrap();
        let map: HashMap<String, OwnedValue> = value.try_into().unwrap();
        let array = zbus::zvariant::Array::try_from(map["gpus"].try_clone().unwrap()).unwrap();
        assert_eq!(array.len(), 2);
        assert_eq!(array.signature().to_string(), "av");
        assert!(value_to_owned(Value::Null).is_none());
    }
}
