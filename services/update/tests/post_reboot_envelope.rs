#[path = "../src/post_reboot_envelope.rs"]
mod envelope;
#[allow(dead_code)]
#[path = "../src/post_reboot_decision.rs"]
mod post_reboot_decision;
use envelope::*;
use post_reboot_decision::Commit;
use serde_json::{json, Value};
const OP: &str = "1096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e";
const BOOT: &str = "2096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e";
fn fixture(commit: Commit) -> (Envelope, Anchor) {
    let payload = Payload {
        schema_version: 1,
        operation_id: OP.into(),
        origin_boot_id: BOOT.into(),
        commit: CommitSummary {
            result: commit,
            checkpoint_sha256: "a".repeat(64),
            approved_plan_sha256: "b".repeat(64),
        },
        target: Target {
            kernel_release: "6.12-test".into(),
            kernel_sha256: "c".repeat(64),
            initramfs_sha256: "d".repeat(64),
            loader_sha256: "e".repeat(64),
            config_sha256: "f".repeat(64),
            entry_id: OP.into(),
            root_filesystem_id: BOOT.into(),
            root_subvolume_id: OP.into(),
        },
        producer: Producer::Synthetic,
        claimed_trust: ClaimedTrust::Unverified,
    };
    let binding = binding_digest(&payload).unwrap();
    let anchor = Anchor {
        operation_id: OP.into(),
        binding_sha256: binding.clone(),
        origin_boot_id: BOOT.into(),
        target: payload.target.clone(),
    };
    (
        Envelope {
            payload,
            binding_sha256: binding,
        },
        anchor,
    )
}
fn encode(value: &impl serde::Serialize) -> Vec<u8> {
    serde_json::to_vec(value).unwrap()
}
fn bad(value: &Value, anchor: &Anchor, reason: Reason) {
    assert_eq!(validate(&encode(value), anchor), Err(reason));
    assert!(!reason.code().is_empty());
}
#[test]
fn valid_schema_does_not_authenticate_source_or_prevent_replay() {
    let (env, anchor) = fixture(Commit::Committed);
    let one = validate(&encode(&env), &anchor).unwrap();
    assert!(one.claims_committed());
    assert_eq!(one.source_authenticity, Guarantee::NotEstablished);
    assert_eq!(one.replay_protection, Guarantee::NotEstablished);
    // Repeated schema validation is deterministic, not a replay authority.
    assert_eq!(validate(&encode(&env), &anchor).unwrap(), one);
}
#[test]
fn noncommitted_claims_are_preserved_without_success() {
    for commit in [
        Commit::Unknown,
        Commit::FailedMayBePartial,
        Commit::InProgress,
        Commit::NotStarted,
    ] {
        let (env, anchor) = fixture(commit);
        let out = validate(&encode(&env), &anchor).unwrap();
        assert_eq!(out.commit, commit);
        assert!(!out.claims_committed());
    }
}
#[test]
fn all_fields_are_required() {
    let (env, anchor) = fixture(Commit::Committed);
    let original = serde_json::to_value(env).unwrap();
    for pointer in ["", "/payload", "/payload/commit", "/payload/target"] {
        let object = original.pointer(pointer).unwrap().as_object().unwrap();
        for key in object.keys() {
            let mut value = original.clone();
            value
                .pointer_mut(pointer)
                .unwrap()
                .as_object_mut()
                .unwrap()
                .remove(key);
            bad(&value, &anchor, Reason::InvalidSchema);
        }
    }
}
#[test]
fn strict_schema_rejects_unknown_secret_fields_at_every_level() {
    let (env, anchor) = fixture(Commit::Committed);
    let original = serde_json::to_value(env).unwrap();
    for pointer in ["", "/payload", "/payload/commit", "/payload/target"] {
        for field in ["token", "password", "private_key", "message", "path"] {
            let mut value = original.clone();
            value
                .pointer_mut(pointer)
                .unwrap()
                .as_object_mut()
                .unwrap()
                .insert(field.into(), json!("secret-sentinel"));
            bad(&value, &anchor, Reason::InvalidSchema);
            assert!(!format!("{:?}", validate(&encode(&value), &anchor)).contains("sentinel"));
        }
    }
}
#[test]
fn version_types_and_duplicate_fields() {
    let (env, anchor) = fixture(Commit::Committed);
    let mut value = serde_json::to_value(&env).unwrap();
    value["payload"]["schema_version"] = json!(2);
    bad(&value, &anchor, Reason::UnsupportedVersion);
    for version in [json!(-1), json!(1.5), json!("1"), Value::Null] {
        value["payload"]["schema_version"] = version;
        bad(&value, &anchor, Reason::InvalidSchema);
    }
    let original = String::from_utf8(encode(&env)).unwrap();
    let duplicate = original.replace(
        "\"schema_version\":1",
        "\"schema_version\":1,\"schema_version\":1",
    );
    assert_eq!(
        validate(duplicate.as_bytes(), &anchor),
        Err(Reason::InvalidSchema)
    );
}
#[test]
fn mismatched_operation_boot_and_targets() {
    let (env, anchor) = fixture(Commit::Committed);
    let original = serde_json::to_value(env).unwrap();
    for (pointer, reason) in [
        ("/payload/operation_id", Reason::OperationMismatch),
        ("/payload/origin_boot_id", Reason::BootMismatch),
    ] {
        let mut value = original.clone();
        *value.pointer_mut(pointer).unwrap() = json!("3096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e");
        bad(&value, &anchor, reason);
    }
    for field in [
        "kernel_release",
        "kernel_sha256",
        "initramfs_sha256",
        "loader_sha256",
        "config_sha256",
        "entry_id",
        "root_filesystem_id",
        "root_subvolume_id",
    ] {
        let mut value = original.clone();
        let replacement = if field.ends_with("sha256") {
            "0".repeat(64)
        } else if field == "kernel_release" {
            "old-kernel".into()
        } else {
            "3096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e".into()
        };
        value["payload"]["target"][field] = json!(replacement);
        bad(&value, &anchor, Reason::TargetMismatch);
    }
}
#[test]
fn commit_summary_and_binding_tampering() {
    let (env, anchor) = fixture(Commit::Committed);
    let original = serde_json::to_value(env).unwrap();
    for field in ["checkpoint_sha256", "approved_plan_sha256"] {
        let mut value = original.clone();
        value["payload"]["commit"][field] = json!("bad");
        bad(&value, &anchor, Reason::InvalidDigest);
        value["payload"]["commit"][field] = json!("0".repeat(64));
        bad(&value, &anchor, Reason::BindingMismatch);
    }
    let mut value = original.clone();
    value["payload"]["commit"]["result"] = json!("unknown");
    bad(&value, &anchor, Reason::BindingMismatch);
    value = original;
    value["binding_sha256"] = json!("0".repeat(64));
    bad(&value, &anchor, Reason::BindingMismatch);
}
#[test]
fn strict_limits_and_sensitive_paths() {
    let (env, anchor) = fixture(Commit::Committed);
    assert_eq!(validate(&[], &anchor), Err(Reason::Empty));
    assert_eq!(
        validate(&vec![b' '; MAX_BYTES + 1], &anchor),
        Err(Reason::TooLarge)
    );
    let original = serde_json::to_value(env).unwrap();
    for release in [
        "".into(),
        "a".repeat(129),
        "/secret/kernel".into(),
        "line\nbreak".into(),
    ] {
        let mut value = original.clone();
        value["payload"]["target"]["kernel_release"] = json!(release);
        bad(&value, &anchor, Reason::InvalidKernel);
    }
    for pointer in [
        "/payload/operation_id",
        "/payload/origin_boot_id",
        "/payload/target/entry_id",
        "/payload/target/root_filesystem_id",
        "/payload/target/root_subvolume_id",
    ] {
        let mut value = original.clone();
        *value.pointer_mut(pointer).unwrap() = json!("/secret/token-sentinel");
        bad(&value, &anchor, Reason::InvalidIdentity);
    }
    assert_eq!(
        validate(&vec![b' '; MAX_BYTES], &anchor),
        Err(Reason::InvalidSchema)
    );
}
#[test]
fn trust_cannot_be_self_asserted() {
    let (env, anchor) = fixture(Commit::Committed);
    let mut value = serde_json::to_value(env).unwrap();
    value["payload"]["claimed_trust"] = json!("verified");
    bad(&value, &anchor, Reason::InvalidSchema);
    value["payload"]["claimed_trust"] = json!("unverified");
    value["payload"]["producer"] = json!("/fake/root/producer");
    bad(&value, &anchor, Reason::InvalidSchema);
}
#[test]
fn canonical_hash_and_anchor_fail_closed() {
    let (env, mut anchor) = fixture(Commit::Committed);
    let pretty = serde_json::to_vec_pretty(&env).unwrap();
    assert!(validate(&pretty, &anchor).is_ok());
    anchor.binding_sha256.clear();
    assert_eq!(validate(&encode(&env), &anchor), Err(Reason::InvalidAnchor));
    let mut payload = env.payload;
    payload.target.kernel_release = "x".repeat(MAX_BYTES);
    assert_eq!(binding_digest(&payload), Err(Reason::InvalidKernel));
}

#[test]
fn malformed_encodings_and_digest_boundaries() {
    let (env, anchor) = fixture(Commit::Committed);
    for bytes in [
        b" ".as_slice(),
        b"{}",
        b"null",
        b"[]",
        b"{",
        b"\xff",
        b"{}{}",
    ] {
        assert_eq!(validate(bytes, &anchor), Err(Reason::InvalidSchema));
    }
    for hash in [
        "".into(),
        "a".repeat(63),
        "a".repeat(65),
        "A".repeat(64),
        "g".repeat(64),
    ] {
        let mut value = serde_json::to_value(&env).unwrap();
        value["payload"]["target"]["kernel_sha256"] = json!(hash);
        bad(&value, &anchor, Reason::InvalidDigest);
    }
    let mut value = serde_json::to_value(&env).unwrap();
    value["payload"]["operation_id"] = json!("00000000-0000-0000-0000-000000000000");
    bad(&value, &anchor, Reason::InvalidIdentity);
}

#[test]
fn claimed_worker_is_still_unauthenticated() {
    let (mut env, mut anchor) = fixture(Commit::Committed);
    env.payload.producer = Producer::UpdateWorker;
    env.payload.target.kernel_release = "x".repeat(128);
    anchor.target = env.payload.target.clone();
    env.binding_sha256 = binding_digest(&env.payload).unwrap();
    anchor.binding_sha256 = env.binding_sha256.clone();
    let result = validate(&encode(&env), &anchor).unwrap();
    assert_eq!(result.source_authenticity, Guarantee::NotEstablished);
    assert_eq!(result.replay_protection, Guarantee::NotEstablished);
}

#[test]
fn reference_match_has_no_provenance_or_replay_upgrade() {
    // Deliberately copied: this demonstrates equality cannot establish independence.
    let (env, _) = fixture(Commit::Committed);
    let reference = BindingReference {
        expected: env.clone(),
    };
    let result = compare_reference(&encode(&env), Some(&reference)).unwrap();
    assert_eq!(result.source_authenticity, Guarantee::NotEstablished);
    assert_eq!(result.replay_protection, Guarantee::NotEstablished);
    assert_eq!(
        compare_reference(&encode(&env), Some(&reference)),
        Ok(result)
    );
}

#[test]
fn reference_absent_incomplete_or_inconsistent_is_rejected() {
    let (env, _) = fixture(Commit::Committed);
    assert_eq!(
        compare_reference(&encode(&env), None),
        Err(Reason::MissingReference)
    );
    for index in 0..5 {
        let mut expected = env.clone();
        match index {
            0 => expected.payload.commit.checkpoint_sha256.clear(),
            1 => expected.payload.target.root_filesystem_id.clear(),
            2 => expected.binding_sha256 = "0".repeat(64),
            3 => expected.payload.schema_version = 2,
            _ => expected.payload.operation_id.clear(),
        }
        assert_eq!(
            compare_reference(&encode(&env), Some(&BindingReference { expected })),
            Err(Reason::InvalidReference)
        );
    }
}

#[test]
fn reference_checks_each_binding_including_commit_summary() {
    let (env, _) = fixture(Commit::Committed);
    let reference = BindingReference {
        expected: env.clone(),
    };
    for (pointer, replacement, reason) in [
        (
            "/payload/operation_id",
            json!(BOOT),
            Reason::OperationMismatch,
        ),
        (
            "/payload/commit/result",
            json!("unknown"),
            Reason::CommitMismatch,
        ),
        (
            "/payload/commit/checkpoint_sha256",
            json!("0".repeat(64)),
            Reason::CommitMismatch,
        ),
        (
            "/payload/commit/approved_plan_sha256",
            json!("0".repeat(64)),
            Reason::CommitMismatch,
        ),
        ("/payload/origin_boot_id", json!(OP), Reason::BootMismatch),
        (
            "/payload/target/kernel_release",
            json!("old-kernel"),
            Reason::TargetMismatch,
        ),
        (
            "/payload/target/kernel_sha256",
            json!("0".repeat(64)),
            Reason::TargetMismatch,
        ),
        (
            "/payload/target/initramfs_sha256",
            json!("0".repeat(64)),
            Reason::TargetMismatch,
        ),
        (
            "/payload/target/loader_sha256",
            json!("0".repeat(64)),
            Reason::TargetMismatch,
        ),
        (
            "/payload/target/config_sha256",
            json!("0".repeat(64)),
            Reason::TargetMismatch,
        ),
        (
            "/payload/target/entry_id",
            json!(BOOT),
            Reason::TargetMismatch,
        ),
        (
            "/payload/target/root_filesystem_id",
            json!(OP),
            Reason::TargetMismatch,
        ),
        (
            "/payload/target/root_subvolume_id",
            json!(BOOT),
            Reason::TargetMismatch,
        ),
        (
            "/payload/schema_version",
            json!(2),
            Reason::ReferenceSchemaMismatch,
        ),
        (
            "/payload/producer",
            json!("update-worker"),
            Reason::ProducerMismatch,
        ),
        (
            "/binding_sha256",
            json!("0".repeat(64)),
            Reason::BindingMismatch,
        ),
    ] {
        let mut candidate = serde_json::to_value(&env).unwrap();
        *candidate.pointer_mut(pointer).unwrap() = replacement;
        assert_eq!(
            compare_reference(&encode(&candidate), Some(&reference)),
            Err(reason)
        );
        assert!(!reason.code().is_empty());
    }
}

#[test]
fn old_reference_cannot_accept_rebound_candidate() {
    let (mut candidate, _) = fixture(Commit::Committed);
    let reference = BindingReference {
        expected: candidate.clone(),
    };
    candidate.payload.target.initramfs_sha256 = "0".repeat(64);
    candidate.binding_sha256 = binding_digest(&candidate.payload).unwrap();
    assert_eq!(
        compare_reference(&encode(&candidate), Some(&reference)),
        Err(Reason::TargetMismatch)
    );
}

#[test]
fn reference_matching_unknown_partial_never_claims_commit_success() {
    for commit in [Commit::Unknown, Commit::FailedMayBePartial] {
        let (env, _) = fixture(commit);
        let reference = BindingReference {
            expected: env.clone(),
        };
        let result = compare_reference(&encode(&env), Some(&reference)).unwrap();
        assert!(!result.claims_committed());
        assert_eq!(result.source_authenticity, Guarantee::NotEstablished);
        assert_eq!(result.replay_protection, Guarantee::NotEstablished);
    }
}

#[test]
fn reference_comparison_preserves_input_limits_and_redacted_errors() {
    let (env, _) = fixture(Commit::Committed);
    let reference = BindingReference { expected: env };
    for (bytes, reason) in [
        (vec![], Reason::Empty),
        (vec![b' '; MAX_BYTES + 1], Reason::TooLarge),
        (
            b"{\"token\":\"/private/sentinel\"}".to_vec(),
            Reason::InvalidSchema,
        ),
    ] {
        let result = compare_reference(&bytes, Some(&reference));
        assert_eq!(result, Err(reason));
        assert!(!format!("{result:?}").contains("sentinel"));
    }
}
