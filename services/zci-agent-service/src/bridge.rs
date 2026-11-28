use crate::config::{AgentConfig, MAX_WRITE_FILE_BYTES};
use crate::planner::{validate_writable_workspace_path, PlanStep, SnapperPreflightReport};
use std::collections::HashMap;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::Path;
use std::time::Duration;
use uuid::Uuid;
use zbus::proxy::CacheProperties;
use zbus::zvariant::OwnedValue;

fn get_bool(map: &HashMap<String, OwnedValue>, key: &str) -> Option<bool> {
    map.get(key).and_then(|v| bool::try_from(v).ok())
}

fn get_u64(map: &HashMap<String, OwnedValue>, key: &str) -> Option<u64> {
    map.get(key).and_then(|v| {
        u64::try_from(v)
            .ok()
            .or_else(|| u32::try_from(v).ok().map(u64::from))
            .or_else(|| i64::try_from(v).ok().and_then(|n| u64::try_from(n).ok()))
    })
}

fn get_string(map: &HashMap<String, OwnedValue>, key: &str) -> Option<String> {
    map.get(key).and_then(|v| {
        v.try_clone()
            .ok()
            .and_then(|cloned| String::try_from(cloned).ok())
    })
}

fn get_data_map(
    reply: &HashMap<String, OwnedValue>,
) -> Result<HashMap<String, OwnedValue>, String> {
    let raw = reply
        .get("data")
        .ok_or_else(|| "D-Bus reply missing 'data' dict.".to_string())?;
    let cloned = raw
        .try_clone()
        .map_err(|e| format!("Failed cloning 'data' dict: {e}"))?;
    HashMap::<String, OwnedValue>::try_from(cloned)
        .map_err(|e| format!("Failed decoding 'data' dict: {e}"))
}

async fn build_proxy<'a>(
    conn: &'a zbus::Connection,
    dest: &str,
    path: &str,
    iface: &str,
) -> Result<zbus::Proxy<'a>, String> {
    zbus::proxy::Builder::new(conn)
        .destination(dest.to_string())
        .map_err(|e| format!("Invalid D-Bus destination '{dest}': {e}"))?
        .path(path.to_string())
        .map_err(|e| format!("Invalid D-Bus path '{path}': {e}"))?
        .interface(iface.to_string())
        .map_err(|e| format!("Invalid D-Bus interface '{iface}': {e}"))?
        .cache_properties(CacheProperties::No)
        .build()
        .await
        .map_err(|e| format!("Proxy build error for '{dest}': {e}"))
}

#[derive(Debug, Clone)]
pub struct DownstreamHealth {
    pub runtime_reachable: bool,
    pub daemon_reachable: bool,
    pub policy_reachable: bool,
    pub recovery_reachable: bool,
    pub runtime_state: String,
    pub vram_locked: bool,
    pub active_vram_mb: u64,
    pub hardware_profile: String,
    pub active_model_id: String,
    pub loopback_only: bool,
    pub last_error: String,
}

impl Default for DownstreamHealth {
    fn default() -> Self {
        Self {
            runtime_reachable: false,
            daemon_reachable: false,
            policy_reachable: false,
            recovery_reachable: false,
            runtime_state: "Standby".to_string(),
            vram_locked: false,
            active_vram_mb: 0,
            hardware_profile: "dev-baseline".to_string(),
            active_model_id: String::new(),
            loopback_only: true,
            last_error: String::new(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct SealedUntrusted {
    pub envelope_id: String,
    pub wrapped_payload: String,
    pub injection_detected: bool,
    pub self_modification_injection: bool,
}

#[derive(Debug, Clone)]
pub enum RuntimePlanInference {
    Completed {
        output_wrapped_payload: String,
        output_envelope_id: String,
        output_executable: bool,
        output_policy_authority: String,
    },
    PassiveStandby,
}

#[derive(Debug, Clone)]
pub struct StepExecutionResult {
    pub succeeded: bool,
    pub code: String,
    pub policy_rule_id: String,
    pub policy_action_class: String,
    pub output_preview: String,
    pub executable: bool,
    pub policy_authority: String,
}

pub async fn probe_downstream_health(
    sys_conn: &zbus::Connection,
    cfg: &AgentConfig,
) -> DownstreamHealth {
    let timeout = Duration::from_millis(cfg.dbus_timeout_ms.min(2_500));
    let mut health = DownstreamHealth::default();

    // 1. ZCI Runtime
    let rt_res: Result<(), String> = async {
        let proxy = tokio::time::timeout(
            timeout,
            build_proxy(
                sys_conn,
                &cfg.runtime_bus_name,
                &cfg.runtime_object_path,
                &cfg.runtime_interface,
            ),
        )
        .await
        .map_err(|_| "zci-runtime probe timed out.".to_string())??;
        let reply: HashMap<String, OwnedValue> =
            tokio::time::timeout(timeout, proxy.call("GetRuntimeStatus", &()))
                .await
                .map_err(|_| "GetRuntimeStatus timed out.".to_string())?
                .map_err(|e| format!("GetRuntimeStatus failed: {e}"))?;
        if !get_bool(&reply, "success").unwrap_or(false) {
            return Err("GetRuntimeStatus returned success=false.".to_string());
        }
        let d = get_data_map(&reply)?;
        health.runtime_reachable = true;
        health.runtime_state =
            get_string(&d, "runtime_state").unwrap_or_else(|| "Standby".to_string());
        health.vram_locked = get_bool(&d, "vram_locked").unwrap_or(false);
        health.active_vram_mb = get_u64(&d, "active_vram_mb").unwrap_or(0);
        health.hardware_profile =
            get_string(&d, "hardware_profile").unwrap_or_else(|| "dev-baseline".to_string());
        health.active_model_id = get_string(&d, "active_model_id").unwrap_or_default();
        health.loopback_only = get_bool(&d, "loopback_only").unwrap_or(true);
        if get_bool(&d, "daemon_bridge_reachable").unwrap_or(false) {
            health.daemon_reachable = true;
        }
        if get_bool(&d, "policy_engine_reachable").unwrap_or(false) {
            health.policy_reachable = true;
        }
        Ok(())
    }
    .await;
    if let Err(err) = rt_res {
        health.last_error = err;
    }

    // 2. ZCI Daemon
    if !health.daemon_reachable {
        if let Ok(Ok(proxy)) = tokio::time::timeout(
            timeout,
            build_proxy(
                sys_conn,
                &cfg.daemon_bus_name,
                &cfg.daemon_object_path,
                &cfg.daemon_interface,
            ),
        )
        .await
        {
            if let Ok(Ok(reply)) = tokio::time::timeout(
                timeout,
                proxy.call::<_, _, HashMap<String, OwnedValue>>("GetDaemonStatus", &()),
            )
            .await
            {
                if get_bool(&reply, "success").unwrap_or(false) {
                    health.daemon_reachable = true;
                    if let Ok(d) = get_data_map(&reply) {
                        if get_bool(&d, "policy_engine_reachable").unwrap_or(false) {
                            health.policy_reachable = true;
                        }
                    }
                }
            }
        }
    }

    // 3. Security Policy Engine
    if !health.policy_reachable {
        if let Ok(Ok(proxy)) = tokio::time::timeout(
            timeout,
            build_proxy(
                sys_conn,
                &cfg.policy_bus_name,
                &cfg.policy_object_path,
                &cfg.policy_interface,
            ),
        )
        .await
        {
            if let Ok(Ok(reply)) = tokio::time::timeout(
                timeout,
                proxy.call::<_, _, HashMap<String, OwnedValue>>("GetPolicySummary", &()),
            )
            .await
            {
                if get_bool(&reply, "success").unwrap_or(false) {
                    health.policy_reachable = true;
                }
            }
        }
    }

    // 4. Read-only Recovery service probe (non-fatal if offline in isolated bus)
    if let Ok(Ok(proxy)) = tokio::time::timeout(
        Duration::from_millis(1_500),
        build_proxy(
            sys_conn,
            &cfg.recovery_bus_name,
            &cfg.recovery_object_path,
            &cfg.recovery_interface,
        ),
    )
    .await
    {
        if let Ok(Ok(reply)) = tokio::time::timeout(
            Duration::from_millis(1_500),
            proxy.call::<_, _, HashMap<String, OwnedValue>>("GetState", &()),
        )
        .await
        {
            if get_bool(&reply, "success").unwrap_or(false) {
                health.recovery_reachable = true;
            }
        }
    }

    health
}

/// 13.03 — ReadOnly Snapper & Recovery Lock Preflight Check.
/// NEVER executes rollback or repair, and fails closed if tampered locks ever report `true`.
pub async fn run_recovery_snapper_preflight(
    sys_conn: &zbus::Connection,
    cfg: &AgentConfig,
) -> Result<SnapperPreflightReport, String> {
    let timeout = Duration::from_millis(cfg.dbus_timeout_ms.min(2_500));
    if let Ok(Ok(proxy)) = tokio::time::timeout(
        timeout,
        build_proxy(
            sys_conn,
            &cfg.recovery_bus_name,
            &cfg.recovery_object_path,
            &cfg.recovery_interface,
        ),
    )
    .await
    {
        if let Ok(Ok(reply)) = tokio::time::timeout(
            timeout,
            proxy.call::<_, _, HashMap<String, OwnedValue>>("GetState", &()),
        )
        .await
        {
            if get_bool(&reply, "success").unwrap_or(false) {
                let d = get_data_map(&reply).unwrap_or_default();
                let rollback_en = get_bool(&d, "rollbackExecutionEnabled")
                    .or_else(|| get_bool(&d, "rollback_execution_enabled"))
                    .unwrap_or(false);
                let repair_en = get_bool(&d, "repairExecutionEnabled")
                    .or_else(|| get_bool(&d, "repair_execution_enabled"))
                    .unwrap_or(false);

                if rollback_en || repair_en {
                    return Err(
                        "Fail-closed güvenlik ihlali: Recovery servisi kilitleri (rollback/repair=false) bozulmuş görünüyor."
                            .to_string(),
                    );
                }
                let snap_count = get_u64(&d, "snapshotCount")
                    .or_else(|| get_u64(&d, "snapshot_count"))
                    .unwrap_or(0);
                let status = get_string(&d, "status")
                    .or_else(|| get_string(&reply, "code"))
                    .unwrap_or_else(|| "ready".to_string());
                return Ok(SnapperPreflightReport {
                    checked: true,
                    recovery_service_reachable: true,
                    recovery_status: status.clone(),
                    snapshot_count: snap_count,
                    rollback_execution_enabled: false,
                    repair_execution_enabled: false,
                    summary: format!(
                        "Recovery.v1.GetState doğrulandı (durum: {status}, anlık görüntü: {snap_count}, rollback=false, repair=false)."
                    ),
                });
            }
        }
    }

    // Read-only local fallback check (zero writes, zero subprocesses) when org.zethropol.Recovery is not on this bus.
    let snapper_cfg_present = Path::new("/etc/snapper/configs/root").exists();
    Ok(SnapperPreflightReport {
        checked: true,
        recovery_service_reachable: false,
        recovery_status: if snapper_cfg_present {
            "local-snapper-config-present".to_string()
        } else {
            "readonly-fallback-locks-preserved".to_string()
        },
        snapshot_count: 0,
        rollback_execution_enabled: false,
        repair_execution_enabled: false,
        summary: if snapper_cfg_present {
            "Snapper root yapılandırması salt-okunur doğrulandı; rollback_execution_enabled=false ve repair_execution_enabled=false korunuyor.".to_string()
        } else {
            "Recovery servisi bu veri yolunda değil; yalnızca izinli kullanıcı-alanı ön-kontrolü uygulandı (rollback=false, repair=false).".to_string()
        },
    })
}

pub async fn seal_untrusted_via_daemon(
    sys_conn: &zbus::Connection,
    cfg: &AgentConfig,
    agent_id: &str,
    source_kind: &str,
    content: &str,
) -> Result<SealedUntrusted, String> {
    let timeout = Duration::from_millis(cfg.dbus_timeout_ms);
    let proxy = tokio::time::timeout(
        timeout,
        build_proxy(
            sys_conn,
            &cfg.daemon_bus_name,
            &cfg.daemon_object_path,
            &cfg.daemon_interface,
        ),
    )
    .await
    .map_err(|_| "Timed out connecting to ZCI Daemon for untrusted sealing.".to_string())??;

    let reply: HashMap<String, OwnedValue> = tokio::time::timeout(
        timeout,
        proxy.call("WrapUntrustedContent", &(agent_id, source_kind, content)),
    )
    .await
    .map_err(|_| "Timed out awaiting ZCI Daemon WrapUntrustedContent.".to_string())?
    .map_err(|e| format!("ZCI Daemon WrapUntrustedContent failed: {e}"))?;

    if !get_bool(&reply, "success").unwrap_or(false) {
        let code = get_string(&reply, "code").unwrap_or_else(|| "wrap-failed".to_string());
        let msg = get_string(&reply, "message").unwrap_or_default();
        return Err(format!("Fail-closed untrusted sealing denied ({code}): {msg}"));
    }

    let data = get_data_map(&reply)?;
    let executable = get_bool(&data, "executable").unwrap_or(true);
    let policy_authority = get_string(&data, "policy_authority").unwrap_or_default();
    let wrapped_payload = get_string(&data, "wrapped_payload")
        .ok_or_else(|| "WrapUntrustedContent reply missing wrapped_payload.".to_string())?;

    if executable || policy_authority != "none" || wrapped_payload.is_empty() {
        return Err(
            "Fail-closed violation: sealed payload did not guarantee executable=false & policy_authority=none."
                .to_string(),
        );
    }

    Ok(SealedUntrusted {
        envelope_id: get_string(&data, "envelope_id").unwrap_or_default(),
        wrapped_payload,
        injection_detected: get_bool(&data, "injection_detected").unwrap_or(false),
        self_modification_injection: get_bool(&data, "self_modification_injection")
            .unwrap_or(false),
    })
}

pub async fn invoke_runtime_planner(
    sys_conn: &zbus::Connection,
    cfg: &AgentConfig,
    agent_id: &str,
    model_id: &str,
    prompt: &str,
) -> Result<RuntimePlanInference, (String, String)> {
    let timeout = Duration::from_millis(cfg.inference_timeout_ms);
    let proxy = tokio::time::timeout(
        Duration::from_millis(cfg.dbus_timeout_ms),
        build_proxy(
            sys_conn,
            &cfg.runtime_bus_name,
            &cfg.runtime_object_path,
            &cfg.runtime_interface,
        ),
    )
    .await
    .map_err(|_| {
        (
            "zci-runtime-unavailable".to_string(),
            "Timed out connecting to ZCI Runtime.".to_string(),
        )
    })?
    .map_err(|e| ("zci-runtime-unavailable".to_string(), e))?;

    let reply: HashMap<String, OwnedValue> = tokio::time::timeout(
        timeout,
        proxy.call("RunInference", &(agent_id, model_id, prompt, 512u32)),
    )
    .await
    .map_err(|_| {
        (
            "inference-timeout".to_string(),
            "Timed out awaiting local ZCI Runtime planner response.".to_string(),
        )
    })?
    .map_err(|e| {
        (
            "zci-runtime-unavailable".to_string(),
            format!("ZCI Runtime RunInference failed: {e}"),
        )
    })?;

    let success = get_bool(&reply, "success").unwrap_or(false);
    let code = get_string(&reply, "code").unwrap_or_else(|| "runtime-error".to_string());
    let message = get_string(&reply, "message").unwrap_or_default();
    let data = get_data_map(&reply).unwrap_or_default();

    if !success {
        return Err((code, message));
    }
    if code == "passive-standby" {
        return Ok(RuntimePlanInference::PassiveStandby);
    }

    let out_exec = get_bool(&data, "output_executable").unwrap_or(true);
    let out_auth = get_string(&data, "output_policy_authority").unwrap_or_default();
    let out_wrapped = get_string(&data, "output_wrapped_payload").unwrap_or_default();
    if out_exec || out_auth != "none" || out_wrapped.is_empty() {
        return Err((
            "output-isolation-violation".to_string(),
            "Fail-closed: ZCI Runtime planner output violated executable=false / policy_authority=none."
                .to_string(),
        ));
    }

    Ok(RuntimePlanInference::Completed {
        output_wrapped_payload: out_wrapped,
        output_envelope_id: get_string(&data, "output_envelope_id").unwrap_or_default(),
        output_executable: out_exec,
        output_policy_authority: out_auth,
    })
}

/// 13.02 — Execute a single validated, approved step strictly through `org.zethropol.ZciDaemon.v1`.
/// Opens a short-lived `zci-daemon` session for the step's `agent_id`, invokes `ExecuteAction`,
/// verifies policy classification & untrusted output sealing, and closes the session cleanly.
pub async fn execute_step_via_daemon(
    sys_conn: &zbus::Connection,
    cfg: &AgentConfig,
    step: &PlanStep,
) -> Result<StepExecutionResult, String> {
    if step.action_kind == "recovery_readiness" {
        let report = run_recovery_snapper_preflight(sys_conn, cfg).await?;
        return Ok(StepExecutionResult {
            succeeded: true,
            code: "ok".to_string(),
            policy_rule_id: "AGENT_READONLY_RECOVERY_PREFLIGHT".to_string(),
            policy_action_class: "ReadOnly".to_string(),
            output_preview: report.summary,
            executable: false,
            policy_authority: "none".to_string(),
        });
    }

    let timeout = Duration::from_millis(cfg.dbus_timeout_ms.max(8_000));
    let daemon = tokio::time::timeout(
        timeout,
        build_proxy(
            sys_conn,
            &cfg.daemon_bus_name,
            &cfg.daemon_object_path,
            &cfg.daemon_interface,
        ),
    )
    .await
    .map_err(|_| "Timed out connecting to ZCI Daemon.".to_string())??;

    // 1. Open ZCI Daemon session for the step's allowlisted agent identity (`zekans-code` / `zci-command`).
    let open_reply: HashMap<String, OwnedValue> = tokio::time::timeout(
        timeout,
        daemon.call("OpenSession", &(step.agent_id.as_str(),)),
    )
    .await
    .map_err(|_| "Timed out awaiting ZCI Daemon OpenSession.".to_string())?
    .map_err(|e| format!("ZCI Daemon OpenSession failed: {e}"))?;

    if !get_bool(&open_reply, "success").unwrap_or(false) {
        let code = get_string(&open_reply, "code").unwrap_or_else(|| "open-session-denied".to_string());
        let msg = get_string(&open_reply, "message").unwrap_or_default();
        return Ok(StepExecutionResult {
            succeeded: false,
            code,
            policy_rule_id: "DAEMON_OPEN_SESSION_DENIED".to_string(),
            policy_action_class: "SystemStateChange".to_string(),
            output_preview: msg,
            executable: false,
            policy_authority: "none".to_string(),
        });
    }
    let open_data = get_data_map(&open_reply)?;
    let daemon_session_id = get_string(&open_data, "session_id")
        .ok_or_else(|| "ZCI Daemon OpenSession reply missing session_id.".to_string())?;

    let (daemon_action_kind, daemon_op, daemon_target, daemon_iface) = match step.action_kind.as_str() {
        "read_file" => ("read_file", "read_file", step.target.as_str(), ""),
        "project_analysis" => ("project_analysis", "list_dir", step.target.as_str(), ""),
        "system_diagnostics" => (
            "system_diagnostics",
            "dbus_call",
            step.method.as_str(),
            step.dbus_interface.as_str(),
        ),
        "write_workspace_file" => ("userspace_preflight", "write_file", step.target.as_str(), ""),
        "create_workspace_file" => ("userspace_preflight", "create_file", step.target.as_str(), ""),
        other => {
            let _ = daemon
                .call::<_, _, HashMap<String, OwnedValue>>("CloseSession", &(daemon_session_id.as_str(),))
                .await;
            return Err(format!("Unsupported step action_kind '{other}' (fail-closed)."));
        }
    };

    // 2. Execute policy-gated action through `org.zethropol.ZciDaemon.v1.ExecuteAction`
    let exec_reply: HashMap<String, OwnedValue> = tokio::time::timeout(
        timeout,
        daemon.call(
            "ExecuteAction",
            &(
                daemon_session_id.as_str(),
                step.agent_id.as_str(),
                daemon_action_kind,
                daemon_op,
                daemon_target,
                daemon_iface,
                "",
            ),
        ),
    )
    .await
    .map_err(|_| "Timed out awaiting ZCI Daemon ExecuteAction.".to_string())?
    .map_err(|e| format!("ZCI Daemon ExecuteAction failed: {e}"))?;

    // Always close short-lived daemon session cleanly
    let _ = daemon
        .call::<_, _, HashMap<String, OwnedValue>>("CloseSession", &(daemon_session_id.as_str(),))
        .await;

    let success = get_bool(&exec_reply, "success").unwrap_or(false);
    let code = get_string(&exec_reply, "code").unwrap_or_else(|| "action-failed".to_string());
    let message = get_string(&exec_reply, "message").unwrap_or_default();
    let d = get_data_map(&exec_reply).unwrap_or_default();
    let action_class = get_string(&d, "action_class").unwrap_or_default();
    let rule_id = get_string(&d, "policy_rule_id")
        .or_else(|| get_string(&d, "rule_id"))
        .unwrap_or_default();

    if !success {
        return Ok(StepExecutionResult {
            succeeded: false,
            code,
            policy_rule_id: rule_id,
            policy_action_class: action_class,
            output_preview: message,
            executable: false,
            policy_authority: "none".to_string(),
        });
    }

    // 3. For approved workspace writes, perform bounded non-symlink atomic write ONLY after
    //    `zci-daemon` returned `delegated-userspace-approved` with `UserSpaceSafe` classification!
    if step.action_kind == "write_workspace_file" || step.action_kind == "create_workspace_file" {
        if code != "delegated-userspace-approved" || action_class != "UserSpaceSafe" {
            return Ok(StepExecutionResult {
                succeeded: false,
                code: "userspace-preflight-mismatch".to_string(),
                policy_rule_id: rule_id,
                policy_action_class: action_class,
                output_preview:
                    "Fail-closed: zci-daemon UserSpaceSafe delegasyon onayı doğrulanamadı."
                        .to_string(),
                executable: false,
                policy_authority: "none".to_string(),
            });
        }
        match execute_approved_workspace_write(
            &step.target,
            &step.content,
            &cfg.home_dir,
            &cfg.extra_workspace_roots,
        ) {
            Ok(bytes_written) => {
                return Ok(StepExecutionResult {
                    succeeded: true,
                    code: "ok".to_string(),
                    policy_rule_id: rule_id,
                    policy_action_class: action_class,
                    output_preview: format!(
                        "Onaylı çalışma alanı dosyası güvenle yazıldı ({bytes_written} bayt): {}",
                        step.target
                    ),
                    executable: false,
                    policy_authority: "none".to_string(),
                });
            }
            Err(err) => {
                return Ok(StepExecutionResult {
                    succeeded: false,
                    code: "workspace-write-denied".to_string(),
                    policy_rule_id: rule_id,
                    policy_action_class: action_class,
                    output_preview: err,
                    executable: false,
                    policy_authority: "none".to_string(),
                });
            }
        }
    }

    // 4. Read-only steps MUST carry `executable=false` and `policy_authority="none"` sealed output
    let exec_flag = get_bool(&d, "executable").unwrap_or(true);
    let auth = get_string(&d, "policy_authority").unwrap_or_default();
    let wrapped = get_string(&d, "wrapped_payload").unwrap_or_default();
    if exec_flag || auth != "none" {
        return Err(
            "Fail-closed: zci-daemon action output violated executable=false / policy_authority=none."
                .to_string(),
        );
    }
    let clean_preview: String = crate::planner::extract_display_text_from_envelope(&wrapped)
        .chars()
        .take(320)
        .collect();

    Ok(StepExecutionResult {
        succeeded: true,
        code,
        policy_rule_id: rule_id,
        policy_action_class: action_class,
        output_preview: if clean_preview.is_empty() {
            message
        } else {
            clean_preview
        },
        executable: false,
        policy_authority: "none".to_string(),
    })
}

/// Strictly bounded atomic user-space file write (zero shell, symlink-refusing, allowlisted roots only).
fn execute_approved_workspace_write(
    raw_target: &str,
    content: &str,
    home_dir: &Path,
    extra_roots: &[std::path::PathBuf],
) -> Result<usize, String> {
    if content.len() > MAX_WRITE_FILE_BYTES || content.contains('\0') {
        return Err("Dosya içeriği boyut veya ikili güvenlik sınırını aşıyor.".to_string());
    }
    let target = validate_writable_workspace_path(raw_target, home_dir, extra_roots)?;
    let parent = target
        .parent()
        .ok_or_else(|| "Hedef dosyanın üst dizini bulunamadı.".to_string())?;
    if parent.exists() {
        let pmeta = fs::symlink_metadata(parent)
            .map_err(|e| format!("Üst dizin incelenemedi: {e}"))?;
        if pmeta.file_type().is_symlink() || !pmeta.is_dir() {
            return Err("Üst dizin sembolik bağlantı olamaz (fail-closed).".to_string());
        }
    } else {
        fs::create_dir_all(parent)
            .map_err(|e| format!("Çalışma alanı dizini oluşturulamadı: {e}"))?;
    }

    if target.exists() {
        let meta = fs::symlink_metadata(&target)
            .map_err(|e| format!("Hedef dosya incelenemedi: {e}"))?;
        if meta.file_type().is_symlink() || !meta.is_file() {
            return Err("Sembolik bağlantı veya normal olmayan dosya üzerine yazılamaz.".to_string());
        }
    }

    let tmp_path = parent.join(format!(".zci-agent-write.tmp.{}", Uuid::new_v4()));
    {
        let mut f = OpenOptions::new()
            .create_new(true)
            .write(true)
            .mode(0o644)
            .open(&tmp_path)
            .map_err(|e| format!("Geçici çalışma dosyası oluşturulamadı: {e}"))?;
        f.write_all(content.as_bytes())
            .map_err(|e| format!("Geçici çalışma dosyası yazılamadı: {e}"))?;
        f.sync_all()
            .map_err(|e| format!("Geçici çalışma dosyası diske eşitlenemedi: {e}"))?;
    }
    let _ = fs::set_permissions(&tmp_path, fs::Permissions::from_mode(0o644));
    fs::rename(&tmp_path, &target)
        .map_err(|e| format!("Atomik dosya yazma tamamlanamadı: {e}"))?;
    Ok(content.len())
}
