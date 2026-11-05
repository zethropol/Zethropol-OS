impl BrokerStore {
    pub(crate) fn release_aur_reservation(
        &self,
        caller: AuthorizedCaller,
        operation: &str,
        now: u64,
    ) -> Result<(), BrokerError> {
        if caller.action != PolicyAction::Preview
            || caller.decided_at > now
            || now.saturating_sub(caller.decided_at) > AUTHORIZATION_MAX_AGE_SECS
        {
            return Err(BrokerError::AuthorizationDenied);
        }
        self.with_lock(|| {
            let path = self.preview_path(caller.identity());
            let record = self.read_record(&path)?;
            ensure_owner(&record, caller.identity())?;
            if record.operation_id != operation
                || record.status != OperationStatus::Previewed
                || record.plan.packages().is_empty()
                || record
                    .plan
                    .packages()
                    .iter()
                    .any(|p| p.repository != "aur-build-reservation")
            {
                return Err(BrokerError::InvalidTransition);
            }
            // Existing persistent retirement retains audit/UUID and removes only
            // this caller's preview, preventing receipt admission or replay.
            self.aur_expire_preview(&path)
        })
    }
}

impl BrokerStore {
    pub(crate) fn require_no_uncertain_commit(&self) -> Result<(), BrokerError> {
        self.with_lock(|| {
            for entry in fs::read_dir(&self.root).map_err(io_error)? {
                let entry = entry.map_err(io_error)?;
                if !entry
                    .file_name()
                    .to_string_lossy()
                    .starts_with("operation-")
                {
                    continue;
                }
                let record = self.read_record(&entry.path())?;
                if matches!(
                    record.evidence.package_commit,
                    CommitState::Unknown | CommitState::InProgress | CommitState::Failed
                ) {
                    return Err(BrokerError::OperationInProgress);
                }
            }
            Ok(())
        })
    }
}
