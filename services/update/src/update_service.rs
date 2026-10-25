use crate::models::{OperationResult, UpdatePackageInfo, UpdateState};
use std::collections::HashMap;
use std::process::Command;
use std::sync::{Arc, Mutex};
use zbus::interface;

#[derive(Debug, Clone)]
pub struct OpState {
    pub status: String, // "pending", "running", "ok", "failed"
    pub message: String,
}

#[derive(Debug)]
pub struct UpdateManager {
    operations: Arc<Mutex<HashMap<String, OpState>>>,
}

impl UpdateManager {
    pub fn new() -> Self {
        Self {
            operations: Arc::new(Mutex::new(HashMap::new())),
        }
    }

    fn check_pacman_updates(&self) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
        let output = Command::new("/usr/bin/checkupdates")
            .output()
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!("Unable to start checkupdates: {error}"))
            })?;

        let stdout = String::from_utf8_lossy(&output.stdout);
        let stderr = String::from_utf8_lossy(&output.stderr);
        parse_checkupdates_result(
            output.status.success(),
            output.status.code(),
            &stdout,
            &stderr,
        )
    }
}

#[interface(name = "org.zethropol.Update.v1")]
impl UpdateManager {
    async fn get_update_state(&self) -> Result<UpdateState, zbus::fdo::Error> {
        let pending = self.check_pacman_updates()?;
        let count = pending.len() as u32;

        Ok(UpdateState {
            is_checking: false,
            is_updating: false,
            last_check_time: "Just now".to_string(),
            pending_updates_count: count,
        })
    }

    async fn list_pending_updates(&self) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
        self.check_pacman_updates()
    }

    fn check_installed_dependency_integrity(&self) -> Result<(), zbus::fdo::Error> {
        let output = Command::new("/usr/bin/pacman")
            .args(["-Dk"])
            .output()
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!(
                    "Unable to check installed dependency integrity: {error}"
                ))
            })?;

        if !output.status.success() {
            return Err(zbus::fdo::Error::Failed(format!(
                "Installed dependency integrity check failed: {}",
                String::from_utf8_lossy(&output.stderr).trim()
            )));
        }

        Ok(())
    }

    async fn preview_transaction(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
        authorize_preview(&header)?;
        self.check_installed_dependency_integrity()?;
        let alpm_plan = alpm_transaction_preflight()?;

        let output = Command::new("/usr/bin/pacman")
            .env("LC_ALL", "C")
            .args(["-Sup", "--print-format", "%n %v %r"])
            .output()
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!("Unable to start transaction preview: {error}"))
            })?;

        if !output.status.success() {
            return Err(zbus::fdo::Error::Failed(format!(
                "Transaction preview failed: {}",
                String::from_utf8_lossy(&output.stderr).trim()
            )));
        }

        if !output.stderr.is_empty() {
            return Err(zbus::fdo::Error::Failed(format!(
                "Transaction preview emitted warnings; refusing to present an unverified plan: {}",
                String::from_utf8_lossy(&output.stderr).trim()
            )));
        }

        let installed_output = Command::new("/usr/bin/pacman")
            .env("LC_ALL", "C")
            .args(["-Q"])
            .output()
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!("Unable to query installed packages: {error}"))
            })?;

        if !installed_output.status.success() {
            return Err(zbus::fdo::Error::Failed(format!(
                "Installed package query failed: {}",
                String::from_utf8_lossy(&installed_output.stderr).trim()
            )));
        }

        let installed: HashMap<String, String> = String::from_utf8_lossy(&installed_output.stdout)
            .lines()
            .map(|line| {
                let mut parts = line.split_whitespace();
                let (Some(name), Some(version)) = (parts.next(), parts.next()) else {
                    return Err(zbus::fdo::Error::Failed(format!(
                        "Malformed installed package entry: {line}"
                    )));
                };
                if parts.next().is_some() || name.trim().is_empty() || version.trim().is_empty() {
                    return Err(zbus::fdo::Error::Failed(format!(
                        "Malformed installed package entry: {line}"
                    )));
                }
                Ok((name.to_string(), version.to_string()))
            })
            .try_fold(HashMap::new(), |mut installed, entry| {
                let (name, version) = entry?;
                if installed.insert(name.clone(), version).is_some() {
                    return Err(zbus::fdo::Error::Failed(format!(
                        "Duplicate installed package entry: {name}"
                    )));
                }
                Ok(installed)
            })?;

        let stdout = String::from_utf8_lossy(&output.stdout);
        let mut packages = Vec::new();

        for line in stdout.lines() {
            let mut parts = line.split_whitespace();
            let (Some(name), Some(new_version), Some(repository)) =
                (parts.next(), parts.next(), parts.next())
            else {
                return Err(zbus::fdo::Error::Failed(format!(
                    "Malformed transaction preview line: {line}"
                )));
            };

            if parts.next().is_some() {
                return Err(zbus::fdo::Error::Failed(format!(
                    "Malformed transaction preview line: {line}"
                )));
            }

            let current_version = installed
                .get(name)
                .cloned()
                .unwrap_or_else(|| "not installed (new dependency)".to_string());

            if current_version != "not installed (new dependency)" {
                let comparison = Command::new("/usr/bin/vercmp")
                    .args([new_version, current_version.as_str()])
                    .output()
                    .ok()
                    .filter(|output| output.status.success())
                    .and_then(|output| String::from_utf8(output.stdout).ok())
                    .and_then(|output| output.trim().parse::<i32>().ok())
                    .filter(|value| [-1, 0, 1].contains(value))
                    .ok_or_else(|| {
                        zbus::fdo::Error::Failed(format!(
                            "Cannot verify package version ordering for {name}."
                        ))
                    })?;
                if comparison < 0 {
                    return Err(zbus::fdo::Error::Failed(format!(
                        "Transaction preview rejected: package {name} would be downgraded from {current_version} to {new_version}."
                    )));
                }
            }

            let download_size = query_pacman_download_size(repository, name, new_version)
                .ok_or_else(|| zbus::fdo::Error::Failed(format!(
                    "Unable to verify download size for {repository}/{name} version {new_version}; transaction preview aborted."
                )))?;

            packages.push(UpdatePackageInfo {
                package_name: name.to_string(),
                current_version,
                new_version: new_version.to_string(),
                repository: repository.to_string(),
                download_size,
            });
        }

        let critical_packages = [
            "linux",
            "linux-lts",
            "linux-cachyos",
            "linux-firmware",
            "mkinitcpio",
            "dracut",
            "limine",
            "grub",
            "systemd",
            "filesystem",
            "glibc",
            "pacman",
            "efibootmgr",
            "btrfs-progs",
            "snapper",
            "grub-btrfs",
            "intel-ucode",
            "amd-ucode",
            "cryptsetup",
            "lvm2",
        ];
        if let Some(package) = packages.iter().find(|package| {
            critical_packages.contains(&package.package_name.as_str())
                || package.package_name.starts_with("linux-")
                || package.package_name.starts_with("grub-")
                || package.package_name.starts_with("mkinitcpio-")
                || package.package_name.starts_with("systemd-")
                || package.package_name.starts_with("limine-")
                || package.package_name.starts_with("dracut-")
                || package.package_name.starts_with("efibootmgr-")
                || package.package_name.starts_with("btrfs-progs-")
                || package.package_name.starts_with("snapper-")
                || package.package_name.starts_with("cryptsetup-")
                || package.package_name.starts_with("lvm2-")
                || package.package_name.starts_with("amd-ucode-")
                || package.package_name.starts_with("intel-ucode-")
        }) {
            return Err(zbus::fdo::Error::Failed(format!(
                "Critical system package {} is in the transaction. Preview rejected until boot recovery and compatibility preflight are implemented.",
                package.package_name
            )));
        }

        let mut seen = std::collections::HashSet::new();
        for package in &packages {
            if package.package_name.trim().is_empty()
                || package.new_version.trim().is_empty()
                || package.repository.trim().is_empty()
                || package.package_name.chars().any(char::is_whitespace)
                || package.new_version.chars().any(char::is_whitespace)
                || package.repository.chars().any(char::is_whitespace)
                || package.package_name.contains("/")
                || package.repository.contains("/")
            {
                return Err(zbus::fdo::Error::Failed(
                    "Transaction preview contains an incomplete package identity.".to_string(),
                ));
            }
            if !seen.insert(package.package_name.as_str()) {
                return Err(zbus::fdo::Error::Failed(format!(
                    "Transaction preview contains duplicate package: {}",
                    package.package_name
                )));
            }
        }

        let preview_plan: std::collections::HashSet<(String, String, String)> = packages
            .iter()
            .map(|package| {
                (
                    package.package_name.clone(),
                    package.new_version.clone(),
                    package.repository.clone(),
                )
            })
            .collect();

        if preview_plan.len() != packages.len() {
            return Err(zbus::fdo::Error::Failed(
                "Transaction preview contains duplicate package identities.".to_string(),
            ));
        }

        let verified_plan: std::collections::HashSet<(String, String, String)> =
            alpm_plan.iter().cloned().collect();

        if verified_plan.len() != alpm_plan.len() || preview_plan != verified_plan {
            return Err(zbus::fdo::Error::Failed(
                "The pacman preview and libalpm transaction plan disagree; refusing to present an unverified plan.".to_string(),
            ));
        }

        Ok(packages)
    }

    async fn get_operation_state(
        &self,
        operation_id: String,
    ) -> Result<HashMap<String, String>, zbus::fdo::Error> {
        let ops = self.operations.lock().unwrap();
        let mut res = HashMap::new();
        if let Some(state) = ops.get(&operation_id) {
            res.insert("status".to_string(), state.status.clone());
            res.insert("message".to_string(), state.message.clone());
        } else {
            res.insert("status".to_string(), "not-found".to_string());
            res.insert(
                "message".to_string(),
                "Operation ID does not exist.".to_string(),
            );
        }
        Ok(res)
    }

    async fn trigger_update(
        &self,
        #[zbus(header)] _header: zbus::message::Header<'_>,
    ) -> Result<OperationResult, zbus::fdo::Error> {
        authorize_update(&_header)?;
        Ok(OperationResult {
            success: false,
            code: "preflight_required".to_string(),
            message: "System updates are disabled until transaction planning, dependency/conflict validation, and authorization are implemented.".to_string(),
            operation_id: String::new(),
            data: HashMap::new(),
        })
    }

    #[zbus(signal)]
    async fn update_status_changed(
        ctxt: &zbus::object_server::SignalEmitter<'_>,
        new_state: UpdateState,
    ) -> zbus::Result<()>;
}
fn authorize_preview(header: &zbus::message::Header<'_>) -> Result<(), zbus::fdo::Error> {
    let sender = header
        .sender()
        .ok_or_else(|| zbus::fdo::Error::Failed("invalid caller".to_string()))?;
    let status = Command::new("pkcheck")
        .args([
            "--action-id",
            "org.zethropol.update.preview",
            "--system-bus-name",
            sender.as_str(),
            "--allow-user-interaction",
        ])
        .status()
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!("Unable to start PolicyKit authorization: {error}"))
        })?;
    if !status.success() {
        return Err(zbus::fdo::Error::AccessDenied(
            "Transaction preview permission denied".to_string(),
        ));
    }
    Ok(())
}

fn authorize_update(header: &zbus::message::Header<'_>) -> Result<(), zbus::fdo::Error> {
    let sender = header
        .sender()
        .ok_or_else(|| zbus::fdo::Error::Failed("invalid caller".to_string()))?;
    let status = Command::new("pkcheck")
        .args([
            "--action-id",
            "org.zethropol.update.install",
            "--system-bus-name",
            sender.as_str(),
            "--allow-user-interaction",
        ])
        .status()
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!("Unable to start PolicyKit authorization: {error}"))
        })?;
    if !status.success() {
        return Err(zbus::fdo::Error::AccessDenied(
            "Authentication required or permission denied".to_string(),
        ));
    }
    Ok(())
}

fn parse_pacman_download_size(
    output: &str,
    expected_repository: &str,
    expected_name: &str,
    expected_version: &str,
) -> Option<u64> {
    let mut repository = None;
    let mut name = None;
    let mut version = None;
    let mut size = None;

    for line in output.lines() {
        let Some((key, value)) = line.split_once(":") else {
            continue;
        };
        let value = value.trim();
        match key.trim() {
            "Repository" => repository = Some(value),
            "Name" => name = Some(value),
            "Version" => version = Some(value),
            "Download Size" => size = Some(value),
            _ => {}
        }
    }

    if repository? != expected_repository || name? != expected_name || version? != expected_version
    {
        return None;
    }

    let raw = size?;
    let mut fields = raw.split_whitespace();
    let amount: f64 = fields.next()?.parse().ok()?;
    let unit = fields.next()?;
    if fields.next().is_some() || !amount.is_finite() || amount < 0.0 {
        return None;
    }

    let multiplier = match unit {
        "B" => 1.0,
        "KiB" => 1024.0,
        "MiB" => 1024.0_f64.powi(2),
        "GiB" => 1024.0_f64.powi(3),
        "TiB" => 1024.0_f64.powi(4),
        _ => return None,
    };

    let bytes = amount * multiplier;
    if bytes > u64::MAX as f64 {
        return None;
    }
    Some(bytes.round() as u64)
}

fn query_pacman_download_size(repository: &str, name: &str, version: &str) -> Option<u64> {
    let target = format!("{repository}/{name}");
    let output = Command::new("/usr/bin/pacman")
        .env("LC_ALL", "C")
        .args(["-Si", &target])
        .output()
        .ok()?;

    if !output.status.success() {
        return None;
    }

    let text = String::from_utf8_lossy(&output.stdout);
    parse_pacman_download_size(&text, repository, name, version)
}

fn pacman_conf_values(args: &[&str]) -> Result<Vec<String>, zbus::fdo::Error> {
    let output = Command::new("/usr/bin/pacman-conf")
        .args(args)
        .output()
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!("Unable to read pacman configuration: {error}"))
        })?;

    if !output.status.success() || !output.stderr.is_empty() {
        return Err(zbus::fdo::Error::Failed(format!(
            "Unable to verify pacman configuration for {:?}: {}",
            args,
            String::from_utf8_lossy(&output.stderr).trim()
        )));
    }

    Ok(String::from_utf8_lossy(&output.stdout)
        .lines()
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_string)
        .collect())
}

fn parse_pacman_siglevel(
    values: &[String],
    fallback: alpm::SigLevel,
) -> Result<alpm::SigLevel, zbus::fdo::Error> {
    use alpm::SigLevel;

    if values.is_empty() {
        return Ok(fallback);
    }

    let mut flags = SigLevel::NONE;
    for value in values {
        match value.as_str() {
            "PackageRequired" => {
                flags.remove(SigLevel::PACKAGE_OPTIONAL);
                flags.insert(SigLevel::PACKAGE);
            }
            "PackageOptional" => {
                flags.insert(SigLevel::PACKAGE | SigLevel::PACKAGE_OPTIONAL);
            }
            "PackageMarginalOK" => flags.insert(SigLevel::PACKAGE_MARGINAL_OK),
            "PackageUnknownOK" => flags.insert(SigLevel::PACKAGE_UNKNOWN_OK),
            "DatabaseRequired" => {
                flags.remove(SigLevel::DATABASE_OPTIONAL);
                flags.insert(SigLevel::DATABASE);
            }
            "DatabaseOptional" => {
                flags.insert(SigLevel::DATABASE | SigLevel::DATABASE_OPTIONAL);
            }
            "DatabaseMarginalOK" => flags.insert(SigLevel::DATABASE_MARGINAL_OK),
            "DatabaseUnknownOK" => flags.insert(SigLevel::DATABASE_UNKNOWN_OK),
            "PackageTrustedOnly" => {
                flags.remove(SigLevel::PACKAGE_MARGINAL_OK | SigLevel::PACKAGE_UNKNOWN_OK);
            }
            "DatabaseTrustedOnly" => {
                flags.remove(SigLevel::DATABASE_MARGINAL_OK | SigLevel::DATABASE_UNKNOWN_OK);
            }
            other => {
                return Err(zbus::fdo::Error::Failed(format!(
                    "Unsupported pacman SigLevel value: {other}"
                )))
            }
        }
    }
    Ok(flags)
}

fn alpm_transaction_preflight() -> Result<Vec<(String, String, String)>, zbus::fdo::Error> {
    use alpm::{Alpm, SigLevel, TransFlag};

    let root_dirs = pacman_conf_values(&["RootDir"])?;
    let db_paths = pacman_conf_values(&["DBPath"])?;
    let gpg_dirs = pacman_conf_values(&["GPGDir"])?;
    let architectures = pacman_conf_values(&["Architecture"])?;
    let cache_dirs = pacman_conf_values(&["CacheDir"])?;
    let repositories = pacman_conf_values(&["--repo-list"])?;

    if root_dirs.len() != 1
        || root_dirs[0] != "/"
        || db_paths.len() != 1
        || gpg_dirs.len() != 1
        || architectures.is_empty()
        || cache_dirs.is_empty()
        || repositories.is_empty()
    {
        return Err(zbus::fdo::Error::Failed(
            "Pacman configuration is incomplete; transaction preview refused.".to_string(),
        ));
    }

    let mut handle = Alpm::new(root_dirs[0].clone(), db_paths[0].clone()).map_err(|error| {
        zbus::fdo::Error::Failed(format!("Unable to initialize libalpm: {error}"))
    })?;

    let architecture_refs: Vec<&str> = architectures.iter().map(String::as_str).collect();
    handle
        .set_architectures(architecture_refs.into_iter())
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!(
                "Unable to configure libalpm architectures: {error}"
            ))
        })?;

    let cache_refs: Vec<&str> = cache_dirs.iter().map(String::as_str).collect();
    handle
        .set_cachedirs(cache_refs.into_iter())
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!(
                "Unable to configure libalpm cache directories: {error}"
            ))
        })?;

    handle.set_gpgdir(gpg_dirs[0].clone()).map_err(|error| {
        zbus::fdo::Error::Failed(format!(
            "Unable to configure libalpm GPG directory: {error}"
        ))
    })?;

    let default_siglevel = parse_pacman_siglevel(
        &pacman_conf_values(&["SigLevel"])?,
        SigLevel::PACKAGE | SigLevel::DATABASE_OPTIONAL,
    )?;
    handle
        .set_default_siglevel(default_siglevel)
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!(
                "Unable to configure libalpm signature policy: {error}"
            ))
        })?;

    let local_siglevel = parse_pacman_siglevel(
        &pacman_conf_values(&["LocalFileSigLevel"])?,
        default_siglevel,
    )?;
    handle
        .set_local_file_siglevel(local_siglevel)
        .map_err(|error| {
            zbus::fdo::Error::Failed(format!(
                "Unable to configure local-file signature policy: {error}"
            ))
        })?;

    let remote_values = pacman_conf_values(&["RemoteFileSigLevel"])?;
    if !remote_values.is_empty() {
        let remote_siglevel = parse_pacman_siglevel(&remote_values, default_siglevel)?;
        handle
            .set_remote_file_siglevel(remote_siglevel)
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!(
                    "Unable to configure remote-file signature policy: {error}"
                ))
            })?;
    }
    let hook_dirs = pacman_conf_values(&["HookDir"])?;
    if !hook_dirs.is_empty() {
        handle
            .set_hookdirs(hook_dirs.iter().map(String::as_str))
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!(
                    "Unable to configure libalpm hook directories: {error}"
                ))
            })?;
    }

    let no_upgrade = pacman_conf_values(&["NoUpgrade"])?;
    if !no_upgrade.is_empty() {
        handle
            .set_noupgrades(no_upgrade.iter().map(String::as_str))
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!("Unable to configure NoUpgrade rules: {error}"))
            })?;
    }

    let no_extract = pacman_conf_values(&["NoExtract"])?;
    if !no_extract.is_empty() {
        handle
            .set_noextracts(no_extract.iter().map(String::as_str))
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!("Unable to configure NoExtract rules: {error}"))
            })?;
    }

    let check_space = pacman_conf_values(&["CheckSpace"])?;
    handle.set_check_space(check_space.iter().any(|value| {
        value.eq_ignore_ascii_case("true") || value.eq_ignore_ascii_case("yes") || value == "1"
    }));

    let parallel_downloads = pacman_conf_values(&["ParallelDownloads"])?;
    if !parallel_downloads.is_empty() {
        let count = parallel_downloads[0].parse::<u32>().map_err(|_| {
            zbus::fdo::Error::Failed(
                "Invalid ParallelDownloads value in pacman configuration.".to_string(),
            )
        })?;
        handle.set_parallel_downloads(count);
    }

    let log_files = pacman_conf_values(&["LogFile"])?;
    if log_files.len() == 1 {
        handle.set_logfile(log_files[0].clone()).map_err(|error| {
            zbus::fdo::Error::Failed(format!("Unable to configure libalpm logfile: {error}"))
        })?;
    } else if log_files.len() > 1 {
        return Err(zbus::fdo::Error::Failed(
            "Multiple LogFile values found; transaction preview refused.".to_string(),
        ));
    }

    let ignored_packages = pacman_conf_values(&["IgnorePkg"])?;
    if !ignored_packages.is_empty() {
        let refs: Vec<&str> = ignored_packages.iter().map(String::as_str).collect();
        handle.set_ignorepkgs(refs.into_iter()).map_err(|error| {
            zbus::fdo::Error::Failed(format!("Unable to configure ignored packages: {error}"))
        })?;
    }

    let ignored_groups = pacman_conf_values(&["IgnoreGroup"])?;
    if !ignored_groups.is_empty() {
        let refs: Vec<&str> = ignored_groups.iter().map(String::as_str).collect();
        handle.set_ignoregroups(refs.into_iter()).map_err(|error| {
            zbus::fdo::Error::Failed(format!("Unable to configure ignored groups: {error}"))
        })?;
    }

    for repository in repositories {
        let servers = pacman_conf_values(&["--repo", repository.as_str(), "Server"])?;
        if servers.is_empty() {
            return Err(zbus::fdo::Error::Failed(format!(
                "Repository {repository} has no verified server configuration."
            )));
        }

        let repo_siglevel = parse_pacman_siglevel(
            &pacman_conf_values(&["--repo", repository.as_str(), "SigLevel"])?,
            default_siglevel,
        )?;
        let usage_values = pacman_conf_values(&["--repo", repository.as_str(), "Usage"])?;
        let usage = if usage_values.is_empty() {
            alpm::Usage::ALL
        } else {
            let mut usage = alpm::Usage::NONE;
            for value in usage_values
                .iter()
                .flat_map(|value| value.split_whitespace())
            {
                match value {
                    "All" => usage = alpm::Usage::ALL,
                    "Sync" => usage.insert(alpm::Usage::SYNC),
                    "Search" => usage.insert(alpm::Usage::SEARCH),
                    "Install" => usage.insert(alpm::Usage::INSTALL),
                    "Upgrade" => usage.insert(alpm::Usage::UPGRADE),
                    other => {
                        return Err(zbus::fdo::Error::Failed(format!(
                            "Unsupported Usage value for repository {repository}: {other}"
                        )))
                    }
                }
            }
            usage
        };

        let db = handle
            .register_syncdb_mut(repository.clone(), repo_siglevel)
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!(
                    "Unable to register repository {repository}: {error}"
                ))
            })?;
        db.set_usage(usage).map_err(|error| {
            zbus::fdo::Error::Failed(format!(
                "Unable to configure Usage for repository {repository}: {error}"
            ))
        })?;
        for server in servers {
            db.add_server(server).map_err(|error| {
                zbus::fdo::Error::Failed(format!(
                    "Unable to configure repository {repository}: {error}"
                ))
            })?;
        }
    }

    handle.trans_init(TransFlag::NONE).map_err(|error| {
        zbus::fdo::Error::Failed(format!(
            "Unable to initialize dependency preflight: {error}"
        ))
    })?;

    let prepare_result = handle
        .sync_sysupgrade(false)
        .map_err(|error| format!("unable to plan system upgrade: {error}"))
        .and_then(|_| {
            handle
                .trans_prepare()
                .map_err(|error| format!("transaction preparation details: {error:?}"))
        })
        .and_then(|_| {
            let removals: Vec<String> = handle
                .trans_remove()
                .iter()
                .map(|package| package.name().to_string())
                .collect();

            if !removals.is_empty() {
                return Err(format!(
                    "The proposed upgrade removes packages ({}); preview refused for safety.",
                    removals.join(", ")
                ));
            }

            let mut planned = Vec::new();
            for package in handle.trans_add().iter() {
                if package.origin() != alpm::PackageFrom::SyncDb {
                    return Err(format!(
                        "Unexpected package origin for {}; transaction preview refused.",
                        package.name()
                    ));
                }

                let repository = package.db().ok_or_else(|| {
                    format!(
                        "No repository identity is available for {}; transaction preview refused.",
                        package.name()
                    )
                })?;

                planned.push((
                    package.name().to_string(),
                    package.version().as_str().to_string(),
                    repository.name().to_string(),
                ));
            }

            Ok(planned)
        });

    let release_result = handle.trans_release();

    let alpm_plan = prepare_result.map_err(|detail| {
        zbus::fdo::Error::Failed(format!("libalpm rejected the update transaction: {detail}"))
    })?;

    release_result.map_err(|error| {
        zbus::fdo::Error::Failed(format!(
            "Unable to release libalpm preflight transaction: {error}"
        ))
    })?;

    Ok(alpm_plan)
}

fn parse_checkupdates_result(
    success: bool,
    exit_code: Option<i32>,
    stdout: &str,
    stderr: &str,
) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
    if !success {
        if exit_code == Some(2) && stdout.trim().is_empty() {
            return Ok(Vec::new());
        }

        let detail = if stderr.trim().is_empty() {
            format!(
                "exit status: {}",
                exit_code
                    .map(|code| code.to_string())
                    .unwrap_or_else(|| "unknown".to_string())
            )
        } else {
            stderr.trim().to_string()
        };

        return Err(zbus::fdo::Error::Failed(format!(
            "Update check failed: {detail}"
        )));
    }

    parse_checkupdates_output(stdout)
}

fn parse_checkupdates_output(stdout: &str) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
    let mut updates = Vec::new();
    let mut seen = std::collections::HashSet::new();

    for line in stdout.lines() {
        if line.trim().is_empty() {
            continue;
        }

        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() != 4 || parts[2] != "->" {
            return Err(zbus::fdo::Error::Failed(format!(
                "Malformed checkupdates output; refusing incomplete update data: {line}"
            )));
        }

        let (name, current_version, new_version) = (parts[0], parts[1], parts[3]);

        if name.contains("/") {
            return Err(zbus::fdo::Error::Failed(format!(
                "Invalid package name in checkupdates output: {name}"
            )));
        }

        if !seen.insert(name.to_string()) {
            return Err(zbus::fdo::Error::Failed(format!(
                "Duplicate package in checkupdates output: {name}"
            )));
        }

        updates.push(UpdatePackageInfo {
            package_name: name.to_string(),
            current_version: current_version.to_string(),
            new_version: new_version.to_string(),
            repository: "pacman".to_string(),
            download_size: 0,
        });
    }

    Ok(updates)
}

#[test]
fn parses_download_size_with_exact_package_identity() {
    let output = "Repository      : cachyos-extra-v3\nName            : 7zip\nVersion         : 26.04-1.1\nDownload Size   : 2.02 MiB\n";
    assert_eq!(
        parse_pacman_download_size(output, "cachyos-extra-v3", "7zip", "26.04-1.1"),
        Some(2_118_124)
    );
}

#[test]
fn rejects_download_size_when_repository_or_version_mismatches() {
    let output = "Repository : extra\nName : 7zip\nVersion : 26.04-1\nDownload Size : 2.02 MiB\n";
    assert_eq!(
        parse_pacman_download_size(output, "cachyos-extra-v3", "7zip", "26.04-1.1"),
        None
    );
}

#[test]
fn rejects_missing_or_unrecognized_download_size() {
    let missing = "Repository : extra\nName : demo\nVersion : 1.0\n";
    let unknown_unit = "Repository : extra\nName : demo\nVersion : 1.0\nDownload Size : 2 MB\n";
    assert_eq!(
        parse_pacman_download_size(missing, "extra", "demo", "1.0"),
        None
    );
    assert_eq!(
        parse_pacman_download_size(unknown_unit, "extra", "demo", "1.0"),
        None
    );
}

#[cfg(test)]
mod tests {
    use super::{parse_checkupdates_output, parse_checkupdates_result};

    #[test]
    fn parses_valid_update_lines() {
        let result = parse_checkupdates_output(
            "alpha 1.0 -> 1.1
beta 2.4-1 -> 3.0-1
",
        )
        .unwrap();

        assert_eq!(result.len(), 2);
        assert_eq!(result[0].package_name, "alpha");
        assert_eq!(result[0].current_version, "1.0");
        assert_eq!(result[0].new_version, "1.1");
        assert_eq!(result[0].repository, "pacman");
        assert_eq!(result[1].package_name, "beta");
        assert_eq!(result[1].new_version, "3.0-1");
    }

    #[test]
    fn rejects_malformed_and_unrelated_lines() {
        let result = parse_checkupdates_output(
            "not an update
alpha 1.0 1.1
short ->
",
        );
        assert!(result.is_err());
    }

    #[test]
    fn accepts_empty_output() {
        assert!(parse_checkupdates_output("").unwrap().is_empty());
        assert!(parse_checkupdates_output("\n\t\n").unwrap().is_empty());
    }

    #[test]
    fn rejects_duplicate_package_names() {
        let result = parse_checkupdates_output(
            "alpha 1.0 -> 1.1
alpha 1.1 -> 1.2
",
        );
        assert!(result.is_err());
    }

    #[test]
    fn treats_exit_code_two_with_empty_output_as_no_updates() {
        let result = parse_checkupdates_result(false, Some(2), "", "").unwrap();
        assert!(result.is_empty());
    }

    #[test]
    fn preserves_real_checkupdates_errors() {
        let result = parse_checkupdates_result(false, Some(1), "", "Cannot fetch updates");
        assert!(result.is_err());
    }
}
