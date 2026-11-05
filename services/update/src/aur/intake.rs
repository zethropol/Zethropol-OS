// Broker-owned intake. No pathname, executable, keyring or signature policy
// supplied by the desktop is used here. The caller supplies the SERVER plan.
use std::io::Write;
use std::os::fd::{AsRawFd, BorrowedFd};
use std::os::unix::fs::{FileExt, OpenOptionsExt};

struct AurIntake {
    directory: PathBuf,
    archives: Vec<AurArchive>,
}
impl Drop for AurIntake {
    fn drop(&mut self) {
        // Only the random private directory created by this receiver is removed.
        let _ = fs::remove_dir_all(&self.directory);
    }
}

fn sealed_size(fd: BorrowedFd<'_>, limit: u64) -> Result<u64, String> {
    let required = libc::F_SEAL_SEAL | libc::F_SEAL_SHRINK | libc::F_SEAL_GROW | libc::F_SEAL_WRITE;
    let seals = unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_GET_SEALS) };
    if seals < 0 || seals & required != required {
        return Err("immutable sealed FD required".into());
    }
    let file = fs::File::from(fd.try_clone_to_owned().map_err(|e| e.to_string())?);
    let metadata = file.metadata().map_err(|e| e.to_string())?;
    if !metadata.is_file() || metadata.len() == 0 || metadata.len() > limit {
        return Err("invalid or oversized sealed file".into());
    }
    Ok(metadata.len())
}
fn copy_sealed(fd: BorrowedFd<'_>, destination: &Path, size: u64) -> Result<(), String> {
    let input = fs::File::from(fd.try_clone_to_owned().map_err(|e| e.to_string())?);
    let mut output = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(destination)
        .map_err(|e| e.to_string())?;
    let mut offset = 0;
    let mut buffer = [0u8; 65536];
    while offset < size {
        let count = input
            .read_at(&mut buffer[..(size - offset).min(65536) as usize], offset)
            .map_err(|e| e.to_string())?;
        if count == 0 {
            return Err("truncated sealed FD".into());
        }
        output
            .write_all(&buffer[..count])
            .map_err(|e| e.to_string())?;
        offset += count as u64;
    }
    output.sync_all().map_err(|e| e.to_string())
}
fn aur_required_signatures(handle: &Alpm) -> SigLevel {
    let mut policy = handle.local_file_siglevel() | SigLevel::PACKAGE;
    policy.remove(
        SigLevel::PACKAGE_OPTIONAL
            | SigLevel::USE_DEFAULT
            | SigLevel::PACKAGE_MARGINAL_OK
            | SigLevel::PACKAGE_UNKNOWN_OK,
    );
    policy
}

fn receive_aur_bytes(
    parent: &Path,
    approved: &CanonicalPlan,
    fds: &[(BorrowedFd<'_>, BorrowedFd<'_>)],
) -> Result<AurIntake, String> {
    if approved.packages().is_empty()
        || approved.packages().len() > 64
        || !approved.removals().is_empty()
        || fds.len() != approved.packages().len()
        || approved
            .packages()
            .iter()
            .any(|p| p.repository != "aur-attested")
    {
        return Err("exact server-approved AUR archive set required".into());
    }
    let mut total = 0u64;
    let mut sizes = Vec::new();
    for (archive, signature) in fds {
        let a = sealed_size(*archive, 1024 * 1024 * 1024)?;
        let s = sealed_size(*signature, 1024 * 1024)?;
        total = total.checked_add(a + s).ok_or("size overflow")?;
        if total > 1024 * 1024 * 1024 {
            return Err("aggregate intake quota exceeded".into());
        }
        sizes.push((a, s));
    }
    // Production parent must be provisioned by the root service, never a client.
    let metadata = fs::symlink_metadata(parent).map_err(|e| e.to_string())?;
    if !metadata.is_dir()
        || metadata.file_type().is_symlink()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.permissions().mode() & 0o077 != 0
    {
        return Err("unsafe private intake directory".into());
    }
    let directory = parent.join(format!("intake-{}", uuid::Uuid::new_v4()));
    std::os::unix::fs::DirBuilderExt::mode(&mut fs::DirBuilder::new(), 0o700)
        .create(&directory)
        .map_err(|e| e.to_string())?;
    let mut intake = AurIntake {
        directory,
        archives: Vec::new(),
    };
    for ((identity, (archive, signature)), (a, s)) in approved.packages().iter().zip(fds).zip(sizes)
    {
        // CanonicalPlan already rejects path separators; also disallow sig aliases.
        if Path::new(&identity.filename)
            .file_name()
            .and_then(|p| p.to_str())
            != Some(&identity.filename)
            || identity.filename.ends_with(".sig")
        {
            return Err("invalid archive basename".into());
        }
        let path = intake.directory.join(&identity.filename);
        copy_sealed(*archive, &path, a)?;
        copy_sealed(
            *signature,
            &path.with_file_name(format!("{}.sig", identity.filename)),
            s,
        )?;
        verify_archive(&path, &identity.sha256)?;
        intake.archives.push(AurArchive {
            path,
            identity: identity.clone(),
        });
    }
    fs::File::open(&intake.directory)
        .and_then(|f| f.sync_all())
        .map_err(|e| e.to_string())?;
    Ok(intake)
}

fn receive_aur_fds(
    parent: &Path,
    approved: &CanonicalPlan,
    fds: &[(BorrowedFd<'_>, BorrowedFd<'_>)],
    handle: &Alpm,
) -> Result<AurIntake, String> {
    let intake = receive_aur_bytes(parent, approved, fds)?;
    for archive in &intake.archives {
        let path = &archive.path;
        let identity = &archive.identity;
        let package = handle
            .pkg_load(
                path.as_os_str().as_encoded_bytes().to_vec(),
                true,
                aur_required_signatures(handle),
            )
            .map_err(|e| format!("broker archive/signature: {e}"))?;
        verify_loaded_archive_identity(
            package.name(),
            package.version().as_str(),
            package.filename(),
            path,
            identity,
        )?;
    }
    Ok(intake)
}
