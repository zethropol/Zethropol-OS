use std::collections::HashMap;

use crate::hardware::cpu::read as read_cpu;
use crate::hardware::diagnostics::assess_storage;
use crate::hardware::gpu::read as read_gpu;
use crate::hardware::intelligence::{assess, assess_hardware_change, hardware_fingerprint};
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
        Value::Array(_) => None,
    }
}

fn collect_state() -> NormalizedHardwareState {
    let cpu = read_cpu();
    let gpu = read_gpu();
    let memory = read_memory();
    let storage = read_storage();
    let network = read_network();
    let power = PowerInfo::read();
    let system = read_system();

    let assessment = assess(
        &cpu,
        gpu.as_ref(),
        &memory,
        storage.as_ref(),
    );

    let fingerprint = hardware_fingerprint(
        &cpu,
        gpu.as_ref(),
        &memory,
        storage.as_ref(),
    );

    let hardware_change_status = assess_hardware_change(&fingerprint);
    let storage_diagnostics = assess_storage(storage.as_ref());

    NormalizedHardwareState::from_assessment(
        &cpu,
        &memory,
        &assessment,
        gpu.as_ref(),
        storage.as_ref(),
        &network,
        &storage_diagnostics,
        &power,
        hardware_change_status,
        &system,
    )
}

pub struct HardwareService;

#[interface(name = "org.zethropol.Hardware.v1")]
impl HardwareService {
    async fn get_state(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let state = collect_state();

        let json = serde_json::to_value(&state)
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        let data = value_to_owned(json)
            .ok_or_else(|| zbus::fdo::Error::Failed(
                "failed to encode hardware state".to_string()
            ))?;

        let mut result = HashMap::new();
        result.insert("success".to_string(), OwnedValue::from(true));
        result.insert(
            "code".to_string(),
            OwnedValue::from(Str::from("ok")),
        );
        result.insert(
            "message".to_string(),
            OwnedValue::from(Str::from("")),
        );
        result.insert(
            "operationId".to_string(),
            OwnedValue::from(Str::from("")),
        );
        result.insert("data".to_string(), data);

        Ok(result)
    }
}
