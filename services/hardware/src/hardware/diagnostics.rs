use crate::hardware::storage::StorageInfo;
use serde::Deserialize;
use std::process::Command;

#[derive(Deserialize)]
struct SmartctlOutput {
    smart_status: Option<SmartStatus>,
    nvme_smart_health_information_log: Option<NvmeHealth>,
    nvme_self_test_log: Option<NvmeSelfTestLog>,
}

#[derive(Deserialize)]
struct SmartStatus {
    passed: bool,
}

#[derive(Deserialize)]
struct NvmeHealth {
    critical_warning: u64,
    temperature: f64,
    available_spare: f64,
    percentage_used: f64,
    unsafe_shutdowns: u64,
    media_errors: u64,
    num_err_log_entries: u64,
}

#[derive(Deserialize)]
struct NvmeSelfTestLog {
    current_self_test_operation: SmartctlSelfTestOperation,
}

#[derive(Deserialize)]
struct SmartctlSelfTestOperation {
    string: String,
}

pub struct StorageDiagnostics {
    pub health_status: String,
    pub temperature_c: Option<f64>,
    pub percentage_used: Option<f64>,
    pub available_spare_percent: Option<f64>,
    pub media_data_integrity_errors: Option<u64>,
    pub unsafe_shutdowns: Option<u64>,
    pub error_log_entries: Option<u64>,
    pub self_test_status: String,
}

pub fn assess_storage(storage: Option<&StorageInfo>) -> StorageDiagnostics {
    let device_path = match storage {
        Some(storage) => &storage.device_path,
        None => return unavailable_storage_diagnostics("storage device unavailable"),
    };

    let output = match Command::new("pkexec")
        .args(["/usr/libexec/zethropol/zethropol-diagnostics-helper", device_path])
        .output() {
        Ok(output) => output,
        Err(_) => return unavailable_storage_diagnostics("smartctl unavailable"),
    };

    let data: SmartctlOutput = match serde_json::from_slice(&output.stdout) {
        Ok(data) => data,
        Err(_) => return unavailable_storage_diagnostics("smartctl JSON unavailable"),
    };

    let health_status = match (data.smart_status, data.nvme_smart_health_information_log.as_ref()) {
        (Some(status), Some(health)) if status.passed && health.critical_warning == 0 => "ok: SMART health passed".to_string(),
        (_, Some(health)) if health.critical_warning != 0 => format!("warning: SMART critical warning 0x{:02x}", health.critical_warning),
        (Some(_), _) => "warning: SMART health failed".to_string(),
        _ => "unknown: SMART health unavailable".to_string(),
    };

    let health = data.nvme_smart_health_information_log;
    let temperature_c = health.as_ref().map(|value| value.temperature);
    let percentage_used = health.as_ref().map(|value| value.percentage_used);
    let available_spare_percent = health.as_ref().map(|value| value.available_spare);
    let media_data_integrity_errors = health.as_ref().map(|value| value.media_errors);
    let unsafe_shutdowns = health.as_ref().map(|value| value.unsafe_shutdowns);
    let error_log_entries = health.as_ref().map(|value| value.num_err_log_entries);

    let self_test_status = data.nvme_self_test_log
        .map(|value| value.current_self_test_operation.string)
        .unwrap_or_else(|| "unknown: self-test status unavailable".to_string());

    StorageDiagnostics {
        health_status,
        temperature_c,
        percentage_used,
        available_spare_percent,
        media_data_integrity_errors,
        unsafe_shutdowns,
        error_log_entries,
        self_test_status,
    }
}

fn unavailable_storage_diagnostics(reason: &str) -> StorageDiagnostics {
    StorageDiagnostics {
        health_status: format!("unknown: {}", reason),
        temperature_c: None,
        percentage_used: None,
        available_spare_percent: None,
        media_data_integrity_errors: None,
        unsafe_shutdowns: None,
        error_log_entries: None,
        self_test_status: "unknown".to_string(),
    }
}
