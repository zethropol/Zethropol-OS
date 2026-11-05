#[allow(dead_code)]
#[path = "../src/post_reboot_envelope.rs"]
mod envelope;
#[allow(dead_code)]
#[path = "../src/reference_lifecycle.rs"]
mod lifecycle;
#[path = "../src/atomic_claim.rs"]
mod model;
#[allow(dead_code)]
#[path = "../src/post_reboot_decision.rs"]
mod post_reboot_decision;
use envelope::*;
use lifecycle::{Action, Event, History, HistoryAssumption, Stage, Version};
use model::Reason;
use model::*;
use post_reboot_decision::Commit;
const OP: &str = "1096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e";
const PRODUCER: &str = "2096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e";
const CLAIM: &str = "3096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e";
const OTHER: &str = "4096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e";
fn fixture() -> (Machine, Request, Authority) {
    let payload = Payload {
        schema_version: 1,
        operation_id: OP.into(),
        origin_boot_id: PRODUCER.into(),
        commit: CommitSummary {
            result: Commit::Committed,
            checkpoint_sha256: "a".repeat(64),
            approved_plan_sha256: "b".repeat(64),
        },
        target: Target {
            kernel_release: "synthetic".into(),
            kernel_sha256: "a".repeat(64),
            initramfs_sha256: "b".repeat(64),
            loader_sha256: "c".repeat(64),
            config_sha256: "d".repeat(64),
            entry_id: OP.into(),
            root_filesystem_id: PRODUCER.into(),
            root_subvolume_id: OP.into(),
        },
        producer: Producer::Synthetic,
        claimed_trust: ClaimedTrust::Unverified,
    };
    let binding_sha256 = binding_digest(&payload).unwrap();
    let reference = BindingReference {
        expected: Envelope {
            payload,
            binding_sha256: binding_sha256.clone(),
        },
    };
    let versions = [Version {
        generation: 1,
        reference: &reference,
    }];
    let events: Vec<_> = [Action::Create, Action::Activate]
        .iter()
        .enumerate()
        .map(|(i, action)| Event {
            sequence: i as u64 + 1,
            operation_id: OP.into(),
            generation: 1,
            binding_sha256: binding_sha256.clone(),
            action: *action,
        })
        .collect();
    let machine = Machine::from_history(
        OP,
        &History {
            assumption: HistoryAssumption::TrustedComplete,
            expected_head: 2,
            versions: &versions,
            events: &events,
        },
    )
    .unwrap();
    let request = Request {
        operation_id: OP.into(),
        generation: 1,
        binding_sha256,
        expected_head: 2,
        producer_id: PRODUCER.into(),
        purpose: Purpose::AssociateBootEvidence,
        claim_id: CLAIM.into(),
    };
    let authority = Authority {
        producer_id: PRODUCER.into(),
        assumed_authorized: true,
        assumed_current: true,
    };
    (machine, request, authority)
}
fn attempt(
    machine: &mut Machine,
    request: Request,
    authority: &Authority,
    fault: Fault,
) -> Result<Receipt, Reason> {
    let pending = machine.prepare(request, authority)?;
    machine.commit(pending, authority, fault)
}
#[test]
fn single_claim_is_one_complete_transition_and_query_is_idempotent() {
    let (mut machine, request, authority) = fixture();
    let before = machine.snapshot();
    let pending = machine.prepare(request.clone(), &authority).unwrap();
    assert_eq!(machine.snapshot().head, before.head);
    assert!(machine.snapshot().claims.is_empty());
    let receipt = machine.commit(pending, &authority, Fault::None).unwrap();
    let after = machine.snapshot();
    assert_eq!(after.stage, Stage::Consumed);
    assert_eq!(after.head, 3);
    assert_eq!(after.claims.len(), 1);
    assert_eq!(after.claims[0].action, Action::Consume);
    assert_eq!(receipt.source_authenticity, Guarantee::NotEstablished);
    assert_eq!(receipt.replay_protection, Guarantee::NotEstablished);
    for _ in 0..2 {
        assert_eq!(
            machine.query(CLAIM, &authority),
            Ok(Query::Recorded(receipt.clone()))
        );
    }
    assert_eq!(
        attempt(&mut machine, request, &authority, Fault::None),
        Ok(receipt)
    );
    assert_eq!(machine.snapshot().head, 3);
    assert_eq!(machine.snapshot().claims.len(), 1);
}
#[test]
fn two_prepared_contenders_only_one_commits_in_either_order() {
    for reverse in [false, true] {
        let (mut machine, request, authority) = fixture();
        let mut other = request.clone();
        other.claim_id = OTHER.into();
        let a = machine.prepare(request, &authority).unwrap();
        let b = machine.prepare(other, &authority).unwrap();
        let (first, second) = if reverse { (b, a) } else { (a, b) };
        assert!(machine.commit(first, &authority, Fault::None).is_ok());
        assert_eq!(
            machine.commit(second, &authority, Fault::None),
            Err(Reason::StaleHead)
        );
        assert_eq!(machine.snapshot().claims.len(), 1);
    }
}
#[test]
fn same_key_changed_inputs_conflict() {
    let (mut machine, request, authority) = fixture();
    attempt(&mut machine, request.clone(), &authority, Fault::None).unwrap();
    for index in 0..5 {
        let mut changed = request.clone();
        match index {
            0 => changed.expected_head = 3,
            1 => changed.generation = 2,
            2 => changed.binding_sha256 = "0".repeat(64),
            3 => changed.operation_id = OTHER.into(),
            _ => changed.producer_id = OTHER.into(),
        }
        let context = Authority {
            producer_id: changed.producer_id.clone(),
            assumed_authorized: true,
            assumed_current: true,
        };
        assert_eq!(
            attempt(&mut machine, changed, &context, Fault::None),
            Err(Reason::Conflict)
        );
    }
}
#[test]
fn invalid_selectors_and_purpose_are_rejected_without_mutation() {
    for (index, reason) in [
        (0, Reason::WrongOperation),
        (1, Reason::WrongGeneration),
        (2, Reason::WrongBinding),
        (3, Reason::StaleHead),
        (4, Reason::InvalidRequest),
    ] {
        let (mut machine, mut request, authority) = fixture();
        match index {
            0 => request.operation_id = OTHER.into(),
            1 => request.generation = 2,
            2 => request.binding_sha256 = "0".repeat(64),
            3 => request.expected_head = 1,
            _ => request.purpose = Purpose::Unsupported,
        }
        assert_eq!(
            attempt(&mut machine, request, &authority, Fault::None),
            Err(reason)
        );
        assert_eq!(machine.snapshot().stage, Stage::Active);
        assert!(!reason.code().is_empty());
    }
}
#[test]
fn unauthorized_and_revoked_producer_cannot_commit_or_query() {
    let (mut machine, request, mut authority) = fixture();
    let pending = machine.prepare(request.clone(), &authority).unwrap();
    authority.assumed_authorized = false;
    assert_eq!(
        machine.commit(pending, &authority, Fault::None),
        Err(Reason::Unauthorized)
    );
    assert_eq!(machine.query(CLAIM, &authority), Err(Reason::Unauthorized));
    authority.assumed_authorized = true;
    authority.producer_id = OTHER.into();
    assert_eq!(
        attempt(&mut machine, request, &authority, Fault::None),
        Err(Reason::Unauthorized)
    );
}
#[test]
fn precommit_crashes_publish_no_partial_state() {
    for fault in [
        Fault::BeforeStart,
        Fault::DuringValidation,
        Fault::ConsumePrepared,
        Fault::HeadPrepared,
        Fault::BeforeCommit,
    ] {
        let (mut machine, request, authority) = fixture();
        assert_eq!(
            attempt(&mut machine, request.clone(), &authority, fault),
            Err(Reason::Interrupted)
        );
        let state = machine.snapshot();
        assert_eq!(state.stage, Stage::Active);
        assert_eq!(state.head, 2);
        assert!(state.claims.is_empty());
        let mut recovered = Machine::recover(state, true).unwrap();
        assert_eq!(recovered.query(CLAIM, &authority), Ok(Query::NotRecorded));
        assert!(attempt(&mut recovered, request, &authority, Fault::None).is_ok());
    }
}
#[test]
fn committed_response_loss_and_after_reply_survive_synthetic_recovery() {
    for fault in [Fault::AfterCommitBeforeReply, Fault::AfterReply] {
        let (mut machine, request, authority) = fixture();
        let response = attempt(&mut machine, request.clone(), &authority, fault);
        if fault == Fault::AfterCommitBeforeReply {
            assert_eq!(response, Err(Reason::Interrupted));
        } else {
            assert!(response.is_ok());
        }
        let mut recovered = Machine::recover(machine.snapshot(), true).unwrap();
        let Query::Recorded(receipt) = recovered.query(CLAIM, &authority).unwrap() else {
            panic!("missing receipt")
        };
        assert_eq!(
            attempt(&mut recovered, request, &authority, Fault::None),
            Ok(receipt)
        );
        assert_eq!(recovered.snapshot().claims.len(), 1);
    }
}
#[test]
fn io_uncertainty_quarantines_both_possible_commit_outcomes() {
    for fault in [
        Fault::IoBeforeCommit,
        Fault::IoAmbiguousBeforeCommit,
        Fault::IoAmbiguousAfterCommit,
    ] {
        let (mut machine, request, authority) = fixture();
        assert_eq!(
            attempt(&mut machine, request.clone(), &authority, fault),
            Err(Reason::Indeterminate)
        );
        assert_eq!(machine.query(CLAIM, &authority), Err(Reason::Indeterminate));
        let mut other = request.clone();
        other.claim_id = OTHER.into();
        assert_eq!(
            attempt(&mut machine, other, &authority, Fault::None),
            Err(Reason::Indeterminate)
        );
        assert!(matches!(
            Machine::recover(machine.snapshot(), false),
            Err(Reason::Indeterminate)
        ));
        let recovered = Machine::recover(machine.snapshot(), true).unwrap();
        if fault == Fault::IoAmbiguousAfterCommit {
            assert!(matches!(
                recovered.query(CLAIM, &authority),
                Ok(Query::Recorded(_))
            ));
        } else {
            assert_eq!(recovered.query(CLAIM, &authority), Ok(Query::NotRecorded));
        }
    }
}
#[test]
fn head_overflow_missing_and_corrupt_history_fail_closed() {
    for index in 0..5 {
        let (machine, _, _) = fixture();
        let mut state = machine.snapshot();
        match index {
            0 => state.head = u64::MAX,
            1 => {
                state.baseline.pop();
            }
            2 => state.stage = Stage::Consumed,
            3 => state.baseline[1].0 = 1,
            _ => state.head = 0,
        }
        assert!(matches!(
            Machine::recover(state, true),
            Err(Reason::Corrupt | Reason::Overflow)
        ));
    }
    let (mut machine, request, authority) = fixture();
    attempt(&mut machine, request, &authority, Fault::None).unwrap();
    let mut state = machine.snapshot();
    state.claims[0].receipt.committed_head = 999;
    assert!(matches!(
        Machine::recover(state, true),
        Err(Reason::Corrupt)
    ));
}
#[test]
fn inactive_references_and_unknown_commit_cannot_be_claimed() {
    // Model admission handles valid revoked/consumed baselines; forged superseded
    // state without the required successor history is rejected as corrupt.
    for stage in [
        Stage::Candidate,
        Stage::Revoked,
        Stage::Consumed,
        Stage::Superseded,
    ] {
        let (machine, request, authority) = fixture();
        let mut state = machine.snapshot();
        match stage {
            Stage::Candidate => {
                state.baseline.pop();
                state.head = 1;
                state.baseline_head = 1;
            }
            Stage::Revoked | Stage::Consumed => {
                state.baseline.push((
                    3,
                    OP.into(),
                    1,
                    request.binding_sha256.clone(),
                    if stage == Stage::Revoked {
                        Action::Revoke
                    } else {
                        Action::Consume
                    },
                ));
                state.head = 3;
                state.baseline_head = 3;
            }
            _ => {}
        }
        state.stage = stage;
        match Machine::recover(state, true) {
            Ok(mut recovered) => {
                let mut current = request;
                current.expected_head = recovered.snapshot().head;
                assert_eq!(
                    attempt(&mut recovered, current, &authority, Fault::None),
                    Err(Reason::Ineligible)
                );
            }
            Err(reason) => assert_eq!(reason, Reason::Corrupt),
        }
    }
    for commit in [Commit::Unknown, Commit::FailedMayBePartial] {
        let (machine, mut request, authority) = fixture();
        let mut state = machine.snapshot();
        state.reference.payload.commit.result = commit;
        state.reference.binding_sha256 = binding_digest(&state.reference.payload).unwrap();
        request.binding_sha256 = state.reference.binding_sha256.clone();
        state.references[0].1 = state.reference.clone();
        for event in &mut state.baseline {
            event.3 = request.binding_sha256.clone();
        }
        let mut recovered = Machine::recover(state, true).unwrap();
        assert_eq!(
            attempt(&mut recovered, request, &authority, Fault::None),
            Err(Reason::Ineligible)
        );
    }
}
#[test]
fn replayed_old_snapshot_is_not_rollback_protected() {
    let (mut machine, request, authority) = fixture();
    let old = machine.snapshot();
    let first = attempt(&mut machine, request.clone(), &authority, Fault::None).unwrap();
    assert_eq!(first.replay_protection, Guarantee::NotEstablished);
    // Counterexample: asserting freshness for a restored old snapshot can admit
    // a new key. No independent anchor is present in this pure model.
    let mut restored = Machine::recover(old, true).unwrap();
    let mut replay = request;
    replay.claim_id = OTHER.into();
    assert!(attempt(&mut restored, replay, &authority, Fault::None).is_ok());
    let no_freshness = Authority {
        producer_id: PRODUCER.into(),
        assumed_authorized: true,
        assumed_current: false,
    };
    assert_eq!(
        restored.query(OTHER, &no_freshness),
        Err(Reason::Indeterminate)
    );
}

#[test]
fn superseded_generation_and_missing_prior_references_fail_closed() {
    let (machine, mut request, authority) = fixture();
    let mut state = machine.snapshot();
    let mut second = state.reference.clone();
    second.payload.target.kernel_release = "new-synthetic".into();
    second.binding_sha256 = binding_digest(&second.payload).unwrap();
    state.references.push((2, second.clone()));
    for (sequence, generation, hash, action) in [
        (3, 2, second.binding_sha256.clone(), Action::Create),
        (4, 1, state.reference.binding_sha256.clone(), Action::Revoke),
        (5, 2, second.binding_sha256.clone(), Action::Activate),
        (
            6,
            1,
            state.reference.binding_sha256.clone(),
            Action::Supersede,
        ),
    ] {
        state
            .baseline
            .push((sequence, OP.into(), generation, hash, action));
    }
    state.reference = second;
    state.generation = 2;
    state.head = 6;
    state.baseline_head = 6;
    let mut recovered = Machine::recover(state.clone(), true).unwrap();
    assert_eq!(
        attempt(&mut recovered, request.clone(), &authority, Fault::None),
        Err(Reason::WrongGeneration)
    );
    request.generation = 2;
    request.expected_head = 6;
    request.binding_sha256 = state.reference.binding_sha256.clone();
    assert!(attempt(&mut recovered, request, &authority, Fault::None).is_ok());
    state.references.remove(0);
    assert!(matches!(
        Machine::recover(state, true),
        Err(Reason::Corrupt)
    ));
}
#[test]
fn prepared_claim_is_rechecked_after_reference_revocation() {
    let (machine, request, authority) = fixture();
    let prepared = machine.prepare(request.clone(), &authority).unwrap();
    let mut state = machine.snapshot();
    state.baseline.push((
        3,
        OP.into(),
        1,
        request.binding_sha256.clone(),
        Action::Revoke,
    ));
    state.stage = Stage::Revoked;
    state.head = 3;
    state.baseline_head = 3;
    let mut revoked = Machine::recover(state, true).unwrap();
    assert_eq!(
        revoked.commit(prepared, &authority, Fault::None),
        Err(Reason::StaleHead)
    );
    assert!(revoked.snapshot().claims.is_empty());
}
#[test]
fn same_prepared_claim_returns_one_receipt_for_both_logical_contenders() {
    let (mut machine, request, authority) = fixture();
    let first = machine.prepare(request.clone(), &authority).unwrap();
    let second = machine.prepare(request, &authority).unwrap();
    let receipt = machine.commit(first, &authority, Fault::None).unwrap();
    assert_eq!(machine.commit(second, &authority, Fault::None), Ok(receipt));
    assert_eq!(machine.snapshot().head, 3);
    assert_eq!(machine.snapshot().claims.len(), 1);
    let other = Authority {
        producer_id: OTHER.into(),
        assumed_authorized: true,
        assumed_current: true,
    };
    assert_eq!(machine.query(CLAIM, &other), Ok(Query::NotRecorded));
}
