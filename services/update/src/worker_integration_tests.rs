//! Isolated transport/process tests. Never call the production worker, system
//! bus, systemd manager, package executor or snapshot commands.
use super::*;
use std::io::{BufRead, BufReader};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

const SERVICE: &str = "org.zethropol.Update.IsolatedTest";

struct Workspace(PathBuf);
impl Workspace {
    fn new() -> Self {
        let path =
            std::env::temp_dir().join(format!("zethropol-worker-isolation-{}", Uuid::new_v4()));
        fs::DirBuilder::new().mode(0o700).create(&path).unwrap();
        Self(path)
    }
    fn store(&self) -> Arc<BrokerStore> {
        Arc::new(
            BrokerStore::open_with(
                self.0.join("records"),
                unsafe { libc::geteuid() },
                Arc::new(UuidTokenSource),
            )
            .unwrap(),
        )
    }
}
impl Drop for Workspace {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
struct Process(Child);
impl Drop for Process {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}
fn plan() -> CanonicalPlan {
    CanonicalPlan::from_packages(vec![UpdatePackageInfo {
        package_name: "isolated-fixture-only".into(),
        current_version: "1".into(),
        new_version: "2".into(),
        repository: "offline-fixture".into(),
        download_size: 1,
        filename: "isolated-fixture.pkg.tar.zst".into(),
        sha256: "a".repeat(64),
        repository_metadata_sha256: "b".repeat(64),
    }])
    .unwrap()
    .with_policy_fingerprint("c".repeat(64))
    .unwrap()
}
fn owner() -> VerifiedIdentity {
    VerifiedIdentity::test_identity(
        ":1.100".into(),
        unsafe { libc::geteuid() },
        "isolated-fixture".into(),
    )
    .unwrap()
}
fn queue(store: &BrokerStore) -> ConsumedPreview {
    store
        .create_preview(
            AuthorizedCaller::test_authorized(owner(), PolicyAction::Preview, 1000),
            plan(),
            1000,
        )
        .unwrap();
    store
        .consume_preview(
            AuthorizedCaller::test_authorized(owner(), PolicyAction::Install, 1001),
            1001,
        )
        .unwrap()
}
fn error(error: BrokerError) -> zbus::fdo::Error {
    zbus::fdo::Error::Failed(format!("{error:?}"))
}

#[derive(Default)]
struct PolicyState {
    preview: AtomicBool,
    install: AtomicBool,
    subjects: Mutex<Vec<(String, String)>>,
}
struct FixturePolicy(Arc<PolicyState>);
#[zbus::interface(name = "org.zethropol.Update.IsolatedPolicy")]
impl FixturePolicy {
    fn check(&self, subject: &str, action: &str) -> bool {
        self.0
            .subjects
            .lock()
            .unwrap()
            .push((subject.into(), action.into()));
        match action {
            "org.zethropol.update.preview" => self.0.preview.load(Ordering::SeqCst),
            "org.zethropol.update.install" => self.0.install.load(Ordering::SeqCst),
            _ => false,
        }
    }
}
struct FixtureBroker {
    store: Arc<BrokerStore>,
}
impl FixtureBroker {
    async fn identity(
        connection: &zbus::Connection,
        header: &zbus::message::Header<'_>,
    ) -> zbus::fdo::Result<VerifiedIdentity> {
        let sender = header
            .sender()
            .ok_or(zbus::fdo::Error::AccessDenied("missing sender".into()))?
            .to_string();
        let bus = zbus::Proxy::new(
            connection,
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
        )
        .await?;
        let uid: u32 = bus
            .call("GetConnectionUnixUser", &(sender.as_str(),))
            .await?;
        let pid: u32 = bus
            .call("GetConnectionUnixProcessID", &(sender.as_str(),))
            .await?;
        if pid == 0 {
            return Err(zbus::fdo::Error::AccessDenied("missing PID".into()));
        }
        // Only the session is mocked. Sender and UID come from this private
        // bus's real credentials; there is no client-supplied identity field.
        VerifiedIdentity::test_identity(sender, uid, "isolated-fixture".into()).map_err(error)
    }
    async fn authorize(
        connection: &zbus::Connection,
        header: &zbus::message::Header<'_>,
        action: PolicyAction,
    ) -> zbus::fdo::Result<AuthorizedCaller> {
        let identity = Self::identity(connection, header).await?;
        let policy = zbus::Proxy::new(
            connection,
            SERVICE,
            "/policy",
            "org.zethropol.Update.IsolatedPolicy",
        )
        .await?;
        let allowed: bool = policy
            .call("Check", &(identity.bus_name(), action.id()))
            .await?;
        if !allowed {
            return Err(zbus::fdo::Error::AccessDenied(
                "fixture policy denied".into(),
            ));
        }
        Ok(AuthorizedCaller::test_authorized(identity, action, 1001))
    }
}
#[zbus::interface(name = "org.zethropol.Update.IsolatedBroker")]
impl FixtureBroker {
    async fn prepare(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> zbus::fdo::Result<()> {
        let caller = Self::authorize(connection, &header, PolicyAction::Preview).await?;
        self.store
            .create_preview(caller, plan(), 1001)
            .map_err(error)
    }
    async fn queue(
        &self,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> zbus::fdo::Result<String> {
        let caller = Self::authorize(connection, &header, PolicyAction::Install).await?;
        self.store
            .consume_preview(caller, 1001)
            .map(|op| op.operation_id)
            .map_err(error)
    }
    async fn state(
        &self,
        id: &str,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> zbus::fdo::Result<String> {
        let identity = Self::identity(connection, &header).await?;
        self.store
            .operation_for_owner(&identity, id)
            .map(|view| view.status.as_str().into())
            .map_err(error)
    }
}

#[tokio::test]
async fn isolated_bus_denial_ownership_replay_concurrency_and_plan_change() {
    let workspace = Workspace::new();
    let store = workspace.store();
    let mut daemon = Process(
        Command::new("dbus-daemon")
            .args(["--session", "--nofork", "--nopidfile", "--print-address=1"])
            .arg(format!("--address=unix:tmpdir={}", workspace.0.display()))
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .unwrap(),
    );
    let mut address = String::new();
    BufReader::new(daemon.0.stdout.take().unwrap())
        .read_line(&mut address)
        .unwrap();
    assert!(
        !address.trim().is_empty(),
        "isolated D-Bus socket unavailable (sandbox restriction is not a policy denial)"
    );
    let policy = Arc::new(PolicyState::default());
    let _service = zbus::connection::Builder::address(address.trim())
        .unwrap()
        .name(SERVICE)
        .unwrap()
        .serve_at("/policy", FixturePolicy(Arc::clone(&policy)))
        .unwrap()
        .serve_at(
            "/broker",
            FixtureBroker {
                store: Arc::clone(&store),
            },
        )
        .unwrap()
        .build()
        .await
        .unwrap();
    let a = zbus::connection::Builder::address(address.trim())
        .unwrap()
        .build()
        .await
        .unwrap();
    let b = zbus::connection::Builder::address(address.trim())
        .unwrap()
        .build()
        .await
        .unwrap();
    let proxy_a = zbus::Proxy::new(
        &a,
        SERVICE,
        "/broker",
        "org.zethropol.Update.IsolatedBroker",
    )
    .await
    .unwrap();
    let proxy_b = zbus::Proxy::new(
        &b,
        SERVICE,
        "/broker",
        "org.zethropol.Update.IsolatedBroker",
    )
    .await
    .unwrap();
    let denied = proxy_a.call::<_, _, ()>("Prepare", &()).await.unwrap_err();
    assert!(denied.to_string().contains("AccessDenied"));
    assert_eq!(
        fs::read_dir(&store.root)
            .unwrap()
            .filter_map(Result::ok)
            .filter(|p| p.file_name().to_string_lossy().starts_with("preview-"))
            .count(),
        0
    );
    policy.preview.store(true, Ordering::SeqCst);
    proxy_a.call::<_, _, ()>("Prepare", &()).await.unwrap();
    let denied = proxy_a
        .call::<_, _, String>("Queue", &())
        .await
        .unwrap_err();
    assert!(denied.to_string().contains("AccessDenied"));
    assert!(store.unresolved_operations().unwrap().is_empty());
    policy.install.store(true, Ordering::SeqCst);
    let (one, two) = tokio::join!(
        proxy_a.call::<_, _, String>("Queue", &()),
        proxy_a.call::<_, _, String>("Queue", &())
    );
    assert_eq!(usize::from(one.is_ok()) + usize::from(two.is_ok()), 1);
    let id = one.or(two).unwrap();
    assert!(proxy_a
        .call::<_, _, String>("Queue", &())
        .await
        .unwrap_err()
        .to_string()
        .contains("NoPreview"));
    assert!(proxy_b
        .call::<_, _, String>("State", &(id.as_str(),))
        .await
        .unwrap_err()
        .to_string()
        .contains("IdentityMismatch"));
    assert!(proxy_a
        .call::<_, _, String>("State", &("not-a-uuid",))
        .await
        .unwrap_err()
        .to_string()
        .contains("InvalidToken"));
    proxy_b.call::<_, _, ()>("Prepare", &()).await.unwrap();
    assert!(proxy_b
        .call::<_, _, String>("Queue", &())
        .await
        .unwrap_err()
        .to_string()
        .contains("OperationInProgress"));
    let mut changed = plan().packages().to_vec();
    changed[0].new_version = "3".into();
    assert_eq!(
        store.claim_worker(&id, &changed, 1002),
        Err(BrokerError::PlanChanged)
    );
    assert_eq!(
        proxy_a
            .call::<_, _, String>("State", &(id.as_str(),))
            .await
            .unwrap(),
        "failed"
    );
    assert_eq!(
        store.claim_worker(&id, plan().packages(), 1003),
        Err(BrokerError::InvalidTransition)
    );
    let subjects = policy.subjects.lock().unwrap();
    assert!(subjects.iter().any(
        |(subject, action)| subject == a.unique_name().unwrap().as_str()
            && action == PolicyAction::Install.id()
    ));
    assert!(subjects.iter().any(
        |(subject, action)| subject == b.unique_name().unwrap().as_str()
            && action == PolicyAction::Install.id()
    ));
}

fn child(workspace: &Workspace, id: &str, mode: &str) -> Process {
    Process(
        Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "broker_protocol::worker_integration_tests::fixture_worker_process",
                "--ignored",
            ])
            .env("ZETHROPOL_WORKER_FIXTURE_DIR", &workspace.0)
            .env("ZETHROPOL_WORKER_FIXTURE_ID", id)
            .env("ZETHROPOL_WORKER_FIXTURE_MODE", mode)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap(),
    )
}

#[test]
#[ignore = "subprocess fixture only; never executes package or systemd code"]
fn fixture_worker_process() {
    let Some(path) = std::env::var_os("ZETHROPOL_WORKER_FIXTURE_DIR") else {
        return;
    };
    let path = PathBuf::from(path);
    assert_eq!(path.parent().unwrap(), std::env::temp_dir());
    assert!(path
        .file_name()
        .unwrap()
        .to_string_lossy()
        .starts_with("zethropol-worker-isolation-"));
    let id = std::env::var("ZETHROPOL_WORKER_FIXTURE_ID").unwrap();
    let mode = std::env::var("ZETHROPOL_WORKER_FIXTURE_MODE").unwrap();
    assert!(matches!(mode.as_str(), "hold" | "duplicate"));
    let store = BrokerStore::open_with(
        path.join("records"),
        unsafe { libc::geteuid() },
        Arc::new(UuidTokenSource),
    )
    .unwrap();
    let _lease = match store.worker_lease(&id) {
        Ok(lease) => lease,
        Err(BrokerError::OperationInProgress) if mode == "duplicate" => {
            fs::write(path.join("duplicate.outcome"), "lease-rejected").unwrap();
            return;
        }
        other => panic!("unexpected fixture lease result: {other:?}"),
    };
    store.require_queued(&id).unwrap();
    store.claim_worker(&id, plan().packages(), 1002).unwrap();
    assert_eq!(mode, "hold");
    fs::write(path.join("ready"), "claimed").unwrap();
    loop {
        std::thread::sleep(Duration::from_millis(10));
    }
}

#[test]
fn subprocess_crash_broker_reopen_and_duplicate_lease_are_terminal() {
    let workspace = Workspace::new();
    let store = workspace.store();
    let op = queue(&store);
    let mut worker = child(&workspace, &op.operation_id, "hold");
    let deadline = std::time::Instant::now() + Duration::from_secs(5);
    while !workspace.0.join("ready").exists() {
        assert!(
            worker.0.try_wait().unwrap().is_none(),
            "fixture worker exited before claim"
        );
        assert!(
            std::time::Instant::now() < deadline,
            "fixture did not become ready"
        );
        std::thread::sleep(Duration::from_millis(10));
    }
    let mut duplicate = child(&workspace, &op.operation_id, "duplicate");
    assert!(duplicate.0.wait().unwrap().success());
    assert_eq!(
        fs::read_to_string(workspace.0.join("duplicate.outcome")).unwrap(),
        "lease-rejected"
    );
    drop(store);
    let reopened = workspace.store();
    reopened
        .reconcile_worker_observation(&op.operation_id, Ok(false), 1003)
        .unwrap();
    assert_eq!(
        reopened
            .operation_for_owner(&owner(), &op.operation_id)
            .unwrap()
            .status,
        OperationStatus::Running
    );
    worker.0.kill().unwrap();
    use std::os::unix::process::ExitStatusExt;
    assert_eq!(worker.0.wait().unwrap().signal(), Some(libc::SIGKILL));
    reopened
        .reconcile_worker_observation(&op.operation_id, Ok(false), 1004)
        .unwrap();
    let result = reopened
        .operation_for_owner(&owner(), &op.operation_id)
        .unwrap();
    assert_eq!(result.status, OperationStatus::Failed);
    assert!(result.message.contains("No automatic retry"));
    assert_eq!(
        reopened.claim_worker(&op.operation_id, plan().packages(), 1005),
        Err(BrokerError::InvalidTransition)
    );
}

#[test]
fn missing_fixture_executable_records_start_failure_without_claim() {
    let workspace = Workspace::new();
    let store = workspace.store();
    let op = queue(&store);
    assert!(Command::new(workspace.0.join("does-not-exist"))
        .spawn()
        .is_err());
    store
        .worker_start_failed(&op.operation_id, "isolated spawn failed")
        .unwrap();
    assert_eq!(
        store
            .operation_for_owner(&owner(), &op.operation_id)
            .unwrap()
            .status,
        OperationStatus::WorkerStartFailed
    );
    assert_eq!(
        store.require_queued(&op.operation_id),
        Err(BrokerError::InvalidTransition)
    );
    assert_eq!(
        store.claim_worker(&op.operation_id, plan().packages(), 1002),
        Err(BrokerError::InvalidTransition)
    );
}
