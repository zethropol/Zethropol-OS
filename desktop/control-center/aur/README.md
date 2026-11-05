# AUR Update development candidate

**WIP — production installation remains closed.**

See [current status](../../../services/update/AUR_WIP_STATUS.md) for the verified
scope, skipped historical checks, remaining external inputs and release limits.
This source tree is not an instruction to install a service or import a key.

The Control Center reviews pinned PKGBUILD/.SRCINFO declarations, maintains
source-specific selections and obtains broker-owned build expectations. The
unprivileged pipeline consumes a signed/provisioned root, seals reviewed inputs,
verifies sources, builds inside a restricted namespace/cgroup and records durable
results. Independent Archive/Attestor providers rebuild and authenticate outputs;
sealed receipt/manifest/archive FDs cross the broker boundary. The existing
locked native resolver rejects identity drift, dependency failures and pending
official upgrades. It does not authorize a partial official upgrade.

The canonical Update service and separately compiled candidate both keep AUR
installation closed. Public trust alone never certifies real authorization,
native-domain separation, snapshots or recovery. The UI blocks installation
requests while broker readiness is missing, closed or incompatible. Confirmation
is selection/receipt-bound and never replaces per-operation PolicyKit checks.

* [Receipt protocol](RECEIPT_PROTOCOL.md)
* [Provider deadline protocol](PROVIDER_DEADLINE_PROTOCOL.md)
* [Root plan protocol](ROOT_PLAN_PROTOCOL.md)
* [Production trust inputs](PRODUCTION_TRUST_INPUTS.md)
* [Disposable-system acceptance plan](VM_ACCEPTANCE_PLAN.md)
* [Test scope and commands](tests/README.md)

The manifest preparation account is UID 949; Archive is UID 951 and independent
Attestor is UID 950. Provider account mapping must match those fixed UIDs. Key
homes remain private to their roles. Only owner-approved public policy and exact
operation-approved artifacts may supply production authority. No LAB key or
historical capture is a production bootstrap.

No installed Control Center, system unit, keyring or host package was changed by
publishing these sources. Deployment, signing and a real native transaction
retain their separate operation authorization boundaries.

## Native broker integration (2026-11-06)

The broker now uses a fresh fixed-domain native child for AUR preview/install;
AUR archives never enter the official ALPM handle. Shared Rust tests execute
real signed transactions only in private LAB roots, including foreign signature,
state/plan drift, replay, cancellation and interrupted-commit refusal. The
private `--aur-native` transport accepts no client-selected trust domain.
Production admission remains closed pending approved PUBLIC trust/artifact
inputs and actual deployed root/guest acceptance. See
[the integration report](../../../services/update/AUR_NATIVE_BROKER_INTEGRATION.md).
