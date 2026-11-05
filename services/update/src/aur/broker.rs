// Additional AUR cancellation transition in the SAME existing BrokerStore.
// The persistent record format, locks, identity and PolicyKit proof types are reused.
impl BrokerStore {
    pub(crate) fn cancel_aur_before_commit(
        &self,
        authorization: AuthorizedCaller,
        operation_id: &str,
        now: u64,
    ) -> Result<(), BrokerError> {
        if authorization.action != PolicyAction::Preview
            || authorization.decided_at > now
            || now.saturating_sub(authorization.decided_at) > AUTHORIZATION_MAX_AGE_SECS
        {
            return Err(BrokerError::AuthorizationDenied);
        }
        self.with_lock(|| {
            let path = self.operation_path(operation_id)?;
            let mut record = self.read_record(&path)?;
            ensure_owner(&record, &authorization.identity)?;
            if record.plan.packages().is_empty()
                || !record
                    .plan
                    .packages()
                    .iter()
                    .all(|p| p.repository == "aur-attested")
            {
                return Err(BrokerError::InvalidPlan);
            }
            if record.status.terminal() {
                return Ok(());
            }
            if !matches!(
                record.status,
                OperationStatus::Queued | OperationStatus::Running
            ) || record.evidence.package_commit != CommitState::NotStarted
            {
                return Err(BrokerError::InvalidTransition);
            }
            record.status = OperationStatus::Failed;
            record.finished_at = Some(now);
            record.message =
                "AUR operation cancelled before durable commit start. No automatic retry.".into();
            self.write_record(&path, &record)
        })
    }
}
impl BrokerStore {
    pub(crate) fn checkpoint_aur_commit_start(
        &self,
        operation_id: &str,
        current: &CanonicalPlan,
        snapshot: &str,
        now: u64,
    ) -> Result<ExecutionEvidence, BrokerError> {
        if !crate::update_service::aur_snapshot_number_valid(snapshot) {
            return Err(BrokerError::InvalidPlan);
        }
        self.with_lock(|| {
            let path = self.operation_path(operation_id)?;
            let mut record = self.read_record(&path)?;
            if record.status != OperationStatus::Running
                || record.evidence.package_commit != CommitState::NotStarted
            {
                return Err(BrokerError::InvalidTransition);
            }
            if record.plan != *current
                || record.plan.packages().is_empty()
                || !record
                    .plan
                    .packages()
                    .iter()
                    .all(|p| p.repository == "aur-attested")
            {
                return Err(BrokerError::PlanChanged);
            }
            if now >= record.expires_at {
                record.status = OperationStatus::Failed;
                record.finished_at = Some(now);
                record.message = "AUR plan expired before commit; no retry".into();
                self.write_record(&path, &record)?;
                return Err(BrokerError::Expired);
            }
            record.evidence.pre_snapshot = Some(snapshot.into());
            record.evidence.package_commit = CommitState::InProgress;
            self.write_record(&path, &record)?;
            Ok(record.evidence)
        })
    }
}
impl BrokerStore {
    pub(crate) fn consume_aur_approved_preview(
        &self,
        authorization: AuthorizedCaller,
        now: u64,
        hash: &str,
    ) -> Result<ConsumedPreview, BrokerError> {
        self.with_lock(|| {
            let record = self.read_record(&self.preview_path(&authorization.identity))?;
            ensure_owner(&record, &authorization.identity)?;
            if record.plan.hash() != hash
                || record.plan.packages().is_empty()
                || !record.plan.removals().is_empty()
                || !record
                    .plan
                    .packages()
                    .iter()
                    .all(|p| p.repository == "aur-attested")
            {
                return Err(BrokerError::InvalidPlan);
            }
            Ok(())
        })?;
        // A subsequent replacement preview must have the exact same complete
        // canonical hash or the existing atomic consume rejects it. No token,
        // authorization, expiry, owner or single-flight check is reimplemented.
        self.consume_approved_preview(authorization, now, hash, false)
    }
}

impl BrokerStore {
    pub(crate) fn aur_commit_evidence(
        &self,
        operation: &str,
        plan: &CanonicalPlan,
    ) -> Result<ExecutionEvidence, BrokerError> {
        self.with_lock(|| {
            let record = self.read_record(&self.operation_path(operation)?)?;
            if record.status != OperationStatus::Running
                || record.plan != *plan
                || record.evidence.package_commit != CommitState::InProgress
                || record.evidence.pre_snapshot.is_none()
                || plan.packages().is_empty()
                || plan
                    .packages()
                    .iter()
                    .any(|p| p.repository != "aur-attested")
            {
                return Err(BrokerError::InvalidTransition);
            }
            Ok(record.evidence)
        })
    }
}

impl BrokerStore {
    // Cancellation can win while Snapper is creating a recovery point. Keep
    // that ID durably without reviving the cancelled operation or allowing
    // commit. This is a worker-only annotation in the same existing record.
    pub(crate) fn record_aur_pre_snapshot(
        &self,
        operation: &str,
        plan: &CanonicalPlan,
        snapshot: &str,
    ) -> Result<(), BrokerError> {
        if !crate::update_service::aur_snapshot_number_valid(snapshot) {
            return Err(BrokerError::InvalidPlan);
        }
        self.with_lock(|| {
            let path = self.operation_path(operation)?;
            let mut record = self.read_record(&path)?;
            if record.plan != *plan
                || plan.packages().is_empty()
                || plan
                    .packages()
                    .iter()
                    .any(|p| p.repository != "aur-attested")
                || record.evidence.package_commit != CommitState::NotStarted
                || !matches!(
                    record.status,
                    OperationStatus::Running | OperationStatus::Failed
                )
                || record
                    .evidence
                    .pre_snapshot
                    .as_deref()
                    .is_some_and(|id| id != snapshot)
            {
                return Err(BrokerError::InvalidTransition);
            }
            record.evidence.pre_snapshot = Some(snapshot.into());
            self.write_record(&path, &record)
        })
    }
}

// Receipt reservations and durable admission reuse this store's owner checks,
// lock and original preview record. Companion tombstones are never auto-retried.
use crate::update_service::{ReceiptContext, ReviewedBuild, VerifiedAurReceipt};
#[derive(serde::Serialize, serde::Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct AurReceiptBinding {
    schema: u32,
    operation_id: String,
    receipt_sha256: String,
    plan_hash: String,
    expires_at: u64,
    deadline: u64,
    trust_sha256: String,
    pub(crate) signatures: Vec<(String, String)>,
    review: ReviewedBuild,
}
impl BrokerStore {
    pub(crate) fn reserve_aur_build(
        &self,
        authorization: AuthorizedCaller,
        review: ReviewedBuild,
        now: u64,
    ) -> Result<ReceiptContext, BrokerError> {
        review.validate().map_err(|_| BrokerError::InvalidPlan)?;
        let packages = review
            .outputs
            .iter()
            .map(|p| UpdatePackageInfo {
                package_name: p.name.clone(),
                current_version: String::new(),
                new_version: p.version.clone(),
                repository: "aur-build-reservation".into(),
                download_size: 0,
                filename: p.filename.clone(),
                sha256: "0".repeat(64),
                repository_metadata_sha256: review.digest(),
            })
            .collect();
        let plan =
            CanonicalPlan::from_packages(packages)?.with_policy_fingerprint(review.digest())?;
        self.with_lock(|| self.aur_receipt_storage_budget())?;
        self.create_preview(authorization.clone(), plan, now)?;
        let context = self.aur_receipt_context(&authorization.identity, review.clone(), now)?;
        self.with_lock(|| {
            let record = self.read_record(&self.preview_path(&authorization.identity))?;
            if record.operation_id != context.operation_id {
                return Err(BrokerError::PlanChanged);
            }
            self.aur_receipt_storage_budget()?;
            let path = self
                .root
                .join(format!("aur-review-{}.json", context.operation_id));
            self.aur_create_companion(
                &path,
                &serde_json::to_vec(&review).map_err(|_| BrokerError::InvalidPlan)?,
            )
        })?;
        Ok(context)
    }
    pub(crate) fn aur_receipt_context(
        &self,
        owner: &VerifiedIdentity,
        review: ReviewedBuild,
        now: u64,
    ) -> Result<ReceiptContext, BrokerError> {
        review.validate().map_err(|_| BrokerError::InvalidPlan)?;
        self.with_lock(|| {
            let record = self.read_record(&self.preview_path(owner))?;
            ensure_owner(&record, owner)?;
            if record.status != OperationStatus::Previewed
                || now >= record.expires_at
                || record.plan.policy_fingerprint() != review.digest()
                || record
                    .plan
                    .packages()
                    .iter()
                    .any(|p| p.repository != "aur-build-reservation")
            {
                return Err(BrokerError::InvalidTransition);
            }
            Ok(ReceiptContext {
                operation_id: record.operation_id,
                uid: owner.uid,
                bus_name: owner.bus_name.clone(),
                session_id: owner.session_id.clone(),
                review,
                reservation_issued_boottime: record.created_at,
                reservation_deadline_boottime: record.expires_at,
            })
        })
    }
    pub(crate) fn admit_aur_receipt(
        &self,
        authorization: AuthorizedCaller,
        verified: &VerifiedAurReceipt,
        now: u64,
        wall_now: u64,
    ) -> Result<CanonicalPlan, BrokerError> {
        if authorization.action != PolicyAction::Preview
            || authorization.decided_at > now
            || now.saturating_sub(authorization.decided_at) > AUTHORIZATION_MAX_AGE_SECS
        {
            return Err(BrokerError::AuthorizationDenied);
        }
        self.with_lock(|| {
            let path = self.preview_path(&authorization.identity);
            let mut record = self.read_record(&path)?;
            ensure_owner(&record, &authorization.identity)?;
            let c = verified.context();
            if c.operation_id != record.operation_id
                || c.uid != record.owner.uid
                || c.bus_name != record.owner.bus_name
                || c.session_id != record.owner.session_id
                || record.status != OperationStatus::Previewed
                || now >= record.expires_at
                || record.plan.policy_fingerprint() != c.review.digest()
                || record
                    .plan
                    .packages()
                    .iter()
                    .any(|p| p.repository != "aur-build-reservation")
                || wall_now >= verified.expiry()
            {
                return Err(BrokerError::InvalidPlan);
            }
            let binding = AurReceiptBinding {
                schema: 1,
                operation_id: c.operation_id.clone(),
                receipt_sha256: verified.receipt_sha256().into(),
                plan_hash: verified.plan().hash(),
                expires_at: verified.expiry(),
                deadline: now
                    .saturating_add(verified.expiry() - wall_now)
                    .min(record.expires_at),
                trust_sha256: verified.trust_sha256().into(),
                signatures: verified.signature_hashes(),
                review: c.review.clone(),
            };
            // O_EXCL tombstone precedes preview publication. A crash in between
            // burns this operation, rather than allowing receipt replay.
            self.aur_receipt_storage_budget()?;
            let marker = self.root.join(format!(
                "aur-receipt-{}.spent",
                valid_uuid(c.operation_id.clone())?
            ));
            let bytes = serde_json::to_vec(&binding).map_err(|_| BrokerError::InvalidPlan)?;
            self.aur_create_companion(&marker, &bytes)?;
            record.plan = verified.plan().clone();
            record.message = "Verified AUR receipt; explicit install approval required.".into();
            self.write_record(&path, &record)?;
            Ok(record.plan)
        })
    }
    fn read_aur_binding(
        &self,
        id: &str,
        plan: &CanonicalPlan,
        now: u64,
        wall_now: u64,
        trust: &str,
    ) -> Result<AurReceiptBinding, BrokerError> {
        let path = self
            .root
            .join(format!("aur-receipt-{}.spent", valid_uuid(id.into())?));
        let file = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC)
            .open(path)
            .map_err(io_error)?;
        let meta = file.metadata().map_err(io_error)?;
        if !meta.is_file()
            || meta.uid() != self.expected_owner
            || meta.permissions().mode() & 0o777 != 0o600
            || meta.len() > 1024 * 1024
        {
            return Err(BrokerError::UnsafeStore);
        }
        let mut bytes = Vec::new();
        file.take(1024 * 1024 + 1)
            .read_to_end(&mut bytes)
            .map_err(io_error)?;
        let bytes = self.aur_unpack_live_bytes(&bytes)?;
        let binding: AurReceiptBinding =
            serde_json::from_slice(&bytes).map_err(|_| BrokerError::CorruptRecord)?;
        if binding.schema != 1
            || binding.operation_id != id
            || binding.plan_hash != plan.hash()
            || binding.trust_sha256 != trust
            || wall_now >= binding.expires_at
            || now >= binding.deadline
            || plan.packages().is_empty()
            || plan.packages().iter().any(|p| {
                p.repository != "aur-attested"
                    || p.repository_metadata_sha256 != binding.receipt_sha256
            })
            || binding.signatures.len() != plan.packages().len()
        {
            return Err(BrokerError::InvalidPlan);
        }
        Ok(binding)
    }
    pub(crate) fn consume_aur_receipt_preview(
        &self,
        authorization: AuthorizedCaller,
        now: u64,
        wall_now: u64,
        hash: &str,
        trust: &str,
    ) -> Result<ConsumedPreview, BrokerError> {
        self.with_lock(|| {
            let record = self.read_record(&self.preview_path(&authorization.identity))?;
            ensure_owner(&record, &authorization.identity)?;
            if record.plan.hash() != hash {
                return Err(BrokerError::PlanChanged);
            }
            self.read_aur_binding(&record.operation_id, &record.plan, now, wall_now, trust)?;
            Ok(())
        })?;
        self.consume_aur_approved_preview(authorization, now, hash)
    }
    pub(crate) fn aur_receipt_binding(
        &self,
        operation: &str,
        plan: &CanonicalPlan,
        now: u64,
        wall_now: u64,
        trust: &str,
    ) -> Result<AurReceiptBinding, BrokerError> {
        self.with_lock(|| {
            let record = self.read_record(&self.operation_path(operation)?)?;
            if record.plan != *plan
                || !matches!(
                    record.status,
                    OperationStatus::Queued | OperationStatus::Running
                )
            {
                return Err(BrokerError::InvalidTransition);
            }
            self.read_aur_binding(operation, plan, now, wall_now, trust)
        })
    }
}

impl BrokerStore {
    pub(crate) fn receipt_context_for_operation(
        &self,
        owner: &VerifiedIdentity,
        operation: &str,
        now: u64,
    ) -> Result<ReceiptContext, BrokerError> {
        let id = valid_uuid(operation.into())?;
        let review = self.with_lock(|| {
            let record = self.read_record(&self.preview_path(owner))?;
            ensure_owner(&record, owner)?;
            if record.operation_id != id {
                return Err(BrokerError::IdentityMismatch);
            }
            let file = OpenOptions::new()
                .read(true)
                .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC)
                .open(self.root.join(format!("aur-review-{id}.json")))
                .map_err(io_error)?;
            let m = file.metadata().map_err(io_error)?;
            if !m.is_file()
                || m.uid() != self.expected_owner
                || m.permissions().mode() & 0o777 != 0o600
                || m.len() > 1024 * 1024
            {
                return Err(BrokerError::UnsafeStore);
            }
            let mut data = Vec::new();
            file.take(1024 * 1024 + 1)
                .read_to_end(&mut data)
                .map_err(io_error)?;
            let data = self.aur_unpack_live_bytes(&data)?;
            serde_json::from_slice::<ReviewedBuild>(&data).map_err(|_| BrokerError::CorruptRecord)
        })?;
        let context = self.aur_receipt_context(owner, review, now)?;
        if context.operation_id != id {
            return Err(BrokerError::PlanChanged);
        }
        Ok(context)
    }
}

impl BrokerStore {
    // New receipt companions must not defeat the existing bounded worker
    // storage model. Retain tombstones rather than deleting replay evidence.
    fn aur_receipt_storage_budget(&self) -> Result<(), BrokerError> {
        self.aur_persistent_storage_budget()?;
        let mut bytes = 0u64;
        let mut count = 0usize;
        for entry in fs::read_dir(&self.root).map_err(io_error)? {
            let entry = entry.map_err(io_error)?;
            let name = entry.file_name();
            let name = name.to_string_lossy();
            if !name.starts_with("aur-review-") && !name.starts_with("aur-receipt-") {
                continue;
            }
            let m = fs::symlink_metadata(entry.path()).map_err(io_error)?;
            if !m.is_file()
                || m.uid() != self.expected_owner
                || m.permissions().mode() & 0o777 != 0o600
            {
                return Err(BrokerError::UnsafeStore);
            }
            count += 1;
            bytes = bytes.checked_add(m.len()).ok_or(BrokerError::UnsafeStore)?;
            if count >= 4096 || bytes > 63 * 1024 * 1024 {
                return Err(BrokerError::Io(
                    "receipt retention quota reached; explicit maintenance required".into(),
                ));
            }
        }
        Ok(())
    }
}
