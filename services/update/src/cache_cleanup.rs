//! No host entry point: invoked only after verified commit/postconditions.
//! Only ordinary installed-package archives in the pinned pacman cache qualify.
use crate::execution_evidence::{ExecutionEvidence, VerificationState};
use alpm::{Alpm, SigLevel, TransFlag};
use std::collections::HashMap;
use std::ffi::CString;
use std::fs::{self, File, Metadata, OpenOptions};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::path::{Component, Path};

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct CleanupReport {
    pub status: String,
    pub removed_files: u64,
    pub removed_bytes: u64,
    /// Observed available-space increase, not a CoW physical-space guarantee.
    pub reclaimed_bytes: u64,
    pub details: String,
}
impl Default for CleanupReport {
    fn default() -> Self {
        Self {
            status: "not-run".into(),
            removed_files: 0,
            removed_bytes: 0,
            reclaimed_bytes: 0,
            details: String::new(),
        }
    }
}
impl CleanupReport {
    fn warning(&mut self, message: impl AsRef<str>) {
        self.status = "warnings".into();
        if self.details.len() < 12000 {
            self.details
                .push_str(&message.as_ref().chars().take(500).collect::<String>());
            self.details.push('\n');
        }
    }
}
pub(crate) fn eligible(e: &ExecutionEvidence) -> bool {
    e.fully_verified()
        && e.pre_snapshot.is_some()
        && e.post_snapshot.is_some()
        && [e.boot.kernels, e.boot.dkms, e.boot.initramfs, e.boot.limine]
            .iter()
            .all(|state| *state == VerificationState::Verified)
}
pub(crate) fn after_success<F>(e: &ExecutionEvidence, factory: F) -> CleanupReport
where
    F: FnOnce() -> Result<Alpm, String>,
{
    if !eligible(e) {
        return CleanupReport { status: "skipped-unverified".into(), details: "Commit, hooks, boot/recovery checks or pre/post snapshots are not fully verified. Cache retained.".into(), ..Default::default() };
    }
    let mut report = CleanupReport {
        status: "completed".into(),
        ..Default::default()
    };
    let mut handle = match factory() {
        Ok(handle) => handle,
        Err(error) => {
            report.warning(error);
            return report;
        }
    };
    // A new handle reads post-commit installed versions; never reuse the
    // pre-upgrade localdb cache. Do not bypass or delete another owner's lock.
    if let Err(error) = handle.trans_init(TransFlag::NONE) {
        report.warning(format!(
            "Cleanup package database lock unavailable: {error}; cache retained."
        ));
        return report;
    }
    for path in handle.cachedirs().iter() {
        let path = Path::new(path);
        if path != Path::new("/var/cache/pacman/pkg") || !safe_ancestors(path) {
            report.warning(format!(
                "Unapproved or unsafe cache directory retained: {}",
                path.display()
            ));
            continue;
        }
        clean_directory(&handle, path, 0, &mut report);
    }
    if let Err(error) = handle.trans_release() {
        report.warning(format!("Cleanup lock release: {error}"));
    }
    report
}
fn safe_ancestors(path: &Path) -> bool {
    let mut current = std::path::PathBuf::new();
    for component in path.components() {
        if !matches!(component, Component::RootDir | Component::Normal(_)) {
            return false;
        }
        current.push(component);
        match fs::symlink_metadata(&current) {
            Ok(m)
                if m.is_dir()
                    && !m.file_type().is_symlink()
                    && m.uid() == 0
                    && m.mode() & 0o022 == 0 => {}
            _ => return false,
        }
    }
    true
}
fn safe_file(m: &Metadata, owner: u32) -> bool {
    m.is_file() && m.uid() == owner && m.mode() & 0o022 == 0 && m.nlink() == 1
}
fn same_file(a: &Metadata, b: &Metadata) -> bool {
    (
        a.dev(),
        a.ino(),
        a.size(),
        a.mtime(),
        a.mtime_nsec(),
        a.ctime(),
        a.ctime_nsec(),
    ) == (
        b.dev(),
        b.ino(),
        b.size(),
        b.mtime(),
        b.mtime_nsec(),
        b.ctime(),
        b.ctime_nsec(),
    )
}
fn archive_name(name: &str) -> bool {
    [
        ".pkg.tar",
        ".pkg.tar.zst",
        ".pkg.tar.xz",
        ".pkg.tar.gz",
        ".pkg.tar.bz2",
    ]
    .iter()
    .any(|suffix| name.ends_with(suffix))
}
struct Candidate {
    filename: String,
    name: String,
    version: String,
    arch: String,
    metadata: Metadata,
}
fn prior_version<'a>(current: &str, versions: impl Iterator<Item = &'a str>) -> Option<String> {
    versions
        .filter(|version| alpm::vercmp(*version, current).is_lt())
        .max_by(|a, b| alpm::vercmp(*a, *b))
        .map(str::to_owned)
}
fn available_bytes(dir: &File) -> Option<u64> {
    let mut stat = std::mem::MaybeUninit::<libc::statvfs>::uninit();
    if unsafe { libc::fstatvfs(dir.as_raw_fd(), stat.as_mut_ptr()) } != 0 {
        return None;
    }
    let stat = unsafe { stat.assume_init() };
    Some(stat.f_bavail.saturating_mul(stat.f_frsize))
}
fn clean_directory(handle: &Alpm, path: &Path, owner: u32, report: &mut CleanupReport) {
    let mut work = || -> Result<(), String> {
        let dir = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(path)
            .map_err(|e| e.to_string())?;
        let dm = dir.metadata().map_err(|e| e.to_string())?;
        if dm.uid() != owner || dm.mode() & 0o022 != 0 {
            return Err("Unsafe cache ownership/mode; retained".into());
        }
        // Pin the directory inode. No recursion, symlink following, temporary
        // download pruning, orphan-package pruning or arbitrary path deletion.
        let pinned = std::path::PathBuf::from(format!("/proc/self/fd/{}", dir.as_raw_fd()));
        let before = available_bytes(&dir);
        let mut candidates = Vec::new();
        for entry in fs::read_dir(&pinned).map_err(|e| e.to_string())? {
            let entry = entry.map_err(|e| e.to_string())?;
            let Some(filename) = entry.file_name().to_str().map(str::to_owned) else {
                continue;
            };
            if !archive_name(&filename) {
                continue;
            }
            let metadata = fs::symlink_metadata(entry.path()).map_err(|e| e.to_string())?;
            if !safe_file(&metadata, owner) {
                report.warning(format!("Unsafe archive retained: {filename}"));
                continue;
            }
            let loaded = match handle.pkg_load(
                entry.path().as_os_str().as_encoded_bytes().to_vec(),
                false,
                SigLevel::NONE,
            ) {
                Ok(loaded) => loaded,
                Err(_) => {
                    report.warning(format!("Unreadable archive retained: {filename}"));
                    continue;
                }
            };
            let package = loaded.pkg();
            // Signature policy is not relaxed for installation: this read is
            // only to classify old, root-owned files for conservative retention.
            let name = package.name().to_string();
            if crate::broker_protocol::critical_removal(&name)
                || name.contains("kernel")
                || name.contains("firmware")
            {
                continue;
            }
            candidates.push(Candidate {
                filename,
                name,
                version: package.version().as_str().into(),
                arch: package.arch().unwrap_or("").into(),
                metadata,
            });
        }
        let installed: HashMap<_, _> = handle
            .localdb()
            .pkgs()
            .iter()
            .map(|p| {
                (
                    p.name().to_owned(),
                    (
                        p.version().as_str().to_owned(),
                        p.arch().unwrap_or("").to_owned(),
                    ),
                )
            })
            .collect();
        for file in &candidates {
            let Some((current, arch)) = installed.get(&file.name) else {
                continue;
            };
            if arch.is_empty() || file.arch != *arch {
                continue;
            }
            let group: Vec<_> = candidates
                .iter()
                .filter(|p| p.name == file.name && p.arch == *arch)
                .collect();
            // No pruning without both current-version and rollback coverage.
            if !group
                .iter()
                .any(|p| alpm::vercmp(p.version.as_str(), current.as_str()).is_eq())
            {
                continue;
            }
            let Some(previous) = prior_version(current, group.iter().map(|p| p.version.as_str()))
            else {
                continue;
            };
            if !alpm::vercmp(file.version.as_str(), previous.as_str()).is_lt() {
                continue;
            }
            let signature = format!("{}.sig", file.filename);
            // Acquire all leases before deleting either member of the pair.
            // A Linux write lease fails if another process has the file open;
            // it also blocks new opens while identity is checked and unlinked.
            let archive = match lease(&dir, &file.filename, &file.metadata, owner) {
                Ok(file) => file,
                Err(error) => {
                    report.warning(format!("{} retained: {error}", file.filename));
                    continue;
                }
            };
            let sig_meta = match fs::symlink_metadata(pinned.join(&signature)) {
                Ok(m) => Some(m),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
                Err(e) => {
                    report.warning(e.to_string());
                    continue;
                }
            };
            let sig = match sig_meta.as_ref() {
                Some(m) => match lease(&dir, &signature, m, owner) {
                    Ok(f) => Some(f),
                    Err(e) => {
                        report.warning(format!("Signature pair retained: {e}"));
                        continue;
                    }
                },
                None => None,
            };
            if let Err(error) = unlink_leased(&dir, &file.filename, &archive, &file.metadata, owner)
            {
                report.warning(error);
                continue;
            }
            report.removed_files += 1;
            report.removed_bytes += file.metadata.len();
            if let (Some(sig), Some(m)) = (sig, sig_meta) {
                match unlink_leased(&dir, &signature, &sig, &m, owner) {
                    Ok(()) => {
                        report.removed_files += 1;
                        report.removed_bytes += m.len();
                    }
                    Err(e) => report.warning(e),
                }
            }
        }
        dir.sync_all().map_err(|e| e.to_string())?;
        if let (Some(before), Some(after)) = (before, available_bytes(&dir)) {
            report.reclaimed_bytes = report
                .reclaimed_bytes
                .saturating_add(after.saturating_sub(before));
        } else {
            report.warning("Available disk space could not be measured");
        }
        Ok(())
    };
    if let Err(error) = work() {
        report.warning(format!("{}: {error}", path.display()));
    }
}
fn lease(dir: &File, name: &str, expected: &Metadata, owner: u32) -> Result<File, String> {
    if name.contains('/') || !safe_file(expected, owner) {
        return Err("Unsafe filename or inode".into());
    }
    let name = CString::new(name).map_err(|e| e.to_string())?;
    let fd = unsafe {
        libc::openat(
            dir.as_raw_fd(),
            name.as_ptr(),
            libc::O_RDWR | libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC,
        )
    };
    if fd < 0 {
        return Err(std::io::Error::last_os_error().to_string());
    }
    let file = unsafe { File::from_raw_fd(fd) };
    let actual = file.metadata().map_err(|e| e.to_string())?;
    if !safe_file(&actual, owner) || !same_file(expected, &actual) {
        return Err("Archive changed; retained".into());
    }
    // SIGURG's default disposition is ignore, unlike SIGIO. This is per-fd;
    // no process-global signal handlers are replaced. Breaks are checked below.
    if unsafe { libc::fcntl(fd, libc::F_SETOWN, libc::getpid()) } < 0
        || unsafe {
            libc::fcntl(
                fd,
                10, /* Linux F_SETSIG, asm-generic/fcntl.h */
                libc::SIGURG,
            )
        } < 0
        || unsafe { libc::fcntl(fd, libc::F_SETLEASE, libc::F_WRLCK) } < 0
    {
        return Err(format!(
            "Exclusive file lease unavailable (busy/unsupported): {}",
            std::io::Error::last_os_error()
        ));
    }
    Ok(file)
}
fn unlink_leased(
    dir: &File,
    name: &str,
    file: &File,
    expected: &Metadata,
    owner: u32,
) -> Result<(), String> {
    if unsafe { libc::fcntl(file.as_raw_fd(), libc::F_GETLEASE) } != libc::F_WRLCK {
        return Err("Lease break requested; retained".into());
    }
    let pinned = format!("/proc/self/fd/{}/{}", dir.as_raw_fd(), name);
    let actual = fs::symlink_metadata(pinned).map_err(|e| e.to_string())?;
    if !safe_file(&actual, owner) || !same_file(expected, &actual) {
        return Err("Archive path changed; retained".into());
    }
    let name = CString::new(name).map_err(|e| e.to_string())?;
    if unsafe { libc::unlinkat(dir.as_raw_fd(), name.as_ptr(), 0) } != 0 {
        return Err(std::io::Error::last_os_error().to_string());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::execution_evidence::{BootVerification, CommitState};
    use std::os::unix::fs::{symlink, PermissionsExt};
    use std::process::Command;
    struct Fixture {
        path: std::path::PathBuf,
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.path);
        }
    }
    impl Fixture {
        fn new() -> Self {
            let path = std::env::temp_dir()
                .join(format!("zethropol-cleanup-test-{}", uuid::Uuid::new_v4()));
            fs::create_dir_all(path.join("db/local/demo-3-1")).unwrap();
            fs::create_dir_all(path.join("cache")).unwrap();
            fs::create_dir_all(path.join("payload")).unwrap();
            fs::set_permissions(path.join("cache"), fs::Permissions::from_mode(0o700)).unwrap();
            fs::write(path.join("db/local/ALPM_DB_VERSION"), "9\n").unwrap();
            fs::write(
                path.join("db/local/demo-3-1/desc"),
                "%NAME%\ndemo\n\n%VERSION%\n3-1\n\n%ARCH%\nany\n",
            )
            .unwrap();
            fs::write(path.join("db/local/demo-3-1/files"), "%FILES%\n").unwrap();
            Self { path }
        }
        fn archive(&self, name: &str, version: &str) -> std::path::PathBuf {
            let pkg = self.path.join("payload/.PKGINFO");
            fs::write(
                &pkg,
                format!(
                    "pkgname = {name}\npkgver = {version}\npkgdesc = test\narch = any\nsize = 1\n"
                ),
            )
            .unwrap();
            let target = self
                .path
                .join("cache")
                .join(format!("{name}-{version}-any.pkg.tar"));
            assert!(Command::new("/usr/bin/bsdtar")
                .arg("-cf")
                .arg(&target)
                .arg("-C")
                .arg(self.path.join("payload"))
                .arg(".PKGINFO")
                .status()
                .unwrap()
                .success());
            target
        }
        fn handle(&self) -> Alpm {
            Alpm::new("/", self.path.join("db").to_str().unwrap()).unwrap()
        }
        fn clean(&self, report: &mut CleanupReport) {
            let mut handle = self.handle();
            handle.trans_init(TransFlag::NONE).unwrap();
            clean_directory(
                &handle,
                &self.path.join("cache"),
                unsafe { libc::geteuid() },
                report,
            );
            handle.trans_release().unwrap();
        }
    }
    fn verified() -> ExecutionEvidence {
        ExecutionEvidence {
            package_commit: CommitState::Committed,
            hooks: VerificationState::Verified,
            boot_preparation: VerificationState::Verified,
            pre_snapshot: Some("1".into()),
            post_snapshot: Some("2".into()),
            boot: BootVerification {
                kernels: VerificationState::Verified,
                dkms: VerificationState::Verified,
                initramfs: VerificationState::Verified,
                limine: VerificationState::Verified,
                new_kernel_boot: VerificationState::Verified,
                ..Default::default()
            },
            ..Default::default()
        }
    }
    #[test]
    fn failed_or_uncertain_updates_never_even_open_cleanup_handle() {
        let good = verified();
        assert!(eligible(&good));
        let mut cases = vec![ExecutionEvidence::default()];
        let mut bad = good.clone();
        bad.package_commit = CommitState::Failed;
        cases.push(bad);
        let mut bad = good.clone();
        bad.hooks = VerificationState::Unknown;
        cases.push(bad);
        let mut bad = good.clone();
        bad.boot_preparation = VerificationState::Failed;
        cases.push(bad);
        let mut bad = good.clone();
        bad.boot.new_kernel_boot = VerificationState::Unknown;
        cases.push(bad);
        let mut bad = good.clone();
        bad.post_snapshot = None;
        cases.push(bad);
        for e in cases {
            assert_eq!(
                after_success(&e, || panic!("must not initialize libalpm")).status,
                "skipped-unverified"
            );
        }
    }
    #[test]
    fn keeps_current_previous_and_future_and_prunes_only_older_pairs() {
        let f = Fixture::new();
        let oldest = f.archive("demo", "0-1");
        let old = f.archive("demo", "1-1");
        let previous = f.archive("demo", "2-1");
        let current = f.archive("demo", "3-1");
        let future = f.archive("demo", "4-1");
        let signature = std::path::PathBuf::from(format!("{}.sig", oldest.display()));
        fs::write(&signature, "fixture-signature").unwrap();
        let mut report = CleanupReport {
            status: "completed".into(),
            ..Default::default()
        };
        f.clean(&mut report);
        assert_eq!(report.status, "completed", "{}", report.details);
        assert_eq!(report.removed_files, 3);
        assert!(report.removed_bytes > 0);
        assert!(!oldest.exists() && !old.exists() && !signature.exists());
        assert!(previous.exists() && current.exists() && future.exists());
    }
    #[test]
    fn open_archive_is_retained_and_cleanup_warning_preserves_commit_result() {
        let f = Fixture::new();
        let old = f.archive("demo", "1-1");
        f.archive("demo", "2-1");
        f.archive("demo", "3-1");
        let reader = File::open(&old).unwrap();
        let mut report = CleanupReport::default();
        f.clean(&mut report);
        assert_eq!(report.status, "warnings");
        assert_eq!(report.removed_files, 0);
        assert!(old.exists());
        drop(reader);
        let evidence = verified();
        let before = evidence.clone();
        let report = after_success(&evidence, || Err("test setup error".into()));
        assert_eq!(report.status, "warnings");
        assert_eq!(evidence, before);
        assert_eq!(evidence.package_commit, CommitState::Committed);
    }
    #[test]
    fn unsafe_links_partial_downloads_orphans_and_recovery_archives_are_retained() {
        let f = Fixture::new();
        let old = f.archive("demo", "1-1");
        f.archive("demo", "2-1");
        f.archive("demo", "3-1");
        let elsewhere = f.path.join("outside");
        fs::rename(&old, &elsewhere).unwrap();
        symlink(&elsewhere, &old).unwrap();
        let orphan = f.archive("orphan", "1-1");
        let kernel = f.archive("linux-test", "1-1");
        let partial = f.path.join("cache/download.pkg.tar.zst.part");
        fs::write(&partial, "unfinished").unwrap();
        let mut report = CleanupReport::default();
        f.clean(&mut report);
        assert_eq!(report.removed_files, 0);
        assert!(elsewhere.exists() && orphan.exists() && kernel.exists() && partial.exists());
        let actual = f.archive("demo", "0-1");
        fs::hard_link(&actual, f.path.join("hardlink")).unwrap();
        f.clean(&mut report);
        assert!(actual.exists());
    }
    #[test]
    fn symlink_signature_keeps_its_archive_and_outside_target() {
        let f = Fixture::new();
        let old = f.archive("demo", "1-1");
        f.archive("demo", "2-1");
        f.archive("demo", "3-1");
        let target = f.path.join("outside-signature");
        fs::write(&target, "keep").unwrap();
        symlink(&target, format!("{}.sig", old.display())).unwrap();
        let mut report = CleanupReport::default();
        f.clean(&mut report);
        assert!(old.exists() && target.exists());
        assert_eq!(report.removed_files, 0);
    }
    #[test]
    fn pacman_lock_collision_retains_everything_without_unlinking_lock() {
        let f = Fixture::new();
        let old = f.archive("demo", "1-1");
        let mut first = f.handle();
        first.trans_init(TransFlag::NONE).unwrap();
        let report = after_success(&verified(), || Ok(f.handle()));
        assert_eq!(report.status, "warnings");
        assert!(old.exists() && f.path.join("db/db.lck").exists());
        first.trans_release().unwrap();
    }
    #[test]
    fn path_swap_and_world_writable_cache_are_rejected() {
        let f = Fixture::new();
        let old = f.archive("demo", "1-1");
        let metadata = fs::metadata(&old).unwrap();
        fs::remove_file(&old).unwrap();
        fs::write(&old, "different").unwrap();
        let dir = File::open(f.path.join("cache")).unwrap();
        assert!(lease(
            &dir,
            old.file_name().unwrap().to_str().unwrap(),
            &metadata,
            unsafe { libc::geteuid() }
        )
        .is_err());
        fs::set_permissions(f.path.join("cache"), fs::Permissions::from_mode(0o777)).unwrap();
        let mut report = CleanupReport::default();
        f.clean(&mut report);
        assert_eq!(report.removed_files, 0);
        assert_eq!(report.status, "warnings");
        assert!(!safe_ancestors(&f.path.join("cache")));
    }
    #[test]
    fn retention_uses_alpm_versions_including_epoch_and_missing_coverage() {
        assert_eq!(
            prior_version("2:1-1", ["1:99-1", "2:0.9-1", "2:2-1"].into_iter()),
            Some("2:0.9-1".into())
        );
        let f = Fixture::new();
        let old = f.archive("demo", "1-1");
        f.archive("demo", "2-1"); // Current 3-1 absent: preserve all.
        let mut report = CleanupReport::default();
        f.clean(&mut report);
        assert_eq!(report.removed_files, 0);
        assert!(old.exists());
    }
}
