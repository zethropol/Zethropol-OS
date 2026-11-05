# Replacement-aware Update model

## Implemented source flow

The configured official repository libalpm transaction is resolved in a private
sync database against installed metadata while holding the real pacman database
lock. No package payload is downloaded or committed by preview. AUR and Flatpak
remain separate scan/transaction models; their user selections do not enter this
plan or grant.

Preview answers yes only to libalpm Replace questions for noncritical packages,
recording each installed package name/version and its incoming replacement name.
It then compares these decisions with the exact trans_remove set. Repeated
identical questions are deduplicated; competing replacement edges for one old
package, absent target additions and unaccounted removals are rejected. Additions
retain name, installed/new version, repository and download size. Names of
conflicting packages and dependency-unresolved targets are reported in errors.

CanonicalPlan binds sorted additions and directed versioned removals with a
length-prefixed, domain-separated SHA-256. PreviewPlan returns the **same** plan
that is stored for the PolicyKit-authorized bus sender/session. Qt shows INSTALL,
UPGRADE, REMOVE and REPLACED BY, the reason and plan hash. A separate unchecked
checkbox is required for removals. A changed preview resets backend consent;
changing the displayed hash disables the confirmation button until reviewed
again. Hashless legacy previews cannot authorize execution. The source D-Bus
policy includes PreviewPlan; installed policy remains untouched.

TriggerUpdate requires the reviewed hash and explicit removal approval, then
atomically consumes the sender-owned nonce using existing install PolicyKit,
expiry, flock and operation checks. The queued record persists the approved
removal set, explicit consent bit and audit hash/consent message. The worker re-resolves under the
pacman lock and compares all fields/removal edges before claim, pre-snapshot or
commit. It retains the existing worker lease, mandatory recovery chain,
snap-pac contract, hook uncertainty and boot evidence behavior.

## Support boundaries

| Effect | Source behavior |
| --- | --- |
| Normal noncritical rename / many old packages replaced by one new target | Allowed only after explicit approval of the full final set |
| Conflict-driven removal | Rejected, with involved package names/reason |
| Unresolvable dependencies / libalpm RemovePkgs target skipping | Rejected; never silently produces a partial upgrade |
| Installed dependency/cascade removal without an explicit replacement edge | Rejected by final removal-set reconciliation |
| One old package with competing replacements / ambiguous provider | Rejected |
| Critical package removal | Rejected even when a replacement is declared |
| Metadata or removal/version/edge drift, expiry, replay, wrong sender/hash | Rejected before commit |
| AUR / Flatpak | Excluded from official transaction authorization |

Critical policy includes kernel, init, libc, package manager, authentication,
base tools and recovery/boot package names and their package-family prefixes.
Resolver additionally rejects removal of installed owners of boot/, efi/,
usr/lib/modules/, usr/share/limine/ and worker-unit files, including custom-named
kernels. This is deliberately conservative, not an exhaustive proof that every
custom critical service is identified. Unsupported critical migrations need a
separate reviewed workflow; the user cannot bypass these gates with a checkbox.

## Verification and production gates

Rust: 93 passed, 0 failed, 2 ignored (95 total); ignored live KDE/policy fixture
and subprocess fixture entry are not enabled as live tests. Actual installed
libalpm is exercised using synthetic local/sync metadata solely under /tmp for
normal replacement, unrelated conflict, critical/custom-kernel removal and
unresolved dependency cases. No download, package commit, real snapshot or
service activation is executed. Store tests cover false consent, wrong hash,
wrong PolicyKit action/owner, replay, full removal drift, ambiguous/critical
plans, format 1–5 compatibility and persistence, archive/repository/policy
drift, checksum mismatch, hook changes and replay rejection. Private-bus introspection checks
the exact PreviewPlan outputs and TriggerUpdate inputs.

Qt tests cover real peer-D-Bus preview decoding, unsupported/duplicate/missing
replacement edges and missing hash; service tests cover explicit/stale/hashless
consent; rendered QML tests exercise unchecked confirmation, plan change,
cancellation and explicit acceptance. Run the full CTest suite, Clippy with
warnings denied, cargo fmt check and Rust release/Qt application builds.

Before production deployment:

- In a disposable VM, exercise actual rename/many-to-one commits and verify
  resulting installed database, dependencies, hooks and exactly one recovery
  snapshot chain; repeat with drift, concurrent pacman, cancel/reconnect and
  broker/worker interruption. Metadata-only tests cannot establish this.
- Validate installed D-Bus policy, KDE session identity, PolicyKit interaction
  and end-to-end Qt consent with matching broker/worker/client versions. Old
  clients fail closed; old binaries cannot read format-5 records. Do not
  downgrade with live records.
- Validate format-5 archive SHA-256, repository database/signature digest,
  effective pacman/hook policy fingerprint and libalpm signature recheck against
  actual configured signed repositories. Synthetic tests do not establish
  package semantic safety or ABI compatibility; repository signature policy
  remains required and is not relaxed.
- HookRunDone still has no per-hook exit result. Hook success and successful new
  kernel boot remain unknown unless directly observed; preserve existing
  warning states. Validate DKMS/initramfs/Limine/reboot/recovery in the VM.

This work changes source files only. Production units, services, packages,
snapshots and boot artifacts were not changed; no Git commit/push was made.
