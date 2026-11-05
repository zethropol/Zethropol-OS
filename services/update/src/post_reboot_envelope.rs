//! Bounded schema validation only. No I/O, authenticity or replay verification.
//! Intentionally not linked into the production service or decision pipeline.
use super::post_reboot_decision::Commit;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

pub const MAX_BYTES: usize = 4096;
pub const SCHEMA_VERSION: u32 = 1;

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Target {
    pub kernel_release: String,
    pub kernel_sha256: String,
    pub initramfs_sha256: String,
    pub loader_sha256: String,
    pub config_sha256: String,
    /// Opaque UUID, never a boot-config path or title.
    pub entry_id: String,
    pub root_filesystem_id: String,
    pub root_subvolume_id: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CommitSummary {
    pub result: Commit,
    /// Claimed digest of a future durable checkpoint, not proof of its existence.
    pub checkpoint_sha256: String,
    pub approved_plan_sha256: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Producer {
    Synthetic,
    UpdateWorker,
}

/// Only an unverified claim is representable on this untrusted wire boundary.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ClaimedTrust {
    Unverified,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Payload {
    pub schema_version: u32,
    pub operation_id: String,
    pub commit: CommitSummary,
    pub origin_boot_id: String,
    pub target: Target,
    pub producer: Producer,
    pub claimed_trust: ClaimedTrust,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Envelope {
    pub payload: Payload,
    /// SHA-256 over domain + fixed-order serialized typed Payload.
    /// Not a signature, MAC, capability, nonce or replay defense.
    pub binding_sha256: String,
}

/// Must come from an independent future trusted layer, never the envelope itself.
/// This validator does not authenticate the anchor either.
pub struct Anchor {
    pub operation_id: String,
    pub binding_sha256: String,
    pub origin_boot_id: String,
    pub target: Target,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Guarantee {
    NotEstablished,
}
#[derive(Debug, PartialEq, Eq)]
pub struct Validated {
    pub commit: Commit,
    pub source_authenticity: Guarantee,
    pub replay_protection: Guarantee,
}
impl Validated {
    /// A schema-level claim only. Callers must not use this as authorization.
    pub fn claims_committed(&self) -> bool {
        self.commit == Commit::Committed
    }
}

macro_rules! reasons {
    ($($variant:ident => $code:literal),+ $(,)?) => {
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub enum Reason { $($variant),+ }
        impl Reason {
            pub fn code(self) -> &'static str {
                match self { $(Self::$variant => $code),+ }
            }
        }
    };
}
reasons! {
    Empty => "envelope_empty",
    TooLarge => "envelope_too_large",
    InvalidSchema => "envelope_invalid_schema",
    UnsupportedVersion => "envelope_unsupported_version",
    InvalidIdentity => "envelope_invalid_identity",
    InvalidDigest => "envelope_invalid_digest",
    InvalidKernel => "envelope_invalid_kernel",
    InvalidAnchor => "envelope_invalid_anchor",
    OperationMismatch => "envelope_operation_mismatch",
    BootMismatch => "envelope_boot_mismatch",
    TargetMismatch => "envelope_target_mismatch",
    BindingMismatch => "envelope_binding_mismatch",
    MissingReference => "reference_missing",
    InvalidReference => "reference_invalid",
    ReferenceSchemaMismatch => "reference_schema_mismatch",
    CommitMismatch => "reference_commit_mismatch",
    ProducerMismatch => "reference_producer_mismatch"
}
fn uuid(value: &str) -> bool {
    uuid::Uuid::parse_str(value).is_ok_and(|id| !id.is_nil() && id.to_string() == value)
}
fn digest(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}
fn target_valid(target: &Target) -> Result<(), Reason> {
    if !uuid(&target.entry_id)
        || !uuid(&target.root_filesystem_id)
        || !uuid(&target.root_subvolume_id)
    {
        return Err(Reason::InvalidIdentity);
    }
    if target.kernel_release.is_empty()
        || target.kernel_release.len() > 128
        || !target
            .kernel_release
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"._+-".contains(&c))
    {
        return Err(Reason::InvalidKernel);
    }
    for hash in [
        &target.kernel_sha256,
        &target.initramfs_sha256,
        &target.loader_sha256,
        &target.config_sha256,
    ] {
        if !digest(hash) {
            return Err(Reason::InvalidDigest);
        }
    }
    Ok(())
}
fn payload_valid(payload: &Payload) -> Result<(), Reason> {
    if payload.schema_version != SCHEMA_VERSION {
        return Err(Reason::UnsupportedVersion);
    }
    if !uuid(&payload.operation_id) || !uuid(&payload.origin_boot_id) {
        return Err(Reason::InvalidIdentity);
    }
    if !digest(&payload.commit.checkpoint_sha256) || !digest(&payload.commit.approved_plan_sha256) {
        return Err(Reason::InvalidDigest);
    }
    target_valid(&payload.target)
}

/// Public for synthetic authors/tests. Validates sizes before allocating encoded
/// bytes. Whitespace and JSON key order do not affect the typed canonical hash.
pub fn binding_digest(payload: &Payload) -> Result<String, Reason> {
    payload_valid(payload)?;
    let bytes = serde_json::to_vec(payload).map_err(|_| Reason::InvalidSchema)?;
    let mut hash = Sha256::new();
    hash.update(b"zethropol-post-reboot-envelope-v1\0");
    hash.update(bytes);
    Ok(format!("{:x}", hash.finalize()))
}

pub fn validate(bytes: &[u8], anchor: &Anchor) -> Result<Validated, Reason> {
    if bytes.is_empty() {
        return Err(Reason::Empty);
    }
    if bytes.len() > MAX_BYTES {
        return Err(Reason::TooLarge);
    }
    // No Value intermediate: duplicate, unknown, missing fields and bad types
    // are rejected by typed serde. Default recursion limits remain enabled.
    let envelope: Envelope = serde_json::from_slice(bytes).map_err(|_| Reason::InvalidSchema)?;
    payload_valid(&envelope.payload)?;
    if !digest(&envelope.binding_sha256) {
        return Err(Reason::InvalidDigest);
    }
    if !uuid(&anchor.operation_id)
        || !uuid(&anchor.origin_boot_id)
        || !digest(&anchor.binding_sha256)
        || target_valid(&anchor.target).is_err()
    {
        return Err(Reason::InvalidAnchor);
    }
    let payload = &envelope.payload;
    if payload.operation_id != anchor.operation_id {
        return Err(Reason::OperationMismatch);
    }
    if payload.origin_boot_id != anchor.origin_boot_id {
        return Err(Reason::BootMismatch);
    }
    if payload.target != anchor.target {
        return Err(Reason::TargetMismatch);
    }
    if envelope.binding_sha256 != anchor.binding_sha256
        || binding_digest(payload)? != envelope.binding_sha256
    {
        return Err(Reason::BindingMismatch);
    }
    Ok(Validated {
        commit: payload.commit.result,
        source_authenticity: Guarantee::NotEstablished,
        replay_protection: Guarantee::NotEstablished,
    })
}

/// Expected independent reference, reusing the complete versioned envelope
/// rather than duplicating commit/target/digest fields. This wrapper carries NO
/// trust marker: constructing it, including by copying a candidate, establishes
/// neither independence nor authority. A future external layer must do that.
pub struct BindingReference {
    pub expected: Envelope,
}

/// Compare only. No source authentication, replay detection, current-reference
/// selection, or persistent integrity guarantee is possible in this pure layer.
/// A stale reference is rejected when it differs from the candidate; matching
/// stale copies cannot be distinguished without an external freshness authority.
pub fn compare_reference(
    bytes: &[u8],
    reference: Option<&BindingReference>,
) -> Result<Validated, Reason> {
    let reference = &reference.ok_or(Reason::MissingReference)?.expected;
    if binding_digest(&reference.payload).map_err(|_| Reason::InvalidReference)?
        != reference.binding_sha256
    {
        return Err(Reason::InvalidReference);
    }
    if bytes.is_empty() {
        return Err(Reason::Empty);
    }
    if bytes.len() > MAX_BYTES {
        return Err(Reason::TooLarge);
    }
    let candidate: Envelope = serde_json::from_slice(bytes).map_err(|_| Reason::InvalidSchema)?;
    if candidate.payload.schema_version != reference.payload.schema_version {
        return Err(Reason::ReferenceSchemaMismatch);
    }
    // Reuse schema, limits, full target equality and canonical digest checks.
    let anchor = Anchor {
        operation_id: reference.payload.operation_id.clone(),
        binding_sha256: reference.binding_sha256.clone(),
        origin_boot_id: reference.payload.origin_boot_id.clone(),
        target: reference.payload.target.clone(),
    };
    // Report component differences before the binding hash mismatch, without
    // copying either record's fields into public errors.
    payload_valid(&candidate.payload)?;
    if candidate.payload.operation_id != anchor.operation_id {
        return Err(Reason::OperationMismatch);
    }
    if candidate.payload.commit != reference.payload.commit {
        return Err(Reason::CommitMismatch);
    }
    if candidate.payload.origin_boot_id != anchor.origin_boot_id {
        return Err(Reason::BootMismatch);
    }
    if candidate.payload.target != anchor.target {
        return Err(Reason::TargetMismatch);
    }
    if candidate.payload.producer != reference.payload.producer
        || candidate.payload.claimed_trust != reference.payload.claimed_trust
    {
        return Err(Reason::ProducerMismatch);
    }
    validate(bytes, &anchor)
}
