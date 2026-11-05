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
