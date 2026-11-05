//! Pure replay of a bounded synthetic history. No storage or authority checks.
//! Caller-supplied completeness/trust/head are assumptions, not established facts.
use super::envelope::{binding_digest, BindingReference, Guarantee};

pub const MAX_EVENTS: usize = 1024;
pub const MAX_REFERENCES: usize = 256;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage {
    Candidate,
    Active,
    Consumed,
    Revoked,
    Superseded,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    Create,
    Activate,
    Consume,
    Revoke,
    Supersede,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HistoryAssumption {
    TrustedComplete,
    Untrusted,
    Incomplete,
}

pub struct Version<'a> {
    pub generation: u64,
    pub reference: &'a BindingReference,
}
pub struct Event {
    pub sequence: u64,
    pub operation_id: String,
    pub generation: u64,
    pub binding_sha256: String,
    pub action: Action,
}
pub struct History<'a> {
    pub assumption: HistoryAssumption,
    /// Independent expected final sequence; not inferred from the event tail.
    pub expected_head: u64,
    pub versions: &'a [Version<'a>],
    pub events: &'a [Event],
}
#[derive(Debug, PartialEq, Eq)]
pub struct Selection {
    /// Logical selector only; not a trusted reference or authorization token.
    pub active_generation: Option<u64>,
    pub stages: Vec<(u64, Stage)>,
    pub source_authenticity: Guarantee,
    pub replay_protection: Guarantee,
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
    Untrusted => "history_untrusted",
    Incomplete => "history_incomplete",
    Limit => "history_limit",
    InvalidOperation => "history_invalid_operation",
    InvalidReference => "history_invalid_reference",
    OperationMismatch => "history_operation_mismatch",
    BindingMismatch => "history_binding_mismatch",
    GenerationMismatch => "history_generation_mismatch",
    SequenceMismatch => "history_sequence_mismatch",
    CounterOverflow => "history_counter_overflow",
    MultipleActive => "history_multiple_active",
    InvalidTransition => "history_invalid_transition",
    OlderGeneration => "history_older_generation",
    MissingRevocation => "history_missing_revocation"
}
fn uuid(value: &str) -> bool {
    uuid::Uuid::parse_str(value).is_ok_and(|id| !id.is_nil() && id.to_string() == value)
}
fn successor(value: u64) -> Result<u64, Reason> {
    value.checked_add(1).ok_or(Reason::CounterOverflow)
}

pub fn select(operation_id: &str, history: &History<'_>) -> Result<Selection, Reason> {
    match history.assumption {
        HistoryAssumption::Untrusted => return Err(Reason::Untrusted),
        HistoryAssumption::Incomplete => return Err(Reason::Incomplete),
        HistoryAssumption::TrustedComplete => {}
    }
    if !uuid(operation_id) {
        return Err(Reason::InvalidOperation);
    }
    if history.events.len() > MAX_EVENTS || history.versions.len() > MAX_REFERENCES {
        return Err(Reason::Limit);
    }
    if history.expected_head != history.events.len() as u64 {
        return Err(Reason::Incomplete);
    }
    // All versions are declared in generation order from 1. No mutable snapshot
    // import: every state is reconstructed from the complete event sequence.
    for (index, version) in history.versions.iter().enumerate() {
        successor(version.generation)?;
        if version.generation != index as u64 + 1 {
            return Err(Reason::GenerationMismatch);
        }
        let reference = &version.reference.expected;
        if reference.payload.operation_id != operation_id {
            return Err(Reason::OperationMismatch);
        }
        if binding_digest(&reference.payload).map_err(|_| Reason::InvalidReference)?
            != reference.binding_sha256
        {
            return Err(Reason::InvalidReference);
        }
    }
    let mut stages: Vec<(u64, Stage)> = Vec::new();
    let mut active = None;
    for (index, event) in history.events.iter().enumerate() {
        successor(event.sequence)?;
        successor(event.generation)?;
        if event.sequence != index as u64 + 1 {
            return Err(Reason::SequenceMismatch);
        }
        if event.operation_id != operation_id {
            return Err(Reason::OperationMismatch);
        }
        let version = history
            .versions
            .iter()
            .find(|v| v.generation == event.generation)
            .ok_or(Reason::GenerationMismatch)?;
        if event.binding_sha256 != version.reference.expected.binding_sha256 {
            return Err(Reason::BindingMismatch);
        }
        if event.action == Action::Create {
            if event.generation != successor(stages.len() as u64)? {
                return Err(Reason::GenerationMismatch);
            }
            // Creating a successor does NOT revoke an earlier active reference.
            stages.push((event.generation, Stage::Candidate));
            continue;
        }
        let newest = stages.last().map(|v| v.0);
        let position = stages
            .iter()
            .position(|v| v.0 == event.generation)
            .ok_or(Reason::Incomplete)?;
        let current = stages[position].1;
        let next = match event.action {
            Action::Activate => {
                if current != Stage::Candidate {
                    return Err(Reason::InvalidTransition);
                }
                if active.is_some() {
                    return Err(Reason::MultipleActive);
                }
                if newest != Some(event.generation) {
                    return Err(Reason::OlderGeneration);
                }
                Stage::Active
            }
            Action::Consume if current == Stage::Active => Stage::Consumed,
            Action::Revoke if matches!(current, Stage::Candidate | Stage::Active) => Stage::Revoked,
            // Explicit Revoke event is required first. A newer active generation
            // alone never manufactures revocation evidence for its predecessor.
            Action::Supersede => {
                if current != Stage::Revoked {
                    return Err(Reason::MissingRevocation);
                }
                if !active.is_some_and(|generation| generation > event.generation) {
                    return Err(Reason::InvalidTransition);
                }
                Stage::Superseded
            }
            _ => return Err(Reason::InvalidTransition),
        };
        stages[position].1 = next;
        if next == Stage::Active {
            active = Some(event.generation);
        } else if active == Some(event.generation) {
            active = None;
        }
    }
    if stages.len() != history.versions.len() {
        return Err(Reason::Incomplete);
    }
    Ok(Selection {
        active_generation: active,
        stages,
        source_authenticity: Guarantee::NotEstablished,
        replay_protection: Guarantee::NotEstablished,
    })
}
