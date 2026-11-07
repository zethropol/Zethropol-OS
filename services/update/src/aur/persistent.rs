// AUR-only persistent use of the ORIGINAL BrokerStore. Never installed or
// opened by host tests. Runtime artifacts/FDs remain under /run.
const AUR_PERSISTENT_ROOT: &str = "/var/lib/zethropol/aur-transactions";
const AUR_EPOCH: &str = "aur-epoch.json";
#[derive(serde::Serialize, serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct AurEnvelope {
    schema: u32,
    clock_boot_id: String,
    sha256: String,
    payload: String,
}
#[derive(serde::Serialize, serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct AurEpoch {
    schema: u32,
    boot_id: String,
}
fn aur_wrap(bytes: &[u8], boot: &str) -> Result<Vec<u8>, BrokerError> {
    valid_uuid(boot.into())?;
    let value = AurEnvelope {
        schema: 1,
        clock_boot_id: boot.into(),
        sha256: aur_checksum(boot, bytes),
        payload: String::from_utf8(bytes.to_vec()).map_err(|_| BrokerError::CorruptRecord)?,
    };
    let bytes = serde_json::to_vec(&value).map_err(|_| BrokerError::CorruptRecord)?;
    if bytes.len() as u64 > MAX_RECORD_BYTES {
        return Err(BrokerError::CorruptRecord);
    }
    Ok(bytes)
}
fn aur_unwrap(bytes: &[u8]) -> Result<Vec<u8>, BrokerError> {
    let value: AurEnvelope =
        serde_json::from_slice(bytes).map_err(|_| BrokerError::CorruptRecord)?;
    valid_uuid(value.clock_boot_id.clone())?;
    if value.schema != 1
        || value.sha256 != aur_checksum(&value.clock_boot_id, value.payload.as_bytes())
    {
        return Err(BrokerError::CorruptRecord);
    }
    Ok(value.payload.into_bytes())
}
impl BrokerStore {
    pub(crate) fn open_aur_persistent() -> Result<Self, BrokerError> {
        if unsafe { libc::geteuid() } != 0 {
            return Err(BrokerError::UnsafeStore);
        }
        let boot = fs::read_to_string("/proc/sys/kernel/random/boot_id").map_err(io_error)?;
        let now = crate::update_service::aur_boottime().map_err(BrokerError::Io)?;
        Self::open_aur_persistent_at(
            PathBuf::from(AUR_PERSISTENT_ROOT),
            0,
            boot.trim(),
            now,
            true,
        )
    }
    fn open_aur_persistent_at(
        root: PathBuf,
        owner: u32,
        boot: &str,
        now: u64,
        ancestors: bool,
    ) -> Result<Self, BrokerError> {
        valid_uuid(boot.into())?;
        let parent = root.parent().ok_or(BrokerError::UnsafeStore)?.to_path_buf();
        for p in parent.ancestors() {
            let m = fs::symlink_metadata(p).map_err(io_error)?;
            if !m.is_dir() || m.uid() != owner || m.permissions().mode() & 0o022 != 0 {
                return Err(BrokerError::UnsafeStore);
            }
            if !ancestors {
                break;
            }
        }
        // Reject unsafe existing lock BEFORE inherited open_with could chmod it.
        match fs::symlink_metadata(root.join(".lock")) {
            Ok(_) => {
                Self::aur_safe_file_at(&root.join(".lock"), owner)?;
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => (),
            Err(e) => return Err(io_error(e)),
        }
        let mut store = Self::open_with(root, owner, Arc::new(UuidTokenSource))?;
        if ancestors {
            use std::os::unix::ffi::OsStrExt;
            let path = std::ffi::CString::new(store.root.as_os_str().as_bytes())
                .map_err(|_| BrokerError::UnsafeStore)?;
            let mut info = std::mem::MaybeUninit::<libc::statfs>::uninit();
            if unsafe { libc::statfs(path.as_ptr(), info.as_mut_ptr()) } != 0 {
                return Err(io_error(std::io::Error::last_os_error()));
            }
            let kind = unsafe { info.assume_init() }.f_type as u64;
            if ![0xef53, 0x9123683e, 0x58465342].contains(&kind) {
                return Err(BrokerError::Io("persistent AUR store requires reviewed ext4/Btrfs/XFS storage; volatile/other filesystems refused".into()));
            }
        }
        // Durably link a newly created store directory into its parent BEFORE
        // publishing an anchor, receipt or commit-start capability.
        File::open(&parent)
            .and_then(|f| f.sync_all())
            .map_err(io_error)?;
        store.with_lock(|| {
            let epoch_path = store.root.join(AUR_EPOCH);
            let old = match store.aur_read_file(&epoch_path) {
                Ok(bytes) => {
                    let epoch: AurEpoch = serde_json::from_slice(&aur_unwrap(&bytes)?)
                        .map_err(|_| BrokerError::CorruptRecord)?;
                    if epoch.schema != 1 {
                        return Err(BrokerError::CorruptRecord);
                    }
                    valid_uuid(epoch.boot_id.clone())?;
                    Some(epoch.boot_id)
                }
                Err(BrokerError::NotFound) => None,
                Err(e) => return Err(e),
            };
            if old.is_none() {
                // Never silently initialize a store that lost its boot anchor.
                for e in fs::read_dir(&store.root).map_err(io_error)? {
                    let e = e.map_err(io_error)?;
                    let name = e.file_name().to_string_lossy().into_owned();
                    if name != ".lock" && !name.starts_with("aur-tmp-") {
                        return Err(BrokerError::CorruptRecord);
                    }
                    Self::aur_safe_file_at(&e.path(), owner)?;
                }
            } else {
                store.aur_validate_inventory()?;
                store.aur_recover_locked(old.as_deref() != Some(boot), now)?;
            }
            // Boot change is committed LAST. Recovery may be replayed after a
            // crash, but no preview/commit capability is revived by doing so.
            let bytes = serde_json::to_vec(&AurEpoch {
                schema: 1,
                boot_id: boot.into(),
            })
            .map_err(|_| BrokerError::CorruptRecord)?;
            store.aur_atomic(&epoch_path, &aur_wrap(&bytes, boot)?, false)
        })?;
        store.aur_durable = true;
        Ok(store)
    }
    fn aur_safe_file_at(path: &Path, owner: u32) -> Result<(), BrokerError> {
        let m = fs::symlink_metadata(path).map_err(|e| {
            if e.kind() == std::io::ErrorKind::NotFound {
                BrokerError::NotFound
            } else {
                io_error(e)
            }
        })?;
        if !m.is_file()
            || m.uid() != owner
            || m.permissions().mode() & 0o7777 != 0o600
            || m.nlink() != 1
            || m.len() > MAX_RECORD_BYTES
        {
            return Err(BrokerError::UnsafeStore);
        }
        Ok(())
    }
    fn aur_read_file(&self, path: &Path) -> Result<Vec<u8>, BrokerError> {
        Self::aur_safe_file_at(path, self.expected_owner)?;
        let file = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC)
            .open(path)
            .map_err(io_error)?;
        let m = file.metadata().map_err(io_error)?;
        if !m.is_file()
            || m.uid() != self.expected_owner
            || m.permissions().mode() & 0o7777 != 0o600
            || m.nlink() != 1
            || m.len() > MAX_RECORD_BYTES
        {
            return Err(BrokerError::UnsafeStore);
        }
        let mut bytes = Vec::new();
        file.take(MAX_RECORD_BYTES + 1)
            .read_to_end(&mut bytes)
            .map_err(io_error)?;
        if bytes.len() as u64 > MAX_RECORD_BYTES {
            return Err(BrokerError::CorruptRecord);
        }
        Ok(bytes)
    }
    fn aur_is_persistent(&self) -> Result<bool, BrokerError> {
        match fs::symlink_metadata(self.root.join(AUR_EPOCH)) {
            Ok(_) => {
                let bytes = self.aur_read_file(&self.root.join(AUR_EPOCH))?;
                let epoch: AurEpoch = serde_json::from_slice(&aur_unwrap(&bytes)?)
                    .map_err(|_| BrokerError::CorruptRecord)?;
                if epoch.schema != 1 {
                    return Err(BrokerError::CorruptRecord);
                }
                valid_uuid(epoch.boot_id)?;
                Ok(true)
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound && self.aur_durable => {
                Err(BrokerError::CorruptRecord)
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(false),
            Err(e) => Err(io_error(e)),
        }
    }
    fn aur_check_persistent_lock(&self) -> Result<(), BrokerError> {
        if self.aur_is_persistent()? {
            Self::aur_safe_file_at(&self.root.join(".lock"), self.expected_owner)?;
        }
        Ok(())
    }
    fn aur_pack_bytes(&self, bytes: &[u8]) -> Result<Vec<u8>, BrokerError> {
        if self.aur_is_persistent()? {
            let epoch: AurEpoch = serde_json::from_slice(&aur_unwrap(
                &self.aur_read_file(&self.root.join(AUR_EPOCH))?,
            )?)
            .map_err(|_| BrokerError::CorruptRecord)?;
            aur_wrap(bytes, &epoch.boot_id)
        } else {
            Ok(bytes.to_vec())
        }
    }
    fn aur_unpack_bytes(&self, bytes: &[u8]) -> Result<Vec<u8>, BrokerError> {
        if self.aur_is_persistent()? {
            aur_unwrap(bytes)
        } else {
            Ok(bytes.to_vec())
        }
    }
    // Same-directory atomic publication. Exclusive companions use Linux
    // RENAME_NOREPLACE; unsupported filesystems fail closed, with no fallback.
    fn aur_atomic(
        &self,
        destination: &Path,
        bytes: &[u8],
        exclusive: bool,
    ) -> Result<(), BrokerError> {
        use std::os::unix::ffi::OsStrExt;
        if destination.parent() != Some(self.root.as_path())
            || bytes.len() as u64 > MAX_RECORD_BYTES
        {
            return Err(BrokerError::UnsafeStore);
        }
        let temp = self.root.join(format!("aur-tmp-{}", Uuid::new_v4()));
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(&temp)
            .map_err(io_error)?;
        file.write_all(bytes)
            .and_then(|_| file.sync_all())
            .map_err(io_error)?;
        #[cfg(test)]
        aur_persistent_crash_point(self, "after-file-fsync");
        let result = if exclusive {
            let from = std::ffi::CString::new(temp.as_os_str().as_bytes())
                .map_err(|_| BrokerError::UnsafeStore)?;
            let to = std::ffi::CString::new(destination.as_os_str().as_bytes())
                .map_err(|_| BrokerError::UnsafeStore)?;
            if unsafe {
                libc::renameat2(
                    libc::AT_FDCWD,
                    from.as_ptr(),
                    libc::AT_FDCWD,
                    to.as_ptr(),
                    libc::RENAME_NOREPLACE,
                )
            } == 0
            {
                Ok(())
            } else {
                let e = std::io::Error::last_os_error();
                Err(if e.kind() == std::io::ErrorKind::AlreadyExists {
                    BrokerError::InvalidToken
                } else {
                    io_error(e)
                })
            }
        } else {
            fs::rename(&temp, destination).map_err(io_error)
        };
        if result.is_err() {
            let _ = fs::remove_file(&temp);
        }
        result?;
        #[cfg(test)]
        aur_persistent_crash_point(self, "after-rename");
        File::open(&self.root)
            .and_then(|f| f.sync_all())
            .map_err(io_error)?;
        #[cfg(test)]
        aur_persistent_crash_point(self, "after-directory-fsync");
        Ok(())
    }
    fn aur_create_companion(&self, path: &Path, bytes: &[u8]) -> Result<(), BrokerError> {
        self.aur_atomic(path, &self.aur_pack_bytes(bytes)?, true)
    }
    fn aur_validate_inventory(&self) -> Result<(), BrokerError> {
        let mut referenced = HashSet::new();
        let mut admitted = Vec::new();
        for entry in fs::read_dir(&self.root).map_err(io_error)? {
            let entry = entry.map_err(io_error)?;
            let p = entry.path();
            let name = entry.file_name().to_string_lossy().into_owned();
            Self::aur_safe_file_at(&p, self.expected_owner)?;
            if name == ".lock" || name == AUR_EPOCH {
                continue;
            }
            if let Some(id) = name
                .strip_prefix("worker-")
                .and_then(|s| s.strip_suffix(".lock"))
            {
                valid_uuid(id.into())?;
                continue;
            }
            if let Some(id) = name
                .strip_prefix("tmp-")
                .and_then(|s| s.strip_suffix(".record"))
                .or_else(|| name.strip_prefix("aur-tmp-"))
            {
                valid_uuid(id.into())?;
                continue; // orphan: never promote or execute
            }
            if name.starts_with("preview-") || name.starts_with("operation-") {
                let r = self.read_record(&p)?;
                self.aur_validate_record(&r)?;
                referenced.insert(r.operation_id.clone());
            } else if let Some(id) = name
                .strip_prefix("retired-preview-")
                .and_then(|s| s.strip_suffix(".record"))
            {
                let r = decode_record(&aur_unwrap(&self.aur_read_file(&p)?)?)?;
                if r.operation_id != valid_uuid(id.into())?
                    || r.status != OperationStatus::Previewed
                {
                    return Err(BrokerError::CorruptRecord);
                }
                self.aur_validate_record(&r)?;
                referenced.insert(r.operation_id.clone());
            } else if let Some(id) = name
                .strip_prefix("aur-review-")
                .and_then(|s| s.strip_suffix(".json"))
            {
                valid_uuid(id.into())?;
                let review: ReviewedBuild =
                    serde_json::from_slice(&aur_unwrap(&self.aur_read_file(&p)?)?)
                        .map_err(|_| BrokerError::CorruptRecord)?;
                review.validate().map_err(|_| BrokerError::CorruptRecord)?;
            } else if let Some(id) = name
                .strip_prefix("aur-receipt-")
                .and_then(|s| s.strip_suffix(".spent"))
            {
                let b: AurReceiptBinding =
                    serde_json::from_slice(&aur_unwrap(&self.aur_read_file(&p)?)?)
                        .map_err(|_| BrokerError::CorruptRecord)?;
                if b.schema != 1
                    || b.operation_id != valid_uuid(id.into())?
                    || !valid_sha256(&b.receipt_sha256)
                    || !valid_sha256(&b.plan_hash)
                    || !valid_sha256(&b.trust_sha256)
                    || b.signatures.is_empty()
                    || b.signatures.len() > 64
                {
                    return Err(BrokerError::CorruptRecord);
                }
                b.review
                    .validate()
                    .map_err(|_| BrokerError::CorruptRecord)?;
                admitted.push(b.operation_id);
            } else {
                return Err(BrokerError::UnsafeStore);
            }
        }
        if admitted.iter().any(|id| !referenced.contains(id)) {
            return Err(BrokerError::CorruptRecord);
        }
        Ok(())
    }
    fn aur_validate_record(&self, r: &Record) -> Result<(), BrokerError> {
        if r.plan.packages().is_empty()
            || r.plan.packages().iter().any(|p| {
                !["aur-local", "aur-build-reservation"].contains(&p.repository.as_str())
            })
        {
            return Err(BrokerError::InvalidPlan);
        }
        if r.plan
            .packages()
            .iter()
            .all(|p| p.repository == "aur-local")
        {
            let marker = self
                .root
                .join(format!("aur-receipt-{}.spent", r.operation_id));
            let b: AurReceiptBinding =
                serde_json::from_slice(&aur_unwrap(&self.aur_read_file(&marker)?)?)
                    .map_err(|_| BrokerError::CorruptRecord)?;
            if b.operation_id != r.operation_id
                || b.plan_hash != r.plan.hash()
                || r.plan
                    .packages()
                    .iter()
                    .any(|p| p.repository_metadata_sha256 != b.receipt_sha256)
            {
                return Err(BrokerError::CorruptRecord);
            }
        }
        Ok(())
    }
    fn aur_recover_locked(&self, new_boot: bool, now: u64) -> Result<(), BrokerError> {
        let mut paths = fs::read_dir(&self.root)
            .map_err(io_error)?
            .map(|e| e.map(|e| e.path()).map_err(io_error))
            .collect::<Result<Vec<_>, _>>()?;
        paths.sort();
        for path in paths {
            let name = path.file_name().unwrap().to_string_lossy();
            if name.starts_with("preview-") && new_boot {
                self.aur_retire_preview(&path)?;
            } else if name.starts_with("operation-") {
                let mut r = self.read_record(&path)?;
                if r.status.terminal() {
                    continue;
                }
                let _lease = match self.worker_lease(&r.operation_id) {
                    Ok(l) => l,
                    Err(BrokerError::OperationInProgress) if !new_boot => continue,
                    Err(e) => return Err(e),
                };
                // A rename-before-publication crash may leave an inert
                // Previewed operation with no Install grant. Preserve it;
                // it cannot claim a transaction or be retried.
                if r.status == OperationStatus::Previewed {
                    self.aur_retire_preview(&path)?;
                    continue;
                }
                r.evidence.interrupted();
                r.status = OperationStatus::Failed;
                r.finished_at = Some(now.max(r.created_at));
                r.message=format!("AUR worker/boot interrupted before durable completion; native commit evidence {}. Inspect retained evidence and incomplete postconditions. No automatic retry.",r.evidence.package_commit.as_str());
                self.write_record(&path, &r)?;
            }
        }
        Ok(())
    }
    // No automatic evidence cleanup. Stop NEW reservations with headroom for
    // the one active transaction's final checkpoints/atomic temporary files.
    fn aur_persistent_storage_budget(&self) -> Result<(), BrokerError> {
        if !self.aur_is_persistent()? {
            return Ok(());
        }
        let mut bytes = 0u64;
        let mut count = 0usize;
        for e in fs::read_dir(&self.root).map_err(io_error)? {
            let e = e.map_err(io_error)?;
            Self::aur_safe_file_at(&e.path(), self.expected_owner)?;
            bytes = bytes
                .checked_add(e.metadata().map_err(io_error)?.len())
                .ok_or(BrokerError::UnsafeStore)?;
            count += 1;
            if bytes > 96 * 1024 * 1024 || count >= 8192 {
                return Err(BrokerError::Io(
                    "persistent AUR retention high-water limit; reviewed offline archival required"
                        .into(),
                ));
            }
        }
        Ok(())
    }
    pub(crate) fn aur_history_for_owner(
        &self,
        authorization: AuthorizedCaller,
        id: &str,
        now: u64,
    ) -> Result<OperationView, BrokerError> {
        if authorization.action != PolicyAction::Preview
            || authorization.decided_at > now
            || now.saturating_sub(authorization.decided_at) > AUTHORIZATION_MAX_AGE_SECS
        {
            return Err(BrokerError::AuthorizationDenied);
        }
        self.with_lock(||{
            let r=match self.read_record(&self.operation_path(id)?) {
                Ok(r)=>r,
                Err(BrokerError::NotFound)=>{
                    let p=self.root.join(format!("retired-preview-{}.record",valid_uuid(id.into())?));
                    let r=decode_record(&aur_unwrap(&self.aur_read_file(&p)?)?)?;
                    if r.operation_id!=id {return Err(BrokerError::CorruptRecord);}
                    if r.owner.uid!=authorization.identity.uid {return Err(BrokerError::IdentityMismatch);}
                    return Ok(OperationView{operation_id:r.operation_id,status:OperationStatus::Failed,
                        message:"AUR preview retired; fresh review and consent required. No automatic retry.".into(),evidence:r.evidence});
                }
                Err(e)=>return Err(e),
            };
            if r.owner.uid!=authorization.identity.uid{return Err(BrokerError::IdentityMismatch);}
            Ok(OperationView{operation_id:r.operation_id,status:r.status,message:r.message,evidence:r.evidence})
        })
    }
}
fn sha256_hex(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

impl BrokerStore {
    fn aur_nonce_retained(&self, id: &str) -> Result<bool, BrokerError> {
        for name in [
            format!("aur-review-{id}.json"),
            format!("aur-receipt-{id}.spent"),
            format!("retired-preview-{id}.record"),
            format!("operation-{id}.record"),
        ] {
            match fs::symlink_metadata(self.root.join(name)) {
                Ok(_) => return Ok(true),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => (),
                Err(e) => return Err(io_error(e)),
            }
        }
        Ok(false)
    }
}

impl BrokerStore {
    fn aur_retire_preview(&self, path: &Path) -> Result<(), BrokerError> {
        let record = self.read_record(path)?;
        if record.status != OperationStatus::Previewed {
            return Err(BrokerError::InvalidTransition);
        }
        let retired = self
            .root
            .join(format!("retired-preview-{}.record", record.operation_id));
        let bytes = self.aur_read_file(path)?;
        match self.aur_read_file(&retired) {
            Ok(existing) if existing == bytes => (),
            Ok(_) => return Err(BrokerError::CorruptRecord),
            Err(BrokerError::NotFound) => self.aur_atomic(&retired, &bytes, true)?,
            Err(e) => return Err(e),
        }
        fs::remove_file(path).map_err(io_error)?;
        File::open(&self.root)
            .and_then(|f| f.sync_all())
            .map_err(io_error)
    }
    fn aur_preserve_replaced_preview(&self, path: &Path) -> Result<(), BrokerError> {
        if !self.aur_is_persistent()? {
            return Ok(());
        }
        match fs::symlink_metadata(path) {
            Ok(_) => self.aur_retire_preview(path),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(io_error(e)),
        }
    }
    fn aur_expire_preview(&self, path: &Path) -> Result<(), BrokerError> {
        if self.aur_is_persistent()? {
            self.aur_retire_preview(path)
        } else {
            remove_owned_record(path, self.expected_owner)
        }
    }
}

#[cfg(test)]
fn aur_persistent_crash_point(store: &BrokerStore, point: &str) {
    if store.aur_is_persistent().unwrap_or(false)
        && std::env::var("ZETHROPOL_AUR_CRASH_POINT").ok().as_deref() == Some(point)
    {
        unsafe { libc::_exit(86) } // deliberate abrupt fixture exit; no Drop runs
    }
}
#[cfg(test)]
mod persistent_tests {
    use super::*;
    include!("persistent_tests.rs");
}

fn aur_checksum(boot: &str, bytes: &[u8]) -> String {
    let mut hash = Sha256::new();
    hash.update(boot.as_bytes());
    hash.update([0]);
    hash.update(bytes);
    format!("{:x}", hash.finalize())
}
impl BrokerStore {
    fn aur_unpack_live_bytes(&self, bytes: &[u8]) -> Result<Vec<u8>, BrokerError> {
        let payload = self.aur_unpack_bytes(bytes)?;
        if self.aur_is_persistent()? {
            let envelope: AurEnvelope =
                serde_json::from_slice(bytes).map_err(|_| BrokerError::CorruptRecord)?;
            let epoch: AurEpoch = serde_json::from_slice(&aur_unwrap(
                &self.aur_read_file(&self.root.join(AUR_EPOCH))?,
            )?)
            .map_err(|_| BrokerError::CorruptRecord)?;
            if envelope.clock_boot_id != epoch.boot_id {
                return Err(BrokerError::Expired);
            }
        }
        Ok(payload)
    }
}
