use std::fs;
use std::os::unix::fs::FileTypeExt;
use std::path::Path;
use std::process::{Command, ExitCode};

const SMARTCTL_BIN: &str = "/usr/bin/smartctl";
const MAX_DEVICE_PATH_LEN: usize = 32;

/// Validates that `device` is a canonical whole block device path under `/dev/`
/// supported for read-only SMART/NVMe inspection (`nvmeXnY`, `sdX`, `vdX`, `mmcblkX`).
pub fn validate_device_path(device: &str) -> Result<(), &'static str> {
    if device.is_empty()
        || device.len() > MAX_DEVICE_PATH_LEN
        || device.starts_with('-')
        || device.contains("..")
        || device.bytes().any(|b| !b.is_ascii() || b.is_ascii_control() || b.is_ascii_whitespace())
    {
        return Err("invalid device path");
    }

    let Some(name) = device.strip_prefix("/dev/") else {
        return Err("device path must start with /dev/");
    };

    if name.contains('/') || name.is_empty() {
        return Err("nested device paths are not permitted");
    }

    if let Some(rest) = name.strip_prefix("nvme") {
        let mut parts = rest.split('n');
        let Some(ctrl) = parts.next() else {
            return Err("invalid nvme device");
        };
        let Some(ns) = parts.next() else {
            return Err("invalid nvme namespace");
        };
        if parts.next().is_some()
            || ctrl.is_empty()
            || ctrl.len() > 3
            || ns.is_empty()
            || ns.len() > 3
            || !ctrl.bytes().all(|b| b.is_ascii_digit())
            || !ns.bytes().all(|b| b.is_ascii_digit())
        {
            return Err("invalid nvme device name");
        }
        return Ok(());
    }

    if let Some(rest) = name.strip_prefix("sd").or_else(|| name.strip_prefix("vd")) {
        if rest.is_empty() || rest.len() > 2 || !rest.bytes().all(|b| b.is_ascii_lowercase()) {
            return Err("invalid disk device name");
        }
        return Ok(());
    }

    if let Some(rest) = name.strip_prefix("mmcblk") {
        if rest.is_empty() || rest.len() > 3 || !rest.bytes().all(|b| b.is_ascii_digit()) {
            return Err("invalid mmcblk device name");
        }
        return Ok(());
    }

    Err("unsupported device class")
}

/// Ensures `path` exists as a non-symlink Linux block device node.
pub fn ensure_block_device(path: &Path) -> Result<(), String> {
    let meta = fs::symlink_metadata(path).map_err(|err| format!("device metadata error: {err}"))?;
    if meta.file_type().is_symlink() {
        return Err("device path must not be a symlink".to_string());
    }
    if !meta.file_type().is_block_device() {
        return Err("target path is not a block device".to_string());
    }
    Ok(())
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 2 {
        eprintln!("diagnostics helper: usage: zethropol-diagnostics-helper <block-device>");
        return ExitCode::from(2);
    }

    let device = &args[1];
    if let Err(reason) = validate_device_path(device) {
        eprintln!("diagnostics helper: rejected device path: {reason}");
        return ExitCode::from(2);
    }

    if let Err(reason) = ensure_block_device(Path::new(device)) {
        eprintln!("diagnostics helper: rejected device target: {reason}");
        return ExitCode::from(2);
    }

    let output = match Command::new(SMARTCTL_BIN)
        .env_clear()
        .env("PATH", "/usr/bin")
        .env("LC_ALL", "C")
        .args(["-a", "--json", "--", device])
        .output()
    {
        Ok(output) => output,
        Err(error) => {
            eprintln!("diagnostics helper: {error}");
            return ExitCode::from(1);
        }
    };

    print!("{}", String::from_utf8_lossy(&output.stdout));
    eprint!("{}", String::from_utf8_lossy(&output.stderr));

    ExitCode::from(output.status.code().unwrap_or(1) as u8)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::symlink;

    #[test]
    fn accepts_canonical_whole_block_devices() {
        for dev in [
            "/dev/nvme0n1",
            "/dev/nvme1n2",
            "/dev/nvme12n10",
            "/dev/sda",
            "/dev/sdz",
            "/dev/sdab",
            "/dev/vda",
            "/dev/vdb",
            "/dev/mmcblk0",
            "/dev/mmcblk12",
        ] {
            assert!(validate_device_path(dev).is_ok(), "expected valid: {dev}");
        }
    }

    #[test]
    fn rejects_flags_partitions_traversal_and_arbitrary_paths() {
        for bad in [
            "",
            "-a",
            "--scan",
            "-d",
            "/dev/",
            "/dev/null",
            "/dev/nvme0",
            "/dev/nvme0n1p1",
            "/dev/nvme0n1n1",
            "/dev/nvmen1",
            "/dev/nvme0000n1",
            "/dev/sda1",
            "/dev/sd",
            "/dev/sdabc",
            "/dev/sdA",
            "/dev/vd1",
            "/dev/mmcblk",
            "/dev/mmcblk0p1",
            "/dev/mmcblk0boot0",
            "/dev/../etc/passwd",
            "/dev/disk/by-id/foo",
            "/etc/shadow",
            "/dev/nvme0n1;id",
            "/dev/nvme0n1\n",
            "/dev/nvme0n1 ",
        ] {
            assert!(validate_device_path(bad).is_err(), "expected invalid: {bad:?}");
        }
    }

    #[test]
    fn rejects_non_block_and_symlink_targets() {
        let base = std::env::temp_dir().join(format!(
            "zethropol-diag-helper-test-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&base);
        fs::create_dir_all(&base).expect("temp dir creation should succeed");

        let regular = base.join("fake-disk");
        fs::write(&regular, b"not a block device").expect("temp file creation should succeed");
        assert!(ensure_block_device(&regular).is_err());

        let link = base.join("symlink-disk");
        symlink(&regular, &link).expect("symlink creation should succeed");
        assert!(ensure_block_device(&link).is_err());

        assert!(ensure_block_device(Path::new("/dev/null")).is_err());

        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn polkit_policy_targets_usr_lib_zethropol_and_requires_auth_admin() {
        let policy_path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("polkit/org.zethropol.diagnostics.helper.policy");
        let policy = fs::read_to_string(&policy_path)
            .expect("Polkit policy file should exist in polkit/");

        assert!(policy.contains(
            "<annotate key=\"org.freedesktop.policykit.exec.path\">/usr/lib/zethropol/zethropol-diagnostics-helper</annotate>"
        ));
        assert!(!policy.contains("/usr/libexec/"));
        assert!(policy.contains("<allow_any>auth_admin</allow_any>"));
        assert!(policy.contains("<allow_inactive>auth_admin</allow_inactive>"));
        assert!(policy.contains("<allow_active>auth_admin</allow_active>"));
    }
}
