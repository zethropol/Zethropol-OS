use crate::bridge::{
    collect_readonly_system_context, invoke_runtime_inference, probe_downstream_health,
    seal_untrusted_via_daemon, RuntimeInferenceResult,
};
use crate::config::{
    ChatConfig, CHAT_BUS_NAME, CHAT_VERSION, MAX_MESSAGES_PER_SESSION, MAX_SESSIONS,
    MAX_USER_MESSAGE_BYTES,
};
use crate::crypto_store::{
    now_unix_secs, ChatMessage, ChatSession, EncryptedChatStore, PersistedChatData,
};
use crate::guard::{
    assemble_and_trim_prompt, extract_display_text_from_envelope, inspect_user_message_safety,
    InputSafetyVerdict,
};
use serde_json::json;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
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
    s.chars().any(|c| c == '\0' || (c.is_control() && c != '\n' && c != '\r' && c != '\t'))
}

fn sanitize_single_line(raw: &str, fallback: &str, max_chars: usize) -> String {
    let cleaned: String = raw
        .chars()
        .filter(|c| !c.is_control() && *c != '\0')
        .take(max_chars)
        .collect();
    let trimmed = cleaned.trim();
    if trimmed.is_empty() {
        fallback.to_string()
    } else {
        trimmed.to_string()
    }
}

fn preview_text(raw: &str, max_chars: usize) -> String {
    let s: String = raw
        .chars()
        .map(|c| if c == '\n' || c == '\r' { ' ' } else { c })
        .take(max_chars)
        .collect();
    s.trim().to_string()
}

#[derive(Default)]
struct ChatState {
    data: PersistedChatData,
    store_error: String,
    blocked_command_attempts: u64,
    blocked_injection_attempts: u64,
    wrapped_payloads_total: u64,
    system_context_injections_total: u64,
}

pub struct ZciChatService {
    config: ChatConfig,
    /// Downstream connection to system bus (`org.zethropol.Security`, `ZciDaemon`, `ZciRuntime`).
    /// In production this is the system D-Bus; in isolated tests this shares the private test bus.
    system_conn: zbus::Connection,
    store: Option<EncryptedChatStore>,
    state: Arc<Mutex<ChatState>>,
}

impl ZciChatService {
    pub fn new(config: ChatConfig, system_conn: zbus::Connection) -> Self {
        let mut st = ChatState::default();
        let store = match EncryptedChatStore::open(config.storage_dir.clone()) {
            Ok(s) => match s.load() {
                Ok(loaded) => {
                    st.data = loaded;
                    Some(s)
                }
                Err(err) => {
                    st.store_error = err;
                    Some(s)
                }
            },
            Err(err) => {
                st.store_error = err;
                None
            }
        };

        Self {
            config,
            system_conn,
            store,
            state: Arc::new(Mutex::new(st)),
        }
    }

    fn persist_locked(&self, st: &mut ChatState) -> Result<(), String> {
        let Some(ref store) = self.store else {
            return Err(format!(
                "Encrypted chat store unavailable: {}",
                st.store_error
            ));
        };
        store.save(&st.data)?;
        st.store_error.clear();
        Ok(())
    }

    async fn build_chat_status(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let health = probe_downstream_health(&self.system_conn, &self.config).await;

        let data = {
            let guard = self
                .state
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Chat state lock poisoned".to_string()))?;

            let session_count = guard.data.sessions.len() as u64;
            let total_messages: u64 = guard
                .data
                .sessions
                .iter()
                .map(|s| s.messages.len() as u64)
                .sum();
            let strict_perms = self
                .store
                .as_ref()
                .map(|s| s.verify_strict_permissions())
                .unwrap_or(false);
            let storage_dir = self
                .store
                .as_ref()
                .map(|s| s.storage_dir().display().to_string())
                .unwrap_or_else(|| self.config.storage_dir.display().to_string());

            let mut d = HashMap::new();
            d.insert("chat_version".to_string(), OwnedValue::from(Str::from(CHAT_VERSION)));
            d.insert("service_name".to_string(), OwnedValue::from(Str::from(CHAT_BUS_NAME)));
            d.insert("fail_closed".to_string(), OwnedValue::from(true));
            d.insert("external_telemetry_enabled".to_string(), OwnedValue::from(false));
            d.insert("command_execution_enabled".to_string(), OwnedValue::from(false));
            d.insert("loopback_only".to_string(), OwnedValue::from(health.loopback_only));
            d.insert("recovery_locks_protected".to_string(), OwnedValue::from(true));

            d.insert("runtime_reachable".to_string(), OwnedValue::from(health.runtime_reachable));
            d.insert("daemon_reachable".to_string(), OwnedValue::from(health.daemon_reachable));
            d.insert("policy_reachable".to_string(), OwnedValue::from(health.policy_reachable));
            d.insert(
                "runtime_state".to_string(),
                OwnedValue::from(Str::from(health.runtime_state.as_str())),
            );
            d.insert("vram_locked".to_string(), OwnedValue::from(health.vram_locked));
            d.insert("active_vram_mb".to_string(), OwnedValue::from(health.active_vram_mb));
            d.insert(
                "hardware_profile".to_string(),
                OwnedValue::from(Str::from(health.hardware_profile.as_str())),
            );
            d.insert(
                "active_model_id".to_string(),
                OwnedValue::from(Str::from(health.active_model_id.as_str())),
            );
            d.insert("max_context_tokens".to_string(), OwnedValue::from(health.max_context_tokens));
            d.insert("max_output_tokens".to_string(), OwnedValue::from(health.max_output_tokens));
            d.insert("cpu_cores".to_string(), OwnedValue::from(health.cpu_cores));
            d.insert("ram_available_mb".to_string(), OwnedValue::from(health.ram_available_mb));
            d.insert("ram_total_mb".to_string(), OwnedValue::from(health.ram_total_mb));
            d.insert("gpu_vram_total_mb".to_string(), OwnedValue::from(health.gpu_vram_total_mb));
            d.insert(
                "downstream_error".to_string(),
                OwnedValue::from(Str::from(health.last_error.as_str())),
            );

            d.insert("storage_dir".to_string(), OwnedValue::from(Str::from(storage_dir.as_str())));
            d.insert("storage_encrypted".to_string(), OwnedValue::from(self.store.is_some()));
            d.insert("storage_permissions_strict".to_string(), OwnedValue::from(strict_perms));
            d.insert(
                "storage_error".to_string(),
                OwnedValue::from(Str::from(guard.store_error.as_str())),
            );
            d.insert("session_count".to_string(), OwnedValue::from(session_count));
            d.insert("total_messages".to_string(), OwnedValue::from(total_messages));
            d.insert(
                "blocked_command_attempts".to_string(),
                OwnedValue::from(guard.blocked_command_attempts),
            );
            d.insert(
                "blocked_injection_attempts".to_string(),
                OwnedValue::from(guard.blocked_injection_attempts),
            );
            d.insert(
                "wrapped_payloads_total".to_string(),
                OwnedValue::from(guard.wrapped_payloads_total),
            );
            d.insert(
                "system_context_injections_total".to_string(),
                OwnedValue::from(guard.system_context_injections_total),
            );
            d
        };

        let all_ok = health.runtime_reachable && health.daemon_reachable && health.policy_reachable;
        let code = if all_ok {
            "ok"
        } else {
            "degraded-downstream-unavailable"
        };
        let msg = if all_ok {
            "ZCI Chatbot hazır; yerel loopback motor, ZCI Daemon ve Politika Motoru bağlı."
        } else {
            "ZCI Chatbot fail-closed beklemede; yerel ZCI Runtime/Daemon/Security servis bağlantısı bekleniyor."
        };
        Ok(envelope(true, code, msg, &operation_id, data))
    }
}

#[interface(name = "org.zethropol.ZciChat.v1")]
impl ZciChatService {
    /// 12.04 — Create a new isolated local chat session persisted in encrypted `0600` user storage.
    async fn create_session(
        &self,
        title: String,
        include_system_context: bool,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let clean_title = sanitize_single_line(&title, "Yeni ZCI Sohbeti", 80);
        let now = now_unix_secs();
        let session_id = format!("chat-{}", Uuid::new_v4());

        let mut guard = self
            .state
            .lock()
            .map_err(|_| zbus::fdo::Error::Failed("Chat state lock poisoned".to_string()))?;

        if guard.data.sessions.len() >= MAX_SESSIONS {
            return Ok(envelope(
                false,
                "session-limit-reached",
                "Maksimum yerel sohbet oturumu sayısına ulaşıldı; önce eski bir oturumu temizleyin.",
                &operation_id,
                HashMap::new(),
            ));
        }

        let session = ChatSession {
            session_id: session_id.clone(),
            title: clean_title.clone(),
            created_at_unix: now,
            updated_at_unix: now,
            include_system_context_default: include_system_context,
            messages: Vec::new(),
        };
        guard.data.sessions.push(session);
        if let Err(err) = self.persist_locked(&mut guard) {
            guard.data.sessions.pop();
            return Ok(envelope(
                false,
                "storage-error",
                &err,
                &operation_id,
                HashMap::new(),
            ));
        }

        let mut data = HashMap::new();
        data.insert("session_id".to_string(), OwnedValue::from(Str::from(session_id.as_str())));
        data.insert("title".to_string(), OwnedValue::from(Str::from(clean_title.as_str())));
        data.insert(
            "include_system_context".to_string(),
            OwnedValue::from(include_system_context),
        );
        data.insert("created_at_unix".to_string(), OwnedValue::from(now));

        Ok(envelope(
            true,
            "ok",
            "Yerel sohbet oturumu oluşturuldu.",
            &operation_id,
            data,
        ))
    }

    /// 12.01, 12.02, 12.03, 12.04 — Send a user turn through safety guard, bidirectional untrusted
    /// sealing, optional ReadOnly system context collector, token trimming, and `zci-runtime`.
    async fn send_message(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        session_id: String,
        user_message: String,
        include_system_context: bool,
        max_tokens: u32,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();

        if has_control_or_nul(&session_id) || has_control_or_nul(&user_message) {
            return Ok(envelope(
                false,
                "invalid-request",
                "Kontrol karakteri veya NUL bayt içeren mesaj reddedildi.",
                &operation_id,
                HashMap::new(),
            ));
        }
        let trimmed_msg = user_message.trim();
        if trimmed_msg.is_empty() {
            return Ok(envelope(
                false,
                "empty-message",
                "Boş sohbet iletisi gönderilemez.",
                &operation_id,
                HashMap::new(),
            ));
        }
        if trimmed_msg.len() > MAX_USER_MESSAGE_BYTES {
            return Ok(envelope(
                false,
                "message-too-large",
                "Sohbet iletisi azami boyut sınırını (16 KiB) aşıyor.",
                &operation_id,
                HashMap::new(),
            ));
        }

        // 1. 12.03 Fail-closed command execution (Stage 13) & prompt-injection guard BEFORE any runtime call!
        match inspect_user_message_safety(trimmed_msg) {
            InputSafetyVerdict::Allow => {}
            InputSafetyVerdict::DenyCommandExecution { pattern, reason } => {
                if let Ok(mut g) = self.state.lock() {
                    g.blocked_command_attempts += 1;
                }
                let mut d = HashMap::new();
                d.insert("blocked_pattern".to_string(), OwnedValue::from(Str::from(pattern.as_str())));
                d.insert("command_execution_enabled".to_string(), OwnedValue::from(false));
                d.insert("executable".to_string(), OwnedValue::from(false));
                d.insert("policy_authority".to_string(), OwnedValue::from(Str::from("none")));
                return Ok(envelope(
                    false,
                    "command-execution-denied-stage13",
                    &reason,
                    &operation_id,
                    d,
                ));
            }
            InputSafetyVerdict::DenyPromptInjection { pattern, reason } => {
                if let Ok(mut g) = self.state.lock() {
                    g.blocked_injection_attempts += 1;
                }
                let mut d = HashMap::new();
                d.insert("blocked_pattern".to_string(), OwnedValue::from(Str::from(pattern.as_str())));
                d.insert("injection_detected".to_string(), OwnedValue::from(true));
                d.insert("executable".to_string(), OwnedValue::from(false));
                d.insert("policy_authority".to_string(), OwnedValue::from(Str::from("none")));
                return Ok(envelope(
                    false,
                    "prompt-injection-blocked",
                    &reason,
                    &operation_id,
                    d,
                ));
            }
        }

        // 2. Snapshot target session history
        let (history_snapshot, default_sys_ctx) = {
            let guard = self
                .state
                .lock()
                .map_err(|_| zbus::fdo::Error::Failed("Chat state lock poisoned".to_string()))?;
            let Some(sess) = guard.data.sessions.iter().find(|s| s.session_id == session_id) else {
                return Ok(envelope(
                    false,
                    "session-not-found",
                    "Geçerli bir yerel sohbet oturumu bulunamadı.",
                    &operation_id,
                    HashMap::new(),
                ));
            };
            (sess.messages.clone(), sess.include_system_context_default)
        };
        let attach_sys_ctx = include_system_context || default_sys_ctx;

        // 3. Probe downstream local ZCI services (`zci-runtime`, `zci-daemon`, `security`)
        let health = probe_downstream_health(&self.system_conn, &self.config).await;
        if !health.daemon_reachable || !health.policy_reachable {
            return Ok(envelope(
                false,
                "zci-daemon-unavailable",
                "Fail-closed engel: ZCI Daemon veya Security Policy Engine erişilemiyor.",
                &operation_id,
                HashMap::new(),
            ));
        }

        // 4. 12.03 Mandatory untrusted sealing of user input (`executable=false`, `policy_authority="none"`)
        let sealed_user =
            match seal_untrusted_via_daemon(&self.system_conn, &self.config, "user_text", trimmed_msg)
                .await
            {
                Ok(s) => s,
                Err(err) => {
                    return Ok(envelope(
                        false,
                        "untrusted-seal-failed",
                        &err,
                        &operation_id,
                        HashMap::new(),
                    ));
                }
            };
        if let Ok(mut g) = self.state.lock() {
            g.wrapped_payloads_total += 1;
        }
        if sealed_user.self_modification_injection {
            if let Ok(mut g) = self.state.lock() {
                g.blocked_injection_attempts += 1;
            }
            return Ok(envelope(
                false,
                "prompt-injection-blocked",
                "Security Policy Engine öz-değişiklik / enjeksiyon girişimi tespit etti; ileti reddedildi.",
                &operation_id,
                HashMap::new(),
            ));
        }

        // 5. 12.02 Optional user-consented ReadOnly system context collection & untrusted sealing
        let sealed_sys_ctx = if attach_sys_ctx {
            match collect_readonly_system_context(&self.system_conn, &self.config, &health).await {
                Ok(ctx) => {
                    if let Ok(mut g) = self.state.lock() {
                        g.wrapped_payloads_total += 1;
                        g.system_context_injections_total += 1;
                    }
                    Some(ctx)
                }
                Err(err) => {
                    return Ok(envelope(
                        false,
                        "system-context-seal-failed",
                        &err,
                        &operation_id,
                        HashMap::new(),
                    ));
                }
            }
        } else {
            None
        };

        // 6. 12.01 Token-budget trimming aligned with active `zci-runtime` profile
        let effective_max_tokens = if max_tokens == 0 {
            health.max_output_tokens
        } else {
            max_tokens.min(health.max_output_tokens)
        };
        let safe_sys_ctx = sealed_sys_ctx
            .as_ref()
            .map(|c| extract_display_text_from_envelope(&c.wrapped_payload));
        let safe_user_turn = extract_display_text_from_envelope(&sealed_user.wrapped_payload);
        let trimmed_prompt = match assemble_and_trim_prompt(
            &history_snapshot,
            safe_sys_ctx.as_deref(),
            &safe_user_turn,
            health.max_context_tokens,
            effective_max_tokens,
        ) {
            Ok(t) => t,
            Err(err) => {
                return Ok(envelope(
                    false,
                    "context-limit-exceeded",
                    &err,
                    &operation_id,
                    HashMap::new(),
                ));
            }
        };

        if !health.runtime_reachable {
            return Ok(envelope(
                false,
                "zci-runtime-unavailable",
                "Fail-closed engel: ZCI Runtime servisi erişilemiyor.",
                &operation_id,
                HashMap::new(),
            ));
        }

        // 7. Execute inference strictly through `org.zethropol.ZciRuntime.v1.RunInference`
        let inference_res = invoke_runtime_inference(
            &self.system_conn,
            &self.config,
            &health.active_model_id,
            &trimmed_prompt.assembled_prompt,
            effective_max_tokens,
        )
        .await;

        match inference_res {
            Err((code, msg)) => Ok(envelope(false, &code, &msg, &operation_id, HashMap::new())),
            Ok(RuntimeInferenceResult::PassiveStandby {
                model_id,
                message,
                latency_ms,
            }) => {
                let mut d = HashMap::new();
                d.insert("session_id".to_string(), OwnedValue::from(Str::from(session_id.as_str())));
                d.insert("model_id".to_string(), OwnedValue::from(Str::from(model_id.as_str())));
                d.insert("runtime_state".to_string(), OwnedValue::from(Str::from("Standby")));
                d.insert("vram_locked".to_string(), OwnedValue::from(false));
                d.insert("latency_ms".to_string(), OwnedValue::from(latency_ms));
                d.insert("executable".to_string(), OwnedValue::from(false));
                d.insert("policy_authority".to_string(), OwnedValue::from(Str::from("none")));
                d.insert(
                    "trimmed_history_messages".to_string(),
                    OwnedValue::from(trimmed_prompt.trimmed_history_messages as u64),
                );
                d.insert(
                    "estimated_prompt_tokens".to_string(),
                    OwnedValue::from(trimmed_prompt.estimated_tokens),
                );
                d.insert("system_context_attached".to_string(), OwnedValue::from(attach_sys_ctx));
                Ok(envelope(
                    true,
                    "passive-standby",
                    &message,
                    &operation_id,
                    d,
                ))
            }
            Ok(RuntimeInferenceResult::Completed {
                model_id,
                output_wrapped_payload,
                output_envelope_id,
                output_executable,
                output_policy_authority,
                output_injection_detected,
                latency_ms,
            }) => {
                let display_reply = extract_display_text_from_envelope(&output_wrapped_payload);
                let now = now_unix_secs();
                let user_msg_id = format!("msg-{}", Uuid::new_v4());
                let asst_msg_id = format!("msg-{}", Uuid::new_v4());

                let user_record = ChatMessage {
                    message_id: user_msg_id.clone(),
                    role: "user".to_string(),
                    content: trimmed_msg.to_string(),
                    timestamp_unix: now,
                    executable: sealed_user.executable,
                    policy_authority: sealed_user.policy_authority.clone(),
                    envelope_id: sealed_user.envelope_id.clone(),
                    injection_detected: sealed_user.injection_detected,
                    system_context_attached: attach_sys_ctx,
                    model_id: String::new(),
                    latency_ms: 0,
                };
                let asst_record = ChatMessage {
                    message_id: asst_msg_id.clone(),
                    role: "assistant".to_string(),
                    content: display_reply.clone(),
                    timestamp_unix: now,
                    executable: output_executable,
                    policy_authority: output_policy_authority.clone(),
                    envelope_id: output_envelope_id.clone(),
                    injection_detected: output_injection_detected,
                    system_context_attached: attach_sys_ctx,
                    model_id: model_id.clone(),
                    latency_ms,
                };

                {
                    let mut guard = self.state.lock().map_err(|_| {
                        zbus::fdo::Error::Failed("Chat state lock poisoned".to_string())
                    })?;
                    guard.wrapped_payloads_total += 1;
                    if let Some(sess) = guard
                        .data
                        .sessions
                        .iter_mut()
                        .find(|s| s.session_id == session_id)
                    {
                        sess.messages.push(user_record);
                        sess.messages.push(asst_record);
                        if sess.messages.len() > MAX_MESSAGES_PER_SESSION {
                            let excess = sess.messages.len() - MAX_MESSAGES_PER_SESSION;
                            sess.messages.drain(0..excess);
                        }
                        sess.updated_at_unix = now;
                    }
                    let _ = self.persist_locked(&mut guard);
                }

                let _ = Self::message_received(
                    &emitter,
                    session_id.clone(),
                    asst_msg_id.clone(),
                    "assistant".to_string(),
                    preview_text(&display_reply, 120),
                    output_executable,
                    output_policy_authority.clone(),
                )
                .await;

                let mut d = HashMap::new();
                d.insert("session_id".to_string(), OwnedValue::from(Str::from(session_id.as_str())));
                d.insert("user_message_id".to_string(), OwnedValue::from(Str::from(user_msg_id.as_str())));
                d.insert("assistant_message_id".to_string(), OwnedValue::from(Str::from(asst_msg_id.as_str())));
                d.insert("reply_text".to_string(), OwnedValue::from(Str::from(display_reply.as_str())));
                d.insert("model_id".to_string(), OwnedValue::from(Str::from(model_id.as_str())));
                d.insert("latency_ms".to_string(), OwnedValue::from(latency_ms));
                d.insert("executable".to_string(), OwnedValue::from(output_executable));
                d.insert(
                    "policy_authority".to_string(),
                    OwnedValue::from(Str::from(output_policy_authority.as_str())),
                );
                d.insert(
                    "input_envelope_id".to_string(),
                    OwnedValue::from(Str::from(sealed_user.envelope_id.as_str())),
                );
                d.insert(
                    "output_envelope_id".to_string(),
                    OwnedValue::from(Str::from(output_envelope_id.as_str())),
                );
                d.insert(
                    "injection_detected".to_string(),
                    OwnedValue::from(sealed_user.injection_detected || output_injection_detected),
                );
                d.insert("system_context_attached".to_string(), OwnedValue::from(attach_sys_ctx));
                d.insert(
                    "kept_history_messages".to_string(),
                    OwnedValue::from(trimmed_prompt.kept_history_messages as u64),
                );
                d.insert(
                    "trimmed_history_messages".to_string(),
                    OwnedValue::from(trimmed_prompt.trimmed_history_messages as u64),
                );
                d.insert(
                    "estimated_prompt_tokens".to_string(),
                    OwnedValue::from(trimmed_prompt.estimated_tokens),
                );
                d.insert("max_context_tokens".to_string(), OwnedValue::from(health.max_context_tokens));

                Ok(envelope(
                    true,
                    "ok",
                    "Yerel sohbet yanıtı güvenli izolasyonla üretildi.",
                    &operation_id,
                    d,
                ))
            }
        }
    }

    /// 12.04 — Retrieve encrypted-store history for a session.
    async fn get_session_history(
        &self,
        session_id: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let guard = self
            .state
            .lock()
            .map_err(|_| zbus::fdo::Error::Failed("Chat state lock poisoned".to_string()))?;

        let Some(sess) = guard.data.sessions.iter().find(|s| s.session_id == session_id) else {
            return Ok(envelope(
                false,
                "session-not-found",
                "Sohbet oturumu bulunamadı.",
                &operation_id,
                HashMap::new(),
            ));
        };

        let messages_json: Vec<String> = sess
            .messages
            .iter()
            .filter_map(|m| serde_json::to_string(m).ok())
            .collect();

        let mut d = HashMap::new();
        d.insert("session_id".to_string(), OwnedValue::from(Str::from(sess.session_id.as_str())));
        d.insert("title".to_string(), OwnedValue::from(Str::from(sess.title.as_str())));
        d.insert(
            "include_system_context".to_string(),
            OwnedValue::from(sess.include_system_context_default),
        );
        d.insert("message_count".to_string(), OwnedValue::from(sess.messages.len() as u64));
        d.insert("messages_json".to_string(), vec_to_owned(&messages_json));

        Ok(envelope(
            true,
            "ok",
            "Sohbet geçmişi yüklendi.",
            &operation_id,
            d,
        ))
    }

    /// List all local user chat sessions (metadata only).
    async fn list_sessions(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let guard = self
            .state
            .lock()
            .map_err(|_| zbus::fdo::Error::Failed("Chat state lock poisoned".to_string()))?;

        let sessions_json: Vec<String> = guard
            .data
            .sessions
            .iter()
            .map(|s| {
                json!({
                    "session_id": s.session_id,
                    "title": s.title,
                    "created_at_unix": s.created_at_unix,
                    "updated_at_unix": s.updated_at_unix,
                    "include_system_context": s.include_system_context_default,
                    "message_count": s.messages.len(),
                })
                .to_string()
            })
            .collect();

        let mut d = HashMap::new();
        d.insert("session_count".to_string(), OwnedValue::from(guard.data.sessions.len() as u64));
        d.insert("sessions_json".to_string(), vec_to_owned(&sessions_json));
        Ok(envelope(
            true,
            "ok",
            "Yerel sohbet oturumları listelendi.",
            &operation_id,
            d,
        ))
    }

    /// 12.04 & 12.06 — Clear a single session's history (or all sessions when `session_id == "*"`).
    async fn clear_session(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        session_id: String,
    ) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        let operation_id = Uuid::new_v4().to_string();
        let target = session_id.trim().to_string();
        let cleared_messages: u64;

        {
            let mut guard = self.state.lock().map_err(|_| {
                zbus::fdo::Error::Failed("Chat state lock poisoned".to_string())
            })?;
            if target == "*" {
                cleared_messages = guard
                    .data
                    .sessions
                    .iter()
                    .map(|s| s.messages.len() as u64)
                    .sum();
                guard.data.sessions.clear();
            } else if let Some(sess) = guard
                .data
                .sessions
                .iter_mut()
                .find(|s| s.session_id == target)
            {
                cleared_messages = sess.messages.len() as u64;
                sess.messages.clear();
                sess.updated_at_unix = now_unix_secs();
            } else {
                return Ok(envelope(
                    false,
                    "session-not-found",
                    "Temizlenecek sohbet oturumu bulunamadı.",
                    &operation_id,
                    HashMap::new(),
                ));
            }
            if let Err(err) = self.persist_locked(&mut guard) {
                return Ok(envelope(
                    false,
                    "storage-error",
                    &err,
                    &operation_id,
                    HashMap::new(),
                ));
            }
        }

        let _ = Self::session_cleared(
            &emitter,
            target.clone(),
            "user-requested-clear".to_string(),
        )
        .await;

        let mut d = HashMap::new();
        d.insert("session_id".to_string(), OwnedValue::from(Str::from(target.as_str())));
        d.insert("cleared_messages".to_string(), OwnedValue::from(cleared_messages));
        Ok(envelope(
            true,
            "ok",
            "Sohbet geçmişi güvenli biçimde temizlendi.",
            &operation_id,
            d,
        ))
    }

    /// 12.04 — Live ZCI Chatbot health, downstream runtime/daemon/policy posture & privacy telemetry.
    async fn get_chat_status(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        self.build_chat_status().await
    }

    async fn get_state(&self) -> zbus::fdo::Result<HashMap<String, OwnedValue>> {
        self.build_chat_status().await
    }

    #[zbus(signal)]
    pub async fn message_received(
        emitter: &SignalEmitter<'_>,
        session_id: String,
        message_id: String,
        role: String,
        content_preview: String,
        executable: bool,
        policy_authority: String,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    pub async fn session_cleared(
        emitter: &SignalEmitter<'_>,
        session_id: String,
        reason: String,
    ) -> zbus::Result<()>;
}
