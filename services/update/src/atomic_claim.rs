//! Pure, single-operation transaction model. No I/O or real
//! authentication, locking, durability or rollback resistance is established.
use super::envelope::{Envelope, Guarantee};
use super::lifecycle::{self, Action, History, Stage};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Purpose {
    AssociateBootEvidence,
    Unsupported,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Request {
    pub operation_id: String,
    pub generation: u64,
    pub binding_sha256: String,
    pub expected_head: u64,
    pub producer_id: String,
    pub purpose: Purpose,
    pub claim_id: String,
}
/// Supplied by a synthetic authority, never verified by this model.
pub struct Authority {
    pub producer_id: String,
    pub assumed_authorized: bool,
    pub assumed_current: bool,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Receipt {
    pub claim_id: String,
    pub committed_head: u64,
    pub source_authenticity: Guarantee,
    pub replay_protection: Guarantee,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Query {
    Recorded(Receipt),
    /// Only under the caller's explicit current-authority assumption. Not a
    /// failed transaction, permission to reuse a reference, or retry instruction.
    NotRecorded,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Fault {
    None,
    BeforeStart,
    DuringValidation,
    ConsumePrepared,
    HeadPrepared,
    BeforeCommit,
    AfterCommitBeforeReply,
    AfterReply,
    IoBeforeCommit,
    IoAmbiguousBeforeCommit,
    IoAmbiguousAfterCommit,
}
macro_rules! reasons {
    ($($variant:ident => $code:literal),+ $(,)?) => {
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub enum Reason { $($variant),+ }
        impl Reason { pub fn code(self) -> &'static str {
            match self { $(Self::$variant => $code),+ }
        } }
    };
}
reasons! {
    Unauthorized => "claim_unauthorized",
    InvalidRequest => "claim_invalid_request",
    WrongOperation => "history_operation_mismatch",
    WrongGeneration => "history_generation_mismatch",
    WrongBinding => "history_binding_mismatch",
    StaleHead => "history_sequence_mismatch",
    Ineligible => "history_invalid_transition",
    Conflict => "claim_conflict",
    Overflow => "history_counter_overflow",
    Corrupt => "claim_store_unavailable",
    Interrupted => "claim_response_lost",
    Indeterminate => "claim_indeterminate"
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ClaimEvent {
    pub request: Request,
    pub receipt: Receipt,
    pub action: Action,
}
/// Synthetic recovery input, NOT an authenticated persistent representation.
/// Full baseline lifecycle events are retained to detect missing history.
#[derive(Clone)]
pub struct Snapshot {
    pub operation_id: String,
    pub generation: u64,
    pub reference: Envelope,
    pub references: Vec<(u64, Envelope)>,
    pub baseline: Vec<(u64, String, u64, String, Action)>,
    pub baseline_head: u64,
    pub stage: Stage,
    pub head: u64,
    pub claims: Vec<ClaimEvent>,
}
/// Private contents prevent callers constructing a prepared authorization.
/// Prepared is still only a proposal; it can be stale at commit time.
pub struct Prepared {
    request: Request,
}
pub struct Machine {
    snapshot: Snapshot,
    quarantined: bool,
}
fn uuid(value: &str) -> bool {
    uuid::Uuid::parse_str(value).is_ok_and(|id| !id.is_nil() && id.to_string() == value)
}
fn hash(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}
fn authorize(authority: &Authority, producer: &str) -> Result<(), Reason> {
    if !authority.assumed_authorized
        || !uuid(&authority.producer_id)
        || authority.producer_id != producer
    {
        return Err(Reason::Unauthorized);
    }
    if !authority.assumed_current {
        return Err(Reason::Indeterminate);
    }
    Ok(())
}
fn request_valid(request: &Request) -> Result<(), Reason> {
    if !uuid(&request.operation_id)
        || !uuid(&request.producer_id)
        || !uuid(&request.claim_id)
        || request.generation == 0
        || !hash(&request.binding_sha256)
        || request.purpose != Purpose::AssociateBootEvidence
    {
        return Err(Reason::InvalidRequest);
    }
    Ok(())
}
impl Machine {
    /// Import the complete synthetic lifecycle, retaining every reference needed
    /// to revalidate earlier generation/revocation history during recovery.
    pub fn from_history(operation: &str, history: &History<'_>) -> Result<Self, Reason> {
        let selected = lifecycle::select(operation, history).map_err(|_| Reason::Corrupt)?;
        let generation = selected
            .active_generation
            .or_else(|| selected.stages.last().map(|row| row.0))
            .ok_or(Reason::Corrupt)?;
        let version = history
            .versions
            .iter()
            .find(|version| version.generation == generation)
            .ok_or(Reason::Corrupt)?;
        let stage = selected
            .stages
            .iter()
            .find(|row| row.0 == generation)
            .ok_or(Reason::Corrupt)?
            .1;
        let snapshot = Snapshot {
            operation_id: operation.into(),
            generation,
            reference: version.reference.expected.clone(),
            references: history
                .versions
                .iter()
                .map(|version| (version.generation, version.reference.expected.clone()))
                .collect(),
            baseline: history
                .events
                .iter()
                .map(|event| {
                    (
                        event.sequence,
                        event.operation_id.clone(),
                        event.generation,
                        event.binding_sha256.clone(),
                        event.action,
                    )
                })
                .collect(),
            baseline_head: history.expected_head,
            stage,
            head: history.expected_head,
            claims: Vec::new(),
        };
        Self::recover(snapshot, true)
    }
    /// `assume_current_complete` is NOT verified. A restored old snapshot can
    /// pass it, explicitly demonstrating the need for external freshness.
    pub fn recover(snapshot: Snapshot, assume_current_complete: bool) -> Result<Self, Reason> {
        if !assume_current_complete {
            return Err(Reason::Indeterminate);
        }
        let machine = Self {
            snapshot,
            quarantined: false,
        };
        machine.check()?;
        Ok(machine)
    }
    pub fn snapshot(&self) -> Snapshot {
        self.snapshot.clone()
    }
    fn check(&self) -> Result<(), Reason> {
        let state = &self.snapshot;
        if state.head == u64::MAX {
            return Err(Reason::Overflow);
        }
        if state.references.is_empty()
            || state.references.len() > lifecycle::MAX_REFERENCES
            || state.baseline.len() > lifecycle::MAX_EVENTS
            || state.claims.len() > 1
        {
            return Err(Reason::Corrupt);
        }
        let references: Vec<_> = state
            .references
            .iter()
            .map(|(_, expected)| super::envelope::BindingReference {
                expected: expected.clone(),
            })
            .collect();
        let versions: Vec<_> = state
            .references
            .iter()
            .zip(&references)
            .map(|((generation, _), reference)| lifecycle::Version {
                generation: *generation,
                reference,
            })
            .collect();
        if !state.references.iter().any(|(generation, expected)| {
            *generation == state.generation && expected == &state.reference
        }) {
            return Err(Reason::Corrupt);
        }
        let events: Vec<_> = state
            .baseline
            .iter()
            .map(|event| lifecycle::Event {
                sequence: event.0,
                operation_id: event.1.clone(),
                generation: event.2,
                binding_sha256: event.3.clone(),
                action: event.4,
            })
            .collect();
        let selected = lifecycle::select(
            &state.operation_id,
            &History {
                assumption: lifecycle::HistoryAssumption::TrustedComplete,
                expected_head: state.baseline_head,
                versions: &versions,
                events: &events,
            },
        )
        .map_err(|_| Reason::Corrupt)?;
        if state.claims.is_empty() {
            if state.head != state.baseline_head
                || Some(state.stage)
                    != selected
                        .stages
                        .iter()
                        .find(|row| row.0 == state.generation)
                        .map(|row| row.1)
            {
                return Err(Reason::Corrupt);
            }
        } else {
            let event = &state.claims[0];
            let head = state.baseline_head.checked_add(1).ok_or(Reason::Overflow)?;
            if selected.active_generation != Some(state.generation)
                || state.reference.payload.commit.result
                    != super::post_reboot_decision::Commit::Committed
                || state.stage != Stage::Consumed
                || state.head != head
                || event.action != Action::Consume
                || event.request.operation_id != state.operation_id
                || event.request.generation != state.generation
                || event.request.binding_sha256 != state.reference.binding_sha256
                || event.request.expected_head != state.baseline_head
                || request_valid(&event.request).is_err()
                || event.receipt != receipt(&event.request, head)
            {
                return Err(Reason::Corrupt);
            }
        }
        Ok(())
    }
    fn ready(&self) -> Result<(), Reason> {
        if self.quarantined {
            return Err(Reason::Indeterminate);
        }
        self.check()
    }
    pub fn query(&self, claim_id: &str, authority: &Authority) -> Result<Query, Reason> {
        authorize(authority, &authority.producer_id)?;
        self.ready()?;
        if !uuid(claim_id) {
            return Err(Reason::InvalidRequest);
        }
        match self
            .snapshot
            .claims
            .iter()
            .find(|event| event.request.claim_id == claim_id)
        {
            Some(event) if event.request.producer_id == authority.producer_id => {
                Ok(Query::Recorded(event.receipt.clone()))
            }
            _ => Ok(Query::NotRecorded), // Do not expose another producer's key.
        }
    }
    pub fn prepare(&self, request: Request, authority: &Authority) -> Result<Prepared, Reason> {
        self.admit(&request, authority)?;
        Ok(Prepared { request })
    }
    fn admit(&self, request: &Request, authority: &Authority) -> Result<Option<Receipt>, Reason> {
        authorize(authority, &request.producer_id)?;
        self.ready()?;
        request_valid(request)?;
        let state = &self.snapshot;
        if let Some(event) = state
            .claims
            .iter()
            .find(|event| event.request.claim_id == request.claim_id)
        {
            return if event.request == *request {
                Ok(Some(event.receipt.clone()))
            } else {
                Err(Reason::Conflict)
            };
        }
        if request.operation_id != state.operation_id {
            return Err(Reason::WrongOperation);
        }
        if request.generation != state.generation {
            return Err(Reason::WrongGeneration);
        }
        if request.binding_sha256 != state.reference.binding_sha256 {
            return Err(Reason::WrongBinding);
        }
        if request.expected_head != state.head {
            return Err(Reason::StaleHead);
        }
        if state.stage != Stage::Active
            || state.reference.payload.commit.result
                != super::post_reboot_decision::Commit::Committed
        {
            return Err(Reason::Ineligible);
        }
        state.head.checked_add(1).ok_or(Reason::Overflow)?;
        Ok(None)
    }
    pub fn commit(
        &mut self,
        prepared: Prepared,
        authority: &Authority,
        fault: Fault,
    ) -> Result<Receipt, Reason> {
        if fault == Fault::BeforeStart {
            return Err(Reason::Interrupted);
        }
        let request = prepared.request;
        // Logical compare-and-swap: repeat admission against current state.
        if let Some(receipt) = self.admit(&request, authority)? {
            return Ok(receipt);
        }
        if fault == Fault::DuringValidation {
            return Err(Reason::Interrupted);
        }
        let mut next = self.snapshot.clone();
        next.stage = Stage::Consumed;
        if fault == Fault::ConsumePrepared {
            return Err(Reason::Interrupted);
        }
        next.head = next.head.checked_add(1).ok_or(Reason::Overflow)?;
        if fault == Fault::HeadPrepared {
            return Err(Reason::Interrupted);
        }
        let result = receipt(&request, next.head);
        next.claims.push(ClaimEvent {
            request,
            receipt: result.clone(),
            action: Action::Consume,
        });
        if fault == Fault::BeforeCommit {
            return Err(Reason::Interrupted);
        }
        if matches!(
            fault,
            Fault::IoBeforeCommit | Fault::IoAmbiguousBeforeCommit
        ) {
            self.quarantined = true;
            return Err(Reason::Indeterminate);
        }
        // Sole publication point: no staged partial state escapes this function.
        self.snapshot = next;
        if fault == Fault::IoAmbiguousAfterCommit {
            self.quarantined = true;
            return Err(Reason::Indeterminate);
        }
        if fault == Fault::AfterCommitBeforeReply {
            return Err(Reason::Interrupted);
        }
        // AfterReply represents a crash AFTER the returned receipt was observed.
        Ok(result)
    }
}
fn receipt(request: &Request, head: u64) -> Receipt {
    Receipt {
        claim_id: request.claim_id.clone(),
        committed_head: head,
        source_authenticity: Guarantee::NotEstablished,
        replay_protection: Guarantee::NotEstablished,
    }
}
