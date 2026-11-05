# Non-installed AUR broker candidate

**WIP; AUR installation remains closed.**

`build.rs` reuses the canonical `services/update` implementation with relocated
includes, the delivery methods and Schema 2 capacity/deadline support. The
`zethropol-update-aur-candidate` binary reuses the canonical service/worker entry
point. Compilation does not install, start or activate it.

The public trust, fixed role separation, durable journal and unresolved-commit
checks are prerequisites. Real PolicyKit/native-domain/snapshot/boot/recovery
acceptance is still mandatory; the candidate cannot return an installation grant
merely because those prerequisites pass. Readiness remains closed. Existing
native checks, caller-bound consent, sealed FD verification, one-use admission,
worker leases and no-retry behavior are retained.

Run from the repository root (private D-Bus/GPG sockets must be permitted):

```sh
cargo check --manifest-path desktop/control-center/aur/update-reuse/Cargo.toml --offline --locked
cargo test --manifest-path desktop/control-center/aur/update-reuse/Cargo.toml --offline --locked -- --test-threads=2
cargo clippy --manifest-path desktop/control-center/aur/update-reuse/Cargo.toml --offline --locked --all-targets -- -D warnings
```

Signed tests generate only ephemeral LAB keys and use private ALPM roots. Positive
native commits in those fixtures are not host or production acceptance. Ignored
live-session/host-scan tests are not counted as passed. See the
[current status](../../../../services/update/AUR_WIP_STATUS.md).

## Native broker integration (2026-11-06)

The broker now uses a fresh fixed-domain native child for AUR preview/install;
AUR archives never enter the official ALPM handle. Shared Rust tests execute
real signed transactions only in private LAB roots, including foreign signature,
state/plan drift, replay, cancellation and interrupted-commit refusal. The
private `--aur-native` transport accepts no client-selected trust domain.
Production admission remains closed pending approved PUBLIC trust/artifact
inputs and actual deployed root/guest acceptance. See
[the integration report](../../../../services/update/AUR_NATIVE_BROKER_INTEGRATION.md).
