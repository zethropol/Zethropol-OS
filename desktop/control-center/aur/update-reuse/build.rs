use std::{env, fs, path::PathBuf};
fn main() {
    let here = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let source = here
        .join("../../../../services/update/src")
        .canonicalize()
        .unwrap();
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let original_adapter = source.join("aur/adapter.rs");
    let original = fs::read_to_string(&original_adapter).unwrap();
    let start = original
        .find("fn require_aur_production_authority() -> Result<(), String> {")
        .expect("authority seam");
    let end = original[start..]
        .find("\nfn aur_execute_received(")
        .unwrap()
        + start;
    let authority = here.join("src/production_authority.rs");
    println!("cargo:rerun-if-changed={}", authority.display());
    let mut adapter = original[..start].to_owned();
    adapter.push_str(&fs::read_to_string(&authority).unwrap());
    adapter.push_str(&original[end..]);
    let receipt_original = fs::read_to_string(source.join("aur/receipt.rs")).unwrap();
    let capacity = here.join("src/root_capacity.rs");
    println!("cargo:rerun-if-changed={}", capacity.display());
    assert_eq!(
        receipt_original.matches("root.schema != 1").count(),
        1,
        "root schema seam changed"
    );
    assert_eq!(
        receipt_original.matches("root.packages.len() > 64").count(),
        1,
        "root count seam changed"
    );
    let mut receipt = receipt_original
        .replace("root.schema != 1", "!root_manifest_capacity_valid(root.schema, root.packages.len())")
        .replace("        || root.packages.len() > 64\n", "")
        .replace("        Ok(trust) => {", "        Ok(trust) => {\n            if let Err(error)=require_independent_roles(&trust.manifest.signers,&trust.archive.signers,&trust.receipt.signers){report.insert(\"trust_policy\".into(),\"invalid-role-separation\".into());report.insert(\"reason\".into(),error);return report;}\n            report.insert(\"installation\".into(),\"closed\".into());");
    receipt.push_str(&format!("\ninclude!({:?});\n", capacity));
    fs::write(out.join("aur_receipt.rs"), receipt).unwrap();
    // Includes remain canonical. Only the candidate authority seam differs.
    for name in ["receipt.rs", "intake.rs", "tests.rs"] {
        adapter = adapter.replace(
            &format!("include!(\"{name}\");"),
            &format!(
                "include!({:?});",
                if name == "receipt.rs" {
                    out.join("aur_receipt.rs")
                } else {
                    source.join("aur").join(name)
                }
            ),
        );
    }
    adapter=adapter.replace("    locked.verify_recovery()?;\n    action(locked)","    require_aur_production_authority()?;\n    locked.verify_recovery()?;\n    action(locked)");
    adapter = adapter.replace(
        "    require_aur_production_authority()?;\n    let _lease = store",
        "    let _lease = store",
    );
    adapter=adapter.replace("    let result = (|| {\n        store\n            .require_queued", "    let result = (|| {\n        require_aur_production_authority()?;\n        store\n            .require_queued");
    fs::write(out.join("aur_adapter.rs"), adapter).unwrap();

    for name in [
        "boot_verifier",
        "cache_cleanup",
        "broker_protocol",
        "execution_evidence",
        "integrity",
        "models",
        "session_identity",
        "snapshot_policy",
        "update_service",
    ] {
        let path = source.join(format!("{name}.rs"));
        println!("cargo:rerun-if-changed={}", path.display());
        // Reuse the existing implementation. Relocate documentation/include
        // paths; inject the development-only AUR methods at the seam below.
        let original = fs::read_to_string(&path).unwrap();
        let mut text = original
            .lines()
            .map(|line| {
                if let Some(rest) = line.strip_prefix("//!") {
                    format!("//{rest}")
                } else if line == "#![allow(dead_code)]" {
                    "// inherited module dead_code allowance".into()
                } else {
                    line.to_owned()
                }
            })
            .collect::<Vec<_>>()
            .join("\n");
        text = text.replace(
            "#[path = \"worker_integration_tests.rs\"]",
            &format!("#[path = {:?}]", source.join("worker_integration_tests.rs")),
        );
        for unit in [
            "zethropol-update-worker@.service",
            "zethropol-update.service",
        ] {
            text = text.replace(
                &format!("include_str!(\"../deployment/systemd/{unit}\")"),
                &format!(
                    "include_str!({:?})",
                    source.join("../deployment/systemd").join(unit)
                ),
            );
        }
        for name in [
            "adapter.rs",
            "broker.rs",
            "persistent.rs",
            "fixture_access.rs",
        ] {
            text = text.replace(
                &format!("include!(\"aur/{name}\");"),
                &format!(
                    "include!({:?});",
                    if name == "adapter.rs" {
                        out.join("aur_adapter.rs")
                    } else {
                        source.join("aur").join(name)
                    }
                ),
            );
        }
        if name == "update_service" {
            let methods = here.join("src/delivery_methods.rs");
            let support = here.join("src/delivery.rs");
            println!("cargo:rerun-if-changed={}", methods.display());
            println!("cargo:rerun-if-changed={}", support.display());
            assert_eq!(
                text.matches("impl UpdateManager {\n    // Metadata review")
                    .count(),
                1,
                "AUR interface seam changed"
            );
            text = text.replacen(
                "impl UpdateManager {\n    // Metadata review",
                &format!(
                    "impl UpdateManager {{\n{}\n    // Metadata review",
                    fs::read_to_string(methods).unwrap()
                ),
                1,
            );
            text.push_str(&format!("\ninclude!({:?});\n", support));
        }
        if name == "broker_protocol" {
            let start = text
                .find("    fn has_active_operation(&self)")
                .expect("active operation seam");
            let end = text[start..].find("    fn operation_path(").unwrap() + start;
            let body=text[start..end].replace("            ) {","            ) || matches!(record.evidence.package_commit, CommitState::Unknown | CommitState::InProgress | CommitState::Failed) {");
            text.replace_range(start..end, &body);

            let support = here.join("src/delivery_store.rs");
            println!("cargo:rerun-if-changed={}", support.display());
            text.push_str(&format!("\ninclude!({:?});\n", support));
        }
        fs::write(out.join(format!("{name}.rs")), text).unwrap();
    }
    // Reuse the canonical CLI, worker-stopped handler and reconciliation loop.
    // Do not create a divergent service implementation or an activation switch.
    let entry = source.join("main.rs");
    println!("cargo:rerun-if-changed={}", entry.display());
    let original = fs::read_to_string(&entry).unwrap();
    let seam = "#[tokio::main]\nasync fn main()";
    assert_eq!(
        original.matches(seam).count(),
        1,
        "canonical entry seam changed"
    );
    let text = original
        .lines()
        .filter(|line| !(line.starts_with("mod ") && line.ends_with(';')))
        .collect::<Vec<_>>()
        .join("\n")
        .replacen(seam, "pub async fn run_service()", 1);
    fs::write(out.join("entry.rs"), text).unwrap();
    println!("cargo:rerun-if-changed={}", source.join("aur").display());
}
