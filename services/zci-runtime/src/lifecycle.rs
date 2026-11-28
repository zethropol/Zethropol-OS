use crate::bridge::BridgeHealthSnapshot;
use crate::config::RuntimeConfig;
use crate::hardware::{detect_host_hardware, HardwareSnapshot};
use crate::manifest::{load_and_verify_manifest, ManifestCatalog, VerifiedModelRecord};
use serde::{Deserialize, Serialize};
use std::sync::{Arc, Mutex};
use std::time::Instant;

/// 11.02 — Runtime lifecycle states aligned with Gemma 4B passive standby.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum RuntimeState {
    Standby,
    Loading,
    Active,
    Unloading,
    Faulted,
}

impl RuntimeState {
    pub fn as_str(&self) -> &'static str {
        match self {
            RuntimeState::Standby => "Standby",
            RuntimeState::Loading => "Loading",
            RuntimeState::Active => "Active",
            RuntimeState::Unloading => "Unloading",
            RuntimeState::Faulted => "Faulted",
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct RuntimeMetrics {
    pub total_inferences: u64,
    pub successful_inferences: u64,
    pub denied_inferences: u64,
    pub fail_closed_denials: u64,
    pub manifest_rejections: u64,
    pub policy_denials: u64,
    pub concurrency_denials: u64,
    pub token_limit_denials: u64,
    pub wrapped_inputs_total: u64,
    pub wrapped_outputs_total: u64,
    pub injections_flagged_total: u64,
    pub auto_sleep_transitions_total: u64,
    pub manifest_reloads_total: u64,
    pub last_latency_ms: u64,
    pub total_latency_ms: u64,
}

impl RuntimeMetrics {
    pub fn avg_latency_ms(&self) -> u64 {
        if self.successful_inferences == 0 {
            0
        } else {
            self.total_latency_ms / self.successful_inferences
        }
    }
}

pub struct RuntimeManager {
    pub config: RuntimeConfig,
    pub hardware: HardwareSnapshot,
    pub catalog: ManifestCatalog,
    pub state: RuntimeState,
    pub active_model_id: Option<String>,
    pub active_requests: u32,
    pub vram_locked: bool,
    pub active_vram_mb: u64,
    pub last_activity: Instant,
    pub last_bridge_health: BridgeHealthSnapshot,
    pub metrics: RuntimeMetrics,
}

pub struct InferenceSlotGuard {
    manager: Arc<Mutex<RuntimeManager>>,
    release_vram_on_drop: bool,
}

impl Drop for InferenceSlotGuard {
    fn drop(&mut self) {
        if let Ok(mut guard) = self.manager.lock() {
            guard.active_requests = guard.active_requests.saturating_sub(1);
            guard.last_activity = Instant::now();
            if self.release_vram_on_drop && guard.active_requests == 0 {
                // Keep state Active only while warm window is non-zero; passive standby sweep
                // transitions back to Standby (or caller explicitly enters Standby).
                // On strict passive dev-baseline with engine sleeping, immediately unlock VRAM.
                guard.vram_locked = false;
                guard.active_vram_mb = 0;
                guard.state = RuntimeState::Standby;
            }
        }
    }
}

impl RuntimeManager {
    pub fn new(mut config: RuntimeConfig) -> Self {
        config.clamp_bounds();
        let hardware = detect_host_hardware(config.hardware_override_file.as_deref());
        let catalog = load_and_verify_manifest(&config.manifest_file, &hardware);
        let rejected = catalog.models.values().filter(|m| !m.verified).count() as u64;
        let mut metrics = RuntimeMetrics::default();
        metrics.manifest_rejections = rejected;

        // Passive standby invariant (11.02): always boot into `Standby` with zero VRAM locked!
        Self {
            config,
            hardware,
            catalog,
            state: RuntimeState::Standby,
            active_model_id: None,
            active_requests: 0,
            vram_locked: false,
            active_vram_mb: 0,
            last_activity: Instant::now(),
            last_bridge_health: BridgeHealthSnapshot::default(),
            metrics,
        }
    }

    pub fn loaded_model_ids(&self) -> Vec<String> {
        self.active_model_id.iter().cloned().collect()
    }

    pub fn active_model_record(&self) -> Option<&VerifiedModelRecord> {
        self.active_model_id
            .as_ref()
            .and_then(|id| self.catalog.models.get(id))
    }

    pub fn idle_elapsed_secs(&self) -> u64 {
        if self.active_requests > 0 {
            0
        } else {
            self.last_activity.elapsed().as_secs()
        }
    }

    /// 11.02 — Auto-sleep passive standby transition when idle timeout elapses.
    /// Returns `Some((prev_model_id))` when an automatic `Active -> Standby` transition occurred.
    pub fn sweep_idle_standby(&mut self) -> Option<String> {
        if self.active_requests > 0 {
            return None;
        }
        if self.state == RuntimeState::Active
            && self.last_activity.elapsed().as_secs() >= self.config.idle_sleep_timeout_secs
        {
            self.state = RuntimeState::Standby;
            self.vram_locked = false;
            self.active_vram_mb = 0;
            self.metrics.auto_sleep_transitions_total += 1;
            return Some(self.active_model_id.clone().unwrap_or_default());
        }
        None
    }

    pub fn enter_standby(&mut self) -> Result<String, (&'static str, String)> {
        if self.active_requests > 0 {
            return Err((
                "runtime-busy",
                "Cannot enter Standby while active inference requests are in flight.".to_string(),
            ));
        }
        self.state = RuntimeState::Standby;
        self.vram_locked = false;
        self.active_vram_mb = 0;
        self.last_activity = Instant::now();
        Ok(self.active_model_id.clone().unwrap_or_default())
    }

    /// 11.04 — Verify & arm a model in passive standby (never locks VRAM until an inference arrives).
    pub fn load_verified_model(
        &mut self,
        model_id: &str,
    ) -> Result<VerifiedModelRecord, (&'static str, String)> {
        if self.active_requests > 0 {
            return Err((
                "runtime-busy",
                "Cannot switch active model while inference requests are in flight.".to_string(),
            ));
        }
        self.state = RuntimeState::Loading;

        // Re-verify manifest on disk before loading so tampered files are caught immediately!
        self.hardware = detect_host_hardware(self.config.hardware_override_file.as_deref());
        self.catalog = load_and_verify_manifest(&self.config.manifest_file, &self.hardware);

        let Some(rec) = self.catalog.models.get(model_id).cloned() else {
            self.state = RuntimeState::Standby;
            self.metrics.manifest_rejections += 1;
            return Err((
                "model-not-in-manifest",
                format!("Fail-closed rejection: model '{model_id}' is not declared in manifest."),
            ));
        };

        if !rec.verified {
            self.state = RuntimeState::Standby;
            self.metrics.manifest_rejections += 1;
            let code = match rec.rejection_code.as_str() {
                "sha256-mismatch" => "sha256-mismatch",
                "unsigned-or-unverified-model" => "unsigned-or-unverified-model",
                "profile-incompatible" => "profile-incompatible",
                "insufficient-vram" => "insufficient-vram",
                "insufficient-ram" => "insufficient-ram",
                "size-mismatch" => "size-mismatch",
                "corrupt-model-header" => "corrupt-model-header",
                _ => "manifest-verification-failed",
            };
            return Err((code, rec.rejection_reason.clone()));
        }

        self.active_model_id = Some(rec.model_id.clone());
        // Passive standby rule (11.02): arming/loading a model keeps VRAM unlocked (0 MiB) until active query!
        self.state = RuntimeState::Standby;
        self.vram_locked = false;
        self.active_vram_mb = 0;
        self.last_activity = Instant::now();
        Ok(rec)
    }

    pub fn unload_model(&mut self, model_id: &str) -> Result<String, (&'static str, String)> {
        if self.active_requests > 0 {
            return Err((
                "runtime-busy",
                "Cannot unload model while inference requests are in flight.".to_string(),
            ));
        }
        let Some(current) = self.active_model_id.clone() else {
            return Err(("no-model-loaded", "No model is currently loaded.".to_string()));
        };
        if !model_id.is_empty() && model_id != current {
            return Err((
                "model-not-loaded",
                format!("Requested model '{model_id}' is not the currently loaded model ('{current}')."),
            ));
        }
        self.state = RuntimeState::Unloading;
        self.active_model_id = None;
        self.vram_locked = false;
        self.active_vram_mb = 0;
        self.state = RuntimeState::Standby;
        self.last_activity = Instant::now();
        Ok(current)
    }

    pub fn reload_manifest_and_config(&mut self) -> Result<(usize, usize), String> {
        if self.active_requests > 0 {
            return Err("Cannot reload manifest/config while inference requests are active.".to_string());
        }
        let mut next_cfg = self.config.clone();
        next_cfg.reload_from_file()?;
        self.config = next_cfg;
        self.hardware = detect_host_hardware(self.config.hardware_override_file.as_deref());
        self.catalog = load_and_verify_manifest(&self.config.manifest_file, &self.hardware);
        self.metrics.manifest_reloads_total += 1;

        // If the currently loaded model disappeared or became corrupted on disk, unload fail-closed!
        if let Some(ref active_id) = self.active_model_id {
            let still_valid = self
                .catalog
                .models
                .get(active_id)
                .map(|m| m.verified)
                .unwrap_or(false);
            if !still_valid {
                self.active_model_id = None;
                self.vram_locked = false;
                self.active_vram_mb = 0;
                self.state = RuntimeState::Standby;
                self.metrics.manifest_rejections += 1;
            }
        }

        let verified = self.catalog.models.values().filter(|m| m.verified).count();
        let rejected = self.catalog.models.values().filter(|m| !m.verified).count();
        Ok((verified, rejected))
    }

    pub fn effective_max_context_tokens(&self, model: &VerifiedModelRecord) -> u32 {
        self.hardware
            .limits
            .max_context_tokens
            .min(model.max_context_tokens)
    }

    /// Acquire a bounded inference execution slot & validate model integrity + token/context caps.
    pub fn try_acquire_inference_slot(
        manager: &Arc<Mutex<RuntimeManager>>,
        requested_model_id: &str,
        prompt: &str,
        requested_max_tokens: u32,
    ) -> Result<(InferenceSlotGuard, VerifiedModelRecord, u32), (&'static str, String)> {
        let mut guard = manager
            .lock()
            .map_err(|_| ("internal-error", "Runtime manager lock poisoned.".to_string()))?;

        guard.sweep_idle_standby();
        guard.metrics.total_inferences += 1;

        if prompt.trim().is_empty() {
            guard.metrics.denied_inferences += 1;
            return Err(("empty-prompt", "Inference prompt must not be empty.".to_string()));
        }
        if prompt.len() > guard.config.max_prompt_bytes {
            guard.metrics.denied_inferences += 1;
            guard.metrics.token_limit_denials += 1;
            return Err((
                "context-limit-exceeded",
                format!(
                    "Prompt exceeds runtime byte ceiling ({} bytes).",
                    guard.config.max_prompt_bytes
                ),
            ));
        }

        // Resolve target model (must be loaded and verified!).
        let target_id = if requested_model_id.trim().is_empty() {
            guard.active_model_id.clone().unwrap_or_default()
        } else {
            requested_model_id.trim().to_string()
        };

        if target_id.is_empty() {
            guard.metrics.denied_inferences += 1;
            guard.metrics.fail_closed_denials += 1;
            return Err((
                "no-model-loaded",
                "Fail-closed denial: no verified model is currently loaded.".to_string(),
            ));
        }
        if guard.active_model_id.as_deref() != Some(target_id.as_str()) {
            guard.metrics.denied_inferences += 1;
            guard.metrics.fail_closed_denials += 1;
            return Err((
                "model-not-loaded",
                format!("Fail-closed denial: model '{target_id}' must be verified and loaded via LoadModel first."),
            ));
        }
        let Some(model) = guard.catalog.models.get(&target_id).cloned() else {
            guard.metrics.denied_inferences += 1;
            guard.metrics.manifest_rejections += 1;
            return Err((
                "model-not-in-manifest",
                format!("Fail-closed denial: model '{target_id}' is missing from manifest."),
            ));
        };
        if !model.verified {
            guard.metrics.denied_inferences += 1;
            guard.metrics.manifest_rejections += 1;
            return Err((
                "model-unverified",
                format!(
                    "Fail-closed denial: model '{}' failed integrity verification ({}).",
                    model.model_id, model.rejection_reason
                ),
            ));
        }

        // Enforce profile & model context/output token bounds (11.02).
        let max_ctx = guard.effective_max_context_tokens(&model);
        // Conservative token estimate (~3 chars/token) so overly large contexts are caught cleanly.
        let est_prompt_tokens = ((prompt.chars().count() / 3) + 1) as u32;
        if est_prompt_tokens > max_ctx {
            guard.metrics.denied_inferences += 1;
            guard.metrics.token_limit_denials += 1;
            return Err((
                "context-limit-exceeded",
                format!(
                    "Estimated prompt tokens ({est_prompt_tokens}) exceed active profile context limit ({max_ctx})."
                ),
            ));
        }

        let max_out_cap = guard.hardware.limits.max_output_tokens;
        let effective_max_tokens = if requested_max_tokens == 0 {
            max_out_cap.min(512)
        } else if requested_max_tokens > max_out_cap {
            guard.metrics.denied_inferences += 1;
            guard.metrics.token_limit_denials += 1;
            return Err((
                "token-limit-exceeded",
                format!(
                    "Requested max_tokens ({requested_max_tokens}) exceeds '{}' profile ceiling ({max_out_cap}).",
                    guard.hardware.profile.as_str()
                ),
            ));
        } else {
            requested_max_tokens
        };

        if guard.active_requests >= guard.hardware.limits.max_concurrent_requests {
            guard.metrics.denied_inferences += 1;
            guard.metrics.concurrency_denials += 1;
            return Err((
                "runtime-concurrency-limit",
                format!(
                    "Active inference concurrency limit ({}) reached for '{}' profile.",
                    guard.hardware.limits.max_concurrent_requests,
                    guard.hardware.profile.as_str()
                ),
            ));
        }

        // Transition Standby -> Active for the duration of the inference call!
        guard.active_requests += 1;
        guard.state = RuntimeState::Active;
        guard.last_activity = Instant::now();
        let vram_est = if model.hybrid_vram_estimate_mb > 0 {
            model
                .hybrid_vram_estimate_mb
                .min(guard.hardware.limits.vram_budget_mb)
        } else {
            guard.hardware.limits.default_active_vram_estimate_mb
        };
        guard.vram_locked = vram_est > 0;
        guard.active_vram_mb = vram_est;

        Ok((
            InferenceSlotGuard {
                manager: Arc::clone(manager),
                release_vram_on_drop: false,
            },
            model,
            effective_max_tokens,
        ))
    }

    pub fn mark_inference_sleeping(guard: &mut InferenceSlotGuard) {
        guard.release_vram_on_drop = true;
    }
}
