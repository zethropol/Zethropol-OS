# Update broker protocol and current integration status

`UpdateManager` now uses `broker_protocol.rs` for `PreviewTransaction`,
`TriggerUpdate`, and `GetOperationState`. Preview and operation records are
stored under `/run/zethropol-update`. Trigger starts the fixed
`zethropol-update-worker@.service` unit with only the broker-created operation
UUID. The old in-memory preview helpers remain only for legacy unit coverage;
the live trigger path does not invoke them.

## Identity and authorization

D-Bus handlers take the unique sender only from the received header. UID and
PID come from the system bus. Direct logind sessions must belong to that UID.
KDE/systemd user-manager processes may live outside any session scope. Only
logind's explicit `NoSessionForPID` permits the fallback: `GetUserByPID` must
resolve the same UID, and that user's real `Display` must be a member of its
`Sessions`, owned by that UID, active, local, graphical (Wayland/X11), and a
user-class session. Missing, lingering-only, remote, foreign, or changing
associations are rejected; no synthetic session or environment hint is used.
Other lookup failures never trigger the fallback.

PolicyKit still checks the fixed action against the original unique bus name,
not the display session or a replacement process. UID/PID are rechecked after
resolution, and the full identity is resolved again after authorization to
reject disconnects and session changes during authentication. No request
argument supplies an identity. Records remain bound to the unique name, UID,
and actual logind session ID.

## Preview, worker, and status lifecycle

Each preview gets a random UUID token and operation UUID. The token remains in
the root-only server record and is not returned to Control Center. A locked
rename consumes the preview on `TriggerUpdate`; concurrent calls cannot
consume it twice. The worker accepts only the UUID argument emitted by the
broker's fixed systemd unit. Before any package-manager access it acquires a
nonblocking per-operation flock lease and verifies the record is queued. The
lease lasts through result publication; a duplicate exits without touching the
record. Lease files are retained until reboot to avoid inode replacement races.
The executor refreshes repositories, acquires libalpm's pacman lock, prepares
the current transaction, and atomically compares the full canonical package
vector and claims the record under the store lock. Pacman keeps its lock until
the transaction is released. A consumed or claimed operation cannot run again.

The operation-state map shape remains unchanged. PreviewPlan adds package
archive and repository metadata identity fields; its current signature is
`a(sssstsss), a(ssss), s`.
Stored statuses map to the existing `pending`, `running`, `ok`, and `failed`
values. Status reads require preview PolicyKit authorization and an exact
match on unique bus name, UID, and logind session.

## Runtime storage and failure behavior

Production storage is fixed at `/run/zethropol-update`; runtime startup
requires effective UID 0. The directory must be root-owned mode `0700`, and
lock and record files root-owned mode `0600`. Opens reject symlinks, updates
use an inter-process `flock`, fsync, and atomic rename, and records have strict
versioned decoding and size limits. Time is `CLOCK_BOOTTIME`; `/run` is cleared
at reboot.

Worker start failure is recorded without overwriting a claim or a completed
result. An unconfirmed start returns its operation ID for inspection. There is
no automatic retry. `ExecStopPost` records interrupted queued/running workers
as failed and preserves completed results. Startup, status reads, and a
30-second broker sweep reconcile systemd ActiveState; a held worker lease
prevents a stale observation from failing a live worker. An abandoned queue
has a 60-second handoff grace. Preview expiry alone never fails a queued
worker: it may still hold its lease while refreshing repositories. Inactive workers without a lease become terminal
failures, including when a stop hook was missed. If systemd inspection fails, reconciliation logs the inspection error and
uses the worker lease plus the same queue handoff grace. It cannot fail a
worker holding a lease; after failure, a delayed worker is rejected before
package-manager access. Store/lease errors remain explicit errors.
A partial transaction is possible; neither retry nor recovery is guaranteed.
Records live in /run and do not provide post-reboot transaction history.

## Validation limits

Unit tests use mock identities and PolicyKit decisions. The private D-Bus test
checks the existing method contract only. A separate ignored live test checks existing KDE peers' actual system-bus
UID/PID, logind resolution and noninteractive preview PolicyKit decision. It
does not activate the update broker or worker, request install authorization,
or establish deployed Control Center integration success. Package verification checks source parity and the vendor worker unit path and
mode. Live systemd activation, effective sandbox permissions, package hooks,
and recovery still require a disposable integration environment; this is not
production validation.

## Execution evidence (record formats 2/3)

PolicyKit, sender binding, preview token consumption, canonical plan comparison,
worker lease and the libalpm transaction lock remain unchanged. The evidence extension originally used
`ZETHROPOL-UPDATE-BROKER-3`; the current writer uses format 5. The reader accepts formats 1–4 but assigns unknown
execution evidence; an old `succeeded` record does not certify hook or boot
success. Old binaries cannot read format 2: deploy broker/worker together and
never downgrade over live format-2 records.

`GetOperationState` retains its `a{ss}` wire type and existing keys. It adds
`package_commit`, `hooks`, `boot_preparation`, `pre_snapshot`, `post_snapshot`.
Commit values: `not-started`, `in-progress`, `committed`,
`failed-may-be-partial`, `unknown`. Hook/boot values: `unknown`, `failed`,
`verified`. There is currently **no production verifier** that emits hook or
boot `verified`. An explicit libalpm `TransHookFailed` records hook `failed`;
commit success alone leaves it unknown. Snapshot IDs are evidence of snapshot
creation, not boot recovery verification.

The worker writes an atomic, fsynced `in-progress` checkpoint before entering
commit, records its return value before releasing the transaction lock, then
records the attempted post-snapshot. A stop/reconciliation turns an interrupted
`in-progress` into `unknown`; confirmed commit evidence is preserved. Terminal
`completed-with-warnings` survives normal ExecStopPost and is not replayable.
On the wire it is `ok_with_warnings`; legacy `succeeded` is also a warning unless
all three execution evidence fields are verified. Qt displays each result and
keeps the warning instead of automatically overwriting it with an up-to-date
scan.

## Read-only artifact extension (record format 3)

The artifact extension introduced `ZETHROPOL-UPDATE-BROKER-3`. Readers accept formats 1/2
without inventing missing observations. Additional `a{ss}` keys and exact proof
scope are documented in [BOOT_VERIFICATION.md](BOOT_VERIFICATION.md). Artifact
results are persisted while the worker owns the pacman lock; they do not mutate
hook execution status. Hook lifecycle records include the linked libalpm
version, but HookRunDone is never interpreted as exit success. A local artifact
check is distinct from `new_kernel_boot`, which remains unknown pre-reboot.
Old binaries cannot read format 3; deployment/downgrade must respect live records.

## Replacement and content integrity (record format 5)

`PreviewPlan() -> a(sssstsss), a(ssss), s` returns separate top-level outputs:
additions (name, installed/new version, repository, download size, filename,
trusted SHA-256, repository metadata SHA-256), removal edges
(name, installed version, reason, replacement target), and lowercase SHA-256.
`TriggerUpdate(s plan_hash, b approve_removals)` requires the exact stored hash;
any nonempty removal set additionally requires true. Preview and install still
use their existing PolicyKit actions, real bus identity/session, TTL, store flock
and one-use token. No client-supplied effect list becomes transaction authority.
Approval is checked inside the atomic consumption lock, before rename. The queued
record retains the complete approved set and authorization timestamp and an explicit consent bit; its audit
message records the approved hash and explicit removal consent. Workers compare
full canonical plans under the real pacman lock before claim and commit.

The hash uses domain-separated length-prefixed UTF-8 fields and fixed-width
big-endian counts/sizes. Canonical additions and removals are sorted; duplicates,
missing replacement targets, critical removals, conflict/dependency/unknown
reasons are rejected. It covers every addition field and removal name, installed
version, reason and replacement edge. Record format 5 appends the removal set
and consent, policy fingerprint, and each package's filename, SHA-256 and
repository metadata digest. The canonical plan hash covers all these values.
Queued/running/terminal removal records without consent are corrupt and cannot
be claimed. Readers accept formats 1–4 as legacy plans without fabricating
content identity; those records cannot authorize a new worker claim. Old
binaries cannot read format 5. Legacy PreviewTransaction rejects replacement plans; old argumentless
TriggerUpdate calls fail. Ship broker/worker/client and the source PreviewPlan
D-Bus allow rule together. Installed production policy was not modified.

Official-repository identity comes from the configured libalpm sync database:
the selected package filename and SHA-256 are bound alongside a digest of the
repository database and detached signature. A fingerprint also binds effective
pacman configuration, keyring and configured hook files. Missing/malformed
trusted checksums, ambiguous repository databases, unreadable or unsupported
policy files fail closed. AUR and Flatpak are separate transaction models and
are not covered by this official-repository attestation.

The worker resolves and claims the approved plan under the pacman lock, then
performs an ALPM download-only phase. It refreshes/re-resolves repository
metadata and policy, checks downloaded archive SHA-256, filename, package
name/version, and loads each archive through libalpm with the originating
repository's package signature policy before pre-snapshot and again immediately
before commit. Any content, metadata, policy, or plan drift aborts before the
real commit. Signature verification settings are not weakened. This source
flow has synthetic tests only; download-only/signature behavior and actual
repository transactions remain a disposable-VM gate.

See [REPLACEMENT_MODEL.md](REPLACEMENT_MODEL.md) for resolver and VM limitations.
