use crate::audit_store::HashChainedAuditStore;
use crate::config::{
    AgentConfig, AGENT_BUS_NAME, AGENT_INTERFACE, AGENT_OBJECT_PATH, DAEMON_BUS_NAME,
    DAEMON_OBJECT_PATH, RECOVERY_BUS_NAME, RECOVERY_OBJECT_PATH, RUNTIME_BUS_NAME,
    RUNTIME_OBJECT_PATH,
};
use crate::dbus_service::ZciAgentService;
use crate::planner::{inspect_text_safety, parse_and_validate_model_plan, SafetyVerdict};
use std::collections::HashMap;
use std::fs;
use std::io::{BufRead, BufReader};
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use uuid::Uuid;
use zbus::interface;
use zbus::proxy::CacheProperties;
use zbus::zvariant::{OwnedValue, Str};

struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new(tag: &str) -> Self {
        let path = std::env::temp_dir().join(format!("zci-agent-test-{tag}-{}", Uuid::new_v4()));
        fs::create_dir_all(&path).expect("create temp dir");
        Self { path }
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

struct PrivateDbus {
    child: Child,
    address: String,
}

impl PrivateDbus {
    fn start() -> Self {
        let mut child = Command::new("/usr/bin/dbus-daemon")
            .args([
                "--session",
                "--nofork",
                "--nopidfile",
                "--print-address=1",
            ])
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .expect("spawn private dbus-daemon");
        let stdout = child.stdout.take().expect("dbus stdout");
        let mut reader = BufReader::new(stdout);
        let mut addr = String::new();
        reader.read_line(&mut addr).expect("read dbus address");
        Self {
            child,
            address: addr.trim().to_string(),
        }
    }
}

impl Drop for PrivateDbus {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn mock_envelope(
    success: bool,
    code: &str,
    message: &str,
    data: HashMap<String, OwnedValue>,
) -> HashMap<String, OwnedValue> {
    let mut out = HashMap::new();
    out.insert("success".to_string(), OwnedValue::from(success));
    out.insert("code".to_string(), OwnedValue::from(Str::from(code)));
    out.insert("message".to_string(), OwnedValue::from(Str::from(message)));
    out.insert("operationId".to_string(), OwnedValue::from(Str::from("mock-op")));
    out.insert("data".to_string(), OwnedValue::from(data));
    out
}

fn get_bool(map: &HashMap<String, OwnedValue>, key: &str) -> bool {
    map.get(key).and_then(|v| bool::try_from(v).ok()).unwrap_or(false)
}

fn get_str(map: &HashMap<String, OwnedValue>, key: &str) -> String {
    map.get(key)
        .and_then(|v| v.try_clone().ok())
        .and_then(|v| String::try_from(v).ok())
        .unwrap_or_default()
}

fn get_u64(map: &HashMap<String, OwnedValue>, key: &str) -> u64 {
    map.get(key)
        .and_then(|v| {
            u64::try_from(v)
                .ok()
                .or_else(|| u32::try_from(v).ok().map(u64::from))
        })
        .unwrap_or(0)
}

fn get_data(reply: &HashMap<String, OwnedValue>) -> HashMap<String, OwnedValue> {
    reply
        .get("data")
        .and_then(|v| v.try_clone().ok())
        .and_then(|v| HashMap::try_from(v).ok())
        .unwrap_or_default()
}

struct MockDaemon {
    exec_count: Arc<AtomicUsize>,
}

#[interface(name = "org.zethropol.ZciDaemon.v1")]
impl MockDaemon {
    async fn get_daemon_status(&self) -> HashMap<String, OwnedValue> {
        let mut d = HashMap::new();
        d.insert("policy_engine_reachable".to_string(), OwnedValue::from(true));
        mock_envelope(true, "ok", "mock daemon ok", d)
    }

    async fn open_session(&self, agent_id: String) -> HashMap<String, OwnedValue> {
        let mut d = HashMap::new();
        d.insert("session_id".to_string(), OwnedValue::from(Str::from("mock-sess-1")));
        d.insert("agent_id".to_string(), OwnedValue::from(Str::from(agent_id.as_str())));
        mock_envelope(true, "ok", "opened", d)
    }

    async fn close_session(&self, _session_id: String) -> HashMap<String, OwnedValue> {
        mock_envelope(true, "ok", "closed", HashMap::new())
    }

    async fn wrap_untrusted_content(
        &self,
        _agent_id: String,
        source_kind: String,
        content: String,
    ) -> HashMap<String, OwnedValue> {
        let wrapped = format!(
            "<<<ZCI_UNTRUSTED_DATA id=e1 source={source_kind} executable=false policy_authority=none>>>\n{content}\n<<<END_ZCI_UNTRUSTED_DATA id=e1>>>"
        );
        let mut d = HashMap::new();
        d.insert("executable".to_string(), OwnedValue::from(false));
        d.insert("policy_authority".to_string(), OwnedValue::from(Str::from("none")));
        d.insert("envelope_id".to_string(), OwnedValue::from(Str::from("env-1")));
        d.insert("wrapped_payload".to_string(), OwnedValue::from(Str::from(wrapped.as_str())));
        d.insert("injection_detected".to_string(), OwnedValue::from(false));
        d.insert("self_modification_injection".to_string(), OwnedValue::from(false));
        mock_envelope(true, "ok", "sealed", d)
    }

    #[allow(clippy::too_many_arguments)]
    async fn execute_action(
        &self,
        _session_id: String,
        _agent_id: String,
        action_kind: String,
        _operation: String,
        target: String,
        _dbus_interface: String,
        _payload: String,
    ) -> HashMap<String, OwnedValue> {
        self.exec_count.fetch_add(1, Ordering::SeqCst);
        let mut d = HashMap::new();
        if action_kind == "userspace_preflight" {
            d.insert("action_class".to_string(), OwnedValue::from(Str::from("UserSpaceSafe")));
            d.insert(
                "policy_rule_id".to_string(),
                OwnedValue::from(Str::from("ALLOW_WORKSPACE_WRITE")),
            );
            return mock_envelope(true, "delegated-userspace-approved", "approved", d);
        }
        let wrapped = format!(
            "<<<ZCI_UNTRUSTED_DATA id=out1 source=result executable=false policy_authority=none>>>\nResult for {target}\n<<<END_ZCI_UNTRUSTED_DATA id=out1>>>"
        );
        d.insert("action_class".to_string(), OwnedValue::from(Str::from("ReadOnly")));
        d.insert(
            "policy_rule_id".to_string(),
            OwnedValue::from(Str::from("ALLOW_READONLY")),
        );
        d.insert("executable".to_string(), OwnedValue::from(false));
        d.insert("policy_authority".to_string(), OwnedValue::from(Str::from("none")));
        d.insert("wrapped_payload".to_string(), OwnedValue::from(Str::from(wrapped.as_str())));
        mock_envelope(true, "ok", "ok", d)
    }
}

struct MockRuntime {
    planned_json: String,
}

#[interface(name = "org.zethropol.ZciRuntime.v1")]
impl MockRuntime {
    async fn get_runtime_status(&self) -> HashMap<String, OwnedValue> {
        let mut d = HashMap::new();
        d.insert("runtime_state".to_string(), OwnedValue::from(Str::from("Standby")));
        d.insert("vram_locked".to_string(), OwnedValue::from(false));
        d.insert("active_vram_mb".to_string(), OwnedValue::from(0u64));
        d.insert("hardware_profile".to_string(), OwnedValue::from(Str::from("dev-baseline")));
        d.insert("active_model_id".to_string(), OwnedValue::from(Str::from("gemma-4-E4B-it-Q4_K_M")));
        d.insert("loopback_only".to_string(), OwnedValue::from(true));
        d.insert("daemon_bridge_reachable".to_string(), OwnedValue::from(true));
        d.insert("policy_engine_reachable".to_string(), OwnedValue::from(true));
        mock_envelope(true, "ok", "ok", d)
    }

    async fn run_inference(
        &self,
        _agent_id: String,
        _model_id: String,
        _prompt: String,
        _max_tokens: u32,
    ) -> HashMap<String, OwnedValue> {
        let wrapped = format!(
            "<<<ZCI_UNTRUSTED_DATA id=m1 source=model_output executable=false policy_authority=none>>>\n{}\n<<<END_ZCI_UNTRUSTED_DATA id=m1>>>",
            self.planned_json
        );
        let mut d = HashMap::new();
        d.insert("output_wrapped_payload".to_string(), OwnedValue::from(Str::from(wrapped.as_str())));
        d.insert("output_envelope_id".to_string(), OwnedValue::from(Str::from("out-plan-1")));
        d.insert("output_executable".to_string(), OwnedValue::from(false));
        d.insert("output_policy_authority".to_string(), OwnedValue::from(Str::from("none")));
        mock_envelope(true, "ok", "ok", d)
    }
}

struct MockRecovery;

#[interface(name = "org.zethropol.Recovery.v1")]
impl MockRecovery {
    async fn get_state(&self) -> HashMap<String, OwnedValue> {
        let mut d = HashMap::new();
        d.insert("status".to_string(), OwnedValue::from(Str::from("installed-and-configured")));
        d.insert("snapshotCount".to_string(), OwnedValue::from(8u64));
        d.insert("rollbackExecutionEnabled".to_string(), OwnedValue::from(false));
        d.insert("repairExecutionEnabled".to_string(), OwnedValue::from(false));
        mock_envelope(true, "ok", "recovery ready", d)
    }
}

#[test]
fn hash_chained_audit_store_enforces_0700_0600_and_detects_tampering() {
    let tmp = TempDir::new("audit");
    let dir = tmp.path.join("zci-agent");
    let mut store = HashChainedAuditStore::open(dir.clone()).expect("open audit store");
    assert!(store.verify_strict_permissions());
    assert!(store.chain_valid());

    let e1 = store
        .append("plan_proposed", "p1", "", "zekans-code", "ReadOnly", "proposed", "step=1")
        .expect("append 1");
    let e2 = store
        .append("step_approved", "p1", "step-1", "zekans-code", "ApprovalRequired", "approved", "ok")
        .expect("append 2");
    assert_eq!(e2.prev_sha256, e1.entry_sha256);
    assert_eq!(store.entry_count(), 2);
    assert_eq!(store.head_sha256(), e2.entry_sha256);

    assert_eq!(fs::metadata(&dir).unwrap().permissions().mode() & 0o777, 0o700);
    assert_eq!(
        fs::metadata(store.audit_file()).unwrap().permissions().mode() & 0o777,
        0o600
    );

    let v_ok = store.verify_chain();
    assert!(v_ok.valid);
    assert_eq!(v_ok.entry_count, 2);

    // Tamper with one character in line 1 -> chain verification MUST fail closed!
    let raw = fs::read_to_string(store.audit_file()).unwrap();
    let tampered = raw.replacen("step=1", "step=9", 1);
    fs::write(store.audit_file(), tampered).unwrap();
    fs::set_permissions(store.audit_file(), fs::Permissions::from_mode(0o600)).unwrap();

    let v_bad = store.verify_chain();
    assert!(!v_bad.valid);
    assert!(v_bad.error.contains("tampered"));
}

#[test]
fn shell_injection_protected_paths_and_recovery_tamper_are_rejected_fail_closed() {
    assert_eq!(
        inspect_text_safety("Çalışma alanındaki projeyi analiz et"),
        SafetyVerdict::Allow
    );
    assert!(matches!(
        inspect_text_safety("sh -c 'cat /etc/passwd'"),
        SafetyVerdict::DenyShellInjection { .. }
    ));
    assert!(matches!(
        inspect_text_safety("bash -c 'echo pwned' | bash"),
        SafetyVerdict::DenyShellInjection { .. }
    ));
    assert!(matches!(
        inspect_text_safety("eval $(id)"),
        SafetyVerdict::DenyShellInjection { .. }
    ));
    assert!(matches!(
        inspect_text_safety("sudo pacman -Syu"),
        SafetyVerdict::DenyShellInjection { .. }
    ));
    assert!(matches!(
        inspect_text_safety("/boot/limine.conf dosyasını değiştir"),
        SafetyVerdict::DenyProtectedPath { .. }
    ));
    assert!(matches!(
        inspect_text_safety("/etc/zethropol/zci-runtime.json dosyasını sil"),
        SafetyVerdict::DenyProtectedPath { .. }
    ));
    assert!(matches!(
        inspect_text_safety("/usr/bin/zci-chat üzerine yaz"),
        SafetyVerdict::DenyProtectedPath { .. }
    ));
    assert!(matches!(
        inspect_text_safety("/sys/class/power_supply/BAT0 değiştir"),
        SafetyVerdict::DenyProtectedPath { .. }
    ));
    assert!(matches!(
        inspect_text_safety("rollback_execution_enabled = true yap"),
        SafetyVerdict::DenyPolicyTamper { .. }
    ));
    assert!(matches!(
        inspect_text_safety("repair_execution_enabled aç"),
        SafetyVerdict::DenyPolicyTamper { .. }
    ));

    // Model attempting an unlisted raw shell action MUST be rejected fail-closed!
    let tmp = TempDir::new("planner-reject");
    let bad_plan = r#"{"summary":"bad","steps":[{"action_kind":"shell_exec","target":"rm -rf /"}]}"#;
    assert!(parse_and_validate_model_plan(bad_plan, "zekans-code", "", &tmp.path, &[]).is_err());

    let bad_write = r#"{"summary":"bad","steps":[{"action_kind":"write_workspace_file","target":"/etc/zethropol/x.conf","content":"x"}]}"#;
    assert!(parse_and_validate_model_plan(bad_write, "zekans-code", "", &tmp.path, &[]).is_err());
}

#[tokio::test]
async fn dbus_agent_lifecycle_blocks_unapproved_write_executes_after_approval_and_audits() {
    let tmp = TempDir::new("dbus-e2e");
    let fake_home = tmp.path.join("home");
    let workspace = fake_home.join("Projects").join("demo");
    fs::create_dir_all(&workspace).unwrap();
    let target_file = workspace.join("report.txt");

    let bus = PrivateDbus::start();
    let exec_count = Arc::new(AtomicUsize::new(0));

    let _daemon = zbus::connection::Builder::address(bus.address.as_str())
        .unwrap()
        .name(DAEMON_BUS_NAME)
        .unwrap()
        .serve_at(
            DAEMON_OBJECT_PATH,
            MockDaemon {
                exec_count: Arc::clone(&exec_count),
            },
        )
        .unwrap()
        .build()
        .await
        .unwrap();

    let planned_json = serde_json::json!({
        "summary": "Proje analizi ve onaylı rapor dosyası oluşturma",
        "steps": [
            {
                "action_kind": "project_analysis",
                "target": workspace.display().to_string(),
                "description": "Çalışma alanını salt-okunur analiz et"
            },
            {
                "action_kind": "create_workspace_file",
                "target": target_file.display().to_string(),
                "content": "Zethropol ZCI Agent onaylı rapor.\n",
                "description": "Onaylı rapor dosyasını oluştur"
            }
        ]
    })
    .to_string();

    let _runtime = zbus::connection::Builder::address(bus.address.as_str())
        .unwrap()
        .name(RUNTIME_BUS_NAME)
        .unwrap()
        .serve_at(RUNTIME_OBJECT_PATH, MockRuntime { planned_json })
        .unwrap()
        .build()
        .await
        .unwrap();

    let _recovery = zbus::connection::Builder::address(bus.address.as_str())
        .unwrap()
        .name(RECOVERY_BUS_NAME)
        .unwrap()
        .serve_at(RECOVERY_OBJECT_PATH, MockRecovery)
        .unwrap()
        .build()
        .await
        .unwrap();

    let sys_conn = zbus::connection::Builder::address(bus.address.as_str())
        .unwrap()
        .build()
        .await
        .unwrap();
    let mut cfg = AgentConfig::default();
    cfg.storage_dir = tmp.path.join("agent-store");
    cfg.home_dir = fake_home.clone();

    let _agent_srv = zbus::connection::Builder::address(bus.address.as_str())
        .unwrap()
        .name(AGENT_BUS_NAME)
        .unwrap()
        .serve_at(AGENT_OBJECT_PATH, ZciAgentService::new(cfg, sys_conn))
        .unwrap()
        .build()
        .await
        .unwrap();

    let client = zbus::connection::Builder::address(bus.address.as_str())
        .unwrap()
        .build()
        .await
        .unwrap();
    let proxy: zbus::Proxy<'_> = zbus::proxy::Builder::new(&client)
        .destination(AGENT_BUS_NAME)
        .unwrap()
        .path(AGENT_OBJECT_PATH)
        .unwrap()
        .interface(AGENT_INTERFACE)
        .unwrap()
        .cache_properties(CacheProperties::No)
        .build()
        .await
        .unwrap();

    // 1. GetAgentStatus checks security locks & audit chain
    let st: HashMap<String, OwnedValue> = proxy.call("GetAgentStatus", &()).await.unwrap();
    assert!(get_bool(&st, "success"));
    let st_d = get_data(&st);
    assert!(!get_bool(&st_d, "raw_shell_execution_enabled"));
    assert!(!get_bool(&st_d, "rollback_execution_enabled"));
    assert!(!get_bool(&st_d, "repair_execution_enabled"));
    assert!(get_bool(&st_d, "audit_chain_valid"));
    assert!(get_bool(&st_d, "audit_permissions_strict"));
    assert!(get_bool(&st_d, "recovery_reachable"));

    // 2. Shell injection goal blocked fail-closed BEFORE runtime or daemon
    let bad: HashMap<String, OwnedValue> = proxy
        .call("ProposePlan", &("sh -c 'rm -rf /tmp/x'", "zekans-code", workspace.to_str().unwrap()))
        .await
        .unwrap();
    assert!(!get_bool(&bad, "success"));
    assert_eq!(get_str(&bad, "code"), "shell-injection-blocked");

    // 3. Propose valid multi-step plan (1 ReadOnly + 1 ApprovalRequired write)
    let proposed: HashMap<String, OwnedValue> = proxy
        .call(
            "ProposePlan",
            &("Projeyi analiz et ve rapor dosyası yaz", "zekans-code", workspace.to_str().unwrap()),
        )
        .await
        .unwrap();
    assert!(get_bool(&proposed, "success"));
    let prop_d = get_data(&proposed);
    let plan_id = get_str(&prop_d, "plan_id");
    assert_eq!(get_u64(&prop_d, "step_count"), 2);
    assert_eq!(get_u64(&prop_d, "approval_required_count"), 1);
    assert!(!get_bool(&prop_d, "executable"));
    assert_eq!(get_str(&prop_d, "policy_authority"), "none");
    assert!(get_bool(&prop_d, "snapper_preflight_checked"));

    // 4. Executing BEFORE approving step-2 MUST FAIL CLOSED and NEVER write the file!
    let blocked: HashMap<String, OwnedValue> = proxy
        .call("ExecuteApprovedPlan", &(plan_id.as_str(),))
        .await
        .unwrap();
    assert!(!get_bool(&blocked, "success"));
    assert_eq!(get_str(&blocked, "code"), "unapproved-steps-pending");
    assert!(!target_file.exists());
    assert_eq!(exec_count.load(Ordering::SeqCst), 0);

    // 5. Explicitly approve step-2 via ApproveStep -> now ExecuteApprovedPlan succeeds!
    let approved: HashMap<String, OwnedValue> = proxy
        .call("ApproveStep", &(plan_id.as_str(), "step-2", true, "Kullanıcı onayı verildi"))
        .await
        .unwrap();
    assert!(get_bool(&approved, "success"));
    assert_eq!(get_str(&get_data(&approved), "approval_status"), "approved");

    let executed: HashMap<String, OwnedValue> = proxy
        .call("ExecuteApprovedPlan", &(plan_id.as_str(),))
        .await
        .unwrap();
    assert!(get_bool(&executed, "success"));
    let exec_d = get_data(&executed);
    assert_eq!(get_str(&exec_d, "status"), "completed");
    assert_eq!(get_u64(&exec_d, "completed_steps"), 2);
    assert_eq!(get_u64(&exec_d, "denied_steps"), 0);
    assert!(target_file.is_file());
    assert_eq!(
        fs::read_to_string(&target_file).unwrap(),
        "Zethropol ZCI Agent onaylı rapor.\n"
    );

    // 6. Verify immutable hash-chained audit trail recorded every decision & execution
    let audit: HashMap<String, OwnedValue> = proxy.call("GetAuditTrail", &(50u32,)).await.unwrap();
    assert!(get_bool(&audit, "success"));
    let aud_d = get_data(&audit);
    assert!(get_bool(&aud_d, "chain_valid"));
    assert!(get_bool(&aud_d, "permissions_strict"));
    assert!(get_u64(&aud_d, "entry_count") >= 7);

    // 7. Propose another plan and verify CancelPlan cleanly closes it
    let p2: HashMap<String, OwnedValue> = proxy
        .call("ProposePlan", &("Durum kontrolü", "zci-command", ""))
        .await
        .unwrap();
    let pid2 = get_str(&get_data(&p2), "plan_id");
    let cancelled: HashMap<String, OwnedValue> = proxy
        .call("CancelPlan", &(pid2.as_str(), "İhtiyaç kalmadı"))
        .await
        .unwrap();
    assert!(get_bool(&cancelled, "success"));
    assert_eq!(get_str(&get_data(&cancelled), "status"), "cancelled");
}
