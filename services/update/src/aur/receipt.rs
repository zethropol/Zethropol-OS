// Broker-side receipt admission. All public inputs are untrusted sealed FDs.
// Neither the worker's claimed metadata nor an output self-signature is authority.
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::io::Read as _;

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct ReviewedOutput {
    pub(crate) name: String,
    pub(crate) version: String,
    pub(crate) filename: String,
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct ReviewedBuild {
    pub(crate) source_package: String,
    pub(crate) source_version: String,
    pub(crate) architecture: String,
    pub(crate) source_plan_sha256: String,
    pub(crate) build_root_sha256: String,
    pub(crate) outputs: Vec<ReviewedOutput>,
}
impl ReviewedBuild {
    pub(crate) fn digest(&self) -> String {
        receipt_hash(&serde_json::to_vec(self).unwrap())
    }
    pub(crate) fn validate(&self) -> Result<(), String> {
        if !receipt_name(&self.source_package)
            || !receipt_text(&self.source_version)
            || !matches!(self.architecture.as_str(), "x86_64" | "aarch64")
            || !receipt_digest(&self.source_plan_sha256)
            || !receipt_digest(&self.build_root_sha256)
            || self.outputs.is_empty()
            || self.outputs.len() > 64
        {
            return Err("invalid reviewed build".into());
        }
        let mut names = std::collections::HashSet::new();
        for p in &self.outputs {
            if !receipt_name(&p.name)
                || !receipt_text(&p.version)
                || !receipt_basename(&p.filename)
                || !names.insert(&p.name)
            {
                return Err("invalid reviewed output set".into());
            }
        }
        if self.outputs.windows(2).any(|w| w[0].name >= w[1].name) {
            return Err("outputs must be uniquely sorted".into());
        }
        Ok(())
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ReceiptContext {
    pub(crate) operation_id: String,
    pub(crate) uid: u32,
    pub(crate) bus_name: String,
    pub(crate) session_id: String,
    pub(crate) review: ReviewedBuild,
    pub(crate) reservation_issued_boottime: u64,
    pub(crate) reservation_deadline_boottime: u64,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ReceiptOutput {
    name: String,
    version: String,
    architecture: String,
    filename: String,
    sha256: String,
    signature_sha256: String,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct BuildReceipt {
    schema: u32,
    purpose: String,
    operation_id: String,
    uid: u32,
    bus_name: String,
    session_id: String,
    source_package: String,
    source_version: String,
    architecture: String,
    build_root_sha256: String,
    source_plan_sha256: String,
    issued_at: u64,
    expires_at: u64,
    packages: Vec<ReceiptOutput>,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct RootPackage {
    name: String,
    version: String,
    sha256: String,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct RootManifest {
    schema: u32,
    purpose: String,
    architecture: String,
    expires: u64,
    packages: Vec<RootPackage>,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ReceiptRoleConfig {
    keyring: String,
    keyring_sha256: String,
    signers: Vec<String>,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ReceiptPolicyConfig {
    schema: u32,
    purpose: String,
    manifest: ReceiptRoleConfig,
    receipt: ReceiptRoleConfig,
    archive: ReceiptRoleConfig,
}
struct ReceiptRole {
    keyring: fs::File,
    signers: Vec<String>,
}
struct ReceiptTrust {
    manifest: ReceiptRole,
    receipt: ReceiptRole,
    archive: ReceiptRole,
    digest: String,
}
fn receipt_hash(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
fn receipt_digest(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
fn receipt_text(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 200
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"@._+:-~".contains(&b))
}
fn receipt_name(s: &str) -> bool {
    receipt_text(s) && s.bytes().next().is_some_and(|b| b.is_ascii_alphanumeric())
}
fn receipt_basename(s: &str) -> bool {
    receipt_name(s) && s != "." && s != ".." && !s.ends_with(".sig")
}
fn read_receipt_fd(fd: BorrowedFd<'_>, limit: u64) -> Result<Vec<u8>, String> {
    let size = sealed_size(fd, limit)?;
    let file = fs::File::from(fd.try_clone_to_owned().map_err(|e| e.to_string())?);
    let mut bytes = vec![0; size as usize];
    file.read_exact_at(&mut bytes, 0)
        .map_err(|e| e.to_string())?;
    Ok(bytes)
}
fn seal_receipt_bytes(bytes: &[u8]) -> Result<fs::File, String> {
    use std::os::fd::FromRawFd;
    let fd = unsafe {
        libc::memfd_create(
            c"aur-trust".as_ptr(),
            libc::MFD_CLOEXEC | libc::MFD_ALLOW_SEALING,
        )
    };
    if fd < 0 {
        return Err(std::io::Error::last_os_error().to_string());
    }
    let mut file = unsafe { fs::File::from_raw_fd(fd) };
    file.write_all(bytes).map_err(|e| e.to_string())?;
    let seals = libc::F_SEAL_SEAL | libc::F_SEAL_WRITE | libc::F_SEAL_GROW | libc::F_SEAL_SHRINK;
    if unsafe { libc::fcntl(file.as_raw_fd(), libc::F_ADD_SEALS, seals) } != 0 {
        return Err(std::io::Error::last_os_error().to_string());
    }
    Ok(file)
}
impl ReceiptTrust {
    fn load() -> Result<Self, String> {
        Self::load_owned(
            Path::new("/etc/zethropol-update/aur-trust/policy.json"),
            0,
            true,
        )
    }
    fn load_owned(path: &Path, owner: u32, all_ancestors: bool) -> Result<Self, String> {
        let parent = path.parent().ok_or("policy parent missing")?;
        for ancestor in parent.ancestors() {
            let m = fs::symlink_metadata(ancestor).map_err(|e| e.to_string())?;
            if !m.is_dir() || m.uid() != owner || m.permissions().mode() & 0o022 != 0 {
                return Err("unsafe trust policy ancestors".into());
            }
            if !all_ancestors {
                break;
            }
        }
        let read = |p: &Path, limit: u64| -> Result<Vec<u8>, String> {
            let file = fs::OpenOptions::new()
                .read(true)
                .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC)
                .open(p)
                .map_err(|e| e.to_string())?;
            let m = file.metadata().map_err(|e| e.to_string())?;
            if !m.is_file()
                || m.uid() != owner
                || m.permissions().mode() & 0o022 != 0
                || m.len() > limit
            {
                return Err("unsafe trust policy/keyring".into());
            }
            let mut bytes = Vec::new();
            std::io::Read::take(file, limit + 1)
                .read_to_end(&mut bytes)
                .map_err(|e| e.to_string())?;
            if bytes.len() as u64 > limit {
                return Err("trust input too large".into());
            }
            Ok(bytes)
        };
        let policy = read(path, 65536)?;
        let config: ReceiptPolicyConfig =
            serde_json::from_slice(&policy).map_err(|e| e.to_string())?;
        if config.schema != 1 || config.purpose != "zethropol-aur-trust" {
            return Err("wrong trust policy schema/purpose".into());
        }
        let role = |r: ReceiptRoleConfig| -> Result<ReceiptRole, String> {
            if !receipt_basename(&r.keyring)
                || !receipt_digest(&r.keyring_sha256)
                || r.signers.is_empty()
                || r.signers.len() > 32
                || r.signers.iter().any(|s| {
                    !matches!(s.len(), 40 | 64)
                        || !s
                            .bytes()
                            .all(|b| b.is_ascii_digit() || (b'A'..=b'F').contains(&b))
                })
            {
                return Err(
                    "explicit role keyring digest and primary fingerprints required".into(),
                );
            }
            let bytes = read(&parent.join(&r.keyring), 16 * 1024 * 1024)?;
            if receipt_hash(&bytes) != r.keyring_sha256 {
                return Err("role keyring digest mismatch".into());
            }
            Ok(ReceiptRole {
                keyring: seal_receipt_bytes(&bytes)?,
                signers: r.signers,
            })
        };
        Ok(Self {
            manifest: role(config.manifest)?,
            receipt: role(config.receipt)?,
            archive: role(config.archive)?,
            digest: receipt_hash(&policy),
        })
    }
}
impl ReceiptRole {
    fn verify(
        &self,
        data: BorrowedFd<'_>,
        signature: BorrowedFd<'_>,
        home: &Path,
    ) -> Result<(), String> {
        // Address held, sealed broker FDs via the parent proc path. No inherited
        // client paths, shell, user executable or GPG home is accepted.
        let fdpath = |fd: i32| format!("/proc/{}/fd/{fd}", std::process::id());
        let out = Command::new("/usr/bin/timeout")
            .args(["--signal=TERM", "--kill-after=2s", "15s", "/usr/bin/gpgv"])
            .arg("--homedir")
            .arg(home)
            .args(["--status-fd", "1", "--keyring"])
            .arg(fdpath(self.keyring.as_raw_fd()))
            .arg("--")
            .arg(fdpath(signature.as_raw_fd()))
            .arg(fdpath(data.as_raw_fd()))
            .env_clear()
            .env("PATH", "/usr/bin")
            .env("LC_ALL", "C")
            .stdin(std::process::Stdio::null())
            .output()
            .map_err(|e| e.to_string())?;
        if !out.status.success() || out.stdout.len() > 1024 * 1024 {
            return Err("detached signature verification failed".into());
        }
        let status = std::str::from_utf8(&out.stdout).map_err(|e| e.to_string())?;
        let mut signers = Vec::new();
        for line in status.lines() {
            let f: Vec<_> = line.split_whitespace().collect();
            if f.first() != Some(&"[GNUPG:]") {
                continue;
            }
            if f.get(1).is_some_and(|v| {
                ["BADSIG", "ERRSIG", "EXPSIG", "EXPKEYSIG", "REVKEYSIG"].contains(v)
            }) {
                return Err("bad/expired/revoked role signature".into());
            }
            if f.get(1) == Some(&"VALIDSIG") {
                signers.push(
                    f.get(11)
                        .or_else(|| f.get(2))
                        .ok_or("missing signature fingerprint")?
                        .to_string(),
                );
            }
        }
        if signers.len() != 1 || !self.signers.contains(&signers[0]) {
            return Err("signer not approved for this role".into());
        }
        Ok(())
    }
}

pub(crate) struct VerifiedAurReceipt {
    context: ReceiptContext,
    receipt_sha256: String,
    expires_at: u64,
    trust_sha256: String,
    plan: CanonicalPlan,
    intake: AurIntake,
    receipt: BuildReceipt,
}
impl VerifiedAurReceipt {
    pub(crate) fn context(&self) -> &ReceiptContext {
        &self.context
    }
    pub(crate) fn plan(&self) -> &CanonicalPlan {
        &self.plan
    }
    pub(crate) fn receipt_sha256(&self) -> &str {
        &self.receipt_sha256
    }
    pub(crate) fn expiry(&self) -> u64 {
        self.expires_at
    }
    pub(crate) fn trust_sha256(&self) -> &str {
        &self.trust_sha256
    }
    pub(crate) fn signature_hashes(&self) -> Vec<(String, String)> {
        self.receipt
            .packages
            .iter()
            .map(|p| (p.name.clone(), p.signature_sha256.clone()))
            .collect()
    }
}
struct ReceiptFds<'a> {
    receipt: BorrowedFd<'a>,
    receipt_signature: BorrowedFd<'a>,
    manifest: BorrowedFd<'a>,
    manifest_signature: BorrowedFd<'a>,
    packages: &'a [(BorrowedFd<'a>, BorrowedFd<'a>)],
}
fn verify_aur_receipt(
    context: &ReceiptContext,
    inputs: ReceiptFds<'_>,
    trust: &ReceiptTrust,
    staging: &Path,
    handle: &mut Alpm,
    now: u64,
    policy_snapshot: impl FnMut() -> Result<String, String>,
) -> Result<VerifiedAurReceipt, String> {
    context.review.validate()?;
    let bytes = read_receipt_fd(inputs.receipt, 1024 * 1024)?;
    read_receipt_fd(inputs.receipt_signature, 1024 * 1024)?;
    let manifest = read_receipt_fd(inputs.manifest, 1024 * 1024)?;
    read_receipt_fd(inputs.manifest_signature, 1024 * 1024)?;
    // Parsing typed structs rejects missing, duplicate and unknown fields.
    let receipt: BuildReceipt = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    let root: RootManifest = serde_json::from_slice(&manifest).map_err(|e| e.to_string())?;
    let r = &context.review;
    if receipt.schema != 1
        || receipt.purpose != "zethropol-aur-build-receipt"
        || receipt.operation_id != context.operation_id
        || receipt.uid != context.uid
        || receipt.bus_name != context.bus_name
        || receipt.session_id != context.session_id
        || receipt.source_package != r.source_package
        || receipt.source_version != r.source_version
        || receipt.architecture != r.architecture
        || receipt.source_plan_sha256 != r.source_plan_sha256
        || receipt.build_root_sha256 != r.build_root_sha256
        || receipt_hash(&manifest) != r.build_root_sha256
        || receipt.issued_at > now
        || receipt.expires_at <= now
        || receipt.expires_at <= receipt.issued_at
        || receipt.expires_at - receipt.issued_at > 300
        || root.schema != 1
        || root.purpose != "zethropol-aur-build-root"
        || root.architecture != r.architecture
        || root.expires <= now
        || root.expires > now.saturating_add(86400)
        || root.packages.is_empty()
        || root.packages.len() > 64
        || receipt.packages.len() != r.outputs.len()
        || inputs.packages.len() != r.outputs.len()
    {
        return Err("receipt/context/manifest identity or lifetime mismatch".into());
    }
    let mut roots = std::collections::HashSet::new();
    if root.packages.iter().any(|p| {
        !receipt_name(&p.name)
            || !receipt_text(&p.version)
            || !receipt_digest(&p.sha256)
            || !roots.insert(&p.name)
    }) {
        return Err("invalid root package closure".into());
    }
    let digest = receipt_hash(&bytes);
    let mut packages = Vec::new();
    let mut output_names = std::collections::HashSet::new();
    for ((p, expected), (archive, signature)) in
        receipt.packages.iter().zip(&r.outputs).zip(inputs.packages)
    {
        if p.name != expected.name
            || p.version != expected.version
            || p.filename != expected.filename
            || !output_names.insert(&p.name)
            || !receipt_digest(&p.sha256)
            || !receipt_digest(&p.signature_sha256)
            || ![r.architecture.as_str(), "any"].contains(&p.architecture.as_str())
            || receipt_hash(&read_receipt_fd(*signature, 1024 * 1024)?) != p.signature_sha256
        {
            return Err("receipt output/signature mismatch".into());
        }
        packages.push(UpdatePackageInfo {
            package_name: p.name.clone(),
            current_version: String::new(),
            new_version: p.version.clone(),
            repository: "aur-attested".into(),
            download_size: sealed_size(*archive, 1024 * 1024 * 1024)?,
            filename: p.filename.clone(),
            sha256: p.sha256.clone(),
            repository_metadata_sha256: digest.clone(),
        });
    }
    let seed = CanonicalPlan::from_packages(packages).map_err(|e| format!("{e:?}"))?;
    let intake = receive_aur_fds(staging, &seed, inputs.packages, handle)?;
    // The server-selected intake directory is private and cleaned on failure.
    trust
        .receipt
        .verify(inputs.receipt, inputs.receipt_signature, &intake.directory)?;
    trust.manifest.verify(
        inputs.manifest,
        inputs.manifest_signature,
        &intake.directory,
    )?;
    for ((archive, signature), (identity, staged)) in inputs
        .packages
        .iter()
        .zip(receipt.packages.iter().zip(&intake.archives))
    {
        trust
            .archive
            .verify(*archive, *signature, &intake.directory)?;
        let pkg = handle
            .pkg_load(
                staged.path.as_os_str().as_encoded_bytes().to_vec(),
                true,
                aur_required_signatures(handle),
            )
            .map_err(|e| e.to_string())?;
        if pkg.arch() != Some(identity.architecture.as_str())
            || pkg.base() != Some(r.source_package.as_str())
        {
            return Err("native archive architecture/pkgbase differs from receipt".into());
        }
    }
    let locked = prepare_aur_locked_with_policy(handle, &intake.archives, policy_snapshot, false)?;
    let plan = locked.plan.clone();
    drop(locked);
    Ok(VerifiedAurReceipt {
        context: context.clone(),
        receipt_sha256: digest,
        expires_at: receipt.expires_at.min(root.expires),
        trust_sha256: trust.digest.clone(),
        plan,
        intake,
        receipt,
    })
}

fn receipt_unix_now() -> Result<u64, String> {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|v| v.as_secs())
        .map_err(|e| e.to_string())
}
fn verify_receipt_signature_delivery(
    plan: &CanonicalPlan,
    signatures: &[(String, String)],
    fds: &[(BorrowedFd<'_>, BorrowedFd<'_>)],
) -> Result<(), String> {
    if signatures.len() != fds.len() || fds.len() != plan.packages().len() {
        return Err("receipt delivery count mismatch".into());
    }
    for ((p, (name, hash)), (_, signature)) in plan.packages().iter().zip(signatures).zip(fds) {
        if &p.package_name != name
            || &receipt_hash(&read_receipt_fd(*signature, 1024 * 1024)?) != hash
        {
            return Err("receipt-bound detached signature changed".into());
        }
    }
    Ok(())
}

async fn reserve_reviewed_aur_message(
    connection: &zbus::Connection,
    header: &zbus::message::Header<'_>,
    review: ReviewedBuild,
) -> Result<ReceiptContext, BrokerError> {
    // `review` is supplied by the server's reviewed source-plan stage. This is
    // deliberately not a D-Bus argument or a worker-provided expectation.
    ReceiptTrust::load().map_err(BrokerError::Io)?;
    let now = boottime_seconds().map_err(|e| BrokerError::Io(e.to_string()))?;
    let caller =
        AuthorizedCaller::check_message(connection, header, PolicyAction::Preview, now).await?;
    let store = persistent_aur_broker()?;
    store.reserve_aur_build(
        caller,
        review,
        boottime_seconds().map_err(|e| BrokerError::Io(e.to_string()))?,
    )
}

fn aur_trust_readiness() -> HashMap<String, String> {
    let mut report = HashMap::new();
    report.insert("installation".into(), "closed".into());
    report.insert("system_acceptance".into(), "not-complete".into());
    report.insert(
        "signed_artifacts".into(),
        "not-verified-by-this-query".into(),
    );
    report.insert(
        "persistent_store".into(),
        "/var/lib/zethropol/aur-transactions; not-provisioned-by-this-query".into(),
    );
    match ReceiptTrust::load() {
        Ok(trust) => {
            report.insert("trust_policy".into(), "verified-public-keyring-pins".into());
            report.insert("trust_sha256".into(), trust.digest);
            report.insert("manifest_signers".into(), trust.manifest.signers.join(","));
            report.insert("receipt_signers".into(), trust.receipt.signers.join(","));
            report.insert("archive_signers".into(), trust.archive.signers.join(","));
        }
        Err(e) => {
            report.insert("trust_policy".into(), "unavailable-or-invalid".into());
            report.insert("reason".into(), e);
        }
    }
    report
}

fn aur_selection_review(
    names: &[String],
    versions: &[String],
) -> Result<HashMap<String, String>, String> {
    if names.is_empty()
        || names.len() > 64
        || names.len() != versions.len()
        || names.iter().any(|n| !receipt_name(n))
        || versions.iter().any(|v| !receipt_text(v))
        || names.iter().collect::<std::collections::HashSet<_>>().len() != names.len()
    {
        return Err("bounded unique AUR package/version selection required".into());
    }
    let mut report = aur_trust_readiness();
    report.insert("api_version".into(), "1".into());
    report.insert("state".into(), "blocked".into());
    report.insert("selected_count".into(), names.len().to_string());
    report.insert(
        "alpm_plan".into(),
        "not-created; signed receipt and native metadata required".into(),
    );
    let reason = if report.get("trust_policy").map(String::as_str)
        != Some("verified-public-keyring-pins")
    {
        format!("AUR blocked: approved project manifest/receipt/output public trust is missing or invalid. {}",report.get("reason").cloned().unwrap_or_default())
    } else {
        "AUR blocked: trusted build-root, independent signed build receipt, current native plan and completed security acceptance are required. No installation was authorized.".into()
    };
    report.insert("reason".into(), reason);
    Ok(report)
}
fn load_approved_aur_review(digest: &str) -> Result<ReviewedBuild, String> {
    if !receipt_digest(digest) {
        return Err("approved source review SHA256 required".into());
    }
    let directory = Path::new("/var/lib/zethropol/aur-approved-reviews");
    for path in directory.ancestors() {
        let m = fs::symlink_metadata(path).map_err(|e| e.to_string())?;
        if !m.is_dir() || m.uid() != 0 || m.permissions().mode() & 0o022 != 0 {
            return Err("unsafe server-approved review ancestors".into());
        }
    }
    let file = fs::OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC)
        .open(directory.join(format!("{digest}.json")))
        .map_err(|e| e.to_string())?;
    let m = file.metadata().map_err(|e| e.to_string())?;
    if !m.is_file() || m.uid() != 0 || m.permissions().mode() & 0o022 != 0 || m.len() > 1024 * 1024
    {
        return Err("unsafe or oversized approved source review".into());
    }
    let mut bytes = Vec::new();
    file.take(1024 * 1024 + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if receipt_hash(&bytes) != digest {
        return Err("approved source review bytes changed".into());
    }
    let review: ReviewedBuild = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    review.validate()?;
    Ok(review)
}
