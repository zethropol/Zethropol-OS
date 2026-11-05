//! Suppress only the audited, default root-only snap-pac chain in this worker.
//! Never edit/mask installed hooks or disable Zethropol's mandatory snapshots.
//! Unknown scripts, extra recovery configurations and snap-pac upgrades fail
//! closed until their incoming hook contract has been reviewed.
use crate::models::UpdatePackageInfo;
use std::{fs, path::Path, process::Command};

const SNAP_PAC_SCRIPT: &str = "/usr/share/libalpm/scripts/snap-pac";
const SNAP_PAC_SHA256: &str = "746eacf0785fd40c59750dd9821f894c6b1c41bb1ec892af6872b94093447dc6";
const HOOKS: [(&str, &str); 2] = [
    ("05-snap-pac-pre.hook", "pre"),
    ("zz-snap-pac-post.hook", "post"),
];

pub(crate) fn validate(plan: &[UpdatePackageInfo], hook_dirs: &[String]) -> Result<(), String> {
    if std::env::var("SNAP_PAC_SKIP").as_deref() != Ok("1") {
        return Err(
            "Worker snapshot policy requires its scoped SNAP_PAC_SKIP=1 environment.".into(),
        );
    }
    validate_incoming_plan(plan)?;
    if hook_dirs
        .iter()
        .any(|dir| dir.trim_end_matches('/') != "/etc/pacman.d/hooks")
    {
        return Err("Custom ALPM hook directories require snapshot policy review.".into());
    }
    for (name, _) in HOOKS {
        let local = Path::new("/etc/pacman.d/hooks").join(name);
        match fs::symlink_metadata(&local) {
            Ok(_) => {
                return Err(format!(
                    "Local snap-pac override requires review: {}",
                    local.display()
                ))
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(format!("Unable to inspect {}: {error}", local.display())),
        }
    }
    if !Path::new(SNAP_PAC_SCRIPT)
        .try_exists()
        .map_err(|error| format!("Unable to inspect snap-pac: {error}"))?
    {
        for (name, _) in HOOKS {
            if Path::new("/usr/share/libalpm/hooks")
                .join(name)
                .try_exists()
                .map_err(|error| format!("Unable to inspect snap-pac hook: {error}"))?
            {
                return Err("snap-pac hook exists without the audited script.".into());
            }
        }
        return Ok(());
    }
    for (name, phase) in HOOKS {
        let path = Path::new("/usr/share/libalpm/hooks").join(name);
        let text =
            fs::read_to_string(&path).map_err(|error| format!("{}: {error}", path.display()))?;
        let expected = format!("{SNAP_PAC_SCRIPT} {phase}");
        let exec: Vec<_> = text
            .lines()
            .filter_map(|line| {
                let (key, value) = line.split_once('=')?;
                (key.trim() == "Exec").then_some(value.trim())
            })
            .collect();
        if exec != [expected.as_str()] {
            return Err(format!(
                "Unrecognized snap-pac hook command: {}",
                path.display()
            ));
        }
    }
    let digest = Command::new("/usr/bin/sha256sum")
        .arg(SNAP_PAC_SCRIPT)
        .output()
        .map_err(|error| format!("Unable to verify snap-pac: {error}"))?;
    if !digest.status.success()
        || String::from_utf8_lossy(&digest.stdout)
            .split_whitespace()
            .next()
            != Some(SNAP_PAC_SHA256)
    {
        return Err(
            "Installed snap-pac script differs from the audited SNAP_PAC_SKIP contract.".into(),
        );
    }
    let configs = fs::read_to_string("/etc/conf.d/snapper")
        .map_err(|error| format!("Unable to verify snap-pac configurations: {error}"))?;
    validate_configs(&configs)?;
    let ini = match fs::read_to_string("/etc/snap-pac.ini") {
        Ok(text) => text,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => String::new(),
        Err(error) => return Err(format!("Unable to verify snap-pac policy: {error}")),
    };
    validate_ini(&ini)
}

fn validate_incoming_plan(plan: &[UpdatePackageInfo]) -> Result<(), String> {
    if plan
        .iter()
        .any(|package| package.package_name == "snap-pac")
    {
        return Err("Updating snap-pac requires reviewing its incoming skip/hook contract before suppressing its snapshots.".into());
    }
    Ok(())
}

fn active_lines(text: &str) -> impl Iterator<Item = &str> {
    text.lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && !line.starts_with('#') && !line.starts_with(';'))
}

fn validate_configs(text: &str) -> Result<(), String> {
    let lines: Vec<_> = active_lines(text).collect();
    if lines.len() == 1 {
        if let Some((key, value)) = lines[0].split_once('=') {
            if key.trim() == "SNAPPER_CONFIGS"
                && ["\"root\"", "'root'", "root"].contains(&value.trim())
            {
                return Ok(());
            }
        }
    }
    Err("Only the audited root-only snap-pac configuration may be suppressed; additional recovery configurations require review.".into())
}

fn validate_ini(text: &str) -> Result<(), String> {
    // Deliberately conservative. Do not silently lose important/userdata,
    // alternate cleanup policies, other subvolumes or custom descriptions.
    let mut root_section = false;
    let mut snapshot_seen = false;
    for line in active_lines(text) {
        if line == "[root]" && !root_section {
            root_section = true;
            continue;
        }
        if let Some((key, value)) = line.split_once('=') {
            if root_section
                && !snapshot_seen
                && key.trim() == "snapshot"
                && value.trim().eq_ignore_ascii_case("true")
            {
                snapshot_seen = true;
                continue;
            }
        }
        return Err("Custom snap-pac policy requires explicit recovery-policy integration; no transaction was committed.".into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn only_default_root_chain_can_be_replaced() {
        assert!(validate_configs("# stock\nSNAPPER_CONFIGS=\"root\"\n").is_ok());
        for config in [
            "SNAPPER_CONFIGS=\"root home\"",
            "SNAPPER_CONFIGS=\"home\"",
            "SNAPPER_CONFIGS=$(echo root)",
            "SNAPPER_CONFIGS=root\nOTHER=yes",
        ] {
            assert!(validate_configs(config).is_err(), "{config}");
        }
        assert!(validate_ini("# defaults\n").is_ok());
        assert!(validate_ini("[root]\nsnapshot = True").is_ok());
        for ini in [
            "[home]\nsnapshot=True",
            "[root]\nuserdata=[\"important=yes\"]",
            "[root]\ncleanup_algorithm=timeline",
            "[root]\nsnapshot=false",
        ] {
            assert!(validate_ini(ini).is_err(), "{ini}");
        }
    }
    #[test]
    fn unaudited_incoming_snap_pac_is_not_silently_skipped() {
        let package = UpdatePackageInfo {
            package_name: "snap-pac".into(),
            current_version: "1".into(),
            new_version: "2".into(),
            repository: "core".into(),
            download_size: 1,
            filename: String::new(),
            sha256: String::new(),
            repository_metadata_sha256: String::new(),
        };
        assert!(validate_incoming_plan(&[package]).is_err());
        assert!(validate_incoming_plan(&[]).is_ok());
    }
}
