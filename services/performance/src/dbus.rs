use std::collections::HashMap;
use std::process::Command;
use std::sync::Mutex;

use serde_json::Value;
use zbus::interface;
use zbus::object_server::SignalEmitter;
use zbus::proxy::CacheProperties;
use zbus::zvariant::{OwnedValue, Str};

use crate::gaming::probe_gaming_readiness;
use crate::profiles::{
    PerformanceProfile, POLKIT_ACTION_MODIFY, RuntimeConfig, apply_amd_gpu_dpm, apply_cpu_governor,
    build_profile_descriptors, detect_gaming_session, evaluate_thermal_guard, load_saved_profile,
    read_amd_gpu_dpm, read_cpu_governor_info, save_profile,
};

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

fn vec_to_owned(items: &[String]) -> OwnedValue {
    let mut array = zbus::zvariant::Array::new(&zbus::zvariant::Signature::Str);
    for item in items {
        let _ = array.append(zbus::zvariant::Value::from(item.as_str()));
    }
    OwnedValue::try_from(zbus::zvariant::Value::Array(array))
        .expect("string array should encode to OwnedValue")
}

fn envelope(
    success: bool,
    code: &str,
    message: &str,
    operation_id: &str,
    data: Option<OwnedValue>,
) -> HashMap<String, OwnedValue> {
    let mut result = HashMap::new();
    result.insert("success".to_string(), OwnedValue::from(success));
    result.insert("code".to_string(), OwnedValue::from(Str::from(code)));
    result.insert("message".to_string(), OwnedValue::from(Str::from(message)));
    result.insert(
        "operationId".to_string(),
        OwnedValue::from(Str::from(operation_id)),
    );
    if let Some(d) = data {
        result.insert("data".to_string(), d);
    }
    result
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

#[derive(Default)]
struct TelemetrySnapshot {
    cpu_temp_c: Option<f64>,
    gpu_temp_c: Option<f64>,
    gpu_vram_total_gb: Option<f64>,
}

async fn read_telemetry_snapshot(connection: &zbus::Connection) -> TelemetrySnapshot {
    let Ok(hw_res) = read_hardware_telemetry(connection).await else {
        return TelemetrySnapshot::default();
    };
    let Ok(hw_data) = unwrap_hardware_envelope(&hw_res) else {
        return TelemetrySnapshot::default();
    };
    let Ok(perf) = extract_performance(hw_data) else {
        return TelemetrySnapshot::default();
    };
    let Some(obj) = perf.as_object() else {
        return TelemetrySnapshot::default();
    };
    TelemetrySnapshot {
        cpu_temp_c: obj.get("cpu_temperature_c").and_then(Value::as_f64),
        gpu_temp_c: obj.get("gpu_temperature_c").and_then(Value::as_f64),
        gpu_vram_total_gb: obj.get("gpu_vram_total_gb").and_then(Value::as_f64),
    }
}

pub struct PerformanceService {
    config: RuntimeConfig,
    active_profile: Mutex<PerformanceProfile>,
}

impl PerformanceService {
    pub fn from_env() -> Self {
        Self::with_config(RuntimeConfig::from_env())
    }

    pub fn with_config(config: RuntimeConfig) -> Self {
        let initial = load_saved_profile(&config.state_file);
        Self {
            config,
            active_profile: Mutex::new(initial),
        }
    }

    async fn authorize_modify(&self, header: &zbus::message::Header<'_>) -> Result<(), String> {
        let sender = header
            .sender()
            .ok_or_else(|| "Geçersiz D-Bus çağırıcısı".to_string())?
            .to_string();
        let pkcheck = self.config.pkcheck_bin.clone();
        if !pkcheck.is_file() {
            return Err("Polkit doğrulama aracı (pkcheck) bulunamadı".to_string());
        }

        let status = tokio::task::spawn_blocking(move || {
            Command::new(pkcheck)
                .args([
                    "--action-id",
                    POLKIT_ACTION_MODIFY,
                    "--system-bus-name",
                    &sender,
                    "--allow-user-interaction",
                ])
                .status()
        })
        .await
        .map_err(|e| format!("Polkit doğrulama görevi başarısız: {e}"))?
        .map_err(|e| format!("Polkit doğrulaması çalıştırılamadı: {e}"))?;

        if status.success() {
            Ok(())
        } else {
            Err("Performans profili değişikliği için Polkit yetkilendirmesi reddedildi.".to_string())
        }
    }

    fn build_profile_state_map(&self, snap: &TelemetrySnapshot) -> HashMap<String, OwnedValue> {
        let active = *self
            .active_profile
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let gov_info = read_cpu_governor_info(&self.config.cpufreq_dir);
        let gpu_dpm = read_amd_gpu_dpm(&self.config.drm_dir)
            .unwrap_or_else(|| "unavailable".to_string());
        let thermal = evaluate_thermal_guard(
            snap.cpu_temp_c,
            snap.gpu_temp_c,
            self.config.cpu_thermal_limit_c,
            self.config.gpu_thermal_limit_c,
        );
        let session = detect_gaming_session(&self.config.proc_dir, thermal.guard_active);
        let descriptors = build_profile_descriptors(&gov_info, &thermal);
        let profiles_json = serde_json::to_string(&descriptors).unwrap_or_else(|_| "[]".to_string());
        let profile_names: Vec<String> = PerformanceProfile::ALL
            .iter()
            .map(|p| p.as_str().to_string())
            .collect();

        let mut data = HashMap::new();
        data.insert(
            "active_profile".to_string(),
            OwnedValue::from(Str::from(active.as_str())),
        );
        data.insert(
            "default_profile".to_string(),
            OwnedValue::from(Str::from(PerformanceProfile::General.as_str())),
        );
        data.insert(
            "available_profiles".to_string(),
            vec_to_owned(&profile_names),
        );
        data.insert(
            "cpu_governor".to_string(),
            OwnedValue::from(Str::from(
                gov_info.current.as_deref().unwrap_or("unavailable"),
            )),
        );
        data.insert(
            "available_governors".to_string(),
            vec_to_owned(&gov_info.available),
        );
        data.insert(
            "gpu_dpm_level".to_string(),
            OwnedValue::from(Str::from(gpu_dpm.as_str())),
        );
        data.insert(
            "thermal_guard_active".to_string(),
            OwnedValue::from(thermal.guard_active),
        );
        data.insert(
            "thermal_guard_reason".to_string(),
            OwnedValue::from(Str::from(thermal.reason.as_str())),
        );
        data.insert(
            "thermal_cpu_limit_c".to_string(),
            OwnedValue::from(thermal.cpu_limit_c),
        );
        data.insert(
            "thermal_gpu_limit_c".to_string(),
            OwnedValue::from(thermal.gpu_limit_c),
        );
        if let Some(t) = snap.cpu_temp_c {
            data.insert("cpu_temperature_c".to_string(), OwnedValue::from(t));
        }
        if let Some(t) = snap.gpu_temp_c {
            data.insert("gpu_temperature_c".to_string(), OwnedValue::from(t));
        }
        if let Some(v) = snap.gpu_vram_total_gb {
            data.insert("gpu_vram_total_gb".to_string(), OwnedValue::from(v));
        }
        data.insert(
            "gaming_session_active".to_string(),
            OwnedValue::from(session.active),
        );
        data.insert(
            "detected_gaming_processes".to_string(),
            vec_to_owned(&session.detected_processes),
        );
        data.insert(
            "recommended_profile".to_string(),
            OwnedValue::from(Str::from(session.recommended_profile.as_str())),
        );
        data.insert(
            "gaming_session_advisory".to_string(),
            OwnedValue::from(Str::from(session.advisory.as_str())),
        );
        data.insert(
            "silent_auto_switch_blocked".to_string(),
            OwnedValue::from(session.silent_auto_switch_blocked),
        );
        data.insert(
            "profiles_json".to_string(),
            OwnedValue::from(Str::from(profiles_json.as_str())),
        );
        data
    }
}

#[interface(name = "org.zethropol.Performance.v1")]
impl PerformanceService {
    #[zbus(signal)]
    async fn profile_changed(
        emitter: &SignalEmitter<'_>,
        state: HashMap<String, OwnedValue>,
    ) -> zbus::Result<()>;

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

        Ok(envelope(true, "ok", "", "", Some(data)))
    }

    async fn get_profile_state(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let snap = read_telemetry_snapshot(connection).await;
        let data = self.build_profile_state_map(&snap);
        Ok(envelope(true, "ok", "", "", Some(OwnedValue::from(data))))
    }

    async fn set_profile(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        profile: &str,
        user_confirmed: bool,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let Some(target) = PerformanceProfile::parse(profile) else {
            return Ok(envelope(
                false,
                "invalid-profile",
                "Geçersiz performans profili. Geçerli profiller: general, gaming, developer, creator.",
                "",
                None,
            ));
        };

        if !user_confirmed {
            return Ok(envelope(
                false,
                "confirmation-required",
                "Performans profili değişikliği için açık kullanıcı onayı gereklidir; sessiz profil dayatması engellendi.",
                "",
                None,
            ));
        }

        if let Err(msg) = self.authorize_modify(&header).await {
            return Ok(envelope(false, "permission-denied", &msg, "", None));
        }

        let snap = read_telemetry_snapshot(connection).await;
        let thermal = evaluate_thermal_guard(
            snap.cpu_temp_c,
            snap.gpu_temp_c,
            self.config.cpu_thermal_limit_c,
            self.config.gpu_thermal_limit_c,
        );

        if target.thermal_guard_blocks() && thermal.guard_active {
            let state_map = self.build_profile_state_map(&snap);
            return Ok(envelope(
                false,
                "thermal-limit",
                &thermal.reason,
                "",
                Some(OwnedValue::from(state_map)),
            ));
        }

        let effective_govs: &[&str] = if target.thermal_soft_fallback() && thermal.guard_active {
            &["schedutil", "ondemand"]
        } else {
            target.preferred_governors()
        };

        let applied_gov = match apply_cpu_governor(&self.config.cpufreq_dir, effective_govs) {
            Ok(v) => v,
            Err(err) => {
                return Ok(envelope(false, "apply-failed", &err, "", None));
            }
        };

        let applied_dpm = match apply_amd_gpu_dpm(&self.config.drm_dir, target.preferred_gpu_dpm()) {
            Ok(v) => v,
            Err(err) => {
                return Ok(envelope(false, "apply-failed", &err, "", None));
            }
        };

        if let Err(err) = save_profile(&self.config.state_file, target) {
            return Ok(envelope(false, "persist-failed", &err, "", None));
        }

        {
            let mut guard = self
                .active_profile
                .lock()
                .unwrap_or_else(|e| e.into_inner());
            *guard = target;
        }

        let state_map = self.build_profile_state_map(&snap);
        let _ = Self::profile_changed(&emitter, state_map.clone()).await;

        let summary = format!(
            "{} profili etkinleştirildi (CPU governor: {}, GPU DPM: {}).",
            target.display_name(),
            applied_gov.as_deref().unwrap_or("değiştirilmedi"),
            applied_dpm.as_deref().unwrap_or("değiştirilmedi"),
        );
        Ok(envelope(
            true,
            "ok",
            &summary,
            "",
            Some(OwnedValue::from(state_map)),
        ))
    }

    async fn get_gaming_readiness(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let snap = read_telemetry_snapshot(connection).await;
        let cfg = self.config.clone();
        let report = tokio::task::spawn_blocking(move || {
            probe_gaming_readiness(&cfg, snap.gpu_vram_total_gb)
        })
        .await
        .map_err(|e| zbus::fdo::Error::Failed(e.to_string()))?;

        let readiness_json =
            serde_json::to_string(&report).unwrap_or_else(|_| "{}".to_string());

        let mut data = HashMap::new();
        data.insert(
            "matched_profile".to_string(),
            OwnedValue::from(Str::from(report.gpu_matrix.matched_profile.as_str())),
        );
        data.insert(
            "tier".to_string(),
            OwnedValue::from(Str::from(report.gpu_matrix.tier.as_str())),
        );
        data.insert(
            "tier_label".to_string(),
            OwnedValue::from(Str::from(report.gpu_matrix.tier_label.as_str())),
        );
        data.insert(
            "gpu_name".to_string(),
            OwnedValue::from(Str::from(report.gpu_matrix.gpu_name.as_str())),
        );
        data.insert(
            "driver_name".to_string(),
            OwnedValue::from(Str::from(report.gpu_matrix.driver_name.as_str())),
        );
        data.insert(
            "driver_info".to_string(),
            OwnedValue::from(Str::from(report.gpu_matrix.driver_info.as_str())),
        );
        data.insert(
            "vulkan_api_version".to_string(),
            OwnedValue::from(Str::from(report.gpu_matrix.vulkan_api_version.as_str())),
        );
        data.insert(
            "vulkan_available".to_string(),
            OwnedValue::from(report.vulkan_available),
        );
        data.insert(
            "hardware_vulkan_13".to_string(),
            OwnedValue::from(report.gpu_matrix.hardware_vulkan_13),
        );
        data.insert(
            "dxvk_supported".to_string(),
            OwnedValue::from(report.gpu_matrix.dxvk_supported),
        );
        data.insert(
            "vkd3d_supported".to_string(),
            OwnedValue::from(report.gpu_matrix.vkd3d_supported),
        );
        data.insert(
            "dxvk_available".to_string(),
            OwnedValue::from(report.dxvk_available),
        );
        data.insert(
            "dxvk_source".to_string(),
            OwnedValue::from(Str::from(report.dxvk_source.as_str())),
        );
        data.insert(
            "vkd3d_available".to_string(),
            OwnedValue::from(report.vkd3d_available),
        );
        data.insert(
            "vkd3d_source".to_string(),
            OwnedValue::from(Str::from(report.vkd3d_source.as_str())),
        );
        data.insert(
            "steam_available".to_string(),
            OwnedValue::from(report.steam_available),
        );
        data.insert(
            "proton_available".to_string(),
            OwnedValue::from(report.proton_available),
        );
        data.insert(
            "proton_source".to_string(),
            OwnedValue::from(Str::from(report.proton_source.as_str())),
        );
        data.insert(
            "gamescope_available".to_string(),
            OwnedValue::from(report.gamescope_available),
        );
        data.insert(
            "mangohud_available".to_string(),
            OwnedValue::from(report.mangohud_available),
        );
        data.insert(
            "gamemode_available".to_string(),
            OwnedValue::from(report.gamemode_available),
        );
        data.insert(
            "wine_available".to_string(),
            OwnedValue::from(report.wine_available),
        );
        data.insert(
            "limitations".to_string(),
            vec_to_owned(&report.gpu_matrix.limitations),
        );
        data.insert(
            "recommendations".to_string(),
            vec_to_owned(&report.gpu_matrix.recommendations),
        );
        data.insert(
            "missing_components".to_string(),
            vec_to_owned(&report.missing_components),
        );
        data.insert(
            "anticheat_eula_notice".to_string(),
            OwnedValue::from(Str::from(report.anticheat_eula_notice.as_str())),
        );
        data.insert(
            "kernel_anticheat_bypass_forbidden".to_string(),
            OwnedValue::from(report.kernel_anticheat_bypass_forbidden),
        );
        data.insert(
            "readiness_json".to_string(),
            OwnedValue::from(Str::from(readiness_json.as_str())),
        );

        Ok(envelope(true, "ok", "", "", Some(OwnedValue::from(data))))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gaming::{classify_gpu_matrix, parse_vulkaninfo_summary};
    use std::os::unix::fs::PermissionsExt;
    use std::path::{Path, PathBuf};

    struct TempFixture {
        root: PathBuf,
    }

    impl TempFixture {
        fn new(label: &str) -> Self {
            let unique = format!(
                "zethropol-perf-{label}-{}-{}",
                std::process::id(),
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap()
                    .as_nanos()
            );
            let root = std::env::temp_dir().join(unique);
            std::fs::create_dir_all(&root).expect("create temp fixture dir");
            Self { root }
        }

        fn write(&self, rel: &str, content: &str) -> PathBuf {
            let p = self.root.join(rel);
            if let Some(parent) = p.parent() {
                std::fs::create_dir_all(parent).unwrap();
            }
            std::fs::write(&p, content).unwrap();
            p
        }

        fn write_exec(&self, rel: &str, body: &str) -> PathBuf {
            let p = self.write(rel, body);
            let mut perms = std::fs::metadata(&p).unwrap().permissions();
            perms.set_mode(0o755);
            std::fs::set_permissions(&p, perms).unwrap();
            p
        }

        fn path(&self, rel: &str) -> PathBuf {
            self.root.join(rel)
        }
    }

    impl Drop for TempFixture {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.root);
        }
    }

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
        assert_eq!(
            String::try_from(decoded["label"].try_clone().unwrap()).unwrap(),
            "ok"
        );
        assert!(!decoded.contains_key("dropped_null"));
        assert!(!decoded.contains_key("dropped_array"));
    }

    #[test]
    fn deployment_dbus_policy_restricts_ownership_to_root() {
        let policy_path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("deployment/dbus/org.zethropol.Performance.conf");
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
    fn deployment_polkit_policy_requires_auth_admin_for_profile_modify() {
        let polkit_path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("deployment/polkit/org.zethropol.performance.policy");
        let polkit = std::fs::read_to_string(&polkit_path)
            .expect("Polkit policy file should exist in deployment/polkit");

        assert!(polkit.contains("id=\"org.zethropol.performance.modify\""));
        assert!(polkit.contains("<allow_any>auth_admin</allow_any>"));
        assert!(polkit.contains("<allow_inactive>auth_admin</allow_inactive>"));
        assert!(polkit.contains("<allow_active>auth_admin</allow_active>"));
    }

    #[test]
    fn deployment_systemd_unit_retains_narrow_hardening_directives() {
        let unit_path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("deployment/systemd/zethropol-performance.service");
        let unit = std::fs::read_to_string(&unit_path)
            .expect("systemd unit file should exist in deployment/systemd");

        for required in [
            "Type=dbus",
            "BusName=org.zethropol.Performance",
            "PrivateTmp=true",
            "ProtectSystem=strict",
            "StateDirectory=zethropol-performance",
            "ReadWritePaths=-/sys/devices/system/cpu/cpufreq -/sys/class/drm",
            "ProtectHome=true",
            "PrivateDevices=true",
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
    }

    #[test]
    fn hardware_client_uses_get_telemetry_only() {
        assert_eq!(HARDWARE_TELEMETRY_METHOD, "GetTelemetry");
    }

    #[test]
    fn unwrap_hardware_envelope_rejects_unsuccessful_or_dataless_replies() {
        let mut failed = HashMap::new();
        failed.insert("success".to_string(), OwnedValue::from(false));
        failed.insert(
            "code".to_string(),
            OwnedValue::from(Str::from("hardware_error")),
        );
        failed.insert(
            "message".to_string(),
            OwnedValue::from(Str::from("sensor offline")),
        );
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

    #[test]
    fn gpu_capability_matrix_classifies_polaris11_broadwell_vega3_and_llvmpipe() {
        let polaris_summary = "\
Devices:
========
GPU0:
\tapiVersion         = 1.4.363
\tvendorID           = 0x1002
\tdeviceID           = 0x67ef
\tdeviceType         = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
\tdeviceName         = AMD Radeon RX 560 Series (RADV POLARIS11)
\tdriverName         = radv
\tdriverInfo         = Mesa 26.3.1
";
        let rx560 = classify_gpu_matrix(&parse_vulkaninfo_summary(polaris_summary), Some(4.0));
        assert_eq!(rx560.matched_profile, "amd-rx560-polaris11");
        assert!(rx560.dxvk_supported);
        assert!(!rx560.vkd3d_supported);
        assert!(rx560.limitations.iter().any(|l| l.contains("4.00 GB")));
        assert!(rx560.limitations.iter().any(|l| l.contains("82 °C")));

        let broadwell_summary = "\
Devices:
========
GPU0:
\tapiVersion         = 1.2.295
\tvendorID           = 0x8086
\tdeviceID           = 0x1616
\tdeviceType         = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
\tdeviceName         = Intel(R) HD Graphics 5500 (BDW GT2)
\tdriverName         = Intel open-source Mesa driver
\tdriverInfo         = Mesa 26.3.1 (hasvk)
";
        let bdw = classify_gpu_matrix(&parse_vulkaninfo_summary(broadwell_summary), None);
        assert_eq!(bdw.matched_profile, "intel-broadwell-hd5500");
        assert!(!bdw.hardware_vulkan_13);
        assert!(!bdw.dxvk_supported);
        assert!(!bdw.vkd3d_supported);
        assert!(bdw.limitations.iter().any(|l| l.contains("Broadwell")));

        let vega3_summary = "\
Devices:
========
GPU0:
\tapiVersion         = 1.3.296
\tvendorID           = 0x1002
\tdeviceID           = 0x15d8
\tdeviceType         = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
\tdeviceName         = AMD Radeon Vega 3 Graphics (RADV RAVEN2)
\tdriverName         = radv
\tdriverInfo         = Mesa 26.3.1
";
        let vega3 = classify_gpu_matrix(&parse_vulkaninfo_summary(vega3_summary), Some(1.0));
        assert_eq!(vega3.matched_profile, "amd-vega3-apu");
        assert!(vega3.dxvk_supported);
        assert!(!vega3.vkd3d_supported);
        assert!(vega3.limitations.iter().any(|l| l.contains("Vega 3")));

        let llvmpipe_summary = "\
Devices:
========
GPU0:
\tapiVersion         = 1.4.363
\tdeviceType         = PHYSICAL_DEVICE_TYPE_CPU
\tdeviceName         = llvmpipe (LLVM 21.1.0, 256 bits)
\tdriverName         = llvmpipe
";
        let sw = classify_gpu_matrix(&parse_vulkaninfo_summary(llvmpipe_summary), None);
        assert_eq!(sw.matched_profile, "software-vulkan");
        assert!(!sw.dxvk_supported);
        assert!(!sw.vkd3d_supported);
    }

    #[test]
    fn gaming_session_detection_is_advisory_and_never_forces_profile() {
        let fx = TempFixture::new("proc-session");
        fx.write("proc/1001/comm", "control_center\n");
        fx.write("proc/1001/cmdline", "/usr/bin/control_center\0");
        fx.write("proc/2002/comm", "gamescope\n");
        fx.write("proc/2002/cmdline", "/usr/bin/gamescope\0-W\01920\0");

        let cool = detect_gaming_session(&fx.path("proc"), false);
        assert!(cool.active);
        assert_eq!(cool.detected_processes, vec!["gamescope".to_string()]);
        assert_eq!(cool.recommended_profile, "gaming");
        assert!(cool.silent_auto_switch_blocked);

        let hot = detect_gaming_session(&fx.path("proc"), true);
        assert!(hot.active);
        assert_eq!(hot.recommended_profile, "general");
        assert!(hot.silent_auto_switch_blocked);
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
        cpu_temp: std::sync::Arc<Mutex<f64>>,
        gpu_temp: std::sync::Arc<Mutex<f64>>,
    }

    #[interface(name = "org.zethropol.Hardware.v1")]
    impl FakeHardwareService {
        async fn get_telemetry(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
            self.get_telemetry_calls
                .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            let cpu_t = *self.cpu_temp.lock().unwrap();
            let gpu_t = *self.gpu_temp.lock().unwrap();

            let data = owned_object(vec![
                (
                    "cpu",
                    owned_object(vec![
                        ("usage_percent", OwnedValue::from(18.5_f64)),
                        ("temperature_c", OwnedValue::from(cpu_t)),
                        ("frequency_ghz", OwnedValue::from(3.6_f64)),
                    ]),
                ),
                (
                    "gpu",
                    owned_object(vec![
                        ("temperature_c", OwnedValue::from(gpu_t)),
                        ("vram_total_gb", OwnedValue::from(4.0_f64)),
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

            Ok(envelope(true, "ok", "", "", Some(data)))
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
            cpu_temp: Arc::new(Mutex::new(49.0)),
            gpu_temp: Arc::new(Mutex::new(50.0)),
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

        let fx = TempFixture::new("dbus-get-state");
        let mut cfg = RuntimeConfig::default();
        cfg.state_file = fx.path("state/profile.json");
        let _perf_conn = zbus::connection::Builder::address(daemon.address.as_str())
            .expect("valid private bus address")
            .name("org.zethropol.Performance")
            .expect("valid performance service name")
            .serve_at("/org/zethropol/Performance", PerformanceService::with_config(cfg))
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

    #[tokio::test]
    async fn private_bus_profiles_enforce_confirmation_polkit_thermal_guard_and_readiness() {
        use std::sync::Arc;
        use std::sync::atomic::AtomicUsize;

        let fx = TempFixture::new("dbus-stage07");
        fx.write("cpufreq/policy0/scaling_available_governors", "schedutil performance powersave\n");
        fx.write("cpufreq/policy0/scaling_governor", "schedutil\n");
        fx.write("drm/card1/device/power_dpm_force_performance_level", "auto\n");
        fx.write("proc/4444/comm", "wine64-preloader\n");
        fx.write("proc/4444/cmdline", "/usr/bin/wine64-preloader\0game.exe\0");

        let pkcheck_flag = fx.path("pkcheck_allow");
        let pkcheck_bin = fx.write_exec(
            "bin/mock-pkcheck",
            &format!(
                "#!/usr/bin/env bash\nif [ -f \"{}\" ]; then exit 0; else exit 1; fi\n",
                pkcheck_flag.display()
            ),
        );
        let vulkaninfo_bin = fx.write_exec(
            "bin/mock-vulkaninfo",
            "#!/usr/bin/env bash\n\
             echo 'Devices:'\n\
             echo '========'\n\
             echo 'GPU0:'\n\
             echo '  apiVersion = 1.4.363'\n\
             echo '  vendorID = 0x1002'\n\
             echo '  deviceID = 0x67ef'\n\
             echo '  deviceType = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU'\n\
             echo '  deviceName = AMD Radeon RX 560 Series (RADV POLARIS11)'\n\
             echo '  driverName = radv'\n\
             echo '  driverInfo = Mesa 26.3.1'\n",
        );
        fx.write_exec("tools/steam", "#!/usr/bin/env bash\nexit 0\n");
        fx.write_exec("tools/mangohud", "#!/usr/bin/env bash\nexit 0\n");
        fx.write("dxvk/d3d11.dll", "DXVK-STUB");

        let cfg = RuntimeConfig {
            cpufreq_dir: fx.path("cpufreq"),
            drm_dir: fx.path("drm"),
            proc_dir: fx.path("proc"),
            state_file: fx.path("state/profile.json"),
            pkcheck_bin,
            vulkaninfo_bin,
            tool_search_dirs: vec![fx.path("tools")],
            dxvk_dirs: vec![fx.path("dxvk")],
            vkd3d_dirs: vec![fx.path("vkd3d-empty")],
            proton_dirs: vec![fx.path("proton-empty")],
            cpu_thermal_limit_c: 85.0,
            gpu_thermal_limit_c: 82.0,
        };

        let daemon = PrivateDbusDaemon::start();
        let cpu_temp = Arc::new(Mutex::new(55.0_f64));
        let gpu_temp = Arc::new(Mutex::new(58.0_f64));

        let fake_hw = FakeHardwareService {
            get_telemetry_calls: Arc::new(AtomicUsize::new(0)),
            get_state_calls: Arc::new(AtomicUsize::new(0)),
            cpu_temp: Arc::clone(&cpu_temp),
            gpu_temp: Arc::clone(&gpu_temp),
        };

        let _hw_conn = zbus::connection::Builder::address(daemon.address.as_str())
            .unwrap()
            .name(HARDWARE_SERVICE)
            .unwrap()
            .serve_at(HARDWARE_PATH, fake_hw)
            .unwrap()
            .build()
            .await
            .unwrap();

        let _perf_conn = zbus::connection::Builder::address(daemon.address.as_str())
            .unwrap()
            .name("org.zethropol.Performance")
            .unwrap()
            .serve_at("/org/zethropol/Performance", PerformanceService::with_config(cfg))
            .unwrap()
            .build()
            .await
            .unwrap();

        let client_conn = zbus::connection::Builder::address(daemon.address.as_str())
            .unwrap()
            .build()
            .await
            .unwrap();

        let proxy: zbus::Proxy<'_> = zbus::proxy::Builder::new(&client_conn)
            .destination("org.zethropol.Performance")
            .unwrap()
            .path("/org/zethropol/Performance")
            .unwrap()
            .interface("org.zethropol.Performance.v1")
            .unwrap()
            .cache_properties(CacheProperties::No)
            .build()
            .await
            .unwrap();

        // 1. Default profile stays "general" even with active gaming session (no silent switch!)
        let state_reply: HashMap<String, OwnedValue> = proxy.call("GetProfileState", &()).await.unwrap();
        assert_eq!(bool::try_from(&state_reply["success"]).unwrap(), true);
        let state_data: HashMap<String, OwnedValue> =
            HashMap::try_from(state_reply["data"].try_clone().unwrap()).unwrap();
        assert_eq!(
            String::try_from(state_data["active_profile"].try_clone().unwrap()).unwrap(),
            "general"
        );
        assert_eq!(bool::try_from(&state_data["gaming_session_active"]).unwrap(), true);
        assert_eq!(bool::try_from(&state_data["silent_auto_switch_blocked"]).unwrap(), true);
        assert_eq!(
            String::try_from(state_data["recommended_profile"].try_clone().unwrap()).unwrap(),
            "gaming"
        );

        // 2. Invalid profile rejected fail-closed
        let bad_reply: HashMap<String, OwnedValue> =
            proxy.call("SetProfile", &("overclock", true)).await.unwrap();
        assert_eq!(bool::try_from(&bad_reply["success"]).unwrap(), false);
        assert_eq!(
            String::try_from(bad_reply["code"].try_clone().unwrap()).unwrap(),
            "invalid-profile"
        );

        // 3. Unconfirmed switch rejected fail-closed (no silent profile forcing!)
        let unconf: HashMap<String, OwnedValue> =
            proxy.call("SetProfile", &("gaming", false)).await.unwrap();
        assert_eq!(bool::try_from(&unconf["success"]).unwrap(), false);
        assert_eq!(
            String::try_from(unconf["code"].try_clone().unwrap()).unwrap(),
            "confirmation-required"
        );

        // 4. Polkit denied -> rejected fail-closed without mutating sysfs
        let denied: HashMap<String, OwnedValue> =
            proxy.call("SetProfile", &("gaming", true)).await.unwrap();
        assert_eq!(bool::try_from(&denied["success"]).unwrap(), false);
        assert_eq!(
            String::try_from(denied["code"].try_clone().unwrap()).unwrap(),
            "permission-denied"
        );
        assert_eq!(
            std::fs::read_to_string(fx.path("cpufreq/policy0/scaling_governor"))
                .unwrap()
                .trim(),
            "schedutil"
        );

        // 5. Allow Polkit, raise GPU temp >= 82 C -> thermal guard blocks "gaming"
        std::fs::write(&pkcheck_flag, "allow").unwrap();
        *gpu_temp.lock().unwrap() = 84.0;
        let hot_reply: HashMap<String, OwnedValue> =
            proxy.call("SetProfile", &("gaming", true)).await.unwrap();
        assert_eq!(bool::try_from(&hot_reply["success"]).unwrap(), false);
        assert_eq!(
            String::try_from(hot_reply["code"].try_clone().unwrap()).unwrap(),
            "thermal-limit"
        );
        assert_eq!(
            std::fs::read_to_string(fx.path("drm/card1/device/power_dpm_force_performance_level"))
                .unwrap()
                .trim(),
            "auto"
        );

        // 6. Cool down GPU -> "gaming" activates governor=performance & GPU DPM=high
        *gpu_temp.lock().unwrap() = 59.0;
        let ok_gaming: HashMap<String, OwnedValue> =
            proxy.call("SetProfile", &("gaming", true)).await.unwrap();
        assert_eq!(bool::try_from(&ok_gaming["success"]).unwrap(), true);
        assert_eq!(
            std::fs::read_to_string(fx.path("cpufreq/policy0/scaling_governor"))
                .unwrap()
                .trim(),
            "performance"
        );
        assert_eq!(
            std::fs::read_to_string(fx.path("drm/card1/device/power_dpm_force_performance_level"))
                .unwrap()
                .trim(),
            "high"
        );
        assert_eq!(
            load_saved_profile(&fx.path("state/profile.json")),
            PerformanceProfile::Gaming
        );

        // 7. Switch to "developer" -> CPU performance, GPU returns to auto
        let ok_dev: HashMap<String, OwnedValue> =
            proxy.call("SetProfile", &("developer", true)).await.unwrap();
        assert_eq!(bool::try_from(&ok_dev["success"]).unwrap(), true);
        assert_eq!(
            std::fs::read_to_string(fx.path("drm/card1/device/power_dpm_force_performance_level"))
                .unwrap()
                .trim(),
            "auto"
        );

        // 8. Switch to "creator" -> CPU schedutil, GPU auto
        let ok_creator: HashMap<String, OwnedValue> =
            proxy.call("SetProfile", &("creator", true)).await.unwrap();
        assert_eq!(bool::try_from(&ok_creator["success"]).unwrap(), true);
        assert_eq!(
            std::fs::read_to_string(fx.path("cpufreq/policy0/scaling_governor"))
                .unwrap()
                .trim(),
            "schedutil"
        );

        // 9. GetGamingReadiness reports Polaris11 + DXVK + Steam + MangoHud + anti-cheat warning
        let readiness_reply: HashMap<String, OwnedValue> =
            proxy.call("GetGamingReadiness", &()).await.unwrap();
        assert_eq!(bool::try_from(&readiness_reply["success"]).unwrap(), true);
        let rdata: HashMap<String, OwnedValue> =
            HashMap::try_from(readiness_reply["data"].try_clone().unwrap()).unwrap();
        assert_eq!(
            String::try_from(rdata["matched_profile"].try_clone().unwrap()).unwrap(),
            "amd-rx560-polaris11"
        );
        assert_eq!(bool::try_from(&rdata["dxvk_available"]).unwrap(), true);
        assert_eq!(bool::try_from(&rdata["vkd3d_available"]).unwrap(), false);
        assert_eq!(bool::try_from(&rdata["steam_available"]).unwrap(), true);
        assert_eq!(bool::try_from(&rdata["mangohud_available"]).unwrap(), true);
        assert_eq!(bool::try_from(&rdata["gamescope_available"]).unwrap(), false);
        assert_eq!(
            bool::try_from(&rdata["kernel_anticheat_bypass_forbidden"]).unwrap(),
            true
        );
    }
}
