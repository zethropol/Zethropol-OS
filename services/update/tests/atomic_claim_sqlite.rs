//! Rootless test-only SQLite execution using Python's maintained stdlib binding.
//! No new Rust dependency, unsafe SQLite FFI, production store or authority.
#[allow(dead_code)]
#[path = "../src/post_reboot_envelope.rs"]
mod envelope;
#[allow(dead_code)]
#[path = "../src/reference_lifecycle.rs"]
mod lifecycle;
#[allow(dead_code)]
#[path = "../src/atomic_claim.rs"]
mod model;
#[allow(dead_code)]
#[path = "../src/post_reboot_decision.rs"]
mod post_reboot_decision;

#[test]
fn sqlite_constraints_transactions_and_pure_model_agree() {
    let script = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures/atomic_claim_sqlite.py");
    let output = std::process::Command::new("python3")
        .args(["-I", "-B"])
        .arg(script)
        .output()
        .expect("Python sqlite3 test dependency must be available; never silently skip");
    assert!(
        output.status.success(),
        "SQLite suite failed:\n{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    let summary: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
    assert_eq!(summary["tests"].as_u64().unwrap(), 50);
    eprintln!(
        "SQLite {}: {} independent fixture tests passed",
        summary["sqlite_version"], summary["tests"]
    );

    // Same envelope and expected transaction as the SQLite fixture. The success
    // receipt/head/count and rejection of a competing request must match the pure
    // model. Neither side establishes real producer trust or freshness.
    let env: envelope::Envelope = serde_json::from_value(summary["envelope"].clone()).unwrap();
    let operation = env.payload.operation_id.clone();
    let binding = env.binding_sha256.clone();
    let reference = envelope::BindingReference { expected: env };
    let versions = [lifecycle::Version {
        generation: 1,
        reference: &reference,
    }];
    let events: Vec<_> = [lifecycle::Action::Create, lifecycle::Action::Activate]
        .iter()
        .enumerate()
        .map(|(index, action)| lifecycle::Event {
            sequence: index as u64 + 1,
            operation_id: operation.clone(),
            generation: 1,
            binding_sha256: binding.clone(),
            action: *action,
        })
        .collect();
    let mut machine = model::Machine::from_history(
        &operation,
        &lifecycle::History {
            assumption: lifecycle::HistoryAssumption::TrustedComplete,
            expected_head: 2,
            versions: &versions,
            events: &events,
        },
    )
    .unwrap();
    let authority = model::Authority {
        producer_id: "2096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e".into(),
        assumed_authorized: true,
        assumed_current: true,
    };
    let request = model::Request {
        operation_id: operation,
        generation: 1,
        binding_sha256: binding,
        expected_head: 2,
        producer_id: authority.producer_id.clone(),
        purpose: model::Purpose::AssociateBootEvidence,
        claim_id: "3096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e".into(),
    };
    let pending = machine.prepare(request.clone(), &authority).unwrap();
    let receipt = machine
        .commit(pending, &authority, model::Fault::None)
        .unwrap();
    assert_eq!(
        receipt.committed_head,
        summary["committed_head"].as_u64().unwrap()
    );
    assert_eq!(
        machine.snapshot().claims.len() as u64,
        summary["claim_count"].as_u64().unwrap()
    );
    assert_eq!(
        receipt.source_authenticity,
        envelope::Guarantee::NotEstablished
    );
    assert_eq!(
        receipt.replay_protection,
        envelope::Guarantee::NotEstablished
    );
    let mut competing = request;
    competing.claim_id = "4096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e".into();
    assert!(machine.prepare(competing, &authority).is_err());
}
