// Intentionally empty build script.
// Previously this file overwrote src/security_service.rs with a legacy version
// that bypassed Polkit authorization. src/security_service.rs is now the single
// authoritative source file.
fn main() {
    println!("cargo:rerun-if-changed=build.rs");
}
