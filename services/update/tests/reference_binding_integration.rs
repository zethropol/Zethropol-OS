//! Test-only composition. TrustedComplete is a synthetic assumption, never an
//! authenticated history. No production service, decision flag or action used.
#[allow(dead_code)]
#[path = "../src/post_reboot_envelope.rs"]
mod envelope;
#[allow(dead_code)]
#[path = "../src/reference_lifecycle.rs"]
mod lifecycle;
#[allow(dead_code)]
#[path = "../src/post_reboot_decision.rs"]
mod post_reboot_decision;
use envelope::{BindingReference, Guarantee, Validated};
use lifecycle::{Action, Event, History, HistoryAssumption, Version};
use post_reboot_decision::Commit;

const OP: &str = "1096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e";
const BOOT: &str = "2096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e";
const MATCH: &str = "synthetic_active_reference_match";

fn compare_active(
    bytes: &[u8],
    operation: &str,
    generation: u64,
    history: &History<'_>,
) -> Result<(Validated, &'static str), &'static str> {
    let selected = lifecycle::select(operation, history).map_err(lifecycle::Reason::code)?;
    let active = selected
        .active_generation
        .ok_or(envelope::Reason::MissingReference.code())?;
    if generation != active {
        return Err(if generation < active {
            lifecycle::Reason::OlderGeneration.code()
        } else {
            lifecycle::Reason::GenerationMismatch.code()
        });
    }
    let version = history
        .versions
        .iter()
        .find(|version| version.generation == active)
        .ok_or(lifecycle::Reason::Incomplete.code())?;
    if version.reference.expected.payload.operation_id != operation {
        return Err(lifecycle::Reason::OperationMismatch.code());
    }
    let result = envelope::compare_reference(bytes, Some(version.reference))
        .map_err(envelope::Reason::code)?;
    Ok((result, MATCH))
}

fn reference() -> BindingReference {
    use envelope::*;
    let payload = Payload {
        schema_version: 1,
        operation_id: OP.into(),
        origin_boot_id: BOOT.into(),
        commit: CommitSummary {
            result: Commit::Committed,
            checkpoint_sha256: "a".repeat(64),
            approved_plan_sha256: "b".repeat(64),
        },
        target: Target {
            kernel_release: "synthetic-kernel".into(),
            kernel_sha256: "a".repeat(64),
            initramfs_sha256: "b".repeat(64),
            loader_sha256: "c".repeat(64),
            config_sha256: "d".repeat(64),
            entry_id: OP.into(),
            root_filesystem_id: BOOT.into(),
            root_subvolume_id: OP.into(),
        },
        producer: Producer::Synthetic,
        claimed_trust: ClaimedTrust::Unverified,
    };
    let binding_sha256 = binding_digest(&payload).unwrap();
    BindingReference {
        expected: Envelope {
            payload,
            binding_sha256,
        },
    }
}
fn encode(reference: &BindingReference) -> Vec<u8> {
    serde_json::to_vec(&reference.expected).unwrap()
}
fn events(refs: &[BindingReference], actions: &[(u64, Action)]) -> Vec<Event> {
    actions
        .iter()
        .enumerate()
        .map(|(index, (generation, action))| Event {
            sequence: index as u64 + 1,
            operation_id: OP.into(),
            generation: *generation,
            binding_sha256: refs[*generation as usize - 1]
                .expected
                .binding_sha256
                .clone(),
            action: *action,
        })
        .collect()
}
fn run(
    refs: &[BindingReference],
    log: &[Event],
    generation: u64,
    bytes: &[u8],
) -> Result<(Validated, &'static str), &'static str> {
    let versions: Vec<_> = refs
        .iter()
        .enumerate()
        .map(|(index, reference)| Version {
            generation: index as u64 + 1,
            reference,
        })
        .collect();
    compare_active(
        bytes,
        OP,
        generation,
        &History {
            assumption: HistoryAssumption::TrustedComplete,
            expected_head: log.len() as u64,
            versions: &versions,
            events: log,
        },
    )
}
#[test]
fn active_match_and_repeat_do_not_establish_authority_or_replay_protection() {
    let refs = [reference()];
    let bytes = encode(&refs[0]);
    let log = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    let first = run(&refs, &log, 1, &bytes).unwrap();
    assert_eq!(first.1, MATCH);
    assert_eq!(first.0.source_authenticity, Guarantee::NotEstablished);
    assert_eq!(first.0.replay_protection, Guarantee::NotEstablished);
    assert_eq!(run(&refs, &log, 1, &bytes).unwrap(), first);
}
#[test]
fn candidate_consumed_and_revoked_rejected_even_when_bytes_match() {
    let refs = [reference()];
    let bytes = encode(&refs[0]);
    for actions in [
        vec![(1, Action::Create)],
        vec![
            (1, Action::Create),
            (1, Action::Activate),
            (1, Action::Consume),
        ],
        vec![
            (1, Action::Create),
            (1, Action::Activate),
            (1, Action::Revoke),
        ],
    ] {
        let log = events(&refs, &actions);
        assert_eq!(
            run(&refs, &log, 1, &bytes),
            Err(envelope::Reason::MissingReference.code())
        );
        // Lifecycle rejection precedes even malformed envelope parsing.
        assert_eq!(
            run(&refs, &log, 1, b"bad"),
            Err(envelope::Reason::MissingReference.code())
        );
    }
}
#[test]
fn superseded_and_older_generation_rejected_despite_identical_payload() {
    // Identical digests in different generations must not erase lifecycle identity.
    let refs = [reference(), reference()];
    let bytes = encode(&refs[0]);
    let log = events(
        &refs,
        &[
            (1, Action::Create),
            (1, Action::Activate),
            (2, Action::Create),
            (1, Action::Revoke),
            (2, Action::Activate),
            (1, Action::Supersede),
        ],
    );
    assert_eq!(
        run(&refs, &log, 1, &bytes),
        Err(lifecycle::Reason::OlderGeneration.code())
    );
    assert!(run(&refs, &log, 2, &bytes).is_ok());
    assert_eq!(
        run(&refs, &log, 3, &bytes),
        Err(lifecycle::Reason::GenerationMismatch.code())
    );
}
#[test]
fn conflicting_active_references_fail_before_comparison() {
    let refs = [reference(), reference()];
    let log = events(
        &refs,
        &[
            (1, Action::Create),
            (1, Action::Activate),
            (2, Action::Create),
            (2, Action::Activate),
        ],
    );
    assert_eq!(
        run(&refs, &log, 2, &encode(&refs[1])),
        Err(lifecycle::Reason::MultipleActive.code())
    );
}
#[test]
fn wrong_operation_in_event_reference_request_or_envelope_is_rejected() {
    let mut refs = [reference()];
    let bytes = encode(&refs[0]);
    let mut log = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    log[1].operation_id = BOOT.into();
    assert_eq!(
        run(&refs, &log, 1, &bytes),
        Err(lifecycle::Reason::OperationMismatch.code())
    );
    log[1].operation_id = OP.into();
    let versions = [Version {
        generation: 1,
        reference: &refs[0],
    }];
    let history = History {
        assumption: HistoryAssumption::TrustedComplete,
        expected_head: 2,
        versions: &versions,
        events: &log,
    };
    assert_eq!(
        compare_active(&bytes, BOOT, 1, &history),
        Err(lifecycle::Reason::OperationMismatch.code())
    );
    let mut candidate = refs[0].expected.clone();
    candidate.payload.operation_id = BOOT.into();
    assert_eq!(
        run(&refs, &log, 1, &serde_json::to_vec(&candidate).unwrap()),
        Err(envelope::Reason::OperationMismatch.code())
    );
    refs[0].expected.payload.operation_id = BOOT.into();
    assert_eq!(
        run(&refs, &log, 1, &bytes),
        Err(lifecycle::Reason::OperationMismatch.code())
    );
}
#[test]
fn binding_mismatch_in_events_and_candidate_is_rejected() {
    let refs = [reference()];
    let bytes = encode(&refs[0]);
    let mut log = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    log[1].binding_sha256 = "0".repeat(64);
    assert_eq!(
        run(&refs, &log, 1, &bytes),
        Err(lifecycle::Reason::BindingMismatch.code())
    );
    log[1].binding_sha256 = refs[0].expected.binding_sha256.clone();
    let mut candidate = refs[0].expected.clone();
    candidate.binding_sha256 = "0".repeat(64);
    assert_eq!(
        run(&refs, &log, 1, &serde_json::to_vec(&candidate).unwrap()),
        Err(envelope::Reason::BindingMismatch.code())
    );
}
#[test]
fn missing_repeated_and_reordered_history_rejects_matching_envelope() {
    let refs = [reference()];
    let bytes = encode(&refs[0]);
    assert_eq!(
        run(&refs, &[], 1, &bytes),
        Err(lifecycle::Reason::Incomplete.code())
    );
    let mut log = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    log[1].sequence = 1;
    assert_eq!(
        run(&refs, &log, 1, &bytes),
        Err(lifecycle::Reason::SequenceMismatch.code())
    );
    log[1].sequence = 2;
    log.swap(0, 1);
    assert_eq!(
        run(&refs, &log, 1, &bytes),
        Err(lifecycle::Reason::SequenceMismatch.code())
    );
    let log = events(
        &refs,
        &[
            (1, Action::Create),
            (1, Action::Activate),
            (1, Action::Activate),
        ],
    );
    assert_eq!(
        run(&refs, &log, 1, &bytes),
        Err(lifecycle::Reason::InvalidTransition.code())
    );
}
#[test]
fn untrusted_incomplete_or_truncated_history_cannot_be_upgraded_by_match() {
    let refs = [reference()];
    let bytes = encode(&refs[0]);
    let log = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    let versions = [Version {
        generation: 1,
        reference: &refs[0],
    }];
    for (assumption, expected_head, reason) in [
        (
            HistoryAssumption::Untrusted,
            2,
            lifecycle::Reason::Untrusted,
        ),
        (
            HistoryAssumption::Incomplete,
            2,
            lifecycle::Reason::Incomplete,
        ),
        (
            HistoryAssumption::TrustedComplete,
            3,
            lifecycle::Reason::Incomplete,
        ),
    ] {
        assert_eq!(
            compare_active(
                &bytes,
                OP,
                1,
                &History {
                    assumption,
                    expected_head,
                    versions: &versions,
                    events: &log
                }
            ),
            Err(reason.code())
        );
    }
}
#[test]
fn noncommitted_matching_active_remains_a_noncommitted_claim() {
    for commit in [Commit::Unknown, Commit::FailedMayBePartial] {
        let mut refs = [reference()];
        refs[0].expected.payload.commit.result = commit;
        refs[0].expected.binding_sha256 =
            envelope::binding_digest(&refs[0].expected.payload).unwrap();
        let log = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
        let result = run(&refs, &log, 1, &encode(&refs[0])).unwrap().0;
        assert!(!result.claims_committed());
        assert_eq!(result.source_authenticity, Guarantee::NotEstablished);
        assert_eq!(result.replay_protection, Guarantee::NotEstablished);
    }
}

// Optimistic test snapshot, NEVER a capability. The supplied history/head must
// be current and authoritative in a future implementation; these tests cannot
// establish that. No snapshot is persisted, authenticated or consumed here.
struct SelectedSnapshot {
    operation: String,
    generation: u64,
    head: u64,
    binding: String,
}
fn select_snapshot(
    operation: &str,
    history: &History<'_>,
) -> Result<SelectedSnapshot, &'static str> {
    let selection = lifecycle::select(operation, history).map_err(lifecycle::Reason::code)?;
    let generation = selection
        .active_generation
        .ok_or(envelope::Reason::MissingReference.code())?;
    let version = history
        .versions
        .iter()
        .find(|version| version.generation == generation)
        .ok_or(lifecycle::Reason::Incomplete.code())?;
    Ok(SelectedSnapshot {
        operation: operation.into(),
        generation,
        head: history.expected_head,
        binding: version.reference.expected.binding_sha256.clone(),
    })
}
fn revalidate_snapshot(
    bytes: &[u8],
    selected: &SelectedSnapshot,
    current: &History<'_>,
) -> Result<(Validated, &'static str), &'static str> {
    // Reconstruct latest state before consulting the earlier selection or bytes.
    let state = lifecycle::select(&selected.operation, current).map_err(lifecycle::Reason::code)?;
    if state.active_generation != Some(selected.generation) {
        return Err(lifecycle::Reason::InvalidTransition.code());
    }
    if current.expected_head != selected.head {
        return Err(lifecycle::Reason::SequenceMismatch.code());
    }
    let version = current
        .versions
        .iter()
        .find(|version| version.generation == selected.generation)
        .ok_or(lifecycle::Reason::Incomplete.code())?;
    if version.reference.expected.binding_sha256 != selected.binding {
        return Err(lifecycle::Reason::BindingMismatch.code());
    }
    compare_active(bytes, &selected.operation, selected.generation, current)
}
fn history<'a>(versions: &'a [Version<'a>], log: &'a [Event], head: u64) -> History<'a> {
    History {
        assumption: HistoryAssumption::TrustedComplete,
        expected_head: head,
        versions,
        events: log,
    }
}
#[test]
fn race_consumed_or_revoked_after_selection_rejects_old_selection() {
    let refs = [reference()];
    let bytes = encode(&refs[0]);
    let versions = [Version {
        generation: 1,
        reference: &refs[0],
    }];
    let initial = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    let selected = select_snapshot(OP, &history(&versions, &initial, 2)).unwrap();
    assert!(revalidate_snapshot(&bytes, &selected, &history(&versions, &initial, 2)).is_ok());
    for action in [Action::Consume, Action::Revoke] {
        let updated = events(
            &refs,
            &[(1, Action::Create), (1, Action::Activate), (1, action)],
        );
        assert_eq!(
            revalidate_snapshot(&bytes, &selected, &history(&versions, &updated, 3)),
            Err(lifecycle::Reason::InvalidTransition.code())
        );
    }
}
#[test]
fn race_new_active_or_supersession_rejects_previous_generation() {
    let refs = [reference(), reference()];
    let bytes = encode(&refs[0]);
    let one = [Version {
        generation: 1,
        reference: &refs[0],
    }];
    let initial = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    let selected = select_snapshot(OP, &history(&one, &initial, 2)).unwrap();
    let both = [
        Version {
            generation: 1,
            reference: &refs[0],
        },
        Version {
            generation: 2,
            reference: &refs[1],
        },
    ];
    let mut actions = vec![
        (1, Action::Create),
        (1, Action::Activate),
        (2, Action::Create),
        (1, Action::Revoke),
        (2, Action::Activate),
    ];
    for supersede in [false, true] {
        if supersede {
            actions.push((1, Action::Supersede));
        }
        let updated = events(&refs, &actions);
        assert_eq!(
            revalidate_snapshot(
                &bytes,
                &selected,
                &history(&both, &updated, updated.len() as u64)
            ),
            Err(lifecycle::Reason::InvalidTransition.code())
        );
    }
}
#[test]
fn race_head_change_requires_reselection_even_when_generation_remains_active() {
    let refs = [reference(), reference()];
    let one = [Version {
        generation: 1,
        reference: &refs[0],
    }];
    let initial = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    let selected = select_snapshot(OP, &history(&one, &initial, 2)).unwrap();
    let both = [
        Version {
            generation: 1,
            reference: &refs[0],
        },
        Version {
            generation: 2,
            reference: &refs[1],
        },
    ];
    let updated = events(
        &refs,
        &[
            (1, Action::Create),
            (1, Action::Activate),
            (2, Action::Create),
        ],
    );
    assert_eq!(
        revalidate_snapshot(&encode(&refs[0]), &selected, &history(&both, &updated, 3)),
        Err(lifecycle::Reason::SequenceMismatch.code())
    );
    let fresh = select_snapshot(OP, &history(&both, &updated, 3)).unwrap();
    assert!(revalidate_snapshot(&encode(&refs[0]), &fresh, &history(&both, &updated, 3)).is_ok());
}
#[test]
fn race_duplicate_reordered_missing_or_conflicting_final_history_fails_closed() {
    let refs = [reference(), reference()];
    let one = [Version {
        generation: 1,
        reference: &refs[0],
    }];
    let initial = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    let selected = select_snapshot(OP, &history(&one, &initial, 2)).unwrap();
    let bytes = encode(&refs[0]);
    let duplicate = events(
        &refs,
        &[
            (1, Action::Create),
            (1, Action::Activate),
            (1, Action::Activate),
        ],
    );
    assert_eq!(
        revalidate_snapshot(&bytes, &selected, &history(&one, &duplicate, 3)),
        Err(lifecycle::Reason::InvalidTransition.code())
    );
    let mut reordered = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    reordered.swap(0, 1);
    assert_eq!(
        revalidate_snapshot(&bytes, &selected, &history(&one, &reordered, 2)),
        Err(lifecycle::Reason::SequenceMismatch.code())
    );
    assert_eq!(
        revalidate_snapshot(&bytes, &selected, &history(&one, &initial, 3)),
        Err(lifecycle::Reason::Incomplete.code())
    );
    assert_eq!(
        revalidate_snapshot(&bytes, &selected, &history(&one, &initial[..1], 2)),
        Err(lifecycle::Reason::Incomplete.code())
    );
    let both = [
        Version {
            generation: 1,
            reference: &refs[0],
        },
        Version {
            generation: 2,
            reference: &refs[1],
        },
    ];
    let conflict = events(
        &refs,
        &[
            (1, Action::Create),
            (1, Action::Activate),
            (2, Action::Create),
            (2, Action::Activate),
        ],
    );
    assert_eq!(
        revalidate_snapshot(&bytes, &selected, &history(&both, &conflict, 4)),
        Err(lifecycle::Reason::MultipleActive.code())
    );
}
#[test]
fn race_second_use_rejected_after_recorded_consumption() {
    let refs = [reference()];
    let bytes = encode(&refs[0]);
    let versions = [Version {
        generation: 1,
        reference: &refs[0],
    }];
    let initial = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    let selected = select_snapshot(OP, &history(&versions, &initial, 2)).unwrap();
    let result = revalidate_snapshot(&bytes, &selected, &history(&versions, &initial, 2))
        .unwrap()
        .0;
    assert_eq!(result.replay_protection, Guarantee::NotEstablished);
    let consumed = events(
        &refs,
        &[
            (1, Action::Create),
            (1, Action::Activate),
            (1, Action::Consume),
        ],
    );
    assert_eq!(
        revalidate_snapshot(&bytes, &selected, &history(&versions, &consumed, 3)),
        Err(lifecycle::Reason::InvalidTransition.code())
    );
}
#[test]
fn race_two_logical_users_serialize_only_if_consumption_is_visible() {
    let refs = [reference()];
    let bytes = encode(&refs[0]);
    let versions = [Version {
        generation: 1,
        reference: &refs[0],
    }];
    let initial = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    let first = select_snapshot(OP, &history(&versions, &initial, 2)).unwrap();
    let second = select_snapshot(OP, &history(&versions, &initial, 2)).unwrap();
    assert!(revalidate_snapshot(&bytes, &first, &history(&versions, &initial, 2)).is_ok());
    let consumed = events(
        &refs,
        &[
            (1, Action::Create),
            (1, Action::Activate),
            (1, Action::Consume),
        ],
    );
    assert_eq!(
        revalidate_snapshot(&bytes, &second, &history(&versions, &consumed, 3)),
        Err(lifecycle::Reason::InvalidTransition.code())
    );
}
#[test]
fn race_limit_same_snapshot_allows_two_checks_without_atomic_consumption() {
    // Explicit counterexample: rechecking an unchanged snapshot is not a one-use
    // claim. Both contenders pass before any Consume is made visible. This test
    // documents the remaining coordination gap, not a concurrency guarantee.
    let refs = [reference()];
    let bytes = encode(&refs[0]);
    let versions = [Version {
        generation: 1,
        reference: &refs[0],
    }];
    let initial = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    let selected = select_snapshot(OP, &history(&versions, &initial, 2)).unwrap();
    let first = revalidate_snapshot(&bytes, &selected, &history(&versions, &initial, 2)).unwrap();
    let second = revalidate_snapshot(&bytes, &selected, &history(&versions, &initial, 2)).unwrap();
    assert_eq!(first, second);
    assert_eq!(second.0.source_authenticity, Guarantee::NotEstablished);
    assert_eq!(second.0.replay_protection, Guarantee::NotEstablished);
}
#[test]
fn race_limit_stale_full_history_cannot_be_detected_without_authoritative_head() {
    let refs = [reference()];
    let bytes = encode(&refs[0]);
    let versions = [Version {
        generation: 1,
        reference: &refs[0],
    }];
    let initial = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    let selected = select_snapshot(OP, &history(&versions, &initial, 2)).unwrap();
    let revoked = events(
        &refs,
        &[
            (1, Action::Create),
            (1, Action::Activate),
            (1, Action::Revoke),
        ],
    );
    assert!(revalidate_snapshot(&bytes, &selected, &history(&versions, &revoked, 3)).is_err());
    // Caller replaying BOTH the log and its claimed head defeats pure freshness
    // checking. Never present this matching result as a live permission.
    assert!(revalidate_snapshot(&bytes, &selected, &history(&versions, &initial, 2)).is_ok());
}
