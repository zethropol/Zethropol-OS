// Normal makepkg artifacts use the existing broker's peer-bound plan and
// PolicyKit consume. No project signing/activation or manifest is consulted.
fn normal_aur_state(db: &Path) -> Result<String, String> {
    aur_database_state_fingerprint(db, &current_policy_fingerprint().map_err(|e| e.1)?)
}
fn normal_aur_official_barrier() -> Result<(), String> {
    let mut h = configured_alpm(None).map_err(|e| e.to_string())?;
    let result = prepare_alpm_transaction_with_policy(&mut h, current_policy_fingerprint);
    let _ = h.trans_release();
    let p = result.map_err(|e| format!("official preflight: {}: {}", e.0, e.1))?;
    if !p.packages().is_empty() || !p.removals().is_empty() {
        return Err(format!("official full upgrade required first ({} packages); AUR installation deferred; no partial upgrade or package changes",p.packages().len()));
    }
    Ok(())
}
fn normal_aur_readiness() -> HashMap<String, String> {
    let mut r = HashMap::from([
        ("api_version".into(), "2".into()),
        ("workflow".into(), "isolated-makepkg-alpm".into()),
        ("uid949".into(), "not-used".into()),
        ("manifest".into(), "not-required".into()),
        (
            "installation".into(),
            "requires-reviewed-artifacts-and-polkit".into(),
        ),
        ("state".into(), "review-required".into()),
    ]);
    let reason=match normal_aur_official_barrier(){Ok(())=>"Review the pinned PKGBUILD and sources, build unprivileged in the existing sandbox, preview immutable package FDs and authorize the exact plan with PolicyKit.".into(),Err(e)=>{r.insert("state".into(),"blocked".into());r.insert("installation".into(),"blocked".into());e}};
    r.insert("reason".into(), reason);
    r
}
fn normal_aur_selection(
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
        return Err("bounded unique package/version selection required".into());
    }
    let mut r = normal_aur_readiness();
    r.insert("selected_count".into(), names.len().to_string());
    Ok(r)
}
fn normal_aur_review(raw: &str) -> Result<ReviewedBuild, String> {
    normal_aur_official_barrier()?;
    if raw.len() > 3 * 1024 * 1024 {
        return Err("review quota exceeded".into());
    }
    let v: serde_json::Value = serde_json::from_str(raw).map_err(|e| e.to_string())?;
    let text = |k: &str| {
        v.get(k)
            .and_then(|v| v.as_str())
            .ok_or_else(|| format!("missing reviewed {k}"))
    };
    let name = text("name")?;
    let version = text("newVersion")?;
    let base = text("packageBase")?;
    let commit = text("commit")?;
    if !receipt_name(name)
        || !receipt_name(base)
        || !receipt_text(version)
        || commit.len() != 40
        || !commit.bytes().all(|b| b.is_ascii_hexdigit())
        || receipt_hash(text("pkgbuild")?.as_bytes()) != text("pkgbuild_sha256")?
        || receipt_hash(text("srcinfo")?.as_bytes()) != text("srcinfo_sha256")?
    {
        return Err("pinned recipe content/hash mismatch".into());
    }
    let mut fields: HashMap<&str, Vec<&str>> = HashMap::new();
    for row in text("srcinfo")?.lines() {
        let row = row.trim();
        if row.is_empty() || row.starts_with('#') {
            continue;
        }
        let (k, val) = row.split_once(" = ").ok_or("invalid .SRCINFO")?;
        fields.entry(k).or_default().push(val);
    }
    let first = |k: &str| fields.get(k).and_then(|v| v.first()).copied().unwrap_or("");
    let actual = format!(
        "{}{}-{}",
        if first("epoch").is_empty() || first("epoch") == "0" {
            String::new()
        } else {
            format!("{}:", first("epoch"))
        },
        first("pkgver"),
        first("pkgrel")
    );
    let arch = if fields.get("arch").is_some_and(|a| a.contains(&"any")) {
        "any"
    } else {
        std::env::consts::ARCH
    };
    if first("pkgbase") != base
        || !fields.get("pkgname").is_some_and(|n| n.contains(&name))
        || actual != version
        || !fields.get("arch").is_some_and(|a| a.contains(&arch))
    {
        return Err("reviewed package/version/architecture mismatch".into());
    }
    let sourcekeys = [
        "source".to_string(),
        format!("source_{}", std::env::consts::ARCH),
    ];
    let sources: Vec<_> = sourcekeys
        .iter()
        .flat_map(|k| fields.get(k.as_str()).into_iter().flatten())
        .collect();
    let strong = [("sha256sums", 64), ("sha512sums", 128), ("b2sums", 128)]
        .iter()
        .any(|(alg, len)| {
            let keys = [
                alg.to_string(),
                format!("{}_{}", alg, std::env::consts::ARCH),
            ];
            let sums: Vec<_> = keys
                .iter()
                .flat_map(|k| fields.get(k.as_str()).into_iter().flatten())
                .collect();
            !sources.is_empty()
                && sums.len() == sources.len()
                && sums
                    .iter()
                    .all(|s| s.len() == *len && s.bytes().all(|b| b.is_ascii_hexdigit()))
        });
    if !strong {
        return Err("complete strong source checksums required; SKIP is not accepted".into());
    }
    let paths = pacman_conf_values(&["DBPath"]).map_err(|e| e.to_string())?;
    if paths.len() != 1 {
        return Err("ambiguous DBPath".into());
    }
    let review = ReviewedBuild {
        source_package: base.into(),
        source_version: version.into(),
        architecture: std::env::consts::ARCH.into(),
        source_plan_sha256: receipt_hash(raw.as_bytes()),
        build_root_sha256: normal_aur_state(Path::new(&paths[0]))?,
        outputs: vec![ReviewedOutput {
            name: name.into(),
            version: version.into(),
            filename: format!(
                "{}-{}-{}.pkg.tar.zst",
                name,
                version.split_once(':').map(|x| x.1).unwrap_or(version),
                arch
            ),
        }],
    };
    review.validate()?;
    Ok(review)
}
async fn normal_reserve_aur_message(
    c: &zbus::Connection,
    h: &zbus::message::Header<'_>,
    review: ReviewedBuild,
) -> Result<ReceiptContext, BrokerError> {
    let now = aur_boottime().map_err(BrokerError::Io)?;
    let caller = AuthorizedCaller::check_message(c, h, PolicyAction::Preview, now).await?;
    persistent_aur_broker()?.reserve_aur_build(
        caller,
        review,
        aur_boottime().map_err(BrokerError::Io)?,
    )
}
fn normal_receive(
    expected: &CanonicalPlan,
    fds: &[zbus::zvariant::OwnedFd],
) -> Result<AurIntake, String> {
    use std::os::fd::AsFd;
    if fds.is_empty() || fds.len() != expected.packages().len() || fds.len() > 64 {
        return Err("exact bounded sealed archive set required".into());
    }
    let parent = Path::new("/run/zethropol-update/aur-artifacts");
    if !parent.exists() {
        std::os::unix::fs::DirBuilderExt::mode(&mut fs::DirBuilder::new(), 0o700)
            .create(parent)
            .map_err(|e| e.to_string())?;
    }
    verify_aur_staging_parent(parent)?;
    let directory = parent.join(format!("intake-{}", uuid::Uuid::new_v4()));
    std::os::unix::fs::DirBuilderExt::mode(&mut fs::DirBuilder::new(), 0o700)
        .create(&directory)
        .map_err(|e| e.to_string())?;
    let mut intake = AurIntake {
        directory,
        archives: vec![],
    };
    let mut total = 0u64;
    for (identity, fd) in expected.packages().iter().zip(fds) {
        let size = sealed_size(fd.as_fd(), 1024 * 1024 * 1024)?;
        total = total.checked_add(size).ok_or("size overflow")?;
        if total > 1024 * 1024 * 1024 || !receipt_basename(&identity.filename) {
            return Err("archive quota or basename rejected".into());
        }
        let path = intake.directory.join(&identity.filename);
        copy_sealed(fd.as_fd(), &path, size)?;
        verify_archive(&path, &identity.sha256)?;
        intake.archives.push(AurArchive {
            path,
            identity: identity.clone(),
        });
    }
    Ok(intake)
}
fn normal_locked<'a>(h: &'a mut Alpm, intake: &AurIntake) -> Result<LockedAur<'a>, String> {
    let db = PathBuf::from(h.dbpath());
    let result = prepare_alpm_transaction_with_policy(h, || {
        normal_aur_state(&db).map_err(|e| ("aur_state_unavailable".into(), e))
    });
    let official = match result {
        Ok(p) => p,
        Err(e) => {
            let _ = h.trans_release();
            return Err(format!("official preflight: {e:?}"));
        }
    };
    finish_aur_locked_transaction(
        h,
        &intake.archives,
        official,
        || normal_aur_state(&db),
        false,
    )
}
fn normal_aur_preview(
    context: &ReceiptContext,
    fds: &[zbus::zvariant::OwnedFd],
) -> Result<CanonicalPlan, String> {
    use std::os::fd::AsFd;
    normal_aur_official_barrier()?;
    if fds.len() != context.review.outputs.len() {
        return Err("reviewed output set differs".into());
    }
    let mut packages = vec![];
    for (output, fd) in context.review.outputs.iter().zip(fds) {
        let size = sealed_size(fd.as_fd(), 1024 * 1024 * 1024)?;
        let file = fs::File::from(fd.as_fd().try_clone_to_owned().map_err(|e| e.to_string())?);
        let mut hash = sha2::Sha256::new();
        let mut offset = 0;
        let mut buf = [0u8; 65536];
        while offset < size {
            let n = file
                .read_at(&mut buf[..(size - offset).min(65536) as usize], offset)
                .map_err(|e| e.to_string())?;
            if n == 0 {
                return Err("truncated package".into());
            }
            hash.update(&buf[..n]);
            offset += n as u64;
        }
        packages.push(UpdatePackageInfo {
            package_name: output.name.clone(),
            current_version: String::new(),
            new_version: output.version.clone(),
            repository: "aur-local".into(),
            download_size: size,
            filename: output.filename.clone(),
            sha256: format!("{:x}", hash.finalize()),
            repository_metadata_sha256: context.review.digest(),
        });
    }
    let expected = CanonicalPlan::from_packages(packages).map_err(|e| format!("{e:?}"))?;
    let intake = normal_receive(&expected, fds)?;
    let mut h = configured_alpm(None).map_err(|e| e.to_string())?;
    if normal_aur_state(Path::new(h.dbpath()))? != context.review.build_root_sha256 {
        return Err("build environment/package policy changed since review".into());
    }
    let locked = normal_locked(&mut h, &intake)?;
    Ok(locked.plan.clone())
}
fn normal_aur_execute(
    store: &BrokerStore,
    consumed: &crate::broker_protocol::ConsumedPreview,
    fds: Vec<zbus::zvariant::OwnedFd>,
) -> Result<(), String> {
    store
        .require_no_uncertain_commit()
        .map_err(|e| format!("unresolved earlier package outcome: {e:?}"))?;
    store
        .require_queued(&consumed.operation_id)
        .map_err(|e| format!("operation not queued: {e:?}"))?;
    let _lease = store
        .worker_lease(&consumed.operation_id)
        .map_err(|e| format!("worker lease: {e:?}"))?;
    let result = (|| {
        normal_aur_official_barrier()?;
        let intake = normal_receive(&consumed.plan, &fds)?;
        let mut h = configured_alpm(None).map_err(|e| e.to_string())?;
        let locked = normal_locked(&mut h, &intake)?;
        if locked.plan != consumed.plan {
            return Err("approved artifacts/native plan changed; no installation".into());
        }
        aur_begin_after_real_snapshot(&locked, store, &consumed.operation_id)?;
        if normal_aur_state(Path::new(locked.handle.dbpath()))? != locked.plan.policy_fingerprint()
        {
            return Err("package policy changed before commit".into());
        }
        aur_commit_started_with_post(locked, store, &consumed.operation_id, create_post_snapshot)
    })();
    if let Err(ref e) = result {
        store
            .finish_worker_result(
                &consumed.operation_id,
                OperationStatus::Failed,
                e,
                aur_boottime()?,
            )
            .map_err(|e| format!("persist failure: {e:?}"))?;
    }
    result
}
