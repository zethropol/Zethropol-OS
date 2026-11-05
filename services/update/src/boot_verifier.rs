//! Read-only, bounded artifact checks. No package manager command, DKMS build,
//! lsinitcpio (which creates temporary files), shell config sourcing or firmware
//! mutation is permitted here. Artifact verification is NOT a successful boot.
use crate::execution_evidence::{BootVerification, VerificationState as State};
use crate::models::UpdatePackageInfo;
use alpm::Alpm;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io::{Read, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Component, Path};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

const MAX_FILE: u64 = 512 * 1024 * 1024;
const MAX_OUTPUT: u64 = 32 * 1024 * 1024;

#[derive(Debug, Clone)]
struct Problem(State, String);
type Result<T> = std::result::Result<T, Problem>;
fn invalid(message: impl Into<String>) -> Problem {
    Problem(State::Failed, message.into())
}
fn unknown(message: impl Into<String>) -> Problem {
    Problem(State::Unknown, message.into())
}
fn io_error(path: &str, error: std::io::Error) -> Problem {
    if error.kind() == std::io::ErrorKind::NotFound {
        invalid(format!("Missing {path}"))
    } else {
        unknown(format!("Cannot inspect {path}: {error}"))
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Tool {
    Archive,
    ArchiveMetadata,
    ArchiveFile,
    Vermagic,
    Blake2,
    Blake3,
    Blake3Long,
    Gzip,
    Xz,
    Zstd,
}
impl Tool {
    fn program(self) -> &'static str {
        match self {
            Self::Archive | Self::ArchiveMetadata | Self::ArchiveFile => "/usr/bin/bsdtar",
            Self::Vermagic => "/usr/bin/modinfo",
            Self::Blake2 => "/usr/bin/b2sum",
            Self::Blake3 | Self::Blake3Long => "/usr/bin/b3sum",
            Self::Gzip => "/usr/bin/gzip",
            Self::Xz => "/usr/bin/xz",
            Self::Zstd => "/usr/bin/zstd",
        }
    }
    fn args(self, path: &str) -> Vec<String> {
        let args: Vec<&str> = match self {
            Self::Archive => vec!["-tf", "-"],
            Self::ArchiveMetadata => vec!["-tvf", "-"],
            Self::ArchiveFile => vec!["-xOf", "-", "--", path],
            Self::Vermagic => vec!["--field=vermagic", "--", path],
            Self::Blake2 | Self::Blake3 => vec!["--", path],
            Self::Blake3Long => vec!["--length", "128", "--", path],
            Self::Gzip | Self::Xz | Self::Zstd => vec!["-cd", "--", path],
        };
        args.into_iter().map(str::to_string).collect()
    }
}

trait Probe {
    fn read(&self, path: &str) -> Result<Vec<u8>>;
    fn canonical(&self, path: &str) -> Result<String>;
    fn directories(&self, path: &str) -> Result<Vec<String>>;
    fn run(&self, tool: Tool, path: &str, input: Option<Vec<u8>>) -> Result<Vec<u8>>;
    fn nonempty(&self, path: &str) -> Result<()> {
        if self.read(path)?.is_empty() {
            Err(invalid(format!("Empty artifact: {path}")))
        } else {
            Ok(())
        }
    }
}
struct Host {
    budget: std::cell::Cell<usize>,
    deadline: Instant,
}
impl Host {
    fn new() -> Self {
        Self {
            budget: std::cell::Cell::new(128),
            deadline: Instant::now() + Duration::from_secs(60),
        }
    }
    fn available(&self) -> Result<()> {
        if Instant::now() >= self.deadline {
            Err(unknown("Artifact inspection deadline exceeded"))
        } else {
            Ok(())
        }
    }
}
impl Probe for Host {
    fn directories(&self, path: &str) -> Result<Vec<String>> {
        self.available()?;
        let mut names = Vec::new();
        for entry in fs::read_dir(path).map_err(|error| io_error(path, error))? {
            let entry = entry.map_err(|error| io_error(path, error))?;
            let kind = entry.file_type().map_err(|error| io_error(path, error))?;
            if kind.is_dir() || kind.is_symlink() {
                names.push(
                    entry
                        .file_name()
                        .into_string()
                        .map_err(|_| unknown("Non-UTF8 directory"))?,
                );
            }
            if names.len() > 4096 {
                return Err(unknown("Directory inspection limit exceeded"));
            }
        }
        Ok(names)
    }
    fn nonempty(&self, path: &str) -> Result<()> {
        self.available()?;
        let metadata = fs::metadata(path).map_err(|error| io_error(path, error))?;
        if !metadata.is_file() || metadata.len() == 0 {
            return Err(invalid(format!("Empty/nonregular artifact: {path}")));
        }
        Ok(())
    }
    fn read(&self, path: &str) -> Result<Vec<u8>> {
        self.available()?;
        // A malformed hook output must not block opening a FIFO. O_NONBLOCK
        // is harmless for regular files; the fd's type is checked before read.
        let file = fs::OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NONBLOCK | libc::O_CLOEXEC)
            .open(path)
            .map_err(|error| io_error(path, error))?;
        if !file
            .metadata()
            .map_err(|error| io_error(path, error))?
            .is_file()
        {
            return Err(unknown(format!("Not a regular readable file: {path}")));
        }
        let mut bytes = Vec::new();
        file.take(MAX_FILE + 1)
            .read_to_end(&mut bytes)
            .map_err(|error| io_error(path, error))?;
        if bytes.len() as u64 > MAX_FILE {
            return Err(unknown(format!("File exceeds inspection limit: {path}")));
        }
        Ok(bytes)
    }
    fn canonical(&self, path: &str) -> Result<String> {
        self.available()?;
        fs::canonicalize(path)
            .map_err(|error| io_error(path, error))?
            .into_os_string()
            .into_string()
            .map_err(|_| unknown("Non-UTF8 artifact path"))
    }
    fn run(&self, tool: Tool, path: &str, input: Option<Vec<u8>>) -> Result<Vec<u8>> {
        self.available()?;
        if self.budget.get() == 0 {
            return Err(unknown("Read-only helper budget exhausted"));
        }
        self.budget.set(self.budget.get() - 1);
        // timeout operates on this read-only helper process group, never on
        // a service or module. No shell is involved and no file is extracted.
        let mut child = Command::new("/usr/bin/timeout")
            .args(["--signal=TERM", "--kill-after=2s"])
            .arg(format!(
                "{}s",
                self.deadline
                    .saturating_duration_since(Instant::now())
                    .as_secs()
                    .clamp(1, 15)
            ))
            .arg(tool.program())
            .args(tool.args(path))
            .env("LC_ALL", "C")
            .stdin(if input.is_some() {
                Stdio::piped()
            } else {
                Stdio::null()
            })
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .map_err(|error| unknown(format!("Helper unavailable {}: {error}", tool.program())))?;
        let writer = input.map(|bytes| {
            let mut stdin = child.stdin.take().expect("piped stdin");
            std::thread::spawn(move || stdin.write_all(&bytes))
        });
        let mut output = Vec::new();
        let read = child
            .stdout
            .take()
            .expect("piped stdout")
            .take(MAX_OUTPUT + 1)
            .read_to_end(&mut output);
        let status = child
            .wait()
            .map_err(|error| unknown(format!("Helper wait: {error}")))?;
        let written = writer
            .map(|thread| thread.join().is_ok_and(|result| result.is_ok()))
            .unwrap_or(true);
        read.map_err(|error| unknown(format!("Helper output: {error}")))?;
        if output.len() as u64 > MAX_OUTPUT {
            return Err(unknown("Helper output limit exceeded"));
        }
        if matches!(status.code(), Some(124 | 125 | 126 | 127 | 137)) {
            return Err(unknown(format!(
                "Helper unavailable or timed out: {}",
                tool.program()
            )));
        }
        if !status.success() || !written {
            // A helper's failure is not a complete explanation: unsupported
            // decoder features or resource termination can also cause it.
            return Err(unknown(format!(
                "Read-only helper could not establish evidence (exit {:?}): {} {path}",
                status.code(),
                tool.program()
            )));
        }
        Ok(output)
    }
}

#[derive(Debug, Clone)]
struct Owner {
    package: String,
    version: String,
}
#[derive(Default)]
struct Inventory {
    files: BTreeMap<String, Owner>,
    ambiguous: BTreeSet<String>,
}
impl Inventory {
    fn from_alpm(handle: &Alpm) -> Self {
        let mut inventory = Self::default();
        for package in handle.localdb().pkgs() {
            for file in package.files().files() {
                let Ok(name) = std::str::from_utf8(file.name()) else {
                    continue;
                };
                if (name.starts_with("usr/lib/modules/")
                    && (name.ends_with("/vmlinuz") || name.ends_with("/pkgbase")))
                    || (name.starts_with("usr/src/") && name.ends_with("/dkms.conf"))
                    || (name.starts_with("usr/share/limine/BOOT") && name.ends_with(".EFI"))
                {
                    if inventory.files.contains_key(&format!("/{name}")) {
                        inventory.ambiguous.insert(format!("/{name}"));
                    }
                    inventory.files.insert(
                        format!("/{name}"),
                        Owner {
                            package: package.name().into(),
                            version: package.version().to_string(),
                        },
                    );
                }
            }
        }
        inventory
    }
}
#[derive(Debug)]
struct Kernel {
    release: String,
    source: String,
    owner: Owner,
    ownership_ambiguous: bool,
}

pub(crate) fn verify(handle: &Alpm, plan: &[UpdatePackageInfo]) -> BootVerification {
    let mut report = inspect(
        &Host::new(),
        &Inventory::from_alpm(handle),
        &plan
            .iter()
            .map(|package| (package.package_name.clone(), package.new_version.clone()))
            .collect(),
    );
    let observed = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|duration| duration.as_secs().to_string())
        .unwrap_or_else(|_| "unknown".into());
    report.details = format!("Read-only artifact observation (Unix UTC seconds {observed}) while the worker held the pacman lock. This is not a continuing health guarantee.\n{}", report.details);
    report
}

pub(crate) fn preparation_state(report: &BootVerification) -> State {
    aggregate(&[report.kernels, report.dkms, report.initramfs, report.limine])
}

fn text(probe: &impl Probe, path: &str) -> Result<String> {
    String::from_utf8(probe.read(path)?).map_err(|_| unknown(format!("Non-UTF8 text: {path}")))
}
fn token(value: &str) -> bool {
    !value.is_empty()
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || b"._+-".contains(&byte))
}
fn clean_absolute(value: &str) -> bool {
    value.starts_with('/')
        && Path::new(value)
            .components()
            .all(|part| matches!(part, Component::RootDir | Component::Normal(_)))
        && !value.contains(['\n', '\r', '\0', '$', '`'])
        && !value.split('/').any(|part| matches!(part, "." | ".."))
}
fn aggregate(states: &[State]) -> State {
    if states.contains(&State::Failed) {
        State::Failed
    } else if states.is_empty() || states.contains(&State::Unknown) {
        State::Unknown
    } else {
        State::Verified
    }
}
fn finding(
    details: &mut Vec<String>,
    category: &str,
    target: &str,
    result: Result<String>,
) -> State {
    let (state, message) = match result {
        Ok(message) => (State::Verified, message),
        Err(Problem(state, message)) => (state, message),
    };
    details.push(format!(
        "{category} [{}] {target}: {message}",
        state.as_str()
    ));
    state
}

fn inspect(
    probe: &impl Probe,
    inventory: &Inventory,
    changed: &BTreeMap<String, String>,
) -> BootVerification {
    let mut report = BootVerification::default();
    let mut details = Vec::new();
    report.running_kernel = match text(probe, "/proc/sys/kernel/osrelease") {
        Ok(release) if token(release.trim()) => release.trim().into(),
        _ => {
            details.push(
                "Running kernel could not be read; it is not inferred from installed packages."
                    .into(),
            );
            String::new()
        }
    };
    let mut kernels = Vec::new();
    let mut kernel_states = Vec::new();
    for (path, owner) in &inventory.files {
        if !path.ends_with("/vmlinuz") {
            continue;
        }
        let release = path
            .trim_start_matches("/usr/lib/modules/")
            .trim_end_matches("/vmlinuz");
        if !token(release) {
            kernel_states.push(State::Unknown);
            continue;
        }
        let kernel = Kernel {
            release: release.into(),
            source: path.clone(),
            owner: owner.clone(),
            ownership_ambiguous: inventory.ambiguous.contains(path),
        };
        if changed
            .get(&kernel.owner.package)
            .is_some_and(|expected| expected != &kernel.owner.version)
        {
            kernel_states.push(finding(
                &mut details,
                "kernel",
                release,
                Err(invalid(format!(
                    "Installed {} version {} differs from approved transaction target {}",
                    kernel.owner.package, kernel.owner.version, changed[&kernel.owner.package]
                ))),
            ));
        }
        kernel_states.push(finding(
            &mut details,
            "kernel",
            release,
            check_kernel(probe, inventory, &kernel),
        ));
        kernels.push(kernel);
    }
    report.installed_kernels = kernels
        .iter()
        .map(|kernel| kernel.release.clone())
        .collect::<Vec<_>>()
        .join(", ");
    report.updated_kernels = kernels
        .iter()
        .filter(|kernel| changed.contains_key(&kernel.owner.package))
        .map(|kernel| kernel.release.clone())
        .collect::<Vec<_>>()
        .join(", ");
    if kernels.is_empty() {
        details.push(
            "No package-owned installed kernel images found; kernel coverage is unknown.".into(),
        );
    }
    report.kernels = aggregate(&kernel_states);
    let mut dkms_states = Vec::new();
    for path in inventory
        .files
        .keys()
        .filter(|path| path.ends_with("/dkms.conf"))
    {
        let owner = &inventory.files[path];
        if inventory.ambiguous.contains(path) {
            dkms_states.push(finding(
                &mut details,
                "dkms",
                path,
                Err(unknown("Ambiguous package ownership of DKMS configuration")),
            ));
        }
        if changed
            .get(&owner.package)
            .is_some_and(|expected| expected != &owner.version)
        {
            dkms_states.push(finding(
                &mut details,
                "dkms",
                path,
                Err(invalid(
                    "Installed DKMS package version differs from the approved transaction target",
                )),
            ));
        }
        for kernel in &kernels {
            dkms_states.push(finding(
                &mut details,
                "dkms",
                &format!("{path} / {}", kernel.release),
                check_dkms(probe, path, kernel),
            ));
        }
    }
    let registered = match probe.directories("/var/lib/dkms") {
        Ok(names) => names,
        Err(problem) if problem.0 == State::Failed => Vec::new(),
        Err(problem) => {
            dkms_states.push(finding(
                &mut details,
                "dkms",
                "registered sources",
                Err(problem),
            ));
            Vec::new()
        }
    };
    let owned_names: BTreeSet<_> = inventory
        .files
        .keys()
        .filter(|path| path.ends_with("/dkms.conf"))
        .filter_map(|path| text(probe, path).ok())
        .filter_map(|config| {
            config.lines().find_map(|line| {
                let (key, value) = line.trim().split_once('=')?;
                (key.trim() == "PACKAGE_NAME")
                    .then(|| literal(value).map(str::to_string))
                    .flatten()
            })
        })
        .collect();
    if registered.iter().any(|name| !owned_names.contains(name)) {
        dkms_states.push(finding(
            &mut details,
            "dkms",
            "unmanaged registrations",
            Err(unknown(
                "Registered DKMS sources are not fully described by package-owned metadata",
            )),
        ));
    }
    report.dkms = if dkms_states.is_empty() && owned_names.is_empty() && registered.is_empty() {
        details.push(
            "dkms [verified] scope: no package-owned or registered DKMS sources observed.".into(),
        );
        State::Verified
    } else {
        aggregate(&dkms_states)
    };
    let entries = load_entries(probe);
    let mut limine_states = Vec::new();
    let mut initramfs_states = Vec::new();
    match entries {
        Err(problem) => {
            limine_states.push(finding(
                &mut details,
                "limine",
                "configuration",
                Err(problem),
            ));
        }
        Ok((esp, entries)) => {
            limine_states.push(finding(
                &mut details,
                "limine",
                "EFI loader",
                check_limine_binary(probe, inventory, &esp),
            ));
            let mut covered = BTreeSet::new();
            let mut unresolved = false;
            for entry in entries {
                if !matches!(
                    entry.protocol.as_str(),
                    "linux" | "efi" | "uefi" | "efi_chainload"
                ) {
                    continue;
                }
                match check_entry(probe, &esp, &entry, &kernels) {
                    Ok(Some((release, init_state, init_details))) => {
                        covered.insert(release.clone());
                        initramfs_states.push(init_state);
                        limine_states.push(State::Verified);
                        details.push(format!("limine [verified] {} -> {release}: package-owned image bytes match installed source.", entry.title));
                        details.push(format!(
                            "initramfs [{}] {}: {init_details}",
                            init_state.as_str(),
                            entry.title
                        ));
                    }
                    Ok(None) => {
                        details.push(format!(
                            "limine [unknown] {}: non-Linux EFI application; not certified.",
                            entry.title
                        ));
                    }
                    Err(problem) => {
                        unresolved |= problem.0 == State::Unknown;
                        limine_states.push(finding(
                            &mut details,
                            "limine",
                            &entry.title,
                            Err(problem),
                        ));
                    }
                }
            }
            for kernel in &kernels {
                if !covered.contains(&kernel.release) {
                    limine_states.push(finding(
                        &mut details,
                        "limine",
                        &kernel.release,
                        Err(if unresolved {
                            unknown(
                                "Entry matching could not be completed for this installed kernel",
                            )
                        } else {
                            invalid(
                                "No readable, matching Linux/UKI entry for this installed kernel",
                            )
                        }),
                    ));
                }
            }
        }
    }
    report.limine = aggregate(&limine_states);
    report.initramfs = aggregate(&initramfs_states);
    details.push("Scope: local installed Linux artifacts only. Firmware selection, Secure Boot signatures, root/cmdline correctness and an actual boot are NOT verified. New kernel boot remains unknown until a separate post-reboot observation.".into());
    // Persist bounded diagnostics, not an unbounded package-generated log.
    let mut summary = details.join("\n");
    if summary.len() > 128 * 1024 {
        let mut end = 128 * 1024;
        while !summary.is_char_boundary(end) {
            end -= 1;
        }
        summary.truncate(end);
        summary.push_str("\nDetails truncated; inspect the journal.");
    }
    report.details = summary;
    report
}

fn check_kernel(probe: &impl Probe, inventory: &Inventory, kernel: &Kernel) -> Result<String> {
    let pkgbase_path = format!("/usr/lib/modules/{}/pkgbase", kernel.release);
    if inventory.ambiguous.contains(&kernel.source) || inventory.ambiguous.contains(&pkgbase_path) {
        return Err(unknown("Ambiguous package ownership for kernel metadata"));
    }
    let pkgbase_owner = inventory
        .files
        .get(&pkgbase_path)
        .ok_or_else(|| unknown("pkgbase ownership unavailable"))?;
    if pkgbase_owner.package != kernel.owner.package
        || pkgbase_owner.version != kernel.owner.version
    {
        return Err(invalid(
            "Kernel image and pkgbase have different package ownership",
        ));
    }
    if text(probe, &pkgbase_path)?.trim() != kernel.owner.package {
        return Err(invalid("pkgbase does not match kernel package"));
    }
    let bytes = probe.read(&kernel.source)?;
    check_bzimage(&bytes, &kernel.release)?;
    let dep = text(
        probe,
        &format!("/usr/lib/modules/{}/modules.dep", kernel.release),
    )?;
    if dep.trim().is_empty() {
        return Err(unknown(
            "Empty modules.dep does not establish module-tree completeness",
        ));
    }
    for line in dep.lines().filter(|line| !line.is_empty()) {
        let (module, dependencies) = line
            .split_once(':')
            .ok_or_else(|| invalid("Malformed modules.dep"))?;
        for module in std::iter::once(module).chain(dependencies.split_whitespace()) {
            if !safe_relative(module) {
                return Err(invalid("Unsafe modules.dep path"));
            }
            probe.nonempty(&format!("/usr/lib/modules/{}/{module}", kernel.release))?;
        }
    }
    Ok(format!("{} {} owns {}; bzImage release and depmod file references match. This is installed, not necessarily running.", kernel.owner.package, kernel.owner.version, kernel.source))
}
fn check_bzimage(bytes: &[u8], release: &str) -> Result<()> {
    if !matches!(std::env::consts::ARCH, "x86_64" | "x86") {
        return Err(unknown(
            "Kernel boot-header validation is implemented only for x86",
        ));
    }
    if bytes.len() < 0x212
        || bytes.get(0x1fe..0x200) != Some(&[0x55, 0xaa])
        || bytes.get(0x202..0x206) != Some(b"HdrS")
    {
        return Err(invalid(
            "Missing/truncated x86 bzImage boot header (other architectures are unsupported)",
        ));
    }
    let offset = 0x200 + u16::from_le_bytes([bytes[0x20e], bytes[0x20f]]) as usize;
    let version = bytes
        .get(offset..)
        .ok_or_else(|| invalid("Kernel version pointer outside image"))?;
    let end = version
        .iter()
        .position(|byte| *byte == 0)
        .ok_or_else(|| invalid("Unterminated kernel version"))?;
    if version[..end]
        .split(|byte| byte.is_ascii_whitespace())
        .next()
        != Some(release.as_bytes())
    {
        return Err(invalid(
            "Embedded kernel version differs from installed module directory",
        ));
    }
    Ok(())
}
fn safe_relative(value: &str) -> bool {
    !value.is_empty()
        && !value.starts_with('/')
        && value
            .split('/')
            .all(|part| !matches!(part, "." | "..") && !part.is_empty())
}
fn literal(value: &str) -> Option<&str> {
    let value = value.trim();
    let value = if value.starts_with('"') && value.ends_with('"')
        || value.starts_with('\'') && value.ends_with('\'')
    {
        value.get(1..value.len().checked_sub(1)?)?
    } else {
        value
    };
    (!value.contains(['$', '`', ';', '\n', '\r'])).then_some(value)
}

fn check_dkms(probe: &impl Probe, config: &str, kernel: &Kernel) -> Result<String> {
    let config = text(probe, config)?;
    let mut name = None;
    let mut version = None;
    let mut modules = Vec::new();
    let mut autoinstall = false;
    for line in config
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
    {
        let Some((key, value)) = line.split_once('=') else {
            return Err(unknown("Executable/conditional DKMS config is not sourced"));
        };
        if !key
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || b"_[]".contains(&byte))
            || literal(value).is_none()
        {
            return Err(unknown("Dynamic DKMS configuration is not executed"));
        }
        let key = key.trim();
        if key == "AUTOINSTALL" && literal(value) == Some("yes") {
            autoinstall = true;
        }
        if key.contains("BUILD_EXCLUSIVE") || key == "AUTOINSTALL" && literal(value) != Some("yes")
        {
            return Err(unknown("DKMS conditional/non-autoinstall policy requires evaluation; config is never sourced"));
        }
        if key == "PACKAGE_NAME" {
            name = literal(value).filter(|value| token(value));
        }
        if key == "PACKAGE_VERSION" {
            version = literal(value).filter(|value| token(value));
        }
        if key.starts_with("BUILT_MODULE_NAME[") {
            modules.push(
                literal(value)
                    .filter(|value| token(value))
                    .ok_or_else(|| unknown("Dynamic DKMS module name"))?,
            );
        }
    }
    if !autoinstall {
        return Err(unknown(
            "DKMS autoinstall policy is not explicit; applicability is unverified",
        ));
    }
    let (name, version) = name
        .zip(version)
        .ok_or_else(|| unknown("DKMS identity is dynamic or absent; config is not executed"))?;
    if modules.is_empty() {
        return Err(unknown("No statically declared DKMS module outputs"));
    }
    let arch = std::env::consts::ARCH;
    let expected = format!("/var/lib/dkms/{name}/{version}/{}/{arch}", kernel.release);
    let registered = probe.canonical(&format!(
        "/var/lib/dkms/{name}/kernel-{}-{arch}",
        kernel.release
    ))?;
    if registered != probe.canonical(&expected)? {
        return Err(invalid("DKMS registered version/kernel target mismatch"));
    }
    let dep = text(
        probe,
        &format!("/usr/lib/modules/{}/modules.dep", kernel.release),
    )?;
    for module in modules {
        let base = format!("{expected}/module/{module}");
        let mut source = None;
        for suffix in [".ko", ".ko.zst", ".ko.xz", ".ko.gz"] {
            let path = format!("{base}{suffix}");
            match probe.nonempty(&path) {
                Ok(()) => {
                    source = Some(path);
                    break;
                }
                Err(problem) if problem.0 == State::Unknown => return Err(problem),
                Err(_) => {}
            }
        }
        let source =
            source.ok_or_else(|| invalid(format!("Missing DKMS build output {module}")))?;
        let candidates: Vec<_> = dep
            .lines()
            .filter_map(|line| line.split_once(':').map(|pair| pair.0))
            .filter(|path| {
                path.rsplit('/').next().is_some_and(|file| {
                    [".ko", ".ko.zst", ".ko.xz", ".ko.gz"]
                        .iter()
                        .any(|suffix| file.strip_suffix(suffix) == Some(module))
                })
            })
            .collect();
        if candidates.len() != 1 || !safe_relative(candidates[0]) {
            return Err(invalid(format!(
                "DKMS installed output ambiguous/missing in modules.dep: {module}"
            )));
        }
        let installed = format!("/usr/lib/modules/{}/{}", kernel.release, candidates[0]);
        for path in [&source, &installed] {
            let magic = probe.run(Tool::Vermagic, path, None)?;
            if magic.split(|byte| byte.is_ascii_whitespace()).next()
                != Some(kernel.release.as_bytes())
            {
                return Err(invalid(format!("DKMS vermagic differs: {path}")));
            }
        }
        let source_bytes = module_bytes(probe, &source)?;
        let installed_bytes = module_bytes(probe, &installed)?;
        if !source_bytes.starts_with(b"\x7fELF") || source_bytes != installed_bytes {
            return Err(invalid(format!(
                "DKMS build/installed module bytes differ: {module}"
            )));
        }
    }
    Ok(format!("{name}/{version}: registered target, build/installed bytes, vermagic and modules.dep agree for {}. Module loading/signature acceptance not tested.", kernel.release))
}
fn module_bytes(probe: &impl Probe, path: &str) -> Result<Vec<u8>> {
    let tool = if path.ends_with(".zst") {
        Some(Tool::Zstd)
    } else if path.ends_with(".xz") {
        Some(Tool::Xz)
    } else if path.ends_with(".gz") {
        Some(Tool::Gzip)
    } else {
        None
    };
    match tool {
        Some(tool) => probe.run(tool, path, None),
        None => probe.read(path),
    }
}

#[derive(Default, Debug)]
struct Entry {
    title: String,
    protocol: String,
    paths: Vec<String>,
    modules: Vec<String>,
}
fn parse_entries(config: &str) -> Result<Vec<Entry>> {
    if config.contains("${") {
        return Err(unknown("Limine macro expansion is not evaluated"));
    }
    let mut entries = Vec::new();
    let mut current: Option<Entry> = None;
    for line in config
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
    {
        if line.starts_with('/') {
            if let Some(entry) = current.take() {
                entries.push(entry);
            }
            current = Some(Entry {
                title: line.into(),
                ..Default::default()
            });
            continue;
        }
        let Some((key, value)) = line.split_once(':') else {
            return Err(unknown("Unsupported Limine config syntax/macros"));
        };
        if let Some(entry) = &mut current {
            match key.trim().to_ascii_lowercase().as_str() {
                "protocol" => {
                    if !entry.protocol.is_empty() {
                        return Err(unknown("Duplicate Limine protocol"));
                    }
                    entry.protocol = value.trim().into();
                }
                "path" | "kernel_path" | "image_path" => entry.paths.push(value.trim().into()),
                "module_path" => entry.modules.push(value.trim().into()),
                _ => {}
            }
        }
    }
    if let Some(entry) = current {
        entries.push(entry);
    }
    Ok(entries)
}
fn load_entries(probe: &impl Probe) -> Result<(String, Vec<Entry>)> {
    let config = text(probe, "/etc/default/limine")
        .map_err(|problem| unknown(format!("Cannot establish configured ESP: {}", problem.1)))?;
    let assignments: Vec<_> = config
        .lines()
        .map(str::trim)
        .filter(|line| !line.starts_with('#') && line.contains("ESP_PATH"))
        .collect();
    if assignments.len() != 1 {
        return Err(unknown(
            "ESP_PATH must be an explicit, unambiguous literal; shell configuration is not sourced",
        ));
    }
    let (key, value) = assignments[0]
        .split_once('=')
        .ok_or_else(|| unknown("Unsupported ESP_PATH assignment"))?;
    let esp = literal(value)
        .filter(|value| key.trim() == "ESP_PATH" && clean_absolute(value))
        .ok_or_else(|| unknown("Unsupported dynamic ESP_PATH"))?;
    // Resolve boot(): only for an explicit mounted filesystem root. This is a
    // local configured-volume mapping, not proof of firmware boot selection.
    let mounts = text(probe, "/proc/self/mountinfo")?;
    if !mounts.lines().any(|line| {
        let fields: Vec<_> = line.split_whitespace().collect();
        fields.len() > 6 && fields[3] == "/" && fields[4] == esp
    }) {
        return Err(unknown(
            "Configured ESP is not an observed filesystem-root mount",
        ));
    }
    Ok((
        esp.into(),
        parse_entries(&text(probe, &format!("{esp}/limine.conf"))?)?,
    ))
}
fn resolve(probe: &impl Probe, esp: &str, uri: &str) -> Result<String> {
    if uri.contains(['$', '\0', '\n', '\r']) {
        return Err(unknown(
            "Dynamic/compressed Limine resources are not resolved",
        ));
    }
    let (uri, hash) = uri
        .split_once('#')
        .map_or((uri, None), |(path, hash)| (path, Some(hash)));
    let (resource, relative) = uri
        .split_once(":/")
        .ok_or_else(|| unknown("Unsupported Limine path"))?;
    if !safe_relative(relative) {
        return Err(unknown("Unsafe/unresolved Limine resource path"));
    }
    let volume = if resource == "boot()" {
        esp.to_string()
    } else if (resource.starts_with("uuid(") || resource.starts_with("guid("))
        && resource.ends_with(')')
    {
        let id = &resource[5..resource.len() - 1];
        if !token(id) {
            return Err(unknown("Invalid UUID path"));
        }
        let devices: BTreeSet<_> = ["by-uuid", "by-partuuid"]
            .iter()
            .filter_map(|kind| probe.canonical(&format!("/dev/disk/{kind}/{id}")).ok())
            .collect();
        let mounts = text(probe, "/proc/self/mountinfo")?;
        let volumes: BTreeSet<_> = mounts
            .lines()
            .filter_map(|line| {
                let fields: Vec<_> = line.split_whitespace().collect();
                let separator = fields.iter().position(|field| *field == "-")?;
                if fields.len() <= separator + 2 || fields.get(3) != Some(&"/") {
                    return None;
                }
                let source = probe.canonical(fields[separator + 2]).ok()?;
                (devices.contains(&source) && clean_absolute(fields[4]))
                    .then(|| fields[4].to_string())
            })
            .collect();
        if volumes.len() != 1 {
            return Err(unknown("UUID volume is unmounted or ambiguous"));
        }
        volumes.into_iter().next().expect("one volume")
    } else {
        return Err(unknown("Unsupported Limine resource device/macro"));
    };
    let path = format!("{}/{relative}", volume.trim_end_matches('/'));
    let canonical = probe.canonical(&path)?;
    let root = probe.canonical(&volume)?;
    if !Path::new(&canonical).starts_with(&root) {
        return Err(invalid(
            "Boot artifact symlink escapes its configured volume",
        ));
    }
    if let Some(hash) = hash {
        let tool = match hash.len() {
            128 => Tool::Blake2,
            64 => Tool::Blake3,
            256 => Tool::Blake3Long,
            _ => return Err(unknown("Unsupported Limine hash length")),
        };
        if !hash.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return Err(invalid("Malformed Limine hash"));
        }
        let digest = probe.run(tool, &canonical, None)?;
        if String::from_utf8_lossy(&digest)
            .split_whitespace()
            .next()
            .map(str::to_ascii_lowercase)
            .as_deref()
            != Some(&hash.to_ascii_lowercase())
        {
            return Err(invalid("Limine artifact hash mismatch"));
        }
    }
    Ok(canonical)
}

fn check_pe_loader(bytes: &[u8]) -> Result<()> {
    if bytes.len() < 64 || !bytes.starts_with(b"MZ") {
        return Err(invalid("EFI loader has no DOS header"));
    }
    let pe = u32::from_le_bytes(bytes[60..64].try_into().expect("four bytes")) as usize;
    let header = bytes
        .get(pe..pe + 24)
        .ok_or_else(|| invalid("Truncated EFI PE header"))?;
    if &header[..4] != b"PE\0\0" {
        return Err(invalid("Invalid EFI PE signature"));
    }
    let machine = u16::from_le_bytes([header[4], header[5]]);
    if std::env::consts::ARCH != "x86_64" {
        return Err(unknown("EFI machine check is implemented only for x86_64"));
    }
    if machine != 0x8664 {
        return Err(invalid("EFI machine differs from x86_64 host"));
    }
    let optional_size = u16::from_le_bytes([header[20], header[21]]) as usize;
    let optional = bytes
        .get(pe + 24..pe + 24 + optional_size)
        .filter(|data| data.len() >= 70)
        .ok_or_else(|| invalid("Missing EFI optional header"))?;
    if u16::from_le_bytes([optional[0], optional[1]]) != 0x20b
        || u16::from_le_bytes([optional[68], optional[69]]) != 10
    {
        return Err(invalid("Expected PE32+ EFI application"));
    }
    Ok(())
}
fn check_limine_binary(probe: &impl Probe, inventory: &Inventory, esp: &str) -> Result<String> {
    let source = "/usr/share/limine/BOOTX64.EFI";
    let owner = inventory
        .files
        .get(source)
        .ok_or_else(|| unknown("Packaged Limine EFI ownership unavailable"))?;
    if inventory.ambiguous.contains(source) {
        return Err(unknown("Ambiguous Limine EFI ownership"));
    }
    let expected = probe.read(source)?;
    check_pe_loader(&expected)?;
    let deployed = probe.read(&format!("{esp}/EFI/limine/limine_x64.efi"))?;
    check_pe_loader(&deployed)?;
    if expected != deployed {
        return Err(unknown("Deployed Limine differs from packaged bytes (possibly signing/config enrollment); identity cannot be certified"));
    }
    Ok(format!("Deployed loader matches {} {} source. Firmware selection and signature acceptance are not tested.",owner.package,owner.version))
}

fn check_entry(
    probe: &impl Probe,
    esp: &str,
    entry: &Entry,
    kernels: &[Kernel],
) -> Result<Option<(String, State, String)>> {
    if entry.paths.len() != 1 {
        return Err(unknown("Missing/ambiguous Limine kernel path"));
    }
    let path = resolve(probe, esp, &entry.paths[0])?;
    let bytes = probe.read(&path)?;
    let (linux, initrd, uki_release) = if entry.protocol != "linux" {
        check_pe_loader(&bytes)?;
        let sections = pe_sections(&bytes)?;
        let Some(linux) = sections.get(".linux") else {
            return Ok(None);
        };
        let initrd = sections
            .get(".initrd")
            .ok_or_else(|| invalid("UKI has no embedded initrd"))?;
        let release = sections
            .get(".uname")
            .ok_or_else(|| unknown("UKI has no .uname identity"))?;
        (
            *linux,
            Some(*initrd),
            Some(
                std::str::from_utf8(release)
                    .map_err(|_| invalid("Invalid UKI .uname"))?
                    .trim_end_matches(['\0', '\n', '\r']),
            ),
        )
    } else {
        (bytes.as_slice(), None, None)
    };
    let mut matched = Vec::new();
    for kernel in kernels {
        if probe.read(&kernel.source)? == linux {
            matched.push(kernel);
        }
    }
    if matched.len() != 1 {
        return Err(unknown("Boot image does not uniquely match an installed package-owned kernel; stale/foreign image or metadata cannot be certified"));
    }
    let kernel = matched[0];
    if kernel.ownership_ambiguous {
        return Err(unknown("Boot image source has ambiguous package ownership"));
    }
    check_bzimage(linux, &kernel.release)?;
    if uki_release.is_some_and(|release| release != kernel.release) {
        return Err(invalid("UKI .uname and embedded Linux image disagree"));
    }
    if let Some(initrd) = initrd {
        let (state, detail) = inspect_initramfs(probe, initrd, &kernel.release);
        return Ok(Some((kernel.release.clone(), state, detail)));
    }
    let mut main_images = Vec::new();
    let mut diagnostics = Vec::new();
    for uri in &entry.modules {
        let path = resolve(probe, esp, uri)?;
        let image = probe.read(&path)?;
        let names = archive_names(probe, &image)?;
        // A separate microcode archive is auxiliary, not proof of an initramfs.
        if names
            .iter()
            .any(|name| name.contains("microcode/") && name.ends_with(".bin"))
            && !names
                .iter()
                .any(|name| name == "init" || name.contains("/modules/"))
        {
            diagnostics.push(format!("Auxiliary microcode archive readable: {path}"));
            continue;
        }
        let (state, detail) = inspect_initramfs(probe, &image, &kernel.release);
        main_images.push(state);
        diagnostics.push(format!("{path}: {detail}"));
    }
    if main_images.is_empty() {
        return Ok(Some((
            kernel.release.clone(),
            State::Failed,
            "No main initramfs module was validated".into(),
        )));
    }
    Ok(Some((
        kernel.release.clone(),
        aggregate(&main_images),
        diagnostics.join("; "),
    )))
}
fn inspect_initramfs(probe: &impl Probe, bytes: &[u8], release: &str) -> (State, String) {
    let checked = (|| -> Result<String> {
        let names = archive_names(probe, bytes)?;
        let (state, detail) = check_initramfs_names(&names, release);
        if state != State::Verified {
            return Err(Problem(state, detail));
        }
        let main = bytes[main_cpio_offset(bytes)?..].to_vec();
        let metadata =
            String::from_utf8(probe.run(Tool::ArchiveMetadata, "", Some(main.clone()))?)
                .map_err(|_| unknown("Unrecognized archive metadata"))?;
        let target = archive_executable_target(&metadata, &names, "init")?;
        let executable = probe.run(Tool::ArchiveFile, &target, Some(main.clone()))?;
        if !(executable.len() >= 64 && executable.starts_with(b"\x7fELF"))
            && !executable.starts_with(b"#!")
        {
            return Err(invalid(
                "Init entry does not contain a recognizable executable/script",
            ));
        }
        if executable.starts_with(b"#!") {
            let first_line = executable
                .split(|byte| *byte == b'\n')
                .next()
                .unwrap_or_default();
            let interpreter = std::str::from_utf8(&first_line[2..])
                .map_err(|_| invalid("Invalid init shebang"))?
                .split_whitespace()
                .next()
                .ok_or_else(|| invalid("Empty init shebang"))?;
            if !clean_absolute(interpreter) {
                return Err(unknown("Unsupported init interpreter path"));
            }
            let interpreter = interpreter.trim_start_matches('/');
            if !names.iter().any(|name| name == interpreter) {
                return Err(invalid("Init script interpreter is absent from archive"));
            }
            let target = archive_executable_target(&metadata, &names, interpreter)?;
            let payload = probe.run(Tool::ArchiveFile, &target, Some(main))?;
            if payload.len() < 64 || !payload.starts_with(b"\x7fELF") {
                return Err(unknown("Init interpreter is not a directly inspectable ELF; execution chain remains unverified"));
            }
        }
        Ok(format!(
            "{detail} Init executable metadata and payload are readable."
        ))
    })();
    match checked {
        Ok(detail) => (State::Verified, detail),
        Err(Problem(state, detail)) => (state, detail),
    }
}

fn archive_executable_target(metadata: &str, names: &[String], start: &str) -> Result<String> {
    let mut target = start.to_string();
    for _ in 0..16 {
        let lines: Vec<_> = metadata
            .lines()
            .filter(|line| {
                line.split(" -> ")
                    .next()
                    .and_then(|prefix| prefix.split_whitespace().last())
                    .is_some_and(|name| name.trim_start_matches("./") == target)
            })
            .collect();
        if lines.len() != 1 {
            return Err(unknown(
                "Init metadata missing/duplicated or unsupported archive listing",
            ));
        }
        let mode = lines[0].split_whitespace().next().unwrap_or("");
        if mode.starts_with('l') {
            let link = lines[0]
                .split_once(" -> ")
                .ok_or_else(|| unknown("Unrecognized init symlink metadata"))?
                .1;
            let link = link.trim_start_matches('/');
            if !safe_relative(link) {
                return Err(unknown("Complex init symlink requires another verifier"));
            }
            target = if lines[0]
                .split_once(" -> ")
                .is_some_and(|(_, value)| value.starts_with('/'))
            {
                link.into()
            } else {
                Path::new(&target)
                    .parent()
                    .unwrap_or(Path::new(""))
                    .join(link)
                    .to_string_lossy()
                    .into_owned()
            };
            if !names.contains(&target) {
                return Err(invalid("Init symlink target is absent from archive"));
            }
        } else if mode.starts_with('-') {
            if !mode.bytes().enumerate().any(|(index, byte)| {
                [3, 6, 9].contains(&index) && matches!(byte, b'x' | b's' | b't')
            }) {
                return Err(invalid("Init entry is not executable"));
            }
            return Ok(target);
        } else {
            return Err(invalid(
                "Init entry is not a regular executable or supported symlink",
            ));
        }
    }
    Err(unknown("Init symlink chain too long/cyclic"))
}
fn check_initramfs_names(names: &[String], release: &str) -> (State, String) {
    if !names.iter().any(|name| name == "init") {
        return (State::Failed, "Initramfs has no init entry".into());
    }
    let mut releases = BTreeSet::new();
    for name in names {
        let name = name.strip_prefix("usr/").unwrap_or(name);
        if let Some(rest) = name.strip_prefix("lib/modules/") {
            if let Some((version, _)) = rest.split_once('/') {
                releases.insert(version);
            }
        }
    }
    if releases.is_empty() {
        return (State::Unknown, "Initramfs has no observable module kernel identity; built-in-only/custom images need another verifier".into());
    }
    if releases != BTreeSet::from([release]) {
        return (
            State::Failed,
            format!("Initramfs module releases {releases:?} differ from {release}"),
        );
    }
    (State::Verified, format!("Archive decoded; init and module tree identify {release}. Root activation, required drivers and a successful boot are not proven."))
}
fn archive_names(probe: &impl Probe, bytes: &[u8]) -> Result<Vec<String>> {
    let offset = main_cpio_offset(bytes)?;
    let main = bytes
        .get(offset..)
        .ok_or_else(|| invalid("Invalid early CPIO boundary"))?;
    if main.is_empty() {
        return Err(invalid("Empty initramfs"));
    }
    if ![
        b"070701".as_slice(),
        b"070702",
        b"\x1f\x8b",
        b"\xfd7zXZ\0",
        b"\x28\xb5\x2f\xfd",
        b"BZh",
        b"\x04\x22\x4d\x18",
    ]
    .iter()
    .any(|magic| main.starts_with(magic))
    {
        return Err(unknown("Unsupported initramfs compression/format"));
    }
    let output = probe.run(Tool::Archive, "", Some(main.to_vec()))?;
    let listing = std::str::from_utf8(&output).map_err(|_| invalid("Non-UTF8 archive names"))?;
    let mut names = Vec::new();
    for line in listing.lines() {
        let name = line.trim_start_matches("./").trim_end_matches('/');
        if name.is_empty() || name == "." {
            continue;
        }
        if !safe_relative(name) {
            return Err(invalid("Unsafe initramfs archive entry"));
        }
        names.push(name.to_string());
    }
    if names.is_empty() {
        return Err(invalid("Empty archive listing"));
    }
    Ok(names)
}
// Parse only the newc early archive framing; never unpack it to the filesystem.
fn main_cpio_offset(bytes: &[u8]) -> Result<usize> {
    if !bytes.starts_with(b"070701") && !bytes.starts_with(b"070702") {
        return Ok(0);
    }
    let mut offset: usize = 0;
    let mut early = false;
    for _ in 0..100_000 {
        let header = bytes
            .get(offset..offset + 110)
            .ok_or_else(|| invalid("Truncated early CPIO header"))?;
        if !header.starts_with(b"070701") && !header.starts_with(b"070702") {
            return Err(invalid("Invalid early CPIO header"));
        }
        let field = |start: usize| -> Result<usize> {
            usize::from_str_radix(
                std::str::from_utf8(&header[start..start + 8])
                    .map_err(|_| invalid("Invalid CPIO field"))?,
                16,
            )
            .map_err(|_| invalid("Invalid CPIO field"))
        };
        let size = field(54)?;
        let name_size = field(94)?;
        if name_size == 0 || name_size > 4096 || size > MAX_FILE as usize {
            return Err(invalid("Invalid early CPIO size"));
        }
        let name_end = offset
            .checked_add(110 + name_size)
            .ok_or_else(|| invalid("CPIO size overflow"))?;
        let name = bytes
            .get(offset + 110..name_end)
            .ok_or_else(|| invalid("Truncated CPIO name"))?;
        early |= name == b"early_cpio\0";
        let data_start = (name_end + 3) & !3;
        let next = data_start
            .checked_add(size)
            .and_then(|end| end.checked_add(3))
            .map(|end| end & !3)
            .ok_or_else(|| invalid("CPIO size overflow"))?;
        if next > bytes.len() {
            return Err(invalid("Truncated CPIO data"));
        }
        if name == b"TRAILER!!!\0" {
            let main = (next + 511) & !511;
            // A raw main CPIO also has a trailer. Only skip the first archive
            // when a nonempty second stream actually follows its padding.
            return Ok(if early && main < bytes.len() { main } else { 0 });
        }
        offset = next;
    }
    Err(unknown("Early CPIO entry limit exceeded"))
}
fn pe_sections(bytes: &[u8]) -> Result<BTreeMap<String, &[u8]>> {
    let u16_at = |offset: usize| {
        bytes
            .get(offset..offset + 2)
            .map(|data| u16::from_le_bytes([data[0], data[1]]) as usize)
            .ok_or_else(|| invalid("Truncated PE header"))
    };
    let u32_at = |offset: usize| {
        bytes
            .get(offset..offset + 4)
            .map(|data| u32::from_le_bytes(data.try_into().expect("four bytes")) as usize)
            .ok_or_else(|| invalid("Truncated PE header"))
    };
    if !bytes.starts_with(b"MZ") {
        return Err(invalid("EFI image lacks DOS/PE header"));
    }
    let pe = u32_at(0x3c)?;
    if bytes.get(pe..pe + 4) != Some(b"PE\0\0") {
        return Err(invalid("Invalid PE signature"));
    }
    let count = u16_at(pe + 6)?;
    let optional = u16_at(pe + 20)?;
    if count > 96 {
        return Err(unknown("PE section limit exceeded"));
    }
    let mut sections = BTreeMap::new();
    for index in 0..count {
        let header = pe + 24 + optional + index * 40;
        let name = bytes
            .get(header..header + 8)
            .ok_or_else(|| invalid("Truncated PE sections"))?;
        let name = std::str::from_utf8(name)
            .map_err(|_| invalid("Invalid PE section name"))?
            .trim_end_matches('\0');
        if !matches!(name, ".linux" | ".initrd" | ".uname") {
            continue;
        }
        let size = u32_at(header + 8)?;
        let raw_size = u32_at(header + 16)?;
        let start = u32_at(header + 20)?;
        if size > raw_size {
            return Err(invalid("PE section virtual size exceeds stored data"));
        }
        let data = bytes
            .get(
                start
                    ..start
                        .checked_add(size)
                        .ok_or_else(|| invalid("PE size overflow"))?,
            )
            .ok_or_else(|| invalid("PE section outside image"))?;
        if sections.insert(name.into(), data).is_some() {
            return Err(unknown("Duplicate PE section"));
        }
    }
    Ok(sections)
}

#[cfg(test)]
mod tests {
    use super::*;
    type Call = (Tool, String, Vec<u8>);
    #[derive(Default)]
    struct Memory {
        files: BTreeMap<String, Vec<u8>>,
        unreadable: BTreeSet<String>,
        aliases: BTreeMap<String, String>,
        directories: BTreeMap<String, Vec<String>>,
        commands: BTreeMap<Call, Result<Vec<u8>>>,
    }
    impl Probe for Memory {
        fn read(&self, path: &str) -> Result<Vec<u8>> {
            if self.unreadable.contains(path) {
                return Err(unknown(format!("Access denied: {path}")));
            }
            self.files
                .get(path)
                .cloned()
                .ok_or_else(|| invalid(format!("Missing {path}")))
        }
        fn canonical(&self, path: &str) -> Result<String> {
            if let Some(target) = self.aliases.get(path) {
                return Ok(target.clone());
            }
            if self.files.contains_key(path)
                || self
                    .files
                    .keys()
                    .any(|name| name.starts_with(&format!("{path}/")))
            {
                Ok(path.into())
            } else {
                Err(invalid(format!("Missing {path}")))
            }
        }
        fn directories(&self, path: &str) -> Result<Vec<String>> {
            if self.unreadable.contains(path) {
                return Err(unknown(format!("Access denied: {path}")));
            }
            Ok(self.directories.get(path).cloned().unwrap_or_default())
        }
        fn run(&self, tool: Tool, path: &str, input: Option<Vec<u8>>) -> Result<Vec<u8>> {
            self.commands
                .get(&(tool, path.into(), input.unwrap_or_default()))
                .cloned()
                .unwrap_or_else(|| Err(unknown(format!("Unobserved helper {tool:?} {path}"))))
        }
    }
    const RELEASE: &str = "7.3.0-fixture";
    fn kernel_image(release: &str) -> Vec<u8> {
        let mut image = vec![0; 1024];
        image[0x1fe..0x200].copy_from_slice(&[0x55, 0xaa]);
        image[0x202..0x206].copy_from_slice(b"HdrS");
        image[0x20e..0x210].copy_from_slice(&0x100u16.to_le_bytes());
        let version = format!("{release} fixture\0");
        image[0x300..0x300 + version.len()].copy_from_slice(version.as_bytes());
        image
    }
    fn archive_mock(memory: &mut Memory, bytes: &[u8], release: &str) {
        memory.commands.insert(
            (Tool::Archive, "".into(), bytes.into()),
            Ok(format!("init\nbin/sh\nusr/lib/modules/{release}/kernel/fixture.ko\n").into_bytes()),
        );
        memory.commands.insert(
            (Tool::ArchiveMetadata, "".into(), bytes.into()),
            Ok(b"-rwxr-xr-x  0 root root 12 Jan 1 1970 init\n-rwxr-xr-x  0 root root 64 Jan 1 1970 bin/sh\n".to_vec()),
        );
        memory.commands.insert(
            (Tool::ArchiveFile, "init".into(), bytes.into()),
            Ok(b"#!/bin/sh\nexec /usr/bin/init\n".to_vec()),
        );
        let mut interpreter = vec![0; 64];
        interpreter[..4].copy_from_slice(b"\x7fELF");
        memory.commands.insert(
            (Tool::ArchiveFile, "bin/sh".into(), bytes.into()),
            Ok(interpreter),
        );
    }
    fn fixture() -> (Memory, Inventory) {
        let mut memory = Memory::default();
        let mut inventory = Inventory::default();
        let loader = uki(&[], &[], "");
        memory
            .files
            .insert("/usr/share/limine/BOOTX64.EFI".into(), loader.clone());
        memory
            .files
            .insert("/boot/EFI/limine/limine_x64.efi".into(), loader);
        inventory.files.insert(
            "/usr/share/limine/BOOTX64.EFI".into(),
            Owner {
                package: "limine".into(),
                version: "12-fixture".into(),
            },
        );
        for suffix in ["vmlinuz", "pkgbase"] {
            inventory.files.insert(
                format!("/usr/lib/modules/{RELEASE}/{suffix}"),
                Owner {
                    package: "linux-fixture".into(),
                    version: "7.3.0-1".into(),
                },
            );
        }
        memory.files.insert(
            format!("/usr/lib/modules/{RELEASE}/vmlinuz"),
            kernel_image(RELEASE),
        );
        memory.files.insert(
            format!("/usr/lib/modules/{RELEASE}/pkgbase"),
            b"linux-fixture\n".to_vec(),
        );
        memory.files.insert(
            format!("/usr/lib/modules/{RELEASE}/modules.dep"),
            b"kernel/fixture.ko:\n".to_vec(),
        );
        memory.files.insert(
            format!("/usr/lib/modules/{RELEASE}/kernel/fixture.ko"),
            b"\x7fELFfixture".to_vec(),
        );
        memory.files.insert(
            "/proc/sys/kernel/osrelease".into(),
            b"6.18-running\n".to_vec(),
        );
        memory.files.insert(
            "/etc/default/limine".into(),
            b"ESP_PATH=\"/boot\"\n".to_vec(),
        );
        memory.files.insert(
            "/proc/self/mountinfo".into(),
            b"1 0 0:1 / /boot rw - vfat /dev/fixture rw\n".to_vec(),
        );
        memory.files.insert("/boot/limine.conf".into(), b"/Zethropol\n protocol: linux\n path: boot():/vmlinuz\n module_path: boot():/initramfs\n".to_vec());
        memory
            .files
            .insert("/boot/vmlinuz".into(), kernel_image(RELEASE));
        let image = b"\x28\xb5\x2f\xfdfixture".to_vec();
        memory.files.insert("/boot/initramfs".into(), image.clone());
        archive_mock(&mut memory, &image, RELEASE);
        (memory, inventory)
    }
    fn report(memory: &Memory, inventory: &Inventory) -> BootVerification {
        inspect(
            memory,
            inventory,
            &BTreeMap::from([("linux-fixture".into(), "7.3.0-1".into())]),
        )
    }
    #[test]
    fn consistent_artifacts_are_not_a_successful_new_kernel_boot() {
        let (memory, inventory) = fixture();
        let report = report(&memory, &inventory);
        assert_eq!(preparation_state(&report), State::Verified);
        assert_eq!(report.running_kernel, "6.18-running");
        assert_eq!(report.installed_kernels, RELEASE);
        assert_eq!(report.updated_kernels, RELEASE);
        assert_eq!(report.new_kernel_boot, State::Unknown);
        assert!(report.details.contains("NOT verified"));
    }

    #[test]
    fn installed_kernel_must_match_the_approved_package_version() {
        let (memory, inventory) = fixture();
        let report = inspect(
            &memory,
            &inventory,
            &BTreeMap::from([("linux-fixture".into(), "not-installed-version".into())]),
        );
        assert_eq!(report.kernels, State::Failed);
        assert!(report.details.contains("differs from approved transaction"));
        assert_eq!(report.running_kernel, "6.18-running");
    }

    #[test]
    fn loader_differences_and_missing_ownership_are_not_assumed_safe() {
        let (mut memory, mut inventory) = fixture();
        memory
            .files
            .get_mut("/boot/EFI/limine/limine_x64.efi")
            .unwrap()
            .push(1);
        assert_eq!(report(&memory, &inventory).limine, State::Unknown);
        memory.files.insert(
            "/boot/EFI/limine/limine_x64.efi".into(),
            b"broken EFI".to_vec(),
        );
        assert_eq!(report(&memory, &inventory).limine, State::Failed);
        inventory.files.remove("/usr/share/limine/BOOTX64.EFI");
        assert_eq!(report(&memory, &inventory).limine, State::Unknown);
    }

    #[test]
    fn duplicate_kernel_ownership_and_helper_limits_are_unknown() {
        let (memory, mut inventory) = fixture();
        inventory
            .ambiguous
            .insert(format!("/usr/lib/modules/{RELEASE}/vmlinuz"));
        assert_eq!(report(&memory, &inventory).kernels, State::Unknown);
        let host = Host {
            budget: std::cell::Cell::new(0),
            deadline: Instant::now() + Duration::from_secs(60),
        };
        assert_eq!(
            host.run(Tool::Archive, "", Some(vec![])).unwrap_err().0,
            State::Unknown
        );
    }

    #[test]
    fn missing_dkms_registration_is_an_observed_failure() {
        let (mut memory, mut inventory) = fixture();
        dkms_fixture(&mut memory, &mut inventory);
        memory.aliases.remove(&format!(
            "/var/lib/dkms/driver/kernel-{RELEASE}-{}",
            std::env::consts::ARCH
        ));
        assert_eq!(report(&memory, &inventory).dkms, State::Failed);
    }
    #[test]
    fn missing_or_corrupt_kernel_and_broken_depmod_are_failures() {
        let (mut memory, inventory) = fixture();
        memory
            .files
            .remove(&format!("/usr/lib/modules/{RELEASE}/vmlinuz"));
        assert_eq!(report(&memory, &inventory).kernels, State::Failed);
        memory.files.insert(
            format!("/usr/lib/modules/{RELEASE}/vmlinuz"),
            b"corrupt".to_vec(),
        );
        assert_eq!(report(&memory, &inventory).kernels, State::Failed);
        memory.files.insert(
            format!("/usr/lib/modules/{RELEASE}/vmlinuz"),
            kernel_image(RELEASE),
        );
        memory
            .files
            .remove(&format!("/usr/lib/modules/{RELEASE}/kernel/fixture.ko"));
        assert_eq!(report(&memory, &inventory).kernels, State::Failed);
        memory.files.insert(
            format!("/usr/lib/modules/{RELEASE}/modules.dep"),
            Vec::new(),
        );
        assert_eq!(report(&memory, &inventory).kernels, State::Unknown);
    }
    #[test]
    fn ownership_is_not_inferred_from_kernel_names() {
        let (memory, mut inventory) = fixture();
        inventory
            .files
            .get_mut(&format!("/usr/lib/modules/{RELEASE}/pkgbase"))
            .unwrap()
            .package = "other-package".into();
        assert_eq!(report(&memory, &inventory).kernels, State::Failed);
        assert_eq!(
            report(&memory, &Inventory::default()).kernels,
            State::Unknown
        );
    }
    #[test]
    fn wrong_kernel_initramfs_and_dangling_init_are_not_accepted() {
        let (mut memory, inventory) = fixture();
        let image = memory.files["/boot/initramfs"].clone();
        archive_mock(&mut memory, &image, "wrong-release");
        assert_eq!(report(&memory, &inventory).initramfs, State::Failed);
        archive_mock(&mut memory, &image, RELEASE);
        memory.commands.insert(
            (Tool::ArchiveMetadata, "".into(), image.clone()),
            Ok(b"lrwxrwxrwx 0 root root 9 Jan 1 1970 init -> missing\n".to_vec()),
        );
        assert_eq!(report(&memory, &inventory).initramfs, State::Failed);
        memory.commands.insert(
            (Tool::ArchiveMetadata, "".into(), image),
            Ok(b"-rw-r--r-- 0 root root 9 Jan 1 1970 init\n".to_vec()),
        );
        assert_eq!(report(&memory, &inventory).initramfs, State::Failed);
    }
    #[test]
    fn missing_helpers_and_unsupported_images_remain_unknown() {
        let (mut memory, inventory) = fixture();
        memory.commands.clear();
        let result = report(&memory, &inventory);
        assert_eq!(result.limine, State::Unknown);
        assert_ne!(preparation_state(&result), State::Verified);
        memory.files.insert(
            "/boot/initramfs".into(),
            b"unsupported image format".to_vec(),
        );
        assert_eq!(report(&memory, &inventory).limine, State::Unknown);
    }

    #[test]
    fn unreadable_evidence_is_unknown_instead_of_success_or_a_missing_file() {
        let (mut memory, inventory) = fixture();
        memory.unreadable.insert("/boot/initramfs".into());
        memory.unreadable.insert("/var/lib/dkms".into());
        let result = report(&memory, &inventory);
        assert_eq!(result.limine, State::Unknown);
        assert_eq!(result.dkms, State::Unknown);
        assert_eq!(preparation_state(&result), State::Unknown);
        assert!(result.details.contains("Access denied"));
    }
    #[test]
    fn boot_entry_missing_and_ambiguous_resource_are_distinct() {
        let (mut memory, inventory) = fixture();
        memory.files.insert(
            "/boot/limine.conf".into(),
            b"/Other\n protocol: bios\n".to_vec(),
        );
        assert_eq!(report(&memory, &inventory).limine, State::Failed);
        memory.files.insert(
            "/boot/limine.conf".into(),
            b"/Linux\n protocol: linux\n path: tftp():/kernel\n".to_vec(),
        );
        assert_eq!(report(&memory, &inventory).limine, State::Unknown);
        memory.files.insert(
            "/etc/default/limine".into(),
            b"ESP_PATH=$(find-esp)\n".to_vec(),
        );
        assert_eq!(report(&memory, &inventory).limine, State::Unknown);
    }
    #[test]
    fn uri_traversal_symlink_escape_and_hash_mismatch_are_rejected() {
        let (mut memory, _) = fixture();
        memory
            .files
            .insert("/boot/${KERNEL}".into(), kernel_image(RELEASE));
        assert_eq!(
            resolve(&memory, "/boot", "boot():/${KERNEL}")
                .unwrap_err()
                .0,
            State::Unknown
        );
        assert_eq!(
            parse_entries("/Linux\nprotocol: linux\npath: boot():/${KERNEL}")
                .unwrap_err()
                .0,
            State::Unknown
        );
        assert!(resolve(&memory, "/boot", "boot():/../etc/shadow").is_err());
        memory
            .aliases
            .insert("/boot/link".into(), "/etc/outside".into());
        assert_eq!(
            resolve(&memory, "/boot", "boot():/link").unwrap_err().0,
            State::Failed
        );
        let hash = "a".repeat(128);
        memory.commands.insert(
            (Tool::Blake2, "/boot/vmlinuz".into(), vec![]),
            Ok(format!("{}  /boot/vmlinuz", "b".repeat(128)).into_bytes()),
        );
        assert_eq!(
            resolve(&memory, "/boot", &format!("boot():/vmlinuz#{hash}"))
                .unwrap_err()
                .0,
            State::Failed
        );
        memory.commands.insert(
            (Tool::Blake2, "/boot/vmlinuz".into(), vec![]),
            Ok(format!("{hash}  /boot/vmlinuz").into_bytes()),
        );
        assert_eq!(
            resolve(&memory, "/boot", &format!("boot():/vmlinuz#{hash}")).unwrap(),
            "/boot/vmlinuz"
        );
    }
    #[test]
    fn uuid_volume_requires_an_observed_unique_mount() {
        let (mut memory, _) = fixture();
        memory
            .aliases
            .insert("/dev/disk/by-uuid/ABCD-1234".into(), "/dev/fixture".into());
        memory
            .aliases
            .insert("/dev/fixture".into(), "/dev/fixture".into());
        assert_eq!(
            resolve(&memory, "/boot", "uuid(ABCD-1234):/vmlinuz").unwrap(),
            "/boot/vmlinuz"
        );
        memory.files.insert("/proc/self/mountinfo".into(),b"1 0 0:1 / /boot rw - vfat /dev/fixture rw\n2 0 0:1 / /other rw - vfat /dev/fixture rw\n".to_vec());
        assert_eq!(
            resolve(&memory, "/boot", "uuid(ABCD-1234):/vmlinuz")
                .unwrap_err()
                .0,
            State::Unknown
        );
    }
    fn dkms_fixture(memory: &mut Memory, inventory: &mut Inventory) {
        let path = "/usr/src/driver-1/dkms.conf";
        memory.files.insert(path.into(),b"PACKAGE_NAME=driver\nPACKAGE_VERSION=1\nBUILT_MODULE_NAME[0]=driver\nAUTOINSTALL=yes\n".to_vec());
        inventory.files.insert(
            path.into(),
            Owner {
                package: "driver-dkms".into(),
                version: "1".into(),
            },
        );
        memory
            .directories
            .insert("/var/lib/dkms".into(), vec!["driver".into()]);
        let base = format!(
            "/var/lib/dkms/driver/1/{RELEASE}/{}",
            std::env::consts::ARCH
        );
        memory.aliases.insert(
            format!(
                "/var/lib/dkms/driver/kernel-{RELEASE}-{}",
                std::env::consts::ARCH
            ),
            base.clone(),
        );
        memory.aliases.insert(base.clone(), base.clone());
        let built = format!("{base}/module/driver.ko");
        let installed = format!("/usr/lib/modules/{RELEASE}/updates/dkms/driver.ko");
        for path in [&built, &installed] {
            memory
                .files
                .insert(path.clone(), b"\x7fELFmodule-fixture".to_vec());
            memory.commands.insert(
                (Tool::Vermagic, path.clone(), vec![]),
                Ok(format!("{RELEASE} SMP\n").into_bytes()),
            );
        }
        memory.files.insert(
            format!("/usr/lib/modules/{RELEASE}/modules.dep"),
            b"kernel/fixture.ko:\nupdates/dkms/driver.ko:\n".to_vec(),
        );
    }
    #[test]
    fn dkms_registration_bytes_and_vermagic_must_all_agree() {
        let (mut memory, mut inventory) = fixture();
        dkms_fixture(&mut memory, &mut inventory);
        assert_eq!(report(&memory, &inventory).dkms, State::Verified);
        let installed = format!("/usr/lib/modules/{RELEASE}/updates/dkms/driver.ko");
        memory.commands.insert(
            (Tool::Vermagic, installed.clone(), vec![]),
            Ok(b"wrong-release SMP".to_vec()),
        );
        assert_eq!(report(&memory, &inventory).dkms, State::Failed);
        memory.commands.insert(
            (Tool::Vermagic, installed.clone(), vec![]),
            Ok(format!("{RELEASE} SMP").into_bytes()),
        );
        memory
            .files
            .insert(installed, b"\x7fELFstale-module".to_vec());
        assert_eq!(report(&memory, &inventory).dkms, State::Failed);
    }
    #[test]
    fn dkms_dynamic_or_unmanaged_sources_are_unknown() {
        let (mut memory, mut inventory) = fixture();
        dkms_fixture(&mut memory, &mut inventory);
        memory.files.insert(
            "/usr/src/driver-1/dkms.conf".into(),
            b"PACKAGE_NAME=driver\nPACKAGE_VERSION=$(get-version)\n".to_vec(),
        );
        assert_eq!(report(&memory, &inventory).dkms, State::Unknown);
        let (mut memory, inventory) = fixture();
        memory
            .directories
            .insert("/var/lib/dkms".into(), vec!["unmanaged".into()]);
        assert_eq!(report(&memory, &inventory).dkms, State::Unknown);
    }
    fn cpio(entries: &[(&str, u32, &[u8])]) -> Vec<u8> {
        let mut out = Vec::new();
        for (name, mode, data) in
            entries
                .iter()
                .copied()
                .chain(std::iter::once(("TRAILER!!!", 0, &[][..])))
        {
            out.extend_from_slice(b"070701");
            for field in [
                0,
                mode,
                0,
                0,
                1,
                0,
                data.len() as u32,
                0,
                0,
                0,
                0,
                (name.len() + 1) as u32,
                0,
            ] {
                out.extend_from_slice(format!("{field:08x}").as_bytes());
            }
            out.extend_from_slice(name.as_bytes());
            out.push(0);
            while !out.len().is_multiple_of(4) {
                out.push(0);
            }
            out.extend_from_slice(data);
            while !out.len().is_multiple_of(4) {
                out.push(0);
            }
        }
        while !out.len().is_multiple_of(512) {
            out.push(0);
        }
        out
    }
    #[test]
    fn early_cpio_framing_rejects_truncation_and_preserves_plain_main_archive() {
        let main = cpio(&[("init", 0o100755, b"#!/bin/sh\n")]);
        assert_eq!(main_cpio_offset(&main).unwrap(), 0);
        let early = cpio(&[("early_cpio", 0o100644, b"1")]);
        let mut combined = early.clone();
        combined.extend_from_slice(&main);
        assert_eq!(main_cpio_offset(&combined).unwrap(), early.len());
        assert_eq!(
            main_cpio_offset(b"070701broken").unwrap_err().0,
            State::Failed
        );
    }
    #[test]
    fn real_read_only_decoder_checks_archive_and_init_without_extracting_files() {
        let path = format!("usr/lib/modules/{RELEASE}/kernel/fixture.ko");
        let mut interpreter = vec![0; 64];
        interpreter[..4].copy_from_slice(b"\x7fELF");
        let image = cpio(&[
            ("bin/sh", 0o100755, interpreter.as_slice()),
            ("init", 0o120777, b"usr/lib/systemd/systemd"),
            ("usr/lib/systemd/systemd", 0o100755, b"#!/bin/sh\n"),
            (&path, 0o100644, b"module"),
        ]);
        let (state, detail) = inspect_initramfs(&Host::new(), &image, RELEASE);
        assert_eq!(state, State::Verified, "{detail}");
        let broken = cpio(&[("init", 0o120777, b"absent"), (&path, 0o100644, b"module")]);
        assert_eq!(
            inspect_initramfs(&Host::new(), &broken, RELEASE).0,
            State::Failed
        );
    }
    fn uki(linux: &[u8], initrd: &[u8], release: &str) -> Vec<u8> {
        let mut out = vec![0u8; 512];
        out[..2].copy_from_slice(b"MZ");
        out[0x3c..0x40].copy_from_slice(&64u32.to_le_bytes());
        out[64..68].copy_from_slice(b"PE\0\0");
        out[68..70].copy_from_slice(&0x8664u16.to_le_bytes());
        out[70..72].copy_from_slice(&3u16.to_le_bytes());
        out[84..86].copy_from_slice(&240u16.to_le_bytes());
        out[88..90].copy_from_slice(&0x20bu16.to_le_bytes());
        out[156..158].copy_from_slice(&10u16.to_le_bytes());
        let sections = [
            (".linux", linux),
            (".initrd", initrd),
            (".uname", release.as_bytes()),
        ];
        for (index, (name, data)) in sections.iter().enumerate() {
            let header = 328 + index * 40;
            out[header..header + name.len()].copy_from_slice(name.as_bytes());
            let start = out.len() as u32;
            let size = data.len() as u32;
            out[header + 8..header + 12].copy_from_slice(&size.to_le_bytes());
            out[header + 16..header + 20].copy_from_slice(&size.to_le_bytes());
            out[header + 20..header + 24].copy_from_slice(&start.to_le_bytes());
            out.extend_from_slice(data);
        }
        out
    }
    #[test]
    fn uki_embedded_identity_is_checked_and_truncated_pe_is_rejected() {
        let (mut memory, inventory) = fixture();
        let linux = kernel_image(RELEASE);
        let initrd = memory.files["/boot/initramfs"].clone();
        memory
            .files
            .insert("/boot/kernel.efi".into(), uki(&linux, &initrd, RELEASE));
        memory.files.insert(
            "/boot/limine.conf".into(),
            b"/UKI\nprotocol: efi\npath: boot():/kernel.efi\n".to_vec(),
        );
        assert_eq!(
            preparation_state(&report(&memory, &inventory)),
            State::Verified
        );
        memory.files.insert(
            "/boot/kernel.efi".into(),
            uki(&linux, &initrd, "other-release"),
        );
        assert_eq!(report(&memory, &inventory).limine, State::Failed);
        assert_eq!(pe_sections(b"MZtruncated").unwrap_err().0, State::Failed);
    }
}
