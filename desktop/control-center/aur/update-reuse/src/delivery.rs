use std::os::unix::fs::DirBuilderExt;
// Development-only delivery seams. No executable, signer or installer.
const AUR_DELIVERY_ROOT: &str = "/var/lib/zethropol/aur-deliveries";
fn delivery_error(error: impl ToString) -> zbus::fdo::Error {
    zbus::fdo::Error::NotSupported(error.to_string())
}
fn delivery_read(path: &Path, limit: u64) -> Result<Vec<u8>, String> {
    for ancestor in path.parent().ok_or("missing delivery parent")?.ancestors() {
        let m = fs::symlink_metadata(ancestor).map_err(|e| e.to_string())?;
        if !m.is_dir() || m.uid() != 0 || m.permissions().mode() & 0o022 != 0 {
            return Err("unsafe root-owned delivery ancestors".into());
        }
    }
    let f = fs::OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC)
        .open(path)
        .map_err(|e| e.to_string())?;
    let m = f.metadata().map_err(|e| e.to_string())?;
    if !m.is_file()
        || m.uid() != 0
        || m.permissions().mode() & 0o022 != 0
        || m.len() == 0
        || m.len() > limit
    {
        return Err("unsafe/oversized delivery".into());
    }
    let mut bytes = Vec::new();
    f.take(limit + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() as u64 > limit {
        return Err("delivery grew beyond quota".into());
    }
    Ok(bytes)
}
fn delivery_request(bytes: &[u8], context: &ReceiptContext) -> Result<String, String> {
    let request: serde_json::Value = serde_json::from_slice(bytes).map_err(|e| e.to_string())?;
    let object = request.as_object().ok_or("delivery object required")?;
    let keys = [
        "store",
        "recipe",
        "plan",
        "plan_sha256",
        "manifest",
        "manifest_signature",
        "packages",
        "build_root",
    ];
    if object.len() != keys.len() || keys.iter().any(|k| !object.contains_key(*k)) {
        return Err("exact provisioning request required".into());
    }
    if request["plan_sha256"].as_str() != Some(&context.review.source_plan_sha256) {
        return Err("provisioning review hash changed".into());
    }
    // Python's canonical JSON uses ASCII escapes; the actual worker verifies its
    // canonical plan digest. The server validates the already-approved digest,
    // fixed root path and signed manifest byte identity independently here.
    let manifest = request["manifest"]
        .as_str()
        .ok_or("manifest path required")?;
    if receipt_hash(&delivery_read(Path::new(manifest), 1024 * 1024)?)
        != context.review.build_root_sha256
    {
        return Err("signed root manifest changed".into());
    }
    let expected = format!(
        "/var/lib/zethropol/aur-build-roots/{}",
        context.review.build_root_sha256
    );
    if request["build_root"].as_str() != Some(&expected) {
        return Err("fixed provisioned root required".into());
    }
    String::from_utf8(bytes.to_vec()).map_err(|e| e.to_string())
}
fn provider_command(action: &str, value: &str) -> Result<(), String> {
    delivery_read(Path::new("/usr/lib/zethropol/aur/provider.py"), 1024 * 1024)?;
    let result = std::process::Command::new("/usr/bin/timeout")
        .args([
            "--signal=TERM",
            "--kill-after=2s",
            "60s",
            "/usr/bin/python3",
            "-I",
            "-B",
            "/usr/lib/zethropol/aur/provider.py",
            action,
            value,
        ])
        .env_clear()
        .env("PATH", "/usr/bin")
        .env("LC_ALL", "C")
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .map_err(|e| e.to_string())?;
    if !result.success() {
        return Err("production provider inputs/configuration/verification unavailable".into());
    }
    Ok(())
}
fn provider_unit(operation: &str, start: bool) -> Result<(), String> {
    if uuid::Uuid::parse_str(operation)
        .map(|id| id.to_string() != operation)
        .unwrap_or(true)
    {
        return Err("canonical provider operation required".into());
    }
    let publisher = format!("zethropol-aur-provider@{operation}.service");
    let sign = format!("zethropol-aur-sign@{operation}.service");
    let attest = format!("zethropol-aur-attest@{operation}.service");
    let mut command = std::process::Command::new("/usr/bin/systemctl");
    command
        .env_clear()
        .env("PATH", "/usr/bin")
        .env("LC_ALL", "C")
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    if start {
        command.args(["start", "--no-block", &publisher]);
    } else {
        command.args(["stop", &publisher, &sign, &attest]);
    }
    if !command.status().map_err(|e| e.to_string())?.success() {
        return Err("provider supervision request unconfirmed; no automatic retry".into());
    }
    Ok(())
}
fn provision_delivery(context: &ReceiptContext) -> Result<String, String> {
    let root = Path::new(AUR_DELIVERY_ROOT);
    for ancestor in root.ancestors() {
        let m = fs::symlink_metadata(ancestor).map_err(|e| e.to_string())?;
        if !m.is_dir() || m.uid() != 0 || m.permissions().mode() & 0o022 != 0 {
            return Err("unsafe delivery root".into());
        }
    }
    let operation = root.join(&context.operation_id);
    let destination = operation.join("build.json");
    if destination.try_exists().map_err(|e| e.to_string())? {
        return delivery_request(&delivery_read(&destination, 1024 * 1024)?, context);
    }
    let template = Path::new("/var/lib/zethropol/aur-approved-reviews")
        .join(format!("{}.build.json", context.review.digest()));
    let bytes = delivery_read(&template, 1024 * 1024)?;
    let request = delivery_request(&bytes, context)?;
    provider_command("provision", &context.review.digest())?;
    fs::DirBuilder::new()
        .mode(0o700)
        .create(&operation)
        .map_err(|e| format!("provisioning publication already exists or unavailable: {e}"))?;
    fs::File::open(root)
        .and_then(|f| f.sync_all())
        .map_err(|e| e.to_string())?;
    let pending = operation.join("build.pending");
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&pending)
        .map_err(|e| e.to_string())?;
    file.write_all(&bytes)
        .and_then(|_| file.sync_all())
        .map_err(|e| e.to_string())?;
    fs::rename(pending, &destination).map_err(|e| e.to_string())?;
    fs::File::open(&operation)
        .and_then(|f| f.sync_all())
        .map_err(|e| e.to_string())?;
    Ok(request)
}
type DeliveryFds = (
    zbus::zvariant::OwnedFd,
    zbus::zvariant::OwnedFd,
    zbus::zvariant::OwnedFd,
    zbus::zvariant::OwnedFd,
    Vec<zbus::zvariant::OwnedFd>,
    Vec<zbus::zvariant::OwnedFd>,
);
fn delivery_fd(
    path: &Path,
    limit: u64,
    total: &mut u64,
) -> Result<zbus::zvariant::OwnedFd, String> {
    let bytes = delivery_read(path, limit)?;
    *total += bytes.len() as u64;
    if *total > 256 * 1024 * 1024 {
        return Err("aggregate attestation delivery quota".into());
    }
    Ok(std::os::fd::OwnedFd::from(seal_receipt_bytes(&bytes)?).into())
}
fn delivery_attestation(context: &ReceiptContext) -> Result<DeliveryFds, String> {
    let receipt_path = Path::new(AUR_DELIVERY_ROOT)
        .join(&context.operation_id)
        .join("attestation/receipt.json");
    if !receipt_path.exists() {
        let unit = format!("zethropol-aur-provider@{}.service", context.operation_id);
        let status = std::process::Command::new("/usr/bin/systemctl")
            .args([
                "show",
                "--property=ActiveState",
                "--property=Result",
                "--value",
                &unit,
            ])
            .env_clear()
            .env("PATH", "/usr/bin")
            .env("LC_ALL", "C")
            .output()
            .map_err(|e| e.to_string())?;
        return Err(format!(
            "independent provider delivery not ready: {}",
            String::from_utf8_lossy(&status.stdout)
                .chars()
                .take(512)
                .collect::<String>()
        ));
    }

    let root = Path::new(AUR_DELIVERY_ROOT)
        .join(&context.operation_id)
        .join("attestation");
    let bytes = delivery_read(&root.join("receipt.json"), 1024 * 1024)?;
    let receipt: BuildReceipt = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    if receipt.operation_id != context.operation_id
        || receipt.uid != context.uid
        || receipt.bus_name != context.bus_name
        || receipt.session_id != context.session_id
        || receipt.source_plan_sha256 != context.review.source_plan_sha256
        || receipt.build_root_sha256 != context.review.build_root_sha256
        || receipt.packages.len() != context.review.outputs.len()
    {
        return Err("independent receipt delivery context mismatch".into());
    }
    let mut total = 0;
    let mut archives = Vec::new();
    let mut signatures = Vec::new();
    for (package, expected) in receipt.packages.iter().zip(&context.review.outputs) {
        if package.filename != expected.filename
            || package.name != expected.name
            || package.version != expected.version
            || !receipt_basename(&package.filename)
        {
            return Err("attestor output set changed".into());
        }
        archives.push(delivery_fd(
            &root.join(&package.filename),
            256 * 1024 * 1024,
            &mut total,
        )?);
        signatures.push(delivery_fd(
            &root.join(format!("{}.sig", package.filename)),
            1024 * 1024,
            &mut total,
        )?);
    }
    // This is a courier, not a verification grant. PreviewAurReceipt performs
    // all independent role signatures, native plan and one-use admission checks.
    Ok((
        delivery_fd(&root.join("receipt.json"), 1024 * 1024, &mut total)?,
        delivery_fd(&root.join("receipt.json.sig"), 1024 * 1024, &mut total)?,
        delivery_fd(&root.join("manifest.json"), 1024 * 1024, &mut total)?,
        delivery_fd(&root.join("manifest.json.sig"), 1024 * 1024, &mut total)?,
        archives,
        signatures,
    ))
}
fn delivery_intake_budget(additional: u64) -> Result<fs::File, String> {
    let root = Path::new(AUR_DELIVERY_ROOT);
    let lock = fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK)
        .open(root.join(".intake.lock"))
        .map_err(|e| e.to_string())?;
    let m = lock.metadata().map_err(|e| e.to_string())?;
    if !m.is_file() || m.uid() != 0 || m.permissions().mode() & 0o777 != 0o600 {
        return Err("unsafe intake lock".into());
    }
    if unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX) } != 0 {
        return Err("intake lock unavailable".into());
    }
    let mut total = additional;
    let mut files = 0;
    for (index, entry) in fs::read_dir(root).map_err(|e| e.to_string())?.enumerate() {
        if index >= 1024 {
            return Err("delivery inventory quota".into());
        }
        let entry = entry.map_err(|e| e.to_string())?;
        let name = entry.file_name();
        let Some(name) = name.to_str() else {
            return Err("invalid delivery directory".into());
        };
        if uuid::Uuid::parse_str(name)
            .map(|id| id.to_string() != name)
            .unwrap_or(true)
        {
            continue;
        }
        let parent = entry.file_type().map_err(|e| e.to_string())?;
        if !parent.is_dir() {
            return Err("unsafe operation delivery directory".into());
        }
        let intake = entry.path().join("untrusted-build");
        match fs::symlink_metadata(&intake) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
            Err(e) => return Err(e.to_string()),
            Ok(m) if !m.is_dir() || m.uid() != 0 || m.permissions().mode() & 0o077 != 0 => {
                return Err("unsafe intake directory".into())
            }
            Ok(_) => {}
        }
        for child in fs::read_dir(&intake).map_err(|e| e.to_string())? {
            let child = child.map_err(|e| e.to_string())?;
            let m = fs::symlink_metadata(child.path()).map_err(|e| e.to_string())?;
            if !m.is_file() || m.uid() != 0 {
                return Err("unsafe retained intake".into());
            }
            total = total.checked_add(m.len()).ok_or("intake size overflow")?;
            files += 1;
            if total > 512 * 1024 * 1024 || files > 4096 {
                return Err("retained intake quota; explicit archival required".into());
            }
        }
    }
    if total > 512 * 1024 * 1024 {
        return Err("retained intake byte quota".into());
    }
    Ok(lock)
}
fn provider_budget_frame(
    context: &ReceiptContext,
    wall_ns: u64,
    boot_ns: u64,
    boot_id: &str,
) -> Result<serde_json::Value, String> {
    let issued = context
        .reservation_issued_boottime
        .checked_mul(1_000_000_000)
        .ok_or("reservation clock overflow")?;
    let deadline = context
        .reservation_deadline_boottime
        .checked_mul(1_000_000_000)
        .ok_or("reservation clock overflow")?;
    let boot = uuid::Uuid::parse_str(boot_id).map_err(|_| "canonical budget boot ID required")?;
    if boot.to_string() != boot_id
        || wall_ns == 0
        || deadline.checked_sub(issued) != Some(300_000_000_000)
        || boot_ns < issued
        || boot_ns >= deadline
    {
        return Err("server reservation budget unavailable or expired".into());
    }
    Ok(
        serde_json::json!({"schema":1,"purpose":"zethropol-provider-reservation-budget","boot_id":boot_id,
        "issued_boottime_ns":issued,"deadline_boottime_ns":deadline,"captured_boottime_ns":boot_ns,"captured_wall_ns":wall_ns}),
    )
}
fn provider_budget(context: &ReceiptContext) -> Result<serde_json::Value, String> {
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    if unsafe { libc::clock_gettime(libc::CLOCK_BOOTTIME, &mut ts) } != 0
        || ts.tv_sec < 0
        || ts.tv_nsec < 0
        || ts.tv_nsec >= 1_000_000_000
    {
        return Err("provider boottime sample unavailable".into());
    }
    let boot_ns = (ts.tv_sec as u64)
        .checked_mul(1_000_000_000)
        .and_then(|v| v.checked_add(ts.tv_nsec as u64))
        .ok_or("provider clock overflow")?;
    let wall_ns = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|e| e.to_string())?
        .as_nanos()
        .try_into()
        .map_err(|_| "provider wall clock overflow")?;
    let boot = fs::read_to_string("/proc/sys/kernel/random/boot_id").map_err(|e| e.to_string())?;
    provider_budget_frame(context, wall_ns, boot_ns, boot.trim())
}

fn submit_delivery(
    context: &ReceiptContext,
    archives: Vec<zbus::zvariant::OwnedFd>,
) -> Result<(), String> {
    use std::os::fd::AsFd;
    if archives.len() != context.review.outputs.len() {
        return Err("complete built output FD set required".into());
    }
    let root = Path::new(AUR_DELIVERY_ROOT).join(&context.operation_id);
    // Parent already provisioned by the trusted delivery agent. No client path,
    // privileged package build, executable invocation or receipt signing here.
    delivery_read(&root.join("build.json"), 1024 * 1024)?;
    let mut required = 1024 * 1024u64;
    for fd in &archives {
        required += sealed_size(fd.as_fd(), 256 * 1024 * 1024)?;
    }
    if required > 257 * 1024 * 1024 {
        return Err("aggregate build delivery quota".into());
    }
    let _admission = delivery_intake_budget(required)?;
    let staging = root.join("untrusted-build");
    fs::DirBuilder::new()
        .mode(0o700)
        .create(&staging)
        .map_err(|e| format!("one-use delivery admission: {e}"))?;
    fs::File::open(&root)
        .and_then(|f| f.sync_all())
        .map_err(|e| e.to_string())?;
    let mut total = 0u64;
    let mut hashes = Vec::new();
    for (fd, expected) in archives.iter().zip(&context.review.outputs) {
        if !receipt_basename(&expected.filename) {
            return Err("invalid reviewed output filename".into());
        }
        let data = read_receipt_fd(fd.as_fd(), 256 * 1024 * 1024)?;
        total += data.len() as u64;
        if total > 256 * 1024 * 1024 {
            return Err("aggregate build delivery quota".into());
        }
        let mut file = fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(staging.join(&expected.filename))
            .map_err(|e| e.to_string())?;
        file.write_all(&data)
            .and_then(|_| file.sync_all())
            .map_err(|e| e.to_string())?;
        hashes.push(serde_json::json!({"filename":expected.filename,"sha256":receipt_hash(&data)}));
    }
    let job = serde_json::json!({"purpose":"zethropol-untrusted-attestation-intake","schema":2,"operation_id":context.operation_id,"uid":context.uid,
        "bus_name":context.bus_name,"session_id":context.session_id,"review":context.review,"archives":hashes,"authorization":"none","budget":provider_budget(context)?});
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(staging.join("ready.pending"))
        .map_err(|e| e.to_string())?;
    file.write_all(&serde_json::to_vec(&job).map_err(|e| e.to_string())?)
        .and_then(|_| file.sync_all())
        .map_err(|e| e.to_string())?;
    fs::rename(staging.join("ready.pending"), staging.join("ready.json"))
        .map_err(|e| e.to_string())?;
    fs::File::open(&staging)
        .and_then(|f| f.sync_all())
        .map_err(|e| e.to_string())?;
    fs::File::open(&root)
        .and_then(|f| f.sync_all())
        .map_err(|e| e.to_string())?;
    provider_unit(&context.operation_id, true)?;
    Ok(())
}
async fn delivery_context(
    connection: &zbus::Connection,
    header: &zbus::message::Header<'_>,
    operation: &str,
) -> Result<ReceiptContext, zbus::fdo::Error> {
    ReceiptTrust::load().map_err(delivery_error)?;
    let caller = AuthorizedCaller::check_message(
        connection,
        header,
        PolicyAction::Preview,
        boottime_seconds()?,
    )
    .await
    .map_err(broker_error)?;
    persistent_aur_broker()
        .map_err(broker_error)?
        .receipt_context_for_operation(caller.identity(), operation, boottime_seconds()?)
        .map_err(broker_error)
}

fn approved_selection(names: &[String], versions: &[String]) -> Result<ReviewedBuild, String> {
    aur_selection_review(names, versions)?;
    let selected: std::collections::BTreeMap<_, _> = names
        .iter()
        .cloned()
        .zip(versions.iter().cloned())
        .collect();
    let directory = Path::new("/var/lib/zethropol/aur-approved-reviews");
    let mut matched = None;
    let entries = fs::read_dir(directory).map_err(|e| e.to_string())?;
    for (index, entry) in entries.enumerate() {
        if index >= 256 {
            return Err("approved source review inventory quota".into());
        }
        let entry = entry.map_err(|e| e.to_string())?;
        let filename = entry.file_name();
        let filename = filename.to_str().ok_or("invalid review filename")?;
        let Some(hash) = filename.strip_suffix(".json") else {
            continue;
        };
        if !receipt_digest(hash) {
            continue;
        }
        let review = load_approved_aur_review(hash)?;
        let outputs: std::collections::BTreeMap<_, _> = review
            .outputs
            .iter()
            .map(|p| (p.name.clone(), p.version.clone()))
            .collect();
        if selected == outputs {
            if matched.is_some() {
                return Err(
                    "multiple approved source reviews match; server must resolve selection".into(),
                );
            }
            matched = Some(review);
        }
    }
    matched.ok_or("no server-approved source review matches selection".into())
}
#[cfg(test)]
mod delivery_tests {
    use super::*;
    fn review() -> ReviewedBuild {
        ReviewedBuild {
            source_package: "delivery-fixture".into(),
            source_version: "2-1".into(),
            architecture: "x86_64".into(),
            source_plan_sha256: "a".repeat(64),
            build_root_sha256: "b".repeat(64),
            outputs: vec![ReviewedOutput {
                name: "delivery-fixture".into(),
                version: "2-1".into(),
                filename: "delivery-fixture-2-1.pkg.tar".into(),
            }],
        }
    }
    #[test]
    fn delivery_budget_uses_original_server_deadline_without_restart() {
        let context = ReceiptContext {
            operation_id: "11111111-1111-4111-8111-111111111111".into(),
            uid: 1000,
            bus_name: ":1.1".into(),
            session_id: "lab".into(),
            review: review(),
            reservation_issued_boottime: 100,
            reservation_deadline_boottime: 400,
        };
        let boot = "22222222-2222-4222-8222-222222222222";
        let frame =
            provider_budget_frame(&context, 2_000_000_000_000, 200_000_000_000, boot).unwrap();
        assert_eq!(frame["deadline_boottime_ns"], 400_000_000_000u64);
        assert_eq!(frame["captured_boottime_ns"], 200_000_000_000u64);
        assert!(provider_budget_frame(&context, 2_000_000_000_000, 400_000_000_000, boot).is_err());
        assert!(provider_budget_frame(&context, 2_000_000_000_000, 99_000_000_000, boot).is_err());
        let mut changed = context.clone();
        changed.reservation_deadline_boottime = 401;
        assert!(provider_budget_frame(&changed, 2_000_000_000_000, 200_000_000_000, boot).is_err());
        assert!(
            provider_budget_frame(&context, 2_000_000_000_000, 200_000_000_000, "invalid").is_err()
        );
    }
    #[test]
    fn delivery_budget_context_carries_persisted_preview_boundaries() {
        let fixture = crate::broker_protocol::aur_fixture::Fixture::new();
        let caller = fixture.owner.clone();
        // Reuse the real scratch BrokerStore reservation path; never a native
        // transaction, signature, host provisioning or production authorization.
        let context = fixture
            .store
            .reserve_aur_build(
                fixture.grant(caller, PolicyAction::Preview, 100),
                review(),
                100,
            )
            .unwrap();
        assert_eq!(context.reservation_issued_boottime, 100);
        assert_eq!(context.reservation_deadline_boottime, 400);
    }
    #[test]
    fn delivery_dbus_contract_has_fd_transport_and_no_client_paths() {
        let manager = UpdateManager {
            state: Arc::new(Mutex::new(ManagerState::default())),
            plan_refresh_lock: Mutex::new(()),
            broker: None,
            test_authentication: false,
        };
        let mut xml = String::new();
        zbus::object_server::Interface::introspect_to_writer(&manager, &mut xml, 0);
        for method in [
            "PrepareAurSelection",
            "GetAurProvisioning",
            "SubmitAurBuild",
            "GetAurAttestation",
            "ReleaseAurBuild",
        ] {
            assert!(xml.contains(&format!("name=\"{method}\"")), "{xml}");
        }
        let output = xml
            .split("<method name=\"GetAurAttestation\">")
            .nth(1)
            .unwrap()
            .split("</method>")
            .next()
            .unwrap();
        assert_eq!(output.matches("type=\"h\"").count(), 4, "{output}");
        assert_eq!(output.matches("type=\"ah\"").count(), 2, "{output}");
    }
    #[test]
    fn delivery_manual_fields_and_incomplete_fd_set_are_rejected_before_io() {
        let context = ReceiptContext {
            operation_id: "11111111-1111-4111-8111-111111111111".into(),
            uid: 1000,
            bus_name: ":1.1".into(),
            session_id: "lab".into(),
            review: review(),
            reservation_issued_boottime: 100,
            reservation_deadline_boottime: 400,
        };
        assert!(delivery_request(br#"{"command":"untrusted"}"#, &context)
            .unwrap_err()
            .contains("exact provisioning"));
        assert!(submit_delivery(&context, vec![])
            .unwrap_err()
            .contains("complete built output"));
    }
    #[test]
    fn delivery_reservation_release_is_owner_bound_durable_and_cannot_replay() {
        use crate::broker_protocol::aur_fixture::Fixture;
        let fixture = Fixture::persistent("11111111-1111-4111-8111-111111111111", 100);
        let caller = || fixture.grant(fixture.owner.clone(), PolicyAction::Preview, 100);
        let context = fixture
            .store
            .reserve_aur_build(caller(), review(), 100)
            .unwrap();
        assert!(fixture
            .store
            .release_aur_reservation(
                fixture.grant(fixture.other_peer(), PolicyAction::Preview, 100),
                &context.operation_id,
                100
            )
            .is_err());
        assert!(fixture
            .store
            .release_aur_reservation(caller(), "22222222-2222-4222-8222-222222222222", 100)
            .is_err());
        assert!(fixture
            .store
            .receipt_context_for_operation(&fixture.owner, &context.operation_id, 100)
            .is_ok());
        fixture
            .store
            .release_aur_reservation(caller(), &context.operation_id, 100)
            .unwrap();
        assert!(fixture
            .store
            .receipt_context_for_operation(&fixture.owner, &context.operation_id, 100)
            .is_err());
        assert!(fixture
            .reboot("22222222-2222-4222-8222-222222222222", 1)
            .unwrap()
            .receipt_context_for_operation(&fixture.owner, &context.operation_id, 1)
            .is_err());
    }
}
