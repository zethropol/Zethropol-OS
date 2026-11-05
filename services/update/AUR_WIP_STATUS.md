# AUR Update WIP status — 2026-11-05

**INCOMPLETE / NOT PRODUCTION READY. Installation stays closed.**

## Published scope

This WIP preserves the existing Update broker/worker, receipt/persistent state,
SQLite/atomic-claim and post-reboot evidence work without extending those
architectures. It adds the previously local AUR worker/provider/candidate,
Control Center source-review/consent/progress/cancellation integration and tests.
Hardware changes, backup files, package build archives, private keys and personal
historical captures are excluded. No service/UI/keyring was installed or changed
on the real user system and no real user package was built or installed.

Additional corrections:

* Keep canonical and candidate production gates closed until real security
  acceptance; keyring pins and durable storage are not acceptance.
* Require explicit complete/ready broker state for both UI install buttons and
  programmatic request/confirmation. A receipt hash or stale consent cannot
  override missing, failed or revoked readiness.
* Enforce Archive UID 951 and independent Attestor UID 950 consistently in
  runtime configuration and sysusers source. Manifest UID 949 stays separate;
  no account or key provisioning was performed.
* Repair the signed-build LAB package fixture to exclude pacman database payload.
  The provisioner still rejects supplied database files and derives metadata
  from signed package identity.
* Make sixty tests of separately retained historical public host captures
  explicit opt-in. Keep core AUR safety tests mandatory and portable.

## Current independent validation

The selected source/test tree was copied into a separate Git worktree and built
without Hardware changes or ignored historical/build artifacts. The first clean
run exposed a missing SQLite design document dependency, which is now included,
and nonportable historical captures, which remain separately supplied opt-in
inputs. Neither failure was represented as a production success.

| Check | Result |
| --- | --- |
| Main Update cargo check, offline/locked | PASS with existing dead-code warnings |
| Main Update Rust tests | 227 passed, 5 ignored; includes 50 SQLite fixture checks inside the adapter test |
| AUR candidate cargo check/test, offline/locked | PASS; 166 passed, 5 ignored |
| AUR candidate Clippy all targets, -D warnings | PASS |
| Main Update Clippy all targets, -D warnings | FAIL: inherited dead-code diagnostics listed below |
| rustfmt on all 49 selected Rust files | PASS |
| Qt/QML independent configure/build | PASS; existing optional Vulkan/Qt policy warnings |
| CTest | 9/9 targets passed |
| Portable Python AUR suite | 481 passed, 60 historical tests skipped; 541 discovered |
| Real LAB sandbox/signed-root build/cgroup lifecycle/PolicyKit denial | PASS; four separate live drivers, no host package installation |
| Diff whitespace and credential-pattern/path scan | PASS; no private key block, GitHub/AWS token, credential URL, Hardware/backup/keyring/build-archive path in selected sources |

D-Bus/GPG socket restrictions were separated from code failures by running tests
in an environment allowing private test sockets. Positive fixture authorization
and private-root native commits are not real production authorization.

Main Clippy inherited diagnostics: `VerifiedIdentity::bus_name`,
`CanonicalPlan::matches`, broker methods `operation_for_message`,
`retry_worker_start`, `claim_worker`, `update_worker_status`, persistent
`sha256_hex`, `AurArchive::from_root_staging`, `prepare_aur_locked`,
`prepare_aur_locked_internal`, and the retained receipt `intake` field.
Some are test-only or reserved integration helpers; they were not deleted or
blanket-suppressed merely to publish WIP. New AUR-target diagnostics are clear.

## Remaining production dependencies and acceptance

1. An authenticated, current, owner-approved PUBLIC Manifest/signature and exact
   distribution artifact set, reviewed source/build-root expectations, and the
   real independent UID951/950 build/output-signature/receipt chain under the
   original deadline. The six discovered local Manifest candidates are expired
   and unsigned. The fixed provider input/build-root directories are empty;
   provider configuration is absent. Existing public keyrings/account names do
   not establish a current approved artifact set. Private approval storage could
   not be inspected and is not claimed absent. No private-key search or signing
   operation was attempted.
2. Actual Archive native trust-domain separation and full provider/broker/UI
   acceptance with those approved artifacts. Existing code verifies sealed
   bytes, package name/version/architecture/hash/signature and locked plan;
   the final path still uses the official ALPM domain. The separate native
   loader is mechanism-only and is not certified as integrated authority.
   No key was silently enrolled in official trust and no signature policy was
   relaxed to make installation succeed.
3. An identified disposable bootable guest/image with reviewed Btrfs/root and
   independent journal layout. QEMU, qemu-img, libvirt/virsh and VirtualBox were
   unavailable; /dev/kvm was absent and no guest identity/image was supplied.
   Available nspawn/bubblewrap cannot replace real boot/power-loss/snapshot
   recovery acceptance. Real PolicyKit grants, guest native installs, snapshot
   restoration, post-boot state and failed-update recovery remain NOT RUN.

The release owner must approve the public artifact/role set and exact signing
operation through the existing authorization mechanism, and identify/provide the
disposable guest. Do not send private keys or passwords. Guest fault/install
operations must retain their operation-bound authorization. Production gates
may only be reconsidered after native-domain integration and all required real
acceptance evidence have been independently reviewed; no runtime enable flag
or LAB key opens them.

## Production acceptance follow-up

See [current follow-up](AUR_PRODUCTION_ACCEPTANCE_STATUS.md) for the new
signature-status checks, downgrade refusal, fresh installed-version readback,
and actual two-domain LAB native transactions. The source preflight entrypoint
now works in both source and staged layouts and always reports installation
closed, even if all public prerequisites pass. These changes do not complete
production native-domain integration or replace missing guest acceptance.

## Git integration

The two existing local commits are preserved. The remote README commit
`81b6ab6b3b6565f37ab644696a188e5da3e18896` was merged normally as
`95c34d41c643d6d5fc09a207b8ef69af1fddd055` before WIP commits.
The original dirty working tree and unrelated packaging/Hardware changes were
preserved. Only verified WIP source/test/document paths are published, with no
force push or completed-release claim. Push/remote HEAD are verified separately
and reported by the development session; this document does not predict them.
