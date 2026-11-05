// Fixed Archive native domain. The broker never loads AUR payloads into its
// official ALPM handle. A fresh exec is mandatory because GPGME caches domain
// initialization process-wide. No client path, command or trust selector exists.
use std::os::fd::{FromRawFd, OwnedFd};
use std::os::unix::process::CommandExt;
use std::process::{Child, Stdio};

const AUR_NATIVE_DOMAIN: &str = "/etc/zethropol-update/aur-native-trust";
const NATIVE_RESPONSE_LIMIT: u64 = 4 * 1024 * 1024;
const NATIVE_COMMIT_LIMIT: u64 = 7200;

#[derive(Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
enum NativeMode {
    Preview,
    Install,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct NativeArtifact {
    identity: UpdatePackageInfo,
    signature_sha256: String,
    architecture: Option<String>,
    archive_fd: i32,
    signature_fd: i32,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct NativeRequest {
    schema: u32,
    nonce: String,
    mode: NativeMode,
    operation_id: String,
    deadline: u64,
    trust_sha256: String,
    official_stamp: String,
    source_package: String,
    architecture: String,
    plan_hash: Option<String>,
    artifacts: Vec<NativeArtifact>,
    response_fd: i32,
    lease_fd: Option<i32>,
    #[cfg(test)]
    fixture: Option<NativeFixturePaths>,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct NativeResponse {
    schema: u32,
    nonce: String,
    packages: Vec<UpdatePackageInfo>,
    policy_sha256: String,
    plan_hash: String,
    error: Option<String>,
}

// Present only in test builds. Release deserialization rejects this field and
// the release CLI always uses root-owned, fixed production paths and gates.
#[cfg(test)]
#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct NativeFixturePaths {
    root: PathBuf,
    db: PathBuf,
    domain: PathBuf,
    policy: PathBuf,
    staging: PathBuf,
    store: PathBuf,
    fault: Option<String>,
}

struct NativePaths {
    domain: PathBuf,
    trust: ReceiptTrust,
    #[cfg(test)]
    fixture: Option<NativeFixturePaths>,
}
impl NativePaths {
    fn production() -> Result<Self, String> {
        if unsafe { libc::getuid() } != 0 || unsafe { libc::geteuid() } != 0 {
            return Err("root-only native broker process required".into());
        }
        Ok(Self {
            domain: AUR_NATIVE_DOMAIN.into(),
            trust: ReceiptTrust::load()?,
            #[cfg(test)]
            fixture: None,
        })
    }
    fn stamp(&self) -> Result<String, String> {
        let owner = unsafe { libc::geteuid() };
        for ancestor in self.domain.ancestors() {
            let m = fs::symlink_metadata(ancestor).map_err(|e| e.to_string())?;
            if !m.is_dir()
                || m.file_type().is_symlink()
                || m.uid() != owner
                || m.mode() & 0o022 != 0
            {
                return Err("unsafe Archive native trust ancestors".into());
            }
            #[cfg(test)]
            if self.fixture.is_some() {
                break;
            }
        }
        for name in ["private-keys-v1.d", "secring.gpg"] {
            let path = self.domain.join(name);
            match fs::symlink_metadata(&path) {
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Ok(m)
                    if m.is_dir()
                        && !m.file_type().is_symlink()
                        && m.uid() == owner
                        && m.mode() & 0o022 == 0
                        && fs::read_dir(&path)
                            .map_err(|e| e.to_string())?
                            .next()
                            .is_none() => {}
                _ => return Err("Archive native domain must contain public material only".into()),
            }
        }
        for entry in fs::read_dir(&self.domain).map_err(|e| e.to_string())? {
            let entry = entry.map_err(|e| e.to_string())?;
            let name = entry.file_name();
            if name == "private-keys-v1.d" {
                continue;
            } // checked empty above
            if !["pubring.kbx", "pubring.kbx~", "trustdb.gpg", "gpg.conf"]
                .iter()
                .any(|v| name == *v)
            {
                return Err("unexpected file/agent in public Archive native domain".into());
            }
            let m = fs::symlink_metadata(entry.path()).map_err(|e| e.to_string())?;
            if !m.is_file()
                || m.file_type().is_symlink()
                || m.uid() != owner
                || m.mode() & 0o022 != 0
                || m.nlink() != 1
                || m.len()
                    > if name == "gpg.conf" {
                        4096
                    } else {
                        16 * 1024 * 1024
                    }
            {
                return Err("unsafe Archive native configuration/backup".into());
            }
        }
        let config = fs::read(self.domain.join("gpg.conf")).map_err(|e| e.to_string())?;
        if config.len() > 4096 {
            return Err("Archive native config too large".into());
        }
        let lines: Vec<_> = std::str::from_utf8(&config)
            .map_err(|e| e.to_string())?
            .lines()
            .map(str::trim)
            .filter(|l| !l.is_empty() && !l.starts_with('#'))
            .collect();
        let required = [
            "no-autostart",
            "no-auto-key-retrieve",
            "no-auto-key-import",
            "no-auto-check-trustdb",
        ];
        if required.iter().any(|r| !lines.contains(r))
            || lines
                .iter()
                .any(|l| !required.contains(l) && *l != "auto-key-locate clear")
            || lines.iter().collect::<std::collections::HashSet<_>>().len() != lines.len()
        {
            return Err(
                "Archive native config may not redirect/expand trust or enable discovery".into(),
            );
        }
        for name in ["pubring.kbx", "trustdb.gpg"] {
            let m = fs::symlink_metadata(self.domain.join(name)).map_err(|e| e.to_string())?;
            if !m.is_file()
                || m.file_type().is_symlink()
                || m.uid() != owner
                || m.mode() & 0o022 != 0
                || m.nlink() != 1
                || m.len() > 16 * 1024 * 1024
            {
                return Err("unsafe Archive native public snapshot".into());
            }
        }
        // The native ring may contain ONLY approved Archive primaries. This
        // neither imports keys nor raises trust. Native Required/TrustedOnly
        // still independently checks trust, revocation, expiry and signatures.
        let output = Command::new("/usr/bin/timeout")
            .args([
                "--kill-after=2s",
                "15s",
                "/usr/bin/gpg",
                "--no-options",
                "--homedir",
            ])
            .arg(&self.domain)
            .args([
                "--batch",
                "--no-tty",
                "--no-autostart",
                "--no-auto-check-trustdb",
                "--no-auto-key-retrieve",
                "--no-auto-key-import",
                "--with-colons",
                "--list-keys",
            ])
            .env_clear()
            .env("PATH", "/usr/bin")
            .env("LC_ALL", "C")
            .stdin(Stdio::null())
            .output()
            .map_err(|e| e.to_string())?;
        if !output.status.success() || output.stdout.len() > 1024 * 1024 {
            return Err("Archive public primary inventory unavailable".into());
        }
        let mut primary = false;
        let mut keys = std::collections::BTreeSet::new();
        for line in std::str::from_utf8(&output.stdout)
            .map_err(|e| e.to_string())?
            .lines()
        {
            let f: Vec<_> = line.split(':').collect();
            match f.first().copied() {
                Some("pub") => primary = true,
                Some("sub") => primary = false,
                Some("fpr") if primary => {
                    keys.insert(f.get(9).ok_or("missing native primary")?.to_string());
                    primary = false;
                }
                _ => {}
            }
        }
        if keys.is_empty() || keys != self.trust.archive.signers.iter().cloned().collect() {
            return Err("Archive native primaries differ from pinned Archive role; cross-domain trust refused".into());
        }
        policy_fingerprint_with_values(
            std::slice::from_ref(&self.domain),
            self.trust.digest.as_bytes(),
        )
    }
    fn state_stamp(&self, db: &Path) -> Result<String, String> {
        let native = self.stamp()?;
        #[cfg(test)]
        if self.fixture.is_some() {
            return aur_database_state_fingerprint(
                db,
                &format!("fixture:{}:{native}", self.trust.digest),
            );
        }
        let official = current_policy_fingerprint().map_err(|e| format!("{e:?}"))?;
        aur_database_state_fingerprint(
            db,
            &format!(
                "{official}:aur-trust:{}:archive-native:{native}",
                self.trust.digest
            ),
        )
    }
    fn handle(&self) -> Result<Alpm, String> {
        #[cfg(test)]
        if let Some(f) = &self.fixture {
            let mut h = Alpm::new(
                f.root.to_str().ok_or("fixture root")?,
                f.db.to_str().ok_or("fixture db")?,
            )
            .map_err(|e| e.to_string())?;
            h.set_gpgdir(self.domain.to_str().ok_or("fixture domain")?)
                .map_err(|e| e.to_string())?;
            h.set_architectures(["x86_64"].into_iter())
                .map_err(|e| e.to_string())?;
            h.set_hookdirs(std::iter::empty::<&str>())
                .map_err(|e| e.to_string())?;
            h.set_logfile(f.root.join("native.log").to_str().ok_or("fixture log")?)
                .map_err(|e| e.to_string())?;
            h.set_default_siglevel(SigLevel::PACKAGE)
                .map_err(|e| e.to_string())?;
            h.set_local_file_siglevel(SigLevel::PACKAGE)
                .map_err(|e| e.to_string())?;
            return Ok(h);
        }
        configured_alpm_domain(None, true).map_err(|e| e.to_string())
    }
    fn staging(&self) -> &Path {
        #[cfg(test)]
        if let Some(f) = &self.fixture {
            return &f.staging;
        }
        Path::new("/run/zethropol-update/aur-artifacts")
    }
    fn store(&self) -> Result<Arc<BrokerStore>, String> {
        #[cfg(test)]
        if let Some(f) = &self.fixture {
            return crate::broker_protocol::aur_fixture::reopen_native(f.store.clone())
                .map(Arc::new)
                .map_err(|e| format!("{e:?}"));
        }
        persistent_aur_broker().map_err(|e| format!("{e:?}"))
    }
}

// Called by the OFFICIAL broker process only. The existing full resolver and
// signature policy remain authoritative; no AUR archive enters this handle.
fn official_aur_preflight(
    handle: &mut Alpm,
    packages: &[UpdatePackageInfo],
    archive_domain: &Path,
    mut stamp: impl FnMut() -> Result<String, String>,
) -> Result<String, String> {
    let official_domain = fs::canonicalize(
        handle
            .gpgdir()
            .ok_or("official native domain unavailable")?,
    )
    .map_err(|e| e.to_string())?;
    let archive_domain = fs::canonicalize(archive_domain).map_err(|e| e.to_string())?;
    if official_domain.starts_with(&archive_domain) || archive_domain.starts_with(&official_domain)
    {
        return Err("official and Archive native domains must be independent directories".into());
    }
    let result = (|| {
        let plan = prepare_alpm_transaction_with_policy(handle, || {
            stamp().map_err(|e| ("aur-state".into(), e))
        })
        .map_err(|e| format!("official preflight: {e:?}"))?;
        for db in handle.syncdbs() {
            db.is_valid()
                .map_err(|e| format!("official repository trust: {e}"))?;
            if packages
                .iter()
                .any(|p| db.pkg(p.package_name.as_str()).is_ok())
            {
                return Err("AUR target belongs to official repository".into());
            }
        }
        if !plan.packages().is_empty() || !plan.removals().is_empty() {
            return Err("official full upgrade required first; no partial upgrade".into());
        }
        if stamp()? != plan.policy_fingerprint() {
            return Err("official state changed during preflight".into());
        }
        Ok(plan.policy_fingerprint().to_owned())
    })();
    let release = handle.trans_release().map_err(|e| e.to_string());
    match (result, release) {
        (Ok(v), Ok(())) => Ok(v),
        (Err(e), _) | (_, Err(e)) => Err(e),
    }
}

fn native_request_valid(r: &NativeRequest, now: u64) -> Result<(), String> {
    if r.schema != 1
        || uuid::Uuid::parse_str(&r.nonce)
            .map(|v| v.to_string())
            .ok()
            .as_deref()
            != Some(&r.nonce)
        || uuid::Uuid::parse_str(&r.operation_id)
            .map(|v| v.to_string())
            .ok()
            .as_deref()
            != Some(&r.operation_id)
        || now >= r.deadline
        || r.deadline.saturating_sub(now) > 300
        || !receipt_digest(&r.official_stamp)
        || !receipt_digest(&r.trust_sha256)
        || !receipt_name(&r.source_package)
        || !matches!(r.architecture.as_str(), "x86_64" | "aarch64")
        || r.artifacts.is_empty()
        || r.artifacts.len() > 64
        || r.response_fd < 3
    {
        return Err("invalid, expired or unbounded native broker request".into());
    }
    let mut fds = std::collections::HashSet::from([r.response_fd]);
    match (r.mode, r.lease_fd) {
        (NativeMode::Preview, None) => {}
        (NativeMode::Install, Some(fd)) if fd >= 3 && fds.insert(fd) => {}
        _ => return Err("native mode requires the exact inherited worker lease".into()),
    }
    for a in &r.artifacts {
        if (r.mode == NativeMode::Preview && a.architecture.is_none())
            || !receipt_name(&a.identity.package_name)
            || !receipt_text(&a.identity.new_version)
            || !receipt_basename(&a.identity.filename)
            || !receipt_digest(&a.identity.sha256)
            || !receipt_digest(&a.identity.repository_metadata_sha256)
            || !receipt_digest(&a.signature_sha256)
            || a.identity.repository != "aur-attested"
            || a.archive_fd < 3
            || a.signature_fd < 3
            || !fds.insert(a.archive_fd)
            || !fds.insert(a.signature_fd)
            || a.architecture
                .as_ref()
                .is_some_and(|v| v != "any" && v != &r.architecture)
        {
            return Err("invalid native artifact identity or descriptor alias".into());
        }
    }
    if r.artifacts
        .windows(2)
        .any(|p| p[0].identity.package_name >= p[1].identity.package_name)
    {
        return Err("native artifacts must be uniquely sorted".into());
    }
    if (r.mode == NativeMode::Install) != r.plan_hash.is_some()
        || r.plan_hash.as_ref().is_some_and(|h| !receipt_digest(h))
    {
        return Err("native mode and approved plan differ".into());
    }
    Ok(())
}

fn native_process_request(r: &NativeRequest, paths: &NativePaths) -> Result<CanonicalPlan, String> {
    use std::os::fd::AsFd;
    native_request_valid(r, aur_boottime()?)?;
    if paths.trust.digest != r.trust_sha256 {
        return Err("native trust changed since broker review".into());
    }
    let domain_before = paths.stamp()?;
    let seed =
        CanonicalPlan::from_packages(r.artifacts.iter().map(|a| a.identity.clone()).collect())
            .and_then(|p| p.with_policy_fingerprint(r.official_stamp.clone()))
            .map_err(|e| format!("{e:?}"))?;
    let store = if r.mode == NativeMode::Install {
        Some(paths.store()?)
    } else {
        None
    };
    let _lease = if let Some(s) = &store {
        let fd = r.lease_fd.ok_or("missing native lease")?;
        if unsafe { libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC) } < 0 {
            return Err("native lease unavailable".into());
        }
        let file = unsafe { fs::File::from_raw_fd(fd) };
        s.verify_native_lease(&r.operation_id, file.as_fd())
            .map_err(|e| format!("native lease: {e:?}"))?;
        Some(file)
    } else {
        None
    };
    if let Some(s) = &store {
        #[cfg(test)]
        let fixture = paths.fixture.is_some();
        #[cfg(not(test))]
        let fixture = false;
        if !fixture {
            require_aur_production_authority()?;
        }
        s.require_no_uncertain_commit()
            .map_err(|e| format!("unresolved native outcome: {e:?}"))?;
        s.require_queued(&r.operation_id)
            .map_err(|e| format!("queued native operation: {e:?}"))?;
        if r.plan_hash.as_deref() != Some(&seed.hash()) {
            return Err("native request plan changed".into());
        }
        let binding = s
            .aur_receipt_binding(
                &r.operation_id,
                &seed,
                aur_boottime()?,
                receipt_unix_now()?,
                &paths.trust.digest,
            )
            .map_err(|e| format!("native receipt binding: {e:?}"))?;
        if binding.native_deadline() != r.deadline
            || binding.native_review().source_package != r.source_package
            || binding.native_review().architecture != r.architecture
        {
            return Err("native request differs from stored receipt context".into());
        }
    }
    let mut owned = Vec::new();
    for a in &r.artifacts {
        // These descriptors belong only to this exec. Restore CLOEXEC before
        // invoking GPG/hook processes; no raw request FD reaches an install script.
        for fd in [a.archive_fd, a.signature_fd] {
            if unsafe { libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC) } < 0 {
                return Err("native descriptor unavailable".into());
            }
            owned.push(unsafe { fs::File::from_raw_fd(fd) });
        }
    }
    let pairs: Vec<_> = owned
        .as_chunks::<2>()
        .0
        .iter()
        .map(|v| (v[0].as_fd(), v[1].as_fd()))
        .collect();
    let mut total = 0u64;
    for (archive, signature) in &pairs {
        total = total
            .checked_add(
                sealed_size(*archive, 1024 * 1024 * 1024)? + sealed_size(*signature, 1024 * 1024)?,
            )
            .ok_or("native size overflow")?;
        if total > 1024 * 1024 * 1024 {
            return Err("aggregate native intake quota exceeded".into());
        }
    }
    for (a, (archive, signature)) in r.artifacts.iter().zip(&pairs) {
        if receipt_hash(&read_receipt_fd(*signature, 1024 * 1024)?) != a.signature_sha256 {
            return Err("receipt-bound native signature changed".into());
        }
        paths
            .trust
            .archive
            .verify(*archive, *signature, paths.staging())?;
    }
    let mut handle = paths.handle()?;
    if !handle.syncdbs().is_empty()
        || handle.gpgdir().map(Path::new) != Some(paths.domain.as_path())
    {
        return Err(
            "Archive native handle must have no official repositories and one fixed domain".into(),
        );
    }
    let intake = receive_aur_fds(paths.staging(), &seed, &pairs, &handle)?;
    for (a, archive) in r.artifacts.iter().zip(&intake.archives) {
        let pkg = handle
            .pkg_load(
                archive.path.as_os_str().as_encoded_bytes().to_vec(),
                true,
                SigLevel::PACKAGE,
            )
            .map_err(|e| format!("native archive: {e}"))?;
        if pkg.base() != Some(r.source_package.as_str())
            || ![Some("any"), Some(r.architecture.as_str())].contains(&pkg.arch())
            || a.architecture
                .as_ref()
                .is_some_and(|v| pkg.arch() != Some(v.as_str()))
        {
            return Err("native archive architecture/pkgbase differs from reviewed output".into());
        }
    }
    if let Some(s) = &store {
        let progress_store = Arc::clone(s);
        let operation = r.operation_id.clone();
        handle.set_progress_cb((), move |progress, package, percent, _, _, _| {
            if let Ok(now) = aur_boottime() {
                let _ = progress_store.update_worker_status(
                    &operation,
                    OperationStatus::Running,
                    &format!("AUR native {progress:?}: {package} ({percent}%)"),
                    now,
                );
            }
        });
    }
    handle.set_question_cb((), |mut question, _| question.set_answer(false));
    handle
        .trans_init(TransFlag::NONE)
        .map_err(|e| format!("native transaction lock: {e}"))?;
    // The official proof was issued by the broker's full resolver before this
    // lock handoff. Exact full DB/config/both-domain equality under THIS lock
    // closes the handoff race; changed state is never silently replanned.
    let current = paths.state_stamp(Path::new(handle.dbpath()));
    if current.as_deref() != Ok(r.official_stamp.as_str()) || paths.stamp()? != domain_before {
        let _ = handle.trans_release();
        return Err("official/native state changed at native lock handoff".into());
    }
    let empty_official = CanonicalPlan::from_packages(vec![])
        .and_then(|p| p.with_policy_fingerprint(r.official_stamp.clone()))
        .map_err(|e| format!("{e:?}"))?;
    let db = PathBuf::from(handle.dbpath());
    let locked = finish_aur_locked_transaction(
        &mut handle,
        &intake.archives,
        empty_official,
        || paths.state_stamp(&db),
        false,
    )?;
    native_request_valid(r, aur_boottime()?)?;
    let plan = locked.plan.clone();
    if let Some(s) = store {
        if r.plan_hash.as_deref() != Some(&plan.hash()) {
            return Err("native final plan differs from install consent".into());
        }
        let binding = s
            .aur_receipt_binding(
                &r.operation_id,
                &plan,
                aur_boottime()?,
                receipt_unix_now()?,
                &paths.trust.digest,
            )
            .map_err(|e| format!("native final receipt: {e:?}"))?;
        verify_receipt_signature_delivery(&plan, &binding.signatures, &pairs)?;
        #[cfg(test)]
        if paths.fixture.is_some() {
            locked
                .claim(&s, &r.operation_id, aur_boottime()?)
                .map_err(|e| format!("{e:?}"))?;
            aur_checkpoint_after_snapshot(
                &s,
                &r.operation_id,
                &plan,
                Ok("17".into()),
                aur_boottime()?,
            )?;
            if paths.fixture.as_ref().and_then(|f| f.fault.as_deref()) == Some("after-checkpoint") {
                std::process::exit(71);
            }
            aur_commit_started_with_post(locked, &s, &r.operation_id, |_| Ok("18".into()))?;
            return Ok(plan);
        }
        require_aur_production_authority()?;
        aur_begin_after_real_snapshot(&locked, &s, &r.operation_id)?;
        aur_commit_started(locked, &s, &r.operation_id)?;
    }
    Ok(plan)
}

pub(crate) fn run_aur_native_process() -> Result<(), String> {
    if std::env::var("GNUPGHOME").ok().as_deref() != Some(AUR_NATIVE_DOMAIN) {
        return Err("fixed Archive process-start trust domain required".into());
    }
    let paths = NativePaths::production()?;
    native_child_entry(paths)
}

fn native_child_entry(paths: NativePaths) -> Result<(), String> {
    use std::os::fd::AsFd;
    let input_fd = unsafe { libc::fcntl(0, libc::F_DUPFD_CLOEXEC, 3) };
    if input_fd < 0 {
        return Err("native request descriptor unavailable".into());
    }
    let input = unsafe { fs::File::from_raw_fd(input_fd) };
    let bytes = read_receipt_fd(input.as_fd(), 1024 * 1024)?;
    let request: NativeRequest = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    native_request_valid(&request, aur_boottime()?)?;
    // No capability leaks into even trusted prerequisite probes. The request
    // itself is no longer needed on stdin once decoded.
    for fd in request
        .artifacts
        .iter()
        .flat_map(|a| [a.archive_fd, a.signature_fd])
        .chain(request.lease_fd)
        .chain(std::iter::once(request.response_fd))
    {
        if unsafe { libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC) } < 0 {
            return Err("native capability unavailable".into());
        }
    }
    drop(input);
    let null = fs::File::open("/dev/null").map_err(|e| e.to_string())?;
    if unsafe { libc::dup2(null.as_raw_fd(), 0) } < 0 {
        return Err("cannot detach native request stdin".into());
    }
    let fd = request.response_fd;
    if unsafe { libc::fcntl(fd, libc::F_GET_SEALS) } < 0
        || unsafe { libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC) } < 0
    {
        return Err("native response must be a private memfd".into());
    }
    let mut response_file = unsafe { fs::File::from_raw_fd(fd) };
    let m = response_file.metadata().map_err(|e| e.to_string())?;
    if !m.is_file() || m.len() != 0 || m.uid() != unsafe { libc::geteuid() } {
        return Err("unsafe native response".into());
    }
    let result = native_process_request(&request, &paths);
    let response = match &result {
        Ok(p) => NativeResponse {
            schema: 1,
            nonce: request.nonce,
            packages: p.packages().to_vec(),
            policy_sha256: p.policy_fingerprint().into(),
            plan_hash: p.hash(),
            error: None,
        },
        Err(e) => NativeResponse {
            schema: 1,
            nonce: request.nonce,
            packages: vec![],
            policy_sha256: String::new(),
            plan_hash: String::new(),
            error: Some(e.chars().take(2048).collect()),
        },
    };
    let bytes = serde_json::to_vec(&response).map_err(|e| e.to_string())?;
    if bytes.len() as u64 > NATIVE_RESPONSE_LIMIT {
        return Err("native response too large".into());
    }
    response_file.write_all(&bytes).map_err(|e| e.to_string())?;
    let seals = libc::F_SEAL_WRITE | libc::F_SEAL_GROW | libc::F_SEAL_SHRINK | libc::F_SEAL_SEAL;
    if unsafe { libc::fcntl(response_file.as_raw_fd(), libc::F_ADD_SEALS, seals) } != 0 {
        return Err("cannot seal native result".into());
    }
    result.map(|_| ())
}

fn native_dup(fd: BorrowedFd<'_>) -> Result<OwnedFd, String> {
    let value = unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_DUPFD_CLOEXEC, 3) };
    if value < 0 {
        return Err(std::io::Error::last_os_error().to_string());
    }
    Ok(unsafe { OwnedFd::from_raw_fd(value) })
}

fn terminate_native(child: &mut Child) {
    let pid = child.id() as i32;
    unsafe {
        libc::kill(-pid, libc::SIGKILL);
    }
    let _ = child.wait();
}

fn exchange_native(
    command: Command,
    request: NativeRequest,
    fds: &[(BorrowedFd<'_>, BorrowedFd<'_>)],
    lease: Option<BorrowedFd<'_>>,
) -> Result<CanonicalPlan, String> {
    exchange_native_observed(command, request, fds, lease, None)
}

fn exchange_native_observed(
    mut command: Command,
    mut request: NativeRequest,
    fds: &[(BorrowedFd<'_>, BorrowedFd<'_>)],
    lease: Option<BorrowedFd<'_>>,
    observer: Option<&BrokerStore>,
) -> Result<CanonicalPlan, String> {
    use std::os::fd::AsFd;
    if fds.len() != request.artifacts.len() {
        return Err("native FD count mismatch".into());
    }
    let mut keep = Vec::new();
    let mut total = 0u64;
    for (a, (archive, signature)) in request.artifacts.iter_mut().zip(fds) {
        total = total
            .checked_add(
                sealed_size(*archive, 1024 * 1024 * 1024)? + sealed_size(*signature, 1024 * 1024)?,
            )
            .ok_or("native size overflow")?;
        if total > 1024 * 1024 * 1024 {
            return Err("aggregate native intake quota exceeded".into());
        }
        keep.push(native_dup(*archive)?);
        a.archive_fd = keep.last().unwrap().as_raw_fd();
        keep.push(native_dup(*signature)?);
        a.signature_fd = keep.last().unwrap().as_raw_fd();
    }
    let fd = unsafe {
        libc::memfd_create(
            c"aur-native-response".as_ptr(),
            libc::MFD_CLOEXEC | libc::MFD_ALLOW_SEALING,
        )
    };
    if fd < 0 {
        return Err(std::io::Error::last_os_error().to_string());
    }
    let output = unsafe { fs::File::from_raw_fd(fd) };
    keep.push(native_dup(output.as_fd())?);
    request.response_fd = keep.last().unwrap().as_raw_fd();
    if let Some(fd) = lease {
        keep.push(native_dup(fd)?);
        request.lease_fd = Some(keep.last().unwrap().as_raw_fd());
    }
    native_request_valid(&request, aur_boottime()?)?;
    let input = seal_receipt_bytes(&serde_json::to_vec(&request).map_err(|e| e.to_string())?)?;
    let inherit: Vec<_> = keep.iter().map(|v| v.as_raw_fd()).collect();
    command
        .env_clear()
        .env("PATH", "/usr/bin")
        .env("LC_ALL", "C")
        .env("GNUPGHOME", AUR_NATIVE_DOMAIN)
        .stdin(Stdio::from(input))
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    #[cfg(test)]
    if let Some(f) = &request.fixture {
        command.env("GNUPGHOME", &f.domain);
    }

    let parent_pid = unsafe { libc::getpid() };
    unsafe {
        command.pre_exec(move || {
            if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL) != 0
                || libc::getppid() != parent_pid
            {
                return Err(std::io::Error::last_os_error());
            }
            if libc::setpgid(0, 0) != 0 {
                return Err(std::io::Error::last_os_error());
            }
            for fd in &inherit {
                if libc::fcntl(*fd, libc::F_SETFD, 0) < 0 {
                    return Err(std::io::Error::last_os_error());
                }
            }
            Ok(())
        });
    }
    let started = aur_boottime()?;
    let mut child = command.spawn().map_err(|e| e.to_string())?;
    let mut next_observation = Instant::now();
    let status = loop {
        match child.try_wait() {
            Ok(Some(s)) => break s,
            Ok(None) => {}
            Err(e) => {
                terminate_native(&mut child);
                return Err(e.to_string());
            }
        }
        if Instant::now() >= next_observation {
            if let Some(store) = observer {
                match store.native_cancelled_before_commit(&request.operation_id) {
                    Ok(false) => {}
                    Ok(true) => {
                        terminate_native(&mut child);
                        return Err("native operation cancelled before commit; no retry".into());
                    }
                    Err(e) => {
                        terminate_native(&mut child);
                        return Err(format!(
                            "native operation state unavailable: {e:?}; no retry"
                        ));
                    }
                }
            }
            next_observation = Instant::now() + Duration::from_millis(250);
        }
        let now = match aur_boottime() {
            Ok(t) => t,
            Err(e) => {
                terminate_native(&mut child);
                return Err(e);
            }
        };
        // Install consent expires before commit. Do not kill an active commit
        // merely because its preview TTL ended; the absolute execution bound
        // still guarantees cleanup and Unknown evidence on an interrupted commit.
        #[cfg(test)]
        let commit_limit = if request.fixture.as_ref().and_then(|f| f.fault.as_deref())
            == Some("install-timeout")
        {
            1
        } else {
            NATIVE_COMMIT_LIMIT
        };
        #[cfg(not(test))]
        let commit_limit = NATIVE_COMMIT_LIMIT;
        let timed_out = if request.mode == NativeMode::Preview {
            now >= request.deadline
        } else {
            now.saturating_sub(started) >= commit_limit
        };
        if timed_out {
            terminate_native(&mut child);
            return Err("native process timed out; inspect durable outcome; no retry".into());
        }
        std::thread::sleep(Duration::from_millis(20));
    };
    if request.mode == NativeMode::Preview && aur_boottime()? >= request.deadline {
        return Err("native preview expired before response admission".into());
    }
    let bytes = read_receipt_fd(output.as_fd(), NATIVE_RESPONSE_LIMIT)
        .map_err(|e| format!("native process ended without a sealed result: {e}; no retry"))?;
    let response: NativeResponse = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    if response.schema != 1 || response.nonce != request.nonce {
        return Err("native response binding mismatch".into());
    }
    if let Some(e) = response.error {
        return Err(e);
    }
    if !status.success() {
        return Err("native process failed; no automatic retry".into());
    }
    let plan = CanonicalPlan::from_packages(response.packages)
        .and_then(|p| p.with_policy_fingerprint(response.policy_sha256))
        .map_err(|e| format!("{e:?}"))?;
    if plan.hash() != response.plan_hash
        || plan.policy_fingerprint() != request.official_stamp
        || plan.packages().len() != request.artifacts.len()
    {
        return Err("native plan response changed".into());
    }
    for (p, a) in plan.packages().iter().zip(&request.artifacts) {
        if p.package_name != a.identity.package_name
            || p.new_version != a.identity.new_version
            || p.sha256 != a.identity.sha256
            || p.filename != a.identity.filename
            || p.repository != "aur-attested"
            || p.repository_metadata_sha256 != a.identity.repository_metadata_sha256
        {
            return Err("native plan differs from reviewed artifact set".into());
        }
    }
    Ok(plan)
}

fn production_native_command() -> Result<Command, String> {
    let executable = fs::canonicalize("/proc/self/exe").map_err(|e| e.to_string())?;
    for path in executable.ancestors() {
        let m = fs::symlink_metadata(path).map_err(|e| e.to_string())?;
        if m.uid() != 0 || m.mode() & 0o022 != 0 || m.file_type().is_symlink() {
            return Err("unsafe native broker executable".into());
        }
    }
    let mut command = Command::new("/proc/self/exe");
    command.arg("--aur-native");
    Ok(command)
}

// Explicit wire fields are kept separate; no caller-controlled domain/path.
#[allow(clippy::too_many_arguments)]
fn make_native_request(
    mode: NativeMode,
    operation: &str,
    deadline: u64,
    source: &str,
    architecture: &str,
    trust: &ReceiptTrust,
    stamp: String,
    packages: Vec<UpdatePackageInfo>,
    signatures: Vec<String>,
    arches: Vec<Option<String>>,
    plan_hash: Option<String>,
) -> Result<NativeRequest, String> {
    if packages.len() != signatures.len() || packages.len() != arches.len() {
        return Err("native metadata count mismatch".into());
    }
    Ok(NativeRequest {
        schema: 1,
        nonce: uuid::Uuid::new_v4().to_string(),
        mode,
        operation_id: operation.into(),
        deadline,
        trust_sha256: trust.digest.clone(),
        official_stamp: stamp,
        source_package: source.into(),
        architecture: architecture.into(),
        plan_hash,
        artifacts: packages
            .into_iter()
            .zip(signatures)
            .zip(arches)
            .map(
                |((identity, signature_sha256), architecture)| NativeArtifact {
                    identity,
                    signature_sha256,
                    architecture,
                    archive_fd: -1,
                    signature_fd: -1,
                },
            )
            .collect(),
        response_fd: -1,
        lease_fd: None,
        #[cfg(test)]
        fixture: None,
    })
}

fn verify_aur_receipt_isolated(
    store: &BrokerStore,
    context: &ReceiptContext,
    inputs: ReceiptFds<'_>,
    trust: &ReceiptTrust,
    staging: &Path,
) -> Result<VerifiedAurReceipt, String> {
    // Bound duplicate previews before copying or starting a native process.
    let _preview_lease = store
        .worker_lease(&context.operation_id)
        .map_err(|e| format!("native preview already active: {e:?}"))?;
    let pairs = inputs.packages;
    verify_aur_receipt_with_native(
        context,
        inputs,
        trust,
        staging,
        receipt_unix_now()?,
        |intake, receipt| {
            let paths = NativePaths::production()?;
            let mut official = configured_alpm(None).map_err(|e| e.to_string())?;
            let db = PathBuf::from(official.dbpath());
            let packages: Vec<_> = intake.archives.iter().map(|a| a.identity.clone()).collect();
            let stamp = official_aur_preflight(&mut official, &packages, &paths.domain, || {
                paths.state_stamp(&db)
            })?;
            drop(official);
            let request = make_native_request(
                NativeMode::Preview,
                &context.operation_id,
                context.reservation_deadline_boottime,
                &context.review.source_package,
                &context.review.architecture,
                trust,
                stamp,
                packages,
                receipt
                    .packages
                    .iter()
                    .map(|p| p.signature_sha256.clone())
                    .collect(),
                receipt
                    .packages
                    .iter()
                    .map(|p| Some(p.architecture.clone()))
                    .collect(),
                None,
            )?;
            exchange_native(production_native_command()?, request, pairs, None)
        },
    )
}

fn install_aur_isolated(
    store: &BrokerStore,
    consumed: &crate::broker_protocol::ConsumedPreview,
    fds: &[(BorrowedFd<'_>, BorrowedFd<'_>)],
    lease: BorrowedFd<'_>,
) -> Result<(), String> {
    require_aur_production_authority()?;
    store
        .require_queued(&consumed.operation_id)
        .map_err(|e| format!("{e:?}"))?;
    let paths = NativePaths::production()?;
    let binding = store
        .aur_receipt_binding(
            &consumed.operation_id,
            &consumed.plan,
            aur_boottime()?,
            receipt_unix_now()?,
            &paths.trust.digest,
        )
        .map_err(|e| format!("{e:?}"))?;
    verify_receipt_signature_delivery(&consumed.plan, &binding.signatures, fds)?;
    let mut official = configured_alpm(None).map_err(|e| e.to_string())?;
    let db = PathBuf::from(official.dbpath());
    let stamp = official_aur_preflight(
        &mut official,
        consumed.plan.packages(),
        &paths.domain,
        || paths.state_stamp(&db),
    )?;
    drop(official);
    if stamp != consumed.plan.policy_fingerprint() {
        return Err("official/native plan changed since consent".into());
    }
    let request = make_native_request(
        NativeMode::Install,
        &consumed.operation_id,
        binding.native_deadline(),
        &binding.native_review().source_package,
        &binding.native_review().architecture,
        &paths.trust,
        stamp,
        consumed.plan.packages().to_vec(),
        binding.signatures.iter().map(|v| v.1.clone()).collect(),
        vec![None; consumed.plan.packages().len()],
        Some(consumed.plan.hash()),
    )?;
    let result = exchange_native_observed(
        production_native_command()?,
        request,
        fds,
        Some(lease),
        Some(store),
    );
    if let Err(error) = result {
        // A killed child may have committed partially. Reuse the existing
        // durable evidence transition, retaining snapshots and prohibiting replay.
        store
            .worker_process_stopped(&consumed.operation_id, aur_boottime()?)
            .map_err(|e| format!("native interruption: {e:?}"))?;
        return Err(error);
    }
    Ok(())
}

#[cfg(test)]
mod native_process_tests {
    include!("native_tests.rs");
}
