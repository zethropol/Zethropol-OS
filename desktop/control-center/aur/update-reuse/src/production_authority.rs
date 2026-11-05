// Eligibility is not an install grant: the existing caller-bound consume,
// receipt/native revalidation, real snapshot and commit checkpoint remain mandatory.
fn require_independent_roles(
    manifest: &[String],
    archive: &[String],
    receipt: &[String],
) -> Result<(), String> {
    if manifest.is_empty() || archive.is_empty() || receipt.is_empty() {
        return Err("all production trust roles required".into());
    }
    if receipt
        .iter()
        .any(|key| archive.contains(key) || manifest.contains(key))
    {
        return Err(
            "receipt authority must be independent of developer/manifest signing authority".into(),
        );
    }
    Ok(())
}
#[derive(serde::Deserialize)]
struct AurRuntimeMount {
    target: String,
    #[serde(rename = "maj:min")]
    device: String,
    fstype: String,
    fsroot: String,
}
fn journal_outside_rollback(root: &AurRuntimeMount, journal: &AurRuntimeMount) -> bool {
    if !matches!(journal.fstype.as_str(), "ext4" | "btrfs" | "xfs") || journal.target == root.target
    {
        return false;
    }
    if journal.device.starts_with("7:") {
        return false;
    } // No file-backed loop journal masquerading as durable storage.
    if root.device != journal.device {
        return true;
    }
    root.fstype == "btrfs"
        && journal.fstype == "btrfs"
        && root.fsroot != "/"
        && journal.fsroot != root.fsroot
        && !journal
            .fsroot
            .starts_with(&format!("{}/", root.fsroot.trim_end_matches('/')))
}
fn require_journal_isolation() -> Result<(), String> {
    fn mount(path: &str) -> Result<AurRuntimeMount, String> {
        #[derive(serde::Deserialize)]
        struct Mounts {
            filesystems: Vec<AurRuntimeMount>,
        }
        let result = std::process::Command::new("/usr/bin/findmnt")
            .args([
                "--json",
                "--first-only",
                "--target",
                path,
                "--output",
                "TARGET,FSTYPE,FSROOT,MAJ:MIN",
            ])
            .env_clear()
            .env("PATH", "/usr/bin")
            .env("LC_ALL", "C")
            .output()
            .map_err(|e| e.to_string())?;
        if !result.status.success() || result.stdout.len() > 65536 {
            return Err("persistent mount evidence unavailable".into());
        }
        let mut data: Mounts = serde_json::from_slice(&result.stdout).map_err(|e| e.to_string())?;
        if data.filesystems.len() != 1 {
            return Err("ambiguous persistent mount evidence".into());
        }
        Ok(data.filesystems.remove(0))
    }
    if !journal_outside_rollback(&mount("/")?, &mount("/var/lib/zethropol/aur-transactions")?) {
        return Err("persistent transaction journal must use durable storage outside root snapshot rollback".into());
    }
    Ok(())
}
fn require_aur_production_authority() -> Result<(), String> {
    if unsafe { libc::geteuid() } != 0 {
        return Err("AUR installation closed: root installation broker required".into());
    }
    let trust =
        ReceiptTrust::load().map_err(|e| format!("production public trust unavailable: {e}"))?;
    require_independent_roles(
        &trust.manifest.signers,
        &trust.archive.signers,
        &trust.receipt.signers,
    )?;
    verify_aur_staging_parent(Path::new("/run/zethropol-update/aur-artifacts"))?;
    require_journal_isolation()?;
    persistent_aur_broker()
        .map_err(|e| format!("persistent AUR state: {e:?}"))?
        .require_no_uncertain_commit()
        .map_err(|e| format!("unresolved AUR commit: {e:?}"))?;
    BrokerStore::open_runtime()
        .map_err(|e| format!("official broker state: {e:?}"))?
        .require_no_uncertain_commit()
        .map_err(|e| format!("unresolved official commit: {e:?}"))?;
    require_aur_runtime_acceptance()
}
// Public trust and durable storage are prerequisites, not evidence that the
// real PolicyKit/native-domain/snapshot/recovery acceptance has succeeded.
// Keep the candidate closed until that acceptance is implemented and reviewed;
// no environment variable, fixture grant or public policy may open this gate.
fn require_aur_runtime_acceptance() -> Result<(), String> {
    Err("AUR installation closed: required PolicyKit/native-domain/snapshot/boot/recovery security acceptance incomplete".into())
}
#[cfg(test)]
mod production_authority_tests {
    use super::*;
    #[test]
    fn public_prerequisites_cannot_replace_runtime_security_acceptance() {
        assert!(require_aur_runtime_acceptance()
            .unwrap_err()
            .contains("AUR installation closed"));
    }
    #[test]
    fn role_separation_denies_same_signer_and_accepts_distinct_public_identities() {
        let developer = vec!["A".repeat(40)];
        let independent = vec!["B".repeat(40)];
        assert!(require_independent_roles(&developer, &developer, &developer).is_err());
        assert!(require_independent_roles(&developer, &developer, &independent).is_ok());
        assert!(require_independent_roles(&developer, &developer, &[]).is_err());
    }
    #[test]
    fn rejected_worker_eligibility_persists_failure_without_claim_or_commit() {
        use crate::broker_protocol::aur_fixture::Fixture;
        let f = Fixture::new();
        let plan = CanonicalPlan::from_packages(vec![UpdatePackageInfo {
            package_name: "authority-fixture".into(),
            current_version: "1-1".into(),
            new_version: "2-1".into(),
            repository: "aur-attested".into(),
            download_size: 1,
            filename: "authority.pkg.tar".into(),
            sha256: "a".repeat(64),
            repository_metadata_sha256: "b".repeat(64),
        }])
        .unwrap()
        .with_policy_fingerprint("c".repeat(64))
        .unwrap();
        let queued = f.queue(plan);
        assert!(aur_execute_received(&f.store, &queued, vec![], vec![]).is_err());
        let view = f
            .store
            .operation_for_owner(&f.owner, &queued.operation_id)
            .unwrap();
        assert_eq!(view.status, OperationStatus::Failed);
        assert_eq!(view.evidence.package_commit, CommitState::NotStarted);
    }
    #[test]
    fn terminal_uncertain_commit_blocks_new_native_authorization() {
        use crate::broker_protocol::aur_fixture::Fixture;
        for repository in ["core", "aur-attested"] {
            let f = Fixture::new();
            let plan = CanonicalPlan::from_packages(vec![UpdatePackageInfo {
                package_name: "uncertain-fixture".into(),
                current_version: "1-1".into(),
                new_version: "2-1".into(),
                repository: repository.into(),
                download_size: 1,
                filename: "uncertain.pkg.tar".into(),
                sha256: "a".repeat(64),
                repository_metadata_sha256: "b".repeat(64),
            }])
            .unwrap()
            .with_policy_fingerprint("c".repeat(64))
            .unwrap();
            let queued = f.queue(plan.clone());
            f.store
                .claim_transaction(&queued.operation_id, &plan, 102)
                .unwrap();
            let evidence = ExecutionEvidence {
                package_commit: CommitState::Unknown,
                ..Default::default()
            };
            f.store
                .record_execution_evidence(&queued.operation_id, &evidence)
                .unwrap();
            f.store
                .finish_worker_result(
                    &queued.operation_id,
                    OperationStatus::Failed,
                    "uncertain fixture",
                    103,
                )
                .unwrap();
            assert!(f.store.require_no_uncertain_commit().is_err());
            f.store
                .create_preview(
                    f.grant(f.owner.clone(), PolicyAction::Preview, 104),
                    plan.clone(),
                    104,
                )
                .unwrap();
            assert!(matches!(
                f.store.consume_approved_preview(
                    f.grant(f.owner.clone(), PolicyAction::Install, 104),
                    104,
                    &plan.hash(),
                    false
                ),
                Err(BrokerError::OperationInProgress)
            ));
        }
    }
    #[test]
    fn journal_isolation_requires_durable_nonrollback_storage() {
        let root = AurRuntimeMount {
            target: "/".into(),
            device: "0:35".into(),
            fstype: "btrfs".into(),
            fsroot: "/@".into(),
        };
        let mut journal = AurRuntimeMount {
            target: "/var/lib/zethropol/aur-transactions".into(),
            device: "0:35".into(),
            fstype: "btrfs".into(),
            fsroot: "/@/var/lib/zethropol/aur-transactions".into(),
        };
        assert!(!journal_outside_rollback(&root, &journal));
        journal.fsroot = "/@aur-transactions".into();
        journal.device = "0:35".into();
        assert!(journal_outside_rollback(&root, &journal));
        journal.fstype = "tmpfs".into();
        journal.device = "0:99".into();
        assert!(!journal_outside_rollback(&root, &journal));
        journal.fstype = "ext4".into();
        journal.device = "8:64".into();
        journal.fsroot = "/".into();
        assert!(journal_outside_rollback(&root, &journal));
        journal.device = "7:0".into();
        assert!(!journal_outside_rollback(&root, &journal));
    }
    #[test]
    fn missing_real_authority_cannot_enable_install() {
        // No keys, root provisioning, PolicyKit grant or host transaction.
        assert!(require_aur_production_authority().is_err());
    }
}
