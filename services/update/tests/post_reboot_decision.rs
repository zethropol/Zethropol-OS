#[path = "../src/post_reboot_decision.rs"]
mod model;
use model::*;

const OP: &str = "1096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e";
const OLD: &str = "2096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e";
const NEW: &str = "3096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e";
fn fixture() -> (Expected, Evidence, Context) {
    let kernel = Kernel {
        release: "test-kernel-1".into(),
        sha256: "a".repeat(64),
    };
    (
        Expected {
            operation_id: OP.into(),
            origin_boot_id: OLD.into(),
            kernel: kernel.clone(),
            entry_id: "entry-1".into(),
            root_id: "fs-1/subvolume-1".into(),
        },
        Evidence {
            operation_id: Some(OP.into()),
            commit: Commit::Committed,
            boot_id: Some(NEW.into()),
            boot_id_after_checks: Some(NEW.into()),
            kernel: Some(kernel),
            entry_id: Some("entry-1".into()),
            root_id: Some("fs-1/subvolume-1".into()),
            source: Check::Passed,
            envelope_integrity: Check::Passed,
            replay_check: Check::Passed,
            observed_at: Some(100),
            execution_binding: Check::Passed,
            kernel_integrity: Check::Passed,
            initramfs_integrity: Check::Passed,
            required_drivers: Check::Passed,
            critical_health: Check::Passed,
            normal_root: Check::Passed,
        },
        Context {
            current_boot_id: NEW.into(),
            now: 110,
            max_age: 10,
        },
    )
}
fn assert_reason(x: &Expected, e: &Evidence, c: &Context, state: State, reason: Reason) {
    let result = evaluate(x, e, c);
    assert_eq!(result.state, state);
    assert!(result.reasons.contains(&reason), "{result:?}");
    assert!(!reason.code().is_empty());
}
#[test]
fn complete_synthetic_evidence_only() {
    let (x, e, c) = fixture();
    assert_eq!(
        evaluate(&x, &e, &c),
        Decision {
            state: State::Verified,
            reasons: vec![Reason::Complete]
        }
    );
}
#[test]
fn every_required_check_fails_closed() {
    for index in 0..9 {
        for check in [Check::Missing, Check::NotVerified, Check::Failed] {
            let (x, mut e, c) = fixture();
            let fields = [
                &mut e.source,
                &mut e.envelope_integrity,
                &mut e.replay_check,
                &mut e.execution_binding,
                &mut e.kernel_integrity,
                &mut e.initramfs_integrity,
                &mut e.required_drivers,
                &mut e.critical_health,
                &mut e.normal_root,
            ];
            *fields.into_iter().nth(index).unwrap() = check;
            assert_ne!(
                evaluate(&x, &e, &c).state,
                State::Verified,
                "{index} {check:?}"
            );
        }
    }
}
#[test]
fn matching_release_and_new_boot_are_insufficient() {
    let (x, mut e, c) = fixture();
    e.execution_binding = Check::Missing;
    assert_reason(&x, &e, &c, State::Unknown, Reason::Execution);
    e.kernel.as_mut().unwrap().sha256 = "b".repeat(64);
    assert_reason(&x, &e, &c, State::Failed, Reason::KernelMismatch);
}
#[test]
fn missing_and_wrong_bindings() {
    let (x, e, c) = fixture();
    for index in 0..6 {
        let mut missing = e.clone();
        match index {
            0 => missing.operation_id = None,
            1 => missing.boot_id = None,
            2 => missing.boot_id_after_checks = None,
            3 => missing.kernel = None,
            4 => missing.entry_id = None,
            _ => missing.root_id = None,
        }
        assert_reason(&x, &missing, &c, State::Unknown, Reason::MissingBinding);
    }
    for (index, reason) in [
        (0, Reason::OperationMismatch),
        (1, Reason::EntryMismatch),
        (2, Reason::RootMismatch),
    ] {
        let mut wrong = e.clone();
        match index {
            0 => wrong.operation_id = Some(OLD.into()),
            1 => wrong.entry_id = Some("other".into()),
            _ => wrong.root_id = Some("recovery-root".into()),
        }
        assert_reason(&x, &wrong, &c, State::Failed, reason);
    }
}
#[test]
fn old_kernel_and_recovery_rollback() {
    let (x, mut e, c) = fixture();
    e.kernel.as_mut().unwrap().release = "old".into();
    assert_reason(&x, &e, &c, State::Failed, Reason::KernelMismatch);
    e.kernel = Some(x.kernel.clone());
    e.normal_root = Check::Failed;
    assert_reason(&x, &e, &c, State::Failed, Reason::Recovery);
}
#[test]
fn explicit_artifact_driver_and_health_failures() {
    for (index, reason) in [
        (0, Reason::Initramfs),
        (1, Reason::Drivers),
        (2, Reason::Health),
        (3, Reason::Kernel),
    ] {
        let (x, mut e, c) = fixture();
        match index {
            0 => e.initramfs_integrity = Check::Failed,
            1 => e.required_drivers = Check::Failed,
            2 => e.critical_health = Check::Failed,
            _ => e.kernel_integrity = Check::Failed,
        }
        assert_reason(&x, &e, &c, State::Failed, reason);
    }
}
#[test]
fn boot_binding_and_replay() {
    let (x, mut e, mut c) = fixture();
    e.boot_id = Some(OLD.into());
    e.boot_id_after_checks = Some(OLD.into());
    c.current_boot_id = OLD.into();
    assert_reason(&x, &e, &c, State::Unverified, Reason::SameBoot);
    c.current_boot_id = NEW.into();
    assert_reason(&x, &e, &c, State::Failed, Reason::BootMismatch);
    e.boot_id_after_checks = Some(NEW.into());
    assert_reason(&x, &e, &c, State::Failed, Reason::BootChangedDuringChecks);
    let (x, mut e, c) = fixture();
    e.replay_check = Check::Failed;
    assert_reason(&x, &e, &c, State::Failed, Reason::Replay);
}
#[test]
fn all_noncommitted_outcomes_remain_closed() {
    for (commit, state, reason) in [
        (
            Commit::NotStarted,
            State::Unverified,
            Reason::CommitNotStarted,
        ),
        (
            Commit::InProgress,
            State::Unknown,
            Reason::CommitInterrupted,
        ),
        (Commit::Unknown, State::Unknown, Reason::CommitUnknown),
        (
            Commit::FailedMayBePartial,
            State::Failed,
            Reason::CommitPartial,
        ),
    ] {
        let (x, mut e, c) = fixture();
        e.commit = commit;
        assert_reason(&x, &e, &c, state, reason);
    }
}
#[test]
fn freshness_inclusive_boundary_and_invalid_clocks() {
    let (x, mut e, mut c) = fixture();
    assert_eq!(evaluate(&x, &e, &c).state, State::Verified); // age == max_age
    c.now = 111;
    assert_reason(&x, &e, &c, State::Unverified, Reason::Stale);
    c.now = 99;
    assert_reason(&x, &e, &c, State::Failed, Reason::Future);
    c.max_age = 0;
    assert_reason(&x, &e, &c, State::Unknown, Reason::InvalidTime);
    c.max_age = 10;
    e.observed_at = None;
    assert_reason(&x, &e, &c, State::Unknown, Reason::MissingTime);
    e.observed_at = Some(0);
    c.now = u64::MAX;
    assert_reason(&x, &e, &c, State::Unverified, Reason::Stale);
}
#[test]
fn corrupt_untrusted_and_malformed_input() {
    let (mut x, mut e, c) = fixture();
    e.envelope_integrity = Check::Failed;
    assert_reason(&x, &e, &c, State::Failed, Reason::Envelope);
    e.source = Check::NotVerified;
    assert_reason(&x, &e, &c, State::Unverified, Reason::Source);
    e.source = Check::Passed;
    e.envelope_integrity = Check::Passed;
    e.operation_id = Some("not-a-uuid".into());
    assert_reason(&x, &e, &c, State::Failed, Reason::InvalidObservation);
    x.kernel.sha256.clear();
    assert_reason(&x, &e, &c, State::Unknown, Reason::InvalidExpectation);
}
#[test]
fn contradictions_outrank_missing_checks_without_leaking_input() {
    let (x, mut e, c) = fixture();
    e.execution_binding = Check::Missing;
    e.critical_health = Check::Failed;
    assert_reason(&x, &e, &c, State::Failed, Reason::Health);
    e.root_id = Some("/secret/token-sentinel".into());
    let result = evaluate(&x, &e, &c);
    assert!(!format!("{result:?}").contains("sentinel"));
    assert_eq!(result.reasons[0].code(), "root_mismatch");
}
