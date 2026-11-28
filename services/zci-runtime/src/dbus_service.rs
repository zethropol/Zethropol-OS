use crate::bridge::{
    evaluate_inference_policy, execute_loopback_inference, is_permitted_inference_class,
    probe_bridge_health, seal_untrusted_via_bridge, LoopbackInferenceOutcome,
};
use crate::config::{
    is_supported_agent, RuntimeConfig, POLKIT_ACTION_RUNTIME_ADMIN, POLKIT_ACTION_RUNTIME_LOAD,
    RUNTIME_BUS_NAME, RUNTIME_VERSION,
};
use crate::lifecycle::RuntimeManager;
use crate::manifest::verify_model_entry;
use serde_json::json;
use std::collections::HashMap;
use std::path::PathBuf;
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use uuid::Uuid;
use zbus::interface;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::{Array, OwnedValue, Signature, Str, Value};

fn vec_to_owned(items: &[String]) -> OwnedValue {
    let mut array = Array::new(&Signature::Str);
    for item in items {
        let _ = array.append(Value::from(item.as_str()));
    }
    OwnedValue::try_from(Value::Array(array))
        .expect("string array should encode to OwnedValue")
}

fn envelope(
    success: bool,
    code: &str,
    message: &str,
    operation_id: &str,
    data: HashMap<String, OwnedValue>,
) -> HashMap<String, OwnedValue> {
    let mut out = HashMap::new();
    out.insert("success".to_string(), OwnedValue::from(success));
    out.insert("code".to_string(), OwnedValue::from(Str::from(code)));
    out.insert("message".to_string(), OwnedValue::from(Str::from(message)));
    out.insert(
        "operationId".to_string(),
        OwnedValue::from(Str::from(operation_id)),
    );
    out.insert("data".to_string(), OwnedValue::from(data));
    out
}

fn has_control_or_nul(s: &str) -> bool {
    s.chars().any(|c| c == '\0' || c.is_control())
}

async fn check_polkit(pkcheck_bin: PathBuf, action_id: &'static str, sender: String) -> bool {
    if sender.is_empty() {
        return false;
    }
    tokio::task::spawn_blocking(move || {
        Command::new(&pkcheck_bin)
            .args([
                "--action-id",
                action_id,
                "--system-bus-name",
                &sender,
                "--allow-user-interaction",
            ])
            .status()
            .map(|status| status.success())
            .unwrap_or(false)
    })
    .await
    .unwrap_or(false)
}

pub struct ZciRuntimeService {
    manager: Arc<Mutex<RuntimeManager>>,
}

impl ZciRuntimeService {
    pub fn with_config(config: RuntimeConfig) -> Self {
        Self {
            manager: Arc::new(Mutex::new(RuntimeManager::new(config))),
        }
    }

    pub fn manager_handle(&self) -> Arc<Mutex<RuntimeManager>> {
        Arc::clone(&self.manager)
    }

    fn snapshot_config(&self) -> Result<RuntimeConfig, zbus::fdo::Error> {
        self.manager
            .lock()
            .map(|g| g.config.clone())
            .map_err(|_| zbus::fdo::Error::Failed("Runtime manager lock poisoned".to_string()))
    }

    async fn build_runtime_status(
        &self,
        connection: &zbus::Connection,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let cfg = self.snapshot_config()?;
        let bridge_health = probe_bridge_health(connection, &cfg).await;

        let data = {
            let mut guard = self.manager.lock().map_err(|_| {
                zbus::fdo::Error::Failed("Runtime manager lock poisoned".to_string())
            })?;
            guard.sweep_idle_standby();
            guard.last_bridge_health = bridge_health.clone();

            let loaded_models = guard.loaded_model_ids();
            let active_model_id = guard.active_model_id.clone().unwrap_or_default();
            let (active_verified, active_sha256) = guard
                .active_model_record()
                .map(|m| (m.verified, m.actual_sha256.clone()))
                .unwrap_or((false, String::new()));

            let verified_count = guard.catalog.models.values().filter(|m| m.verified).count() as u64;
            let rejected_count = guard.catalog.models.values().filter(|m| !m.verified).count() as u64;
            let polkit_actions = vec![
                POLKIT_ACTION_RUNTIME_LOAD.to_string(),
                POLKIT_ACTION_RUNTIME_ADMIN.to_string(),
            ];

            let mut d = HashMap::new();
            d.insert("runtime_version".to_string(), OwnedValue::from(Str::from(RUNTIME_VERSION)));
            d.insert("service_name".to_string(), OwnedValue::from(Str::from(RUNTIME_BUS_NAME)));
            d.insert("fail_closed".to_string(), OwnedValue::from(true));
            d.insert("recovery_locks_protected".to_string(), OwnedValue::from(true));
            d.insert("loopback_only".to_string(), OwnedValue::from(true));
            d.insert("external_network_disabled".to_string(), OwnedValue::from(true));
            d.insert(
                "engine_endpoint".to_string(),
                OwnedValue::from(Str::from(guard.config.engine_endpoint.to_string().as_str())),
            );
            d.insert(
                "require_daemon_bridge".to_string(),
                OwnedValue::from(guard.config.require_daemon_bridge),
            );
            d.insert(
                "daemon_bridge_reachable".to_string(),
                OwnedValue::from(bridge_health.daemon_reachable),
            );
            d.insert(
                "daemon_version".to_string(),
                OwnedValue::from(Str::from(bridge_health.daemon_version.as_str())),
            );
            d.insert(
                "policy_engine_reachable".to_string(),
                OwnedValue::from(bridge_health.policy_reachable),
            );
            d.insert(
                "policy_default_deny".to_string(),
                OwnedValue::from(bridge_health.policy_default_deny),
            );
            d.insert(
                "policy_self_mod_lock".to_string(),
                OwnedValue::from(bridge_health.policy_self_mod_lock),
            );
            d.insert(
                "bridge_last_error".to_string(),
                OwnedValue::from(Str::from(bridge_health.last_error.as_str())),
            );

            // Hardware profile & memory summary (11.03 & 11.06)
            d.insert(
                "hardware_profile".to_string(),
                OwnedValue::from(Str::from(guard.hardware.profile.as_str())),
            );
            d.insert(
                "profile_reason".to_string(),
                OwnedValue::from(Str::from(guard.hardware.profile_reason.as_str())),
            );
            d.insert("cpu_cores".to_string(), OwnedValue::from(guard.hardware.cpu_cores));
            d.insert("ram_total_mb".to_string(), OwnedValue::from(guard.hardware.ram_total_mb));
            d.insert("ram_available_mb".to_string(), OwnedValue::from(guard.hardware.ram_available_mb));
            d.insert(
                "gpu_vendor".to_string(),
                OwnedValue::from(Str::from(guard.hardware.gpu_vendor.as_str())),
            );
            d.insert(
                "gpu_device".to_string(),
                OwnedValue::from(Str::from(guard.hardware.gpu_device.as_str())),
            );
            d.insert("gpu_vram_total_mb".to_string(), OwnedValue::from(guard.hardware.gpu_vram_total_mb));
            d.insert("gpu_vram_used_mb".to_string(), OwnedValue::from(guard.hardware.gpu_vram_used_mb));
            d.insert("vram_budget_mb".to_string(), OwnedValue::from(guard.hardware.limits.vram_budget_mb));
            d.insert(
                "passive_standby_mode".to_string(),
                OwnedValue::from(Str::from(guard.hardware.limits.passive_standby_mode.as_str())),
            );

            // Passive standby & lifecycle state (11.02 & 11.06)
            d.insert(
                "runtime_state".to_string(),
                OwnedValue::from(Str::from(guard.state.as_str())),
            );
            d.insert("passive_standby_enabled".to_string(), OwnedValue::from(true));
            d.insert("vram_locked".to_string(), OwnedValue::from(guard.vram_locked));
            d.insert("active_vram_mb".to_string(), OwnedValue::from(guard.active_vram_mb));
            d.insert(
                "idle_sleep_timeout_secs".to_string(),
                OwnedValue::from(guard.config.idle_sleep_timeout_secs),
            );
            d.insert("idle_elapsed_secs".to_string(), OwnedValue::from(guard.idle_elapsed_secs()));
            d.insert(
                "auto_sleep_transitions_total".to_string(),
                OwnedValue::from(guard.metrics.auto_sleep_transitions_total),
            );

            // Loaded & manifest models (11.04 & 11.06)
            d.insert(
                "active_model_id".to_string(),
                OwnedValue::from(Str::from(active_model_id.as_str())),
            );
            d.insert("active_model_verified".to_string(), OwnedValue::from(active_verified));
            d.insert(
                "active_model_sha256".to_string(),
                OwnedValue::from(Str::from(active_sha256.as_str())),
            );
            d.insert("loaded_models".to_string(), vec_to_owned(&loaded_models));
            let manifest_path_str = guard.catalog.manifest_path.display().to_string();
            d.insert(
                "manifest_path".to_string(),
                OwnedValue::from(Str::from(manifest_path_str.as_str())),
            );
            d.insert("manifest_present".to_string(), OwnedValue::from(guard.catalog.manifest_present));
            d.insert(
                "manifest_error".to_string(),
                OwnedValue::from(Str::from(guard.catalog.manifest_error.as_str())),
            );
            d.insert("verified_model_count".to_string(), OwnedValue::from(verified_count));
            d.insert("rejected_model_count".to_string(), OwnedValue::from(rejected_count));

            // Token/concurrency bounds & latency telemetry (11.02 & 11.06)
            d.insert(
                "max_context_tokens".to_string(),
                OwnedValue::from(guard.hardware.limits.max_context_tokens),
            );
            d.insert(
                "max_output_tokens".to_string(),
                OwnedValue::from(guard.hardware.limits.max_output_tokens),
            );
            d.insert(
                "max_concurrent_requests".to_string(),
                OwnedValue::from(guard.hardware.limits.max_concurrent_requests),
            );
            d.insert("active_requests".to_string(), OwnedValue::from(guard.active_requests));
            d.insert("total_inferences".to_string(), OwnedValue::from(guard.metrics.total_inferences));
            d.insert(
                "successful_inferences".to_string(),
                OwnedValue::from(guard.metrics.successful_inferences),
            );
            d.insert("denied_inferences".to_string(), OwnedValue::from(guard.metrics.denied_inferences));
            d.insert(
                "fail_closed_denials".to_string(),
                OwnedValue::from(guard.metrics.fail_closed_denials),
            );
            d.insert(
                "manifest_rejections".to_string(),
                OwnedValue::from(guard.metrics.manifest_rejections),
            );
            d.insert("policy_denials".to_string(), OwnedValue::from(guard.metrics.policy_denials));
            d.insert("token_limit_denials".to_string(), OwnedValue::from(guard.metrics.token_limit_denials));
            d.insert("wrapped_inputs_total".to_string(), OwnedValue::from(guard.metrics.wrapped_inputs_total));
            d.insert("wrapped_outputs_total".to_string(), OwnedValue::from(guard.metrics.wrapped_outputs_total));
            d.insert(
                "injections_flagged_total".to_string(),
                OwnedValue::from(guard.metrics.injections_flagged_total),
            );
            d.insert("last_latency_ms".to_string(), OwnedValue::from(guard.metrics.last_latency_ms));
            d.insert("avg_latency_ms".to_string(), OwnedValue::from(guard.metrics.avg_latency_ms()));
            d.insert("admin_polkit_actions".to_string(), vec_to_owned(&polkit_actions));
            d
        };

        let bridges_ok = bridge_health.policy_reachable
            && !bridge_health.policy_state_corrupted
            && (!cfg.require_daemon_bridge || bridge_health.daemon_reachable);

        let code = if bridges_ok {
            "ok"
        } else {
            "degraded-bridge-unreachable"
        };
        let msg = if bridges_ok {
            "ZCI Runtime healthy; loopback-only passive standby engine and Security/Daemon bridges ready."
        } else {
            "ZCI Runtime active in fail-closed degraded mode: Security Policy Engine or ZCI Daemon bridge unreachable."
        };
        Ok(envelope(true, code, msg, &operation_id, data))
    }
}

#[interface(name = "org.zethropol.ZciRuntime.v1")]
impl ZciRuntimeService {
    /// 11.06 — Full health, hardware profile, passive/active state, loaded models, latency & memory summary.
    async fn get_runtime_status(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        self.build_runtime_status(connection).await
    }

    /// Standard Zethropol `GetState` alias for uniform service observability.
    async fn get_state(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        self.build_runtime_status(connection).await
    }

    /// 11.03 — Detailed hardware capability snapshot and selected profile (`dev-baseline` vs `ancha-production`).
    async fn get_hardware_profile(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let guard = self
            .manager
            .lock()
            .map_err(|_| zbus::fdo::Error::Failed("Runtime manager lock poisoned".to_string()))?;
        let hw = &guard.hardware;

        let mut data = HashMap::new();
        data.insert(
            "hardware_profile".to_string(),
            OwnedValue::from(Str::from(hw.profile.as_str())),
        );
        data.insert(
            "profile_reason".to_string(),
            OwnedValue::from(Str::from(hw.profile_reason.as_str())),
        );
        data.insert("cpu_cores".to_string(), OwnedValue::from(hw.cpu_cores));
        data.insert("ram_total_mb".to_string(), OwnedValue::from(hw.ram_total_mb));
        data.insert("ram_available_mb".to_string(), OwnedValue::from(hw.ram_available_mb));
        data.insert("gpu_vendor".to_string(), OwnedValue::from(Str::from(hw.gpu_vendor.as_str())));
        data.insert("gpu_device".to_string(), OwnedValue::from(Str::from(hw.gpu_device.as_str())));
        data.insert("gpu_vram_total_mb".to_string(), OwnedValue::from(hw.gpu_vram_total_mb));
        data.insert("gpu_vram_used_mb".to_string(), OwnedValue::from(hw.gpu_vram_used_mb));
        data.insert(
            "passive_standby_mode".to_string(),
            OwnedValue::from(Str::from(hw.limits.passive_standby_mode.as_str())),
        );
        data.insert("max_context_tokens".to_string(), OwnedValue::from(hw.limits.max_context_tokens));
        data.insert("max_output_tokens".to_string(), OwnedValue::from(hw.limits.max_output_tokens));
        data.insert(
            "max_concurrent_requests".to_string(),
            OwnedValue::from(hw.limits.max_concurrent_requests),
        );
        data.insert("vram_budget_mb".to_string(), OwnedValue::from(hw.limits.vram_budget_mb));
        data.insert("hybrid_cpu_ram_gpu".to_string(), OwnedValue::from(hw.limits.hybrid_cpu_ram_gpu));

        Ok(envelope(
            true,
            "ok",
            "Hardware profile detected.",
            &operation_id,
            data,
        ))
    }

    /// 11.04 & 11.06 — List all manifest models with SHA-256 verification status and loaded state.
    async fn list_models(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let guard = self
            .manager
            .lock()
            .map_err(|_| zbus::fdo::Error::Failed("Runtime manager lock poisoned".to_string()))?;

        let mut models_json = Vec::new();
        for rec in guard.catalog.models.values() {
            let loaded = guard.active_model_id.as_deref() == Some(rec.model_id.as_str());
            let entry = json!({
                "model_id": rec.model_id,
                "display_name": rec.display_name,
                "format": rec.format,
                "expected_size_bytes": rec.expected_size_bytes,
                "actual_size_bytes": rec.actual_size_bytes,
                "expected_sha256": rec.expected_sha256,
                "actual_sha256": rec.actual_sha256,
                "license": rec.license,
                "min_profile": rec.min_profile,
                "signature_status": rec.signature_status,
                "verified": rec.verified,
                "loaded": loaded,
                "rejection_code": rec.rejection_code,
                "rejection_reason": rec.rejection_reason,
            });
            models_json.push(entry.to_string());
        }

        let verified_count = guard.catalog.models.values().filter(|m| m.verified).count() as u64;
        let rejected_count = guard.catalog.models.values().filter(|m| !m.verified).count() as u64;

        let mut data = HashMap::new();
        data.insert("models_json".to_string(), vec_to_owned(&models_json));
        data.insert("loaded_models".to_string(), vec_to_owned(&guard.loaded_model_ids()));
        data.insert("verified_model_count".to_string(), OwnedValue::from(verified_count));
        data.insert("rejected_model_count".to_string(), OwnedValue::from(rejected_count));
        data.insert("manifest_present".to_string(), OwnedValue::from(guard.catalog.manifest_present));
        data.insert(
            "manifest_error".to_string(),
            OwnedValue::from(Str::from(guard.catalog.manifest_error.as_str())),
        );

        Ok(envelope(
            true,
            "ok",
            "Manifest model catalog enumerated.",
            &operation_id,
            data,
        ))
    }

    /// 11.04 — On-demand streaming SHA-256 & compatibility verification for a declared model.
    async fn verify_model_integrity(
        &self,
        model_id: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        if has_control_or_nul(&model_id) || model_id.trim().is_empty() {
            return Ok(envelope(
                false,
                "invalid-request",
                "Invalid model_id.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let (manifest_path, hw) = {
            let guard = self.manager.lock().map_err(|_| {
                zbus::fdo::Error::Failed("Runtime manager lock poisoned".to_string())
            })?;
            (guard.config.manifest_file.clone(), guard.hardware.clone())
        };

        // Re-read and verify on a blocking thread so large multi-GB GGUF hashing doesn't stall D-Bus.
        let target_id = model_id.trim().to_string();
        let verify_res = tokio::task::spawn_blocking(move || {
            let raw = std::fs::read_to_string(&manifest_path)
                .map_err(|e| format!("Failed reading manifest: {e}"))?;
            let parsed: crate::manifest::ModelManifestFile =
                serde_json::from_str(&raw).map_err(|e| format!("Invalid manifest JSON: {e}"))?;
            let entry = parsed
                .models
                .into_iter()
                .find(|m| m.model_id.trim() == target_id)
                .ok_or_else(|| format!("Model '{target_id}' is not declared in manifest."))?;
            Ok::<_, String>(verify_model_entry(&entry, &hw))
        })
        .await
        .map_err(|e| zbus::fdo::Error::Failed(format!("Verification task join error: {e}")))?;

        match verify_res {
            Ok(rec) => {
                let mut data = HashMap::new();
                data.insert("model_id".to_string(), OwnedValue::from(Str::from(rec.model_id.as_str())));
                data.insert("verified".to_string(), OwnedValue::from(rec.verified));
                data.insert("format".to_string(), OwnedValue::from(Str::from(rec.format.as_str())));
                data.insert("expected_size_bytes".to_string(), OwnedValue::from(rec.expected_size_bytes));
                data.insert("actual_size_bytes".to_string(), OwnedValue::from(rec.actual_size_bytes));
                data.insert(
                    "expected_sha256".to_string(),
                    OwnedValue::from(Str::from(rec.expected_sha256.as_str())),
                );
                data.insert(
                    "actual_sha256".to_string(),
                    OwnedValue::from(Str::from(rec.actual_sha256.as_str())),
                );
                data.insert(
                    "rejection_code".to_string(),
                    OwnedValue::from(Str::from(rec.rejection_code.as_str())),
                );
                data.insert(
                    "rejection_reason".to_string(),
                    OwnedValue::from(Str::from(rec.rejection_reason.as_str())),
                );

                if rec.verified {
                    Ok(envelope(
                        true,
                        "ok",
                        "Model integrity (SHA-256, size, format header, hardware compatibility) verified.",
                        &operation_id,
                        data,
                    ))
                } else {
                    Ok(envelope(
                        false,
                        &rec.rejection_code,
                        &rec.rejection_reason,
                        &operation_id,
                        data,
                    ))
                }
            }
            Err(err) => Ok(envelope(
                false,
                "model-not-in-manifest",
                &err,
                &operation_id,
                HashMap::new(),
            )),
        }
    }

    /// 11.04 & 11.07 — Polkit-gated (`org.zethropol.zci.runtime.load`) model loading & manifest verification.
    async fn load_model(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        model_id: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        if has_control_or_nul(&model_id) || model_id.trim().is_empty() {
            return Ok(envelope(
                false,
                "invalid-request",
                "Invalid model_id.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let sender = header.sender().map(|s| s.to_string()).unwrap_or_default();
        let cfg = self.snapshot_config()?;
        if !check_polkit(cfg.pkcheck_bin, POLKIT_ACTION_RUNTIME_LOAD, sender).await {
            return Ok(envelope(
                false,
                "permission-denied",
                "Polkit authorization (org.zethropol.zci.runtime.load) required to load AI models.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let manager = Arc::clone(&self.manager);
        let id_clean = model_id.trim().to_string();
        let load_res = tokio::task::spawn_blocking(move || {
            let mut guard = manager
                .lock()
                .map_err(|_| ("internal-error", "Runtime manager lock poisoned.".to_string()))?;
            guard.load_verified_model(&id_clean)
        })
        .await
        .map_err(|e| zbus::fdo::Error::Failed(format!("LoadModel task error: {e}")))?;

        match load_res {
            Ok(rec) => {
                let _ = Self::runtime_state_changed(
                    &emitter,
                    "Standby".to_string(),
                    rec.model_id.clone(),
                    "model-verified-and-armed-passive-standby".to_string(),
                )
                .await;

                let mut data = HashMap::new();
                data.insert("model_id".to_string(), OwnedValue::from(Str::from(rec.model_id.as_str())));
                data.insert("verified".to_string(), OwnedValue::from(true));
                data.insert("runtime_state".to_string(), OwnedValue::from(Str::from("Standby")));
                data.insert("vram_locked".to_string(), OwnedValue::from(false));
                data.insert("active_vram_mb".to_string(), OwnedValue::from(0u64));
                data.insert(
                    "sha256".to_string(),
                    OwnedValue::from(Str::from(rec.actual_sha256.as_str())),
                );
                data.insert(
                    "license".to_string(),
                    OwnedValue::from(Str::from(rec.license.as_str())),
                );
                Ok(envelope(
                    true,
                    "ok",
                    "Model SHA-256 verified and armed in passive Standby (zero idle VRAM locked).",
                    &operation_id,
                    data,
                ))
            }
            Err((code, reason)) => Ok(envelope(false, code, &reason, &operation_id, HashMap::new())),
        }
    }

    /// 11.02 & 11.07 — Polkit-gated (`org.zethropol.zci.runtime.load`) model unloading.
    async fn unload_model(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        model_id: String,
        reason: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        if has_control_or_nul(&model_id) || has_control_or_nul(&reason) {
            return Ok(envelope(
                false,
                "invalid-request",
                "Control characters rejected.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let sender = header.sender().map(|s| s.to_string()).unwrap_or_default();
        let cfg = self.snapshot_config()?;
        if !check_polkit(cfg.pkcheck_bin, POLKIT_ACTION_RUNTIME_LOAD, sender).await {
            return Ok(envelope(
                false,
                "permission-denied",
                "Polkit authorization (org.zethropol.zci.runtime.load) required to unload AI models.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let res = {
            let mut guard = self.manager.lock().map_err(|_| {
                zbus::fdo::Error::Failed("Runtime manager lock poisoned".to_string())
            })?;
            guard.unload_model(model_id.trim())
        };

        match res {
            Ok(unloaded_id) => {
                let _ = Self::runtime_state_changed(
                    &emitter,
                    "Standby".to_string(),
                    String::new(),
                    if reason.trim().is_empty() {
                        "model-unloaded".to_string()
                    } else {
                        reason.trim().to_string()
                    },
                )
                .await;
                let mut data = HashMap::new();
                data.insert(
                    "unloaded_model_id".to_string(),
                    OwnedValue::from(Str::from(unloaded_id.as_str())),
                );
                data.insert("runtime_state".to_string(), OwnedValue::from(Str::from("Standby")));
                data.insert("vram_locked".to_string(), OwnedValue::from(false));
                data.insert("active_vram_mb".to_string(), OwnedValue::from(0u64));
                Ok(envelope(
                    true,
                    "ok",
                    "Model unloaded and runtime returned to Standby.",
                    &operation_id,
                    data,
                ))
            }
            Err((code, msg)) => Ok(envelope(false, code, &msg, &operation_id, HashMap::new())),
        }
    }

    /// 11.02 & 11.07 — Polkit-gated (`org.zethropol.zci.runtime.admin`) explicit passive Standby transition.
    async fn enter_standby(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        reason: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let sender = header.sender().map(|s| s.to_string()).unwrap_or_default();
        let cfg = self.snapshot_config()?;
        if !check_polkit(cfg.pkcheck_bin, POLKIT_ACTION_RUNTIME_ADMIN, sender).await {
            return Ok(envelope(
                false,
                "permission-denied",
                "Polkit authorization (org.zethropol.zci.runtime.admin) required.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let res = {
            let mut guard = self.manager.lock().map_err(|_| {
                zbus::fdo::Error::Failed("Runtime manager lock poisoned".to_string())
            })?;
            guard.enter_standby()
        };

        match res {
            Ok(model_id) => {
                let _ = Self::runtime_state_changed(
                    &emitter,
                    "Standby".to_string(),
                    model_id.clone(),
                    if reason.trim().is_empty() {
                        "manual-passive-standby".to_string()
                    } else {
                        reason.trim().to_string()
                    },
                )
                .await;
                let mut data = HashMap::new();
                data.insert("runtime_state".to_string(), OwnedValue::from(Str::from("Standby")));
                data.insert("vram_locked".to_string(), OwnedValue::from(false));
                data.insert("active_vram_mb".to_string(), OwnedValue::from(0u64));
                Ok(envelope(
                    true,
                    "ok",
                    "ZCI Runtime entered passive Standby; VRAM unlocked.",
                    &operation_id,
                    data,
                ))
            }
            Err((code, msg)) => Ok(envelope(false, code, &msg, &operation_id, HashMap::new())),
        }
    }

    /// 11.04 & 11.07 — Polkit-gated (`org.zethropol.zci.runtime.admin`) manifest & loopback config reload.
    async fn reload_manifest(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        reason: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        if has_control_or_nul(&reason) {
            return Ok(envelope(
                false,
                "invalid-request",
                "Control characters rejected.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let sender = header.sender().map(|s| s.to_string()).unwrap_or_default();
        let cfg = self.snapshot_config()?;
        if !check_polkit(cfg.pkcheck_bin, POLKIT_ACTION_RUNTIME_ADMIN, sender).await {
            return Ok(envelope(
                false,
                "permission-denied",
                "Polkit authorization (org.zethropol.zci.runtime.admin) required to reload manifest.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let manager = Arc::clone(&self.manager);
        let reload_res = tokio::task::spawn_blocking(move || {
            let mut guard = manager
                .lock()
                .map_err(|_| "Runtime manager lock poisoned.".to_string())?;
            guard.reload_manifest_and_config()
        })
        .await
        .map_err(|e| zbus::fdo::Error::Failed(format!("Reload task error: {e}")))?;

        match reload_res {
            Ok((verified, rejected)) => {
                let mut data = HashMap::new();
                data.insert("verified_model_count".to_string(), OwnedValue::from(verified as u64));
                data.insert("rejected_model_count".to_string(), OwnedValue::from(rejected as u64));
                Ok(envelope(
                    true,
                    "ok",
                    "Model manifest and runtime configuration reloaded and re-verified.",
                    &operation_id,
                    data,
                ))
            }
            Err(err) => Ok(envelope(
                false,
                "reload-failed",
                &err,
                &operation_id,
                HashMap::new(),
            )),
        }
    }

    /// 11.01, 11.02, 11.04, 11.05 — Policy-gated, loopback-only local AI inference with
    /// bidirectional untrusted sealing (`executable=false`, `policy_authority="none"`).
    async fn run_inference(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        agent_id: String,
        model_id: String,
        prompt: String,
        max_tokens: u32,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();

        if has_control_or_nul(&agent_id)
            || has_control_or_nul(&model_id)
            || !is_supported_agent(agent_id.trim())
        {
            return Ok(envelope(
                false,
                "unknown-agent",
                "Unknown or invalid ZCI agent identity denied.",
                &operation_id,
                HashMap::new(),
            ));
        }
        let agent = agent_id.trim().to_string();

        // 1. Validate loaded verified model, context/token ceilings, concurrency & enter Active state.
        let (mut slot_guard, model, effective_max_tokens) =
            match RuntimeManager::try_acquire_inference_slot(
                &self.manager,
                &model_id,
                &prompt,
                max_tokens,
            ) {
                Ok(tuple) => tuple,
                Err((code, msg)) => {
                    let _ = Self::inference_audited(
                        &emitter,
                        agent,
                        model_id,
                        code.to_string(),
                        0,
                    )
                    .await;
                    return Ok(envelope(false, code, &msg, &operation_id, HashMap::new()));
                }
            };

        let _ = Self::runtime_state_changed(
            &emitter,
            "Active".to_string(),
            model.model_id.clone(),
            "inference-active".to_string(),
        )
        .await;

        let cfg = self.snapshot_config()?;

        // 2. Mandatory fail-closed Security Policy Engine check (`EvaluateAction`).
        let eval = match evaluate_inference_policy(connection, &cfg, &agent, &prompt).await {
            Ok(ev) => ev,
            Err(err) => {
                RuntimeManager::mark_inference_sleeping(&mut slot_guard);
                if let Ok(mut guard) = self.manager.lock() {
                    guard.metrics.denied_inferences += 1;
                    guard.metrics.fail_closed_denials += 1;
                }
                let _ = Self::inference_audited(
                    &emitter,
                    agent,
                    model.model_id.clone(),
                    "policy-engine-unavailable".to_string(),
                    0,
                )
                .await;
                return Ok(envelope(
                    false,
                    "policy-engine-unavailable",
                    &format!("Fail-closed denial: {err}"),
                    &operation_id,
                    HashMap::new(),
                ));
            }
        };

        if !eval.allowed || !is_permitted_inference_class(&eval.action_class) {
            RuntimeManager::mark_inference_sleeping(&mut slot_guard);
            if let Ok(mut guard) = self.manager.lock() {
                guard.metrics.denied_inferences += 1;
                guard.metrics.policy_denials += 1;
            }
            let _ = Self::inference_audited(
                &emitter,
                agent,
                model.model_id.clone(),
                eval.code.clone(),
                0,
            )
            .await;
            let mut data = HashMap::new();
            data.insert(
                "action_class".to_string(),
                OwnedValue::from(Str::from(eval.action_class.as_str())),
            );
            data.insert("rule_id".to_string(), OwnedValue::from(Str::from(eval.rule_id.as_str())));
            data.insert("lockout_active".to_string(), OwnedValue::from(eval.lockout_active));
            data.insert("hard_locked".to_string(), OwnedValue::from(eval.hard_locked));
            data.insert(
                "audit_event_id".to_string(),
                OwnedValue::from(Str::from(eval.audit_event_id.as_str())),
            );
            return Ok(envelope(
                false,
                &eval.code,
                &eval.reason,
                &operation_id,
                data,
            ));
        }

        // 3. 11.05 MANDATORY input sealing via `zci-daemon` / Policy Engine `WrapUntrustedData`.
        let sealed_in = match seal_untrusted_via_bridge(
            connection,
            &cfg,
            &agent,
            "runtime_prompt",
            &prompt,
        )
        .await
        {
            Ok(s) => s,
            Err(err) => {
                RuntimeManager::mark_inference_sleeping(&mut slot_guard);
                if let Ok(mut guard) = self.manager.lock() {
                    guard.metrics.denied_inferences += 1;
                    guard.metrics.fail_closed_denials += 1;
                }
                let _ = Self::inference_audited(
                    &emitter,
                    agent,
                    model.model_id.clone(),
                    "bridge-untrusted-wrap-failed".to_string(),
                    0,
                )
                .await;
                return Ok(envelope(
                    false,
                    "bridge-untrusted-wrap-failed",
                    &format!("Fail-closed denial: could not seal model input ({err})."),
                    &operation_id,
                    HashMap::new(),
                ));
            }
        };

        if let Ok(mut guard) = self.manager.lock() {
            guard.metrics.wrapped_inputs_total += 1;
            if sealed_in.injection_detected {
                guard.metrics.injections_flagged_total += 1;
            }
        }

        // 4. 11.01 Execute strictly loopback-only local inference (`127.0.0.1` / `::1`).
        let outcome = match execute_loopback_inference(
            cfg.engine_endpoint,
            &model.model_id,
            &sealed_in.wrapped_payload,
            effective_max_tokens,
            Duration::from_millis(cfg.request_timeout_ms),
        )
        .await
        {
            Ok(o) => o,
            Err(err) => {
                RuntimeManager::mark_inference_sleeping(&mut slot_guard);
                if let Ok(mut guard) = self.manager.lock() {
                    guard.metrics.denied_inferences += 1;
                }
                let _ = Self::inference_audited(
                    &emitter,
                    agent,
                    model.model_id.clone(),
                    "engine-error".to_string(),
                    0,
                )
                .await;
                return Ok(envelope(
                    false,
                    "engine-error",
                    &err,
                    &operation_id,
                    HashMap::new(),
                ));
            }
        };

        match outcome {
            LoopbackInferenceOutcome::EngineSleeping {
                endpoint,
                latency_ms,
                detail,
            } => {
                RuntimeManager::mark_inference_sleeping(&mut slot_guard);
                let _ = Self::runtime_state_changed(
                    &emitter,
                    "Standby".to_string(),
                    model.model_id.clone(),
                    "loopback-engine-passive-standby".to_string(),
                )
                .await;
                let _ = Self::inference_audited(
                    &emitter,
                    agent,
                    model.model_id.clone(),
                    "passive-standby".to_string(),
                    latency_ms,
                )
                .await;

                let mut data = HashMap::new();
                data.insert("model_id".to_string(), OwnedValue::from(Str::from(model.model_id.as_str())));
                data.insert("engine_endpoint".to_string(), OwnedValue::from(Str::from(endpoint.as_str())));
                data.insert("runtime_state".to_string(), OwnedValue::from(Str::from("Standby")));
                data.insert("vram_locked".to_string(), OwnedValue::from(false));
                data.insert("latency_ms".to_string(), OwnedValue::from(latency_ms));
                data.insert(
                    "input_envelope_id".to_string(),
                    OwnedValue::from(Str::from(sealed_in.envelope_id.as_str())),
                );
                data.insert("input_executable".to_string(), OwnedValue::from(sealed_in.executable));
                data.insert(
                    "input_policy_authority".to_string(),
                    OwnedValue::from(Str::from(sealed_in.policy_authority.as_str())),
                );
                Ok(envelope(
                    true,
                    "passive-standby",
                    &detail,
                    &operation_id,
                    data,
                ))
            }
            LoopbackInferenceOutcome::Responded {
                status_code,
                raw_output,
                latency_ms,
            } => {
                // 5. 11.05 MANDATORY output sealing (`executable=false`, `policy_authority="none"`).
                let sealed_out = match seal_untrusted_via_bridge(
                    connection,
                    &cfg,
                    &agent,
                    "model_output",
                    &raw_output,
                )
                .await
                {
                    Ok(s) => s,
                    Err(err) => {
                        RuntimeManager::mark_inference_sleeping(&mut slot_guard);
                        if let Ok(mut guard) = self.manager.lock() {
                            guard.metrics.denied_inferences += 1;
                            guard.metrics.fail_closed_denials += 1;
                        }
                        return Ok(envelope(
                            false,
                            "output-isolation-failed",
                            &format!(
                                "Fail-closed denial: raw model output dropped because sealing failed ({err})."
                            ),
                            &operation_id,
                            HashMap::new(),
                        ));
                    }
                };

                if let Ok(mut guard) = self.manager.lock() {
                    guard.metrics.successful_inferences += 1;
                    guard.metrics.wrapped_outputs_total += 1;
                    guard.metrics.last_latency_ms = latency_ms;
                    guard.metrics.total_latency_ms =
                        guard.metrics.total_latency_ms.saturating_add(latency_ms);
                    if sealed_out.injection_detected {
                        guard.metrics.injections_flagged_total += 1;
                    }
                }

                let _ = Self::inference_audited(
                    &emitter,
                    agent,
                    model.model_id.clone(),
                    "ok".to_string(),
                    latency_ms,
                )
                .await;

                let mut data = HashMap::new();
                data.insert("model_id".to_string(), OwnedValue::from(Str::from(model.model_id.as_str())));
                data.insert(
                    "model_sha256".to_string(),
                    OwnedValue::from(Str::from(model.actual_sha256.as_str())),
                );
                data.insert("http_status".to_string(), OwnedValue::from(u32::from(status_code)));
                data.insert("latency_ms".to_string(), OwnedValue::from(latency_ms));
                data.insert("max_tokens_applied".to_string(), OwnedValue::from(effective_max_tokens));
                data.insert("runtime_state".to_string(), OwnedValue::from(Str::from("Active")));

                // Input sealing metadata
                data.insert(
                    "input_envelope_id".to_string(),
                    OwnedValue::from(Str::from(sealed_in.envelope_id.as_str())),
                );
                data.insert("input_executable".to_string(), OwnedValue::from(sealed_in.executable));
                data.insert(
                    "input_policy_authority".to_string(),
                    OwnedValue::from(Str::from(sealed_in.policy_authority.as_str())),
                );
                data.insert(
                    "input_injection_detected".to_string(),
                    OwnedValue::from(sealed_in.injection_detected),
                );
                data.insert(
                    "input_detected_patterns".to_string(),
                    vec_to_owned(&sealed_in.detected_patterns),
                );

                // Output sealing metadata (NEVER returns raw unsealed model output!)
                data.insert(
                    "output_trust_level".to_string(),
                    OwnedValue::from(Str::from(sealed_out.trust_level.as_str())),
                );
                data.insert("output_executable".to_string(), OwnedValue::from(sealed_out.executable));
                data.insert(
                    "output_policy_authority".to_string(),
                    OwnedValue::from(Str::from(sealed_out.policy_authority.as_str())),
                );
                data.insert(
                    "output_source_kind".to_string(),
                    OwnedValue::from(Str::from(sealed_out.source_kind.as_str())),
                );
                data.insert(
                    "output_sha256".to_string(),
                    OwnedValue::from(Str::from(sealed_out.content_sha256.as_str())),
                );
                data.insert(
                    "output_envelope_id".to_string(),
                    OwnedValue::from(Str::from(sealed_out.envelope_id.as_str())),
                );
                data.insert(
                    "output_injection_detected".to_string(),
                    OwnedValue::from(sealed_out.injection_detected),
                );
                data.insert(
                    "output_self_modification_injection".to_string(),
                    OwnedValue::from(sealed_out.self_modification_injection),
                );
                data.insert(
                    "output_detected_patterns".to_string(),
                    vec_to_owned(&sealed_out.detected_patterns),
                );
                data.insert(
                    "output_wrapped_payload".to_string(),
                    OwnedValue::from(Str::from(sealed_out.wrapped_payload.as_str())),
                );

                Ok(envelope(
                    true,
                    "ok",
                    "Local loopback inference completed with bidirectional untrusted isolation.",
                    &operation_id,
                    data,
                ))
            }
        }
    }

    #[zbus(signal)]
    pub async fn runtime_state_changed(
        emitter: &SignalEmitter<'_>,
        state: String,
        model_id: String,
        reason: String,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    pub async fn inference_audited(
        emitter: &SignalEmitter<'_>,
        agent_id: String,
        model_id: String,
        outcome: String,
        latency_ms: u64,
    ) -> zbus::Result<()>;
}
