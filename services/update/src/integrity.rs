//! Read-only content identities used by preview and the privileged worker.
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::fs::{self, OpenOptions};
use std::io::Read;
use std::os::unix::fs::FileTypeExt;
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};

const MAX_FILE: u64 = 512 * 1024 * 1024;
const MAX_POLICY_BYTES: u64 = 64 * 1024 * 1024;

pub(crate) fn hash_file(path: &Path) -> Result<String, String> {
    let mut file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(path)
        .map_err(|e| format!("open {}: {e}", path.display()))?;
    let metadata = file
        .metadata()
        .map_err(|e| format!("stat {}: {e}", path.display()))?;
    if !metadata.is_file() || metadata.len() > MAX_FILE {
        return Err(format!("{} is not a bounded regular file", path.display()));
    }
    let mut hash = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let count = file
            .read(&mut buffer)
            .map_err(|e| format!("read {}: {e}", path.display()))?;
        if count == 0 {
            break;
        }
        hash.update(&buffer[..count]);
    }
    Ok(format!("{:x}", hash.finalize()))
}

/// Hash the exact repository database and detached signature checked/loaded by
/// libalpm. Missing or ambiguous database material fails closed.
pub(crate) fn repository_database_hash(
    sync_dir: &Path,
    repository: &str,
) -> Result<String, String> {
    let entries = fs::read_dir(sync_dir).map_err(|e| format!("read repository cache: {e}"))?;
    let mut databases = Vec::new();
    for entry in entries {
        let entry = entry.map_err(|e| e.to_string())?;
        let name = entry.file_name().to_string_lossy().into_owned();
        if (name == format!("{repository}.db")
            || name.starts_with(&format!("{repository}.db.tar.")))
            && !name.ends_with(".sig")
        {
            databases.push(entry.path());
        }
    }
    databases.sort();
    if databases.len() != 1 {
        return Err(format!(
            "repository {repository} has {} matching database files",
            databases.len()
        ));
    }
    let database = &databases[0];
    let mut hash = Sha256::new();
    for (path, optional) in [
        (database.clone(), false),
        (PathBuf::from(format!("{}.sig", database.display())), true),
    ] {
        if optional
            && matches!(fs::symlink_metadata(&path), Err(ref e) if e.kind() == std::io::ErrorKind::NotFound)
        {
            continue;
        }
        let digest = hash_file(&path)?;
        let name = path.file_name().unwrap().to_string_lossy();
        hash.update((name.len() as u64).to_be_bytes());
        hash.update(name.as_bytes());
        hash.update(digest.as_bytes());
    }
    Ok(format!("{:x}", hash.finalize()))
}

/// Snapshot all pacman configuration/include directories and effective hook
/// directories. Symlinks, special files, unreadable content and excessive size
/// are rejected instead of being silently omitted.
#[cfg(test)]
pub(crate) fn policy_fingerprint(roots: &[PathBuf]) -> Result<String, String> {
    policy_fingerprint_with_values(roots, &[])
}

pub(crate) fn policy_fingerprint_with_values(
    roots: &[PathBuf],
    effective_config: &[u8],
) -> Result<String, String> {
    let mut files = BTreeSet::new();
    for root in roots {
        collect(root, &mut files)?;
    }
    if files.is_empty() {
        return Err("no package policy or hook files were found".into());
    }
    let mut total = 0u64;
    let mut hash = Sha256::new();
    hash.update(b"zethropol-pacman-policy-v1\0");
    hash.update((effective_config.len() as u64).to_be_bytes());
    hash.update(effective_config);
    for path in files {
        let metadata =
            fs::symlink_metadata(&path).map_err(|e| format!("stat {}: {e}", path.display()))?;
        total = total
            .checked_add(metadata.len())
            .ok_or("policy size overflow")?;
        if total > MAX_POLICY_BYTES {
            return Err("package policy snapshot exceeds limit".into());
        }
        let bytes = fs::read(&path).map_err(|e| format!("read policy {}: {e}", path.display()))?;
        let name = path.as_os_str().as_encoded_bytes();
        hash.update((name.len() as u64).to_be_bytes());
        hash.update(name);
        hash.update((bytes.len() as u64).to_be_bytes());
        hash.update(bytes);
    }
    Ok(format!("{:x}", hash.finalize()))
}

fn collect(root: &Path, files: &mut BTreeSet<PathBuf>) -> Result<(), String> {
    let metadata =
        fs::symlink_metadata(root).map_err(|e| format!("policy path {}: {e}", root.display()))?;
    if metadata.file_type().is_symlink() {
        return Err(format!("policy symlink is unsupported: {}", root.display()));
    }
    if metadata.is_file() {
        files.insert(root.to_path_buf());
        return Ok(());
    }
    if metadata.file_type().is_socket() {
        return Ok(());
    }
    if !metadata.is_dir() {
        return Err(format!(
            "policy path is not a file or directory: {}",
            root.display()
        ));
    }
    let mut children = fs::read_dir(root)
        .map_err(|e| format!("read {}: {e}", root.display()))?
        .map(|entry| entry.map(|e| e.path()).map_err(|e| e.to_string()))
        .collect::<Result<Vec<_>, _>>()?;
    children.sort();
    for child in children {
        collect(&child, files)?;
    }
    Ok(())
}

pub(crate) fn verify_archive(path: &Path, expected_sha256: &str) -> Result<(), String> {
    if expected_sha256.len() != 64 || !expected_sha256.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("trusted repository SHA-256 is missing or malformed".into());
    }
    let actual = hash_file(path)?;
    if !actual.eq_ignore_ascii_case(expected_sha256) {
        return Err(format!("archive checksum mismatch for {}", path.display()));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs::File;
    use std::io::Write;
    use uuid::Uuid;
    struct Temp(PathBuf);
    impl Temp {
        fn new() -> Self {
            let p = std::env::temp_dir().join(format!("zethropol-integrity-{}", Uuid::new_v4()));
            fs::create_dir_all(&p).unwrap();
            Self(p)
        }
    }
    impl Drop for Temp {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    fn write(path: &Path, content: &[u8]) {
        if let Some(p) = path.parent() {
            fs::create_dir_all(p).unwrap();
        }
        File::create(path).unwrap().write_all(content).unwrap();
    }

    #[test]
    fn archive_identity_accepts_expected_and_rejects_changed_or_missing_checksum() {
        let t = Temp::new();
        let p = t.0.join("pkg.pkg.tar.zst");
        write(&p, b"signed package fixture");
        let digest = hash_file(&p).unwrap();
        assert!(verify_archive(&p, &digest).is_ok());
        write(&p, b"changed package fixture");
        assert!(verify_archive(&p, &digest)
            .unwrap_err()
            .contains("mismatch"));
        assert!(verify_archive(&p, "").is_err());
    }
    #[test]
    fn repository_database_fingerprint_binds_database_and_signature() {
        let t = Temp::new();
        let sync = t.0.join("sync");
        write(&sync.join("core.db"), b"metadata-v1");
        write(&sync.join("core.db.sig"), b"signature");
        let one = repository_database_hash(&sync, "core").unwrap();
        write(&sync.join("core.db"), b"metadata-v2");
        let two = repository_database_hash(&sync, "core").unwrap();
        assert_ne!(one, two);
        write(&sync.join("core.db"), b"metadata-v1");
        write(&sync.join("core.db.sig"), b"other-signature");
        assert_ne!(one, repository_database_hash(&sync, "core").unwrap());
        write(&sync.join("core.db.tar.zst"), b"ambiguous");
        assert!(repository_database_hash(&sync, "core").is_err());
    }
    #[test]
    fn policy_hook_changes_are_detected_and_symlinks_fail_closed() {
        let t = Temp::new();
        let config = t.0.join("pacman.conf");
        let hooks = t.0.join("hooks");
        write(&config, b"SigLevel = Required\n");
        write(&hooks.join("10-test.hook"), b"[Action]\n");
        let roots = vec![config.clone(), hooks.clone()];
        let one = policy_fingerprint(&roots).unwrap();
        write(&hooks.join("10-test.hook"), b"[Action]\nExec = changed\n");
        assert_ne!(one, policy_fingerprint(&roots).unwrap());
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(&config, hooks.join("link.hook")).unwrap();
            assert!(policy_fingerprint(&roots).is_err());
        }
    }
}
