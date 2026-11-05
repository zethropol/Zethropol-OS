#[allow(dead_code)]
#[path = "../src/post_reboot_envelope.rs"]
mod envelope;
#[path = "../src/reference_lifecycle.rs"]
mod lifecycle;
#[allow(dead_code)]
#[path = "../src/post_reboot_decision.rs"]
mod post_reboot_decision;
use envelope::*;
use lifecycle::Reason;
use lifecycle::*;
use post_reboot_decision::Commit;
const OP: &str = "1096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e";
const BOOT: &str = "2096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e";
fn reference(suffix: char) -> BindingReference {
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
            kernel_release: format!("test-{suffix}"),
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
fn run(refs: &[BindingReference], events: &[Event]) -> Result<Selection, Reason> {
    let versions: Vec<_> = refs
        .iter()
        .enumerate()
        .map(|(i, reference)| Version {
            generation: i as u64 + 1,
            reference,
        })
        .collect();
    select(
        OP,
        &History {
            assumption: HistoryAssumption::TrustedComplete,
            expected_head: events.len() as u64,
            versions: &versions,
            events,
        },
    )
}
#[test]
fn initial_candidate_and_activation() {
    let refs = [reference('a')];
    let result = run(&refs, &events(&refs, &[(1, Action::Create)])).unwrap();
    assert_eq!(result.stages, vec![(1, Stage::Candidate)]);
    assert_eq!(result.active_generation, None);
    let result = run(
        &refs,
        &events(&refs, &[(1, Action::Create), (1, Action::Activate)]),
    )
    .unwrap();
    assert_eq!(result.active_generation, Some(1));
    assert_eq!(result.stages[0].1, Stage::Active);
    assert_eq!(result.source_authenticity, Guarantee::NotEstablished);
    assert_eq!(result.replay_protection, Guarantee::NotEstablished);
}
#[test]
fn creation_does_not_revoke_and_second_active_is_rejected() {
    let refs = [reference('a'), reference('b')];
    let mut actions = vec![
        (1, Action::Create),
        (1, Action::Activate),
        (2, Action::Create),
    ];
    assert_eq!(
        run(&refs, &events(&refs, &actions))
            .unwrap()
            .active_generation,
        Some(1)
    );
    actions.push((2, Action::Activate));
    assert_eq!(
        run(&refs, &events(&refs, &actions)),
        Err(Reason::MultipleActive)
    );
}
#[test]
fn explicit_revoke_then_new_activation_then_supersession() {
    let refs = [reference('a'), reference('b')];
    let mut actions = vec![
        (1, Action::Create),
        (1, Action::Activate),
        (2, Action::Create),
        (1, Action::Revoke),
        (2, Action::Activate),
        (1, Action::Supersede),
    ];
    let result = run(&refs, &events(&refs, &actions)).unwrap();
    assert_eq!(
        result.stages,
        vec![(1, Stage::Superseded), (2, Stage::Active)]
    );
    actions.push((1, Action::Activate));
    assert_eq!(
        run(&refs, &events(&refs, &actions)),
        Err(Reason::InvalidTransition)
    );
}
#[test]
fn revoked_and_consumed_cannot_return() {
    for action in [Action::Revoke, Action::Consume] {
        let refs = [reference('a')];
        let mut actions = vec![(1, Action::Create), (1, Action::Activate), (1, action)];
        assert_eq!(
            run(&refs, &events(&refs, &actions))
                .unwrap()
                .active_generation,
            None
        );
        actions.push((1, Action::Activate));
        assert_eq!(
            run(&refs, &events(&refs, &actions)),
            Err(Reason::InvalidTransition)
        );
    }
}
#[test]
fn older_candidate_cannot_activate_or_supersede_without_revocation() {
    let refs = [reference('a'), reference('b')];
    let mut actions = vec![
        (1, Action::Create),
        (2, Action::Create),
        (1, Action::Activate),
    ];
    assert_eq!(
        run(&refs, &events(&refs, &actions)),
        Err(Reason::OlderGeneration)
    );
    actions[2] = (2, Action::Activate);
    actions.push((1, Action::Supersede));
    assert_eq!(
        run(&refs, &events(&refs, &actions)),
        Err(Reason::MissingRevocation)
    );
}
#[test]
fn wrong_operation_binding_and_corrupt_reference() {
    let mut refs = [reference('a')];
    let mut log = events(&refs, &[(1, Action::Create)]);
    log[0].operation_id = BOOT.into();
    assert_eq!(run(&refs, &log), Err(Reason::OperationMismatch));
    log[0].operation_id = OP.into();
    log[0].binding_sha256 = "0".repeat(64);
    assert_eq!(run(&refs, &log), Err(Reason::BindingMismatch));
    refs[0].expected.payload.operation_id = BOOT.into();
    assert_eq!(run(&refs, &log), Err(Reason::OperationMismatch));
    refs[0].expected.payload.operation_id = OP.into();
    refs[0].expected.binding_sha256.clear();
    assert_eq!(run(&refs, &log), Err(Reason::InvalidReference));
}
#[test]
fn duplicate_reordered_and_replayed_events_fail() {
    let refs = [reference('a')];
    let mut log = events(&refs, &[(1, Action::Create), (1, Action::Activate)]);
    log[1].sequence = 1;
    assert_eq!(run(&refs, &log), Err(Reason::SequenceMismatch));
    log[1].sequence = 2;
    log.swap(0, 1);
    assert_eq!(run(&refs, &log), Err(Reason::SequenceMismatch));
    let log = events(
        &refs,
        &[
            (1, Action::Create),
            (1, Action::Activate),
            (1, Action::Activate),
        ],
    );
    assert_eq!(run(&refs, &log), Err(Reason::InvalidTransition));
    let log = events(&refs, &[(1, Action::Create), (1, Action::Create)]);
    assert_eq!(run(&refs, &log), Err(Reason::GenerationMismatch));
}
#[test]
fn missing_history_and_reset_generations_fail() {
    let refs = [reference('a')];
    assert_eq!(run(&refs, &[]), Err(Reason::Incomplete));
    assert_eq!(
        run(&refs, &events(&refs, &[(1, Action::Activate)])),
        Err(Reason::Incomplete)
    );
    let mut log = events(&refs, &[(1, Action::Create)]);
    log[0].generation = 0;
    assert_eq!(run(&refs, &log), Err(Reason::GenerationMismatch));
    log[0].generation = u64::MAX;
    assert_eq!(run(&refs, &log), Err(Reason::CounterOverflow));
    log[0].generation = 1;
    log[0].sequence = u64::MAX;
    assert_eq!(run(&refs, &log), Err(Reason::CounterOverflow));
}
#[test]
fn history_assumptions_and_independent_head_fail_closed() {
    let refs = [reference('a')];
    let versions = [Version {
        generation: 1,
        reference: &refs[0],
    }];
    let log = events(&refs, &[(1, Action::Create)]);
    for (assumption, head, reason) in [
        (HistoryAssumption::Untrusted, 1, Reason::Untrusted),
        (HistoryAssumption::Incomplete, 1, Reason::Incomplete),
        (HistoryAssumption::TrustedComplete, 2, Reason::Incomplete),
    ] {
        assert_eq!(
            select(
                OP,
                &History {
                    assumption,
                    expected_head: head,
                    versions: &versions,
                    events: &log
                }
            ),
            Err(reason)
        );
        assert!(!reason.code().is_empty());
    }
    let result = run(&refs, &log).unwrap(); // Assumption is not authenticated by model.
    assert_eq!(result.source_authenticity, Guarantee::NotEstablished);
    assert_eq!(result.replay_protection, Guarantee::NotEstablished);
}
#[test]
fn bounded_and_invalid_operation() {
    assert_eq!(
        select(
            "/secret/sentinel",
            &History {
                assumption: HistoryAssumption::TrustedComplete,
                expected_head: 0,
                versions: &[],
                events: &[]
            }
        ),
        Err(Reason::InvalidOperation)
    );
    let refs = [reference('a')];
    let actions = vec![(1, Action::Create); MAX_EVENTS + 1];
    assert_eq!(run(&refs, &events(&refs, &actions)), Err(Reason::Limit));
    assert!(!format!("{:?}", Reason::InvalidOperation).contains("sentinel"));
    let result = run(&[], &[]).unwrap();
    assert_eq!(result.active_generation, None);
}

#[test]
fn version_counter_reset_overflow_and_excessive_history_are_rejected() {
    let reference = reference('a');
    for (generation, reason) in [
        (0, Reason::GenerationMismatch),
        (2, Reason::GenerationMismatch),
        (u64::MAX, Reason::CounterOverflow),
    ] {
        let versions = [Version {
            generation,
            reference: &reference,
        }];
        assert_eq!(
            select(
                OP,
                &History {
                    assumption: HistoryAssumption::TrustedComplete,
                    expected_head: 0,
                    versions: &versions,
                    events: &[]
                }
            ),
            Err(reason)
        );
    }
    let versions: Vec<_> = (1..=MAX_REFERENCES + 1)
        .map(|i| Version {
            generation: i as u64,
            reference: &reference,
        })
        .collect();
    assert_eq!(
        select(
            OP,
            &History {
                assumption: HistoryAssumption::TrustedComplete,
                expected_head: 0,
                versions: &versions,
                events: &[]
            }
        ),
        Err(Reason::Limit)
    );
}

#[test]
fn repeated_terminal_transitions_and_generation_reset_fail() {
    let refs = [reference('a')];
    for action in [Action::Revoke, Action::Consume] {
        let log = events(
            &refs,
            &[
                (1, Action::Create),
                (1, Action::Activate),
                (1, action),
                (1, action),
            ],
        );
        assert_eq!(run(&refs, &log), Err(Reason::InvalidTransition));
    }
    let log = events(
        &refs,
        &[
            (1, Action::Create),
            (1, Action::Revoke),
            (1, Action::Create),
        ],
    );
    assert_eq!(run(&refs, &log), Err(Reason::GenerationMismatch));
}
