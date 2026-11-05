# AUR native broker integration — 2026-11-06

**Source integration and isolated regression: PASS. Production admission: CLOSED.**

## Implemented boundary

The official broker no longer loads AUR archives into its official ALPM handle.
It runs the existing full official upgrade resolver under the normal pacman
transaction lock and refuses pending official upgrades, removals or official
repository targets. Official signature policy and keyring remain unchanged.

A fresh self-exec (`--aur-native`, private broker protocol, not a client API)
initializes GPGME with the fixed process-start public Archive domain
`/etc/zethropol-update/aur-native-trust`. Its Required/TrustedOnly native ALPM
handle registers no official repositories. It inherits host transaction rules,
architecture and hooks but cannot resolve/install official dependencies there.
No NOLOCK, signature/dependency bypass or caller-selected path is used.

The domain and ancestors must be root owned without group/other write access.
Only bounded public keybox/trustdb/config files and an empty private-key directory
are accepted. Primary fingerprints must exactly match the pinned Archive role;
redirects, discovery, extra keys, signing material and agent sockets are refused.
`deployment/aur/native-gpg.conf.example` documents the restricted configuration;
it does not provision keys, certify trust or deploy anything.

The parent validates the signed receipt and copies/seals artifacts without native
loading. The child verifies independent Archive signatures plus native trust,
name/version/architecture/pkgbase, SHA-256 and receipt signature digests. It
returns a nonce-bound canonical plan through a private sealed memfd. Install
requires the persisted, one-use, peer/operation-bound consent and inherited worker
lease; the child verifies the same lease inode/open description. Duplicate
workers cannot publish over the active worker.

The official DB/config/keyring/hooks and Archive policy/domain stamp must match
at the native lock handoff, after preparation, after snapshot and before commit.
Any change requires a new preview. Old AUR state fingerprints intentionally
become stale. No changed plan is silently authorized. The existing receipt TTL,
PolicyKit, production readiness, snapshot and durable admission checks remain.
Readiness is rechecked at dispatch and in the child; no LAB switch exists in the
release binary. Fixture path/grant/fault hooks compile only in tests.

Cancel before commit terminates the native process. A running commit is not
cancelled merely because preview TTL expires. A bounded execution timeout or
crash preserves Committed/Unknown evidence and snapshots, prohibits replay and
never unlinks a stale pacman lock or retries automatically. The existing fresh
installed-version readback follows native commit. Outcome persistence errors
propagate. No SQLite, evidence schema or post-reboot architecture was extended.

## Verified results

| Check | Result |
| --- | --- |
| Update Rust, offline/locked | 239 passed, 6 ignored |
| AUR candidate Rust, offline/locked | 178 passed, 6 ignored |
| Both cargo check targets | PASS; existing main dead-code warnings |
| AUR Clippy, all targets, -D warnings | PASS |
| Main Clippy, all targets, -D warnings | FAIL: inherited dead-code diagnostics; no suppression |
| Qt/QML build / CTest | PASS / 9 of 9 PASS |
| Live LAB sandbox, signed-root, cgroup lifecycle, actual PolicyKit denial | Four drivers PASS; no host installation |
| Python AUR suite | 481 passed, 60 historical capture cases skipped (541 discovered) |
| Changed Rust formatting / diff / credential-pattern scan | PASS |

Twelve new mandatory Rust cases exercise shared receipt/native exchange and real
ALPM commits to private LAB roots: valid signed delivery/readback/replay denial,
foreign/unsigned/mutated artifacts, plan/identity/state drift, strict public trust
inventory/configuration, official-upgrade barrier and domain independence,
lease/concurrency, authorization binding, cancellation, timeout and interrupted
commit evidence. The sixth ignored Rust helper is an explicitly invoked child
fixture, not a skipped acceptance scenario. Existing real two-domain LAB tests,
D-Bus validation, PolicyKit denial and UI readiness regression remain covered.
LAB grants/snapshots are fixture inputs; root deployment, real PolicyKit success,
boot and recovery acceptance are NOT RUN. No host package, account, keyring,
service or installed Control Center was changed.

Inherited main Clippy groups: `VerifiedIdentity::bus_name`,
`CanonicalPlan::matches`, broker `operation_for_message`/`retry_worker_start`/
`claim_worker`, persistent `sha256_hex`, `AurArchive::from_root_staging`, retained
receipt `intake`. These predate the integration; `update_worker_status` is now
used and obsolete mixed-handle preparation helpers are test-only.

## Remaining external acceptance prerequisites

1. A current, authenticated, owner-approved PUBLIC Manifest/signature/exact
   artifact set and independent UID949/951/950 chain. Previously discovered local
   manifests are expired/unsigned, not production inputs. A separately approved
   public native Archive keybox/trustdb must match the pinned Archive primaries;
   it must never enroll Archive keys in official pacman trust. No private key or
   password is requested, searched or published.
2. An identified approved disposable bootable guest/image and reviewed snapshot/
   durable journal layout. No supplied guest identity/image or usable local
   QEMU/libvirt/VirtualBox/KVM infrastructure was found. Follow
   [VM_ACCEPTANCE_PLAN.md](../../desktop/control-center/aur/VM_ACCEPTANCE_PLAN.md)
   with operation-bound authorization for guest privilege, install and faults.
   Verify real PolicyKit/D-Bus identity, deployed root native transactions,
   snapshots, power loss, boot and failed-update recovery with the approved
   public inputs. nspawn/bubblewrap and LAB transactions cannot certify this.

Both canonical and candidate production admission stay unconditionally closed.
These verified WIP changes do not claim production readiness or acceptance.
