# Atomic claim SQLite storage design

Task 16 design, exercised by Task 17 test-only fixtures. No production database,
dependency or code/policy change is introduced. This refines
ATOMIC_CLAIM_RECOVERY_REVIEW.md.

## Scope and correspondence to the pure model

`atomic_claim.rs` publishes claim, Consume event and incremented head together;
it rechecks prepared requests and quarantines uncertain I/O outcomes. Recovery
validates complete baseline lifecycle history, references and receipt linkage.
Its producer/currentness booleans remain assumptions. This proposed store must
replace those assumptions with reviewed admission/authority mechanisms before
production use; storing a boolean in SQLite would not establish trust.

Keep the existing `/run` broker and its seven-day cleanup unchanged. The broker's
root-owned directories, safe-open checks, flock, fsync and atomic replacement
are useful precedents, but do not make separate claim/event/head files atomic.
Use one separate root-controlled database, one owner service and one writer API
for ALL reference lifecycle transitions. No independent writable log/head.

## Schema version, representation and limits

Require a supported SQLite build with STRICT tables (minimum syntax version
3.37.0), foreign keys and triggers enabled. Pin/review an actual supported
release before implementation. Store schema version is independent of envelope
schema v1. Require matching application_id, user_version, metadata and expected
schema/trigger definitions; version integers alone do not attest schema safety.

IDs below are 16-byte UUID BLOBs; digests are 32-byte SHA-256 BLOBs. Convert
only after the existing canonical UUID/hex validators succeed. BLOBs avoid text
collation/case ambiguities. SQL length checks do not authenticate UUIDs/hashes.
Envelope bytes remain limited to 4096; parse and recompute their binding with
the existing validator before admission. Duplicated SQL commit/digest columns
must equal the decoded envelope, or admission/recovery fails.

SQLite INTEGER is signed: explicitly limit counters to 0..9223372036854775807,
generations to 1..9223372036854775807. Checked conversion from model u64 is
mandatory; no REAL conversion, casts, truncation or wrap. At exhaustion freeze
the operation/epoch, never reset it. Preserve current bounded history/reference
limits (1024 events, 256 references per operation), with an explicit future
review for larger histories. Retain receipts/tombstones until epoch retirement;
runtime evidence expiry must not resurrect consumption or permit key reuse.

## SQL schema draft (executed only in isolated test fixtures)

Forward table references and declared deferred FKs intentionally support cyclic
claim/event and reference/claim linkage. Every composite parent key below has
an exact PRIMARY KEY or UNIQUE constraint. No ON DELETE CASCADE, REPLACE,
IGNORE, or mutation via UPSERT is allowed. Views are not new trust boundaries.

```sql
CREATE TABLE store_meta (
  singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
  schema_version INTEGER NOT NULL CHECK(schema_version = 1),
  epoch BLOB NOT NULL UNIQUE CHECK(length(epoch) = 16),
  admission_state TEXT NOT NULL
    CHECK(admission_state IN ('RECOVERY_REQUIRED','READY','RETIRED'))
) STRICT;

CREATE TABLE operation_heads (
  epoch BLOB NOT NULL CHECK(length(epoch) = 16),
  operation_id BLOB NOT NULL CHECK(length(operation_id) = 16),
  head INTEGER NOT NULL CHECK(head >= 1),
  latest_generation INTEGER NOT NULL CHECK(latest_generation >= 1),
  active_generation INTEGER,
  CHECK(active_generation IS NULL OR
        (active_generation >= 1 AND active_generation <= latest_generation)),
  PRIMARY KEY(epoch, operation_id),
  FOREIGN KEY(epoch) REFERENCES store_meta(epoch),
  FOREIGN KEY(epoch, operation_id, latest_generation)
    REFERENCES binding_references(epoch, operation_id, generation)
    DEFERRABLE INITIALLY DEFERRED,
  FOREIGN KEY(epoch, operation_id, active_generation)
    REFERENCES binding_references(epoch, operation_id, generation)
    DEFERRABLE INITIALLY DEFERRED,
  FOREIGN KEY(epoch, operation_id, head)
    REFERENCES lifecycle_events(epoch, operation_id, sequence)
    DEFERRABLE INITIALLY DEFERRED
) STRICT;

CREATE TABLE binding_references (
  epoch BLOB NOT NULL CHECK(length(epoch) = 16),
  operation_id BLOB NOT NULL CHECK(length(operation_id) = 16),
  generation INTEGER NOT NULL CHECK(generation >= 1),
  binding_sha256 BLOB NOT NULL CHECK(length(binding_sha256) = 32),
  envelope_schema INTEGER NOT NULL CHECK(envelope_schema = 1),
  envelope BLOB NOT NULL CHECK(length(envelope) BETWEEN 1 AND 4096),
  commit_result TEXT NOT NULL CHECK(commit_result IN
    ('not-started','in-progress','committed','unknown','failed-may-be-partial')),
  state TEXT NOT NULL CHECK(state IN
    ('Candidate','Active','Consumed','Revoked','Superseded')),
  consumed_claim_id BLOB,
  CHECK((state = 'Consumed' AND consumed_claim_id IS NOT NULL
         AND length(consumed_claim_id) = 16)
     OR (state <> 'Consumed' AND consumed_claim_id IS NULL)),
  PRIMARY KEY(epoch, operation_id, generation),
  UNIQUE(epoch, operation_id, generation, binding_sha256),
  UNIQUE(epoch, operation_id, generation, binding_sha256, state,
         consumed_claim_id),
  FOREIGN KEY(epoch, operation_id)
    REFERENCES operation_heads(epoch, operation_id)
    DEFERRABLE INITIALLY DEFERRED,
  FOREIGN KEY(consumed_claim_id, epoch, operation_id, generation, binding_sha256)
    REFERENCES claims(claim_id, epoch, operation_id, generation, binding_sha256)
    DEFERRABLE INITIALLY DEFERRED
) STRICT;

CREATE UNIQUE INDEX one_active_reference
  ON binding_references(epoch, operation_id) WHERE state = 'Active';

CREATE TABLE lifecycle_events (
  epoch BLOB NOT NULL CHECK(length(epoch) = 16),
  operation_id BLOB NOT NULL CHECK(length(operation_id) = 16),
  sequence INTEGER NOT NULL CHECK(sequence >= 1),
  generation INTEGER NOT NULL CHECK(generation >= 1),
  binding_sha256 BLOB NOT NULL CHECK(length(binding_sha256) = 32),
  action TEXT NOT NULL CHECK(action IN
    ('Create','Activate','Consume','Revoke','Supersede')),
  claim_id BLOB,
  CHECK((action = 'Consume' AND claim_id IS NOT NULL AND length(claim_id) = 16)
     OR (action <> 'Consume' AND claim_id IS NULL)),
  PRIMARY KEY(epoch, operation_id, sequence),
  UNIQUE(epoch, operation_id, generation, binding_sha256, sequence, claim_id),
  FOREIGN KEY(epoch, operation_id, generation, binding_sha256)
    REFERENCES binding_references(epoch, operation_id, generation, binding_sha256),
  FOREIGN KEY(epoch, operation_id, generation, binding_sha256, sequence, claim_id)
    REFERENCES claims(epoch, operation_id, generation, binding_sha256,
                      committed_head, claim_id)
    DEFERRABLE INITIALLY DEFERRED
) STRICT;

CREATE UNIQUE INDEX one_consumption
  ON lifecycle_events(epoch, operation_id, generation) WHERE action = 'Consume';

CREATE TABLE claims (
  claim_id BLOB PRIMARY KEY NOT NULL CHECK(length(claim_id) = 16),
  epoch BLOB NOT NULL CHECK(length(epoch) = 16),
  operation_id BLOB NOT NULL CHECK(length(operation_id) = 16),
  generation INTEGER NOT NULL CHECK(generation >= 1),
  binding_sha256 BLOB NOT NULL CHECK(length(binding_sha256) = 32),
  expected_head INTEGER NOT NULL
    CHECK(expected_head >= 1 AND expected_head < 9223372036854775807),
  committed_head INTEGER NOT NULL CHECK(committed_head = expected_head + 1),
  producer_id BLOB NOT NULL CHECK(length(producer_id) = 16),
  purpose TEXT NOT NULL CHECK(purpose = 'associate_boot_evidence'),
  request_sha256 BLOB NOT NULL CHECK(length(request_sha256) = 32),
  required_state TEXT NOT NULL DEFAULT 'Consumed' CHECK(required_state = 'Consumed'),
  UNIQUE(epoch, operation_id, generation),
  UNIQUE(claim_id, epoch, operation_id, generation, binding_sha256),
  UNIQUE(epoch, operation_id, generation, binding_sha256, committed_head, claim_id),
  FOREIGN KEY(epoch, operation_id, generation, binding_sha256,
              required_state, claim_id)
    REFERENCES binding_references(epoch, operation_id, generation,
                                  binding_sha256, state, consumed_claim_id)
    DEFERRABLE INITIALLY DEFERRED,
  FOREIGN KEY(epoch, operation_id, generation, binding_sha256,
              committed_head, claim_id)
    REFERENCES lifecycle_events(epoch, operation_id, generation,
                                binding_sha256, sequence, claim_id)
    DEFERRABLE INITIALLY DEFERRED
) STRICT;

CREATE VIEW consumed_events AS
  SELECT epoch, operation_id, generation, binding_sha256, sequence, claim_id
  FROM lifecycle_events WHERE action = 'Consume';

CREATE TRIGGER claims_no_update BEFORE UPDATE ON claims
BEGIN SELECT RAISE(ABORT, 'immutable_claim'); END;
CREATE TRIGGER claims_no_delete BEFORE DELETE ON claims
BEGIN SELECT RAISE(ABORT, 'immutable_claim'); END;
CREATE TRIGGER events_no_update BEFORE UPDATE ON lifecycle_events
BEGIN SELECT RAISE(ABORT, 'immutable_event'); END;
CREATE TRIGGER events_no_delete BEFORE DELETE ON lifecycle_events
BEGIN SELECT RAISE(ABORT, 'immutable_event'); END;

CREATE TRIGGER reference_no_identity_update BEFORE UPDATE ON binding_references
WHEN NEW.epoch IS NOT OLD.epoch OR NEW.operation_id IS NOT OLD.operation_id
  OR NEW.generation IS NOT OLD.generation OR NEW.binding_sha256 IS NOT OLD.binding_sha256
  OR NEW.envelope IS NOT OLD.envelope OR NEW.envelope_schema IS NOT OLD.envelope_schema
  OR NEW.commit_result IS NOT OLD.commit_result
BEGIN SELECT RAISE(ABORT, 'immutable_reference'); END;
CREATE TRIGGER reference_transition BEFORE UPDATE OF state ON binding_references
WHEN NOT ((OLD.state = 'Candidate' AND NEW.state IN ('Active','Revoked'))
       OR (OLD.state = 'Active' AND NEW.state IN ('Consumed','Revoked'))
       OR (OLD.state = 'Revoked' AND NEW.state = 'Superseded'))
BEGIN SELECT RAISE(ABORT, 'invalid_reference_transition'); END;
-- Consume must name the head published by this transaction, with no Active pointer.
CREATE TRIGGER consume_matches_head BEFORE INSERT ON lifecycle_events
WHEN NEW.action = 'Consume' AND NOT EXISTS (
  SELECT 1 FROM operation_heads h
  WHERE h.epoch = NEW.epoch AND h.operation_id = NEW.operation_id
    AND h.head = NEW.sequence AND h.active_generation IS NULL
)
BEGIN SELECT RAISE(ABORT, 'consume_head_mismatch'); END;
CREATE TRIGGER heads_monotonic BEFORE UPDATE ON operation_heads
WHEN NEW.epoch IS NOT OLD.epoch OR NEW.operation_id IS NOT OLD.operation_id
  OR OLD.head = 9223372036854775807 OR NEW.head <> OLD.head + 1
  OR NEW.latest_generation < OLD.latest_generation
BEGIN SELECT RAISE(ABORT, 'invalid_head_transition'); END;
```

One consumption per generation is enforced independently by claims UNIQUE and
the partial consumed-event index. Deferred mutual FKs require matching claim,
Consume event and Consumed reference at commit; a claim without its pair cannot
commit. Non-Consume events intentionally have NULL claim_id, so their claim FK
is skipped. Every required claim/reference identity field is NOT NULL.

Not every semantic invariant is expressible here. The writer must validate full
ordered event replay, contiguous heads/generations, active pointer equality,
latest generation, approved committed envelope content and explicit revocation
before supersession. No constraints prove source trust. All creation, revoke,
activate and supersede APIs must share the same transaction boundary and replay
checks. They are NOT being implemented by this draft. Row deletion, epoch
retirement, provisioning and migrations require an exclusive administrative
path; normal runtime has no delete/DDL API. This is not protection from hostile
root or arbitrary SQL with constraints disabled.

## Connection configuration and authority gate

Before any transaction, set AND query/verify configuration:

```sql
PRAGMA journal_mode = DELETE;
PRAGMA synchronous = EXTRA;
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 2000;
PRAGMA trusted_schema = OFF;
PRAGMA read_uncommitted = OFF;
```

Two seconds is a proposed bounded operational timeout, not a security proof or
guaranteed total deadline. Enforce an overall request deadline separately.
Disable extension loading, ATTACH, arbitrary SQL and caller-provided filenames;
use a reviewed SQLite authorizer/defensive configuration. Do not turn off FKs,
CHECKs or triggers; do not toggle defer_foreign_keys during transactions.
Check the selected mode rather than assuming a PRAGMA succeeded.

Startup, migration and uncertain I/O always begin with a CLOSED process-wide
admission gate. Only one trusted service owns this store; other processes may
not bypass that gate through their own connections. `admission_state` is a
diagnostic durable marker, NOT proof of freshness or a crash-safe replacement
for startup recovery: persisting it can itself fail. READY in a restored DB
must never automatically enable admission.

Recover SQLite's journal normally; never remove journal files manually. Before
opening admission, require exact schema/configuration, `integrity_check` with
exactly one `ok`, zero `foreign_key_check` rows, full bounded semantic replay,
claim/event/reference joins, and independent epoch/freshness/producer checks.
Full integrity scans have storage-dependent cost: run with admission closed;
timeout or unsupported validation means unavailable, not skip-and-continue.

## Atomic claim transaction sequence

All parameters are bound values. `:epoch` and `:producer` come from trusted
server context; client inputs cannot supply an authenticated identity. Validate
the actual internal producer channel/peer and allowed purpose first. Do not
equate UID 0, a producer UUID, claim ID or D-Bus argument with authorization.
Cross-session user identity remains outside this internal-only claim contract.
Recheck revocation/current authority at admission; a future transport must
specify how identity changes serialize with the transaction.

1. Require the process authority gate open; validate request bounds and u64 to
   i64 conversion. Recompute domain-separated request digest from ALL typed
   fields including epoch, principal, purpose and expected head; compare full
   stored fields as well as digest. No client-supplied hash is authority.
2. Start `BEGIN IMMEDIATE`. Check metadata epoch/state and producer policy.
3. Look up `claims` by claim_id. Same authorized producer and identical full
   request returns the existing committed receipt without writes, regardless
   of now-stale request head. Different fields reject as claim_conflict; foreign
   callers get a neutral unauthorized result without key-existence disclosure.
4. Read current head and active reference in THIS transaction. Revalidate the
   stored envelope and complete logical history. Do not use a cached selection.
5. Apply both conditional updates below; each must affect exactly one direct
   row. If not, roll back the WHOLE transaction. Check the affected-row result
   immediately after each statement, before executing another statement.

```sql
UPDATE binding_references
SET state = 'Consumed', consumed_claim_id = :claim
WHERE epoch = :epoch AND operation_id = :op AND generation = :gen
  AND binding_sha256 = :binding AND state = 'Active'
  AND commit_result = 'committed'
  AND EXISTS (
    SELECT 1 FROM operation_heads h
    WHERE h.epoch = :epoch AND h.operation_id = :op
      AND h.head = :expected_head AND h.active_generation = :gen
      AND h.head < 9223372036854775807
  );

UPDATE operation_heads
SET head = head + 1, active_generation = NULL
WHERE epoch = :epoch AND operation_id = :op
  AND head = :expected_head AND active_generation = :gen
  AND head < 9223372036854775807;

INSERT INTO claims
 (claim_id, epoch, operation_id, generation, binding_sha256,
  expected_head, committed_head, producer_id, purpose, request_sha256)
VALUES
 (:claim, :epoch, :op, :gen, :binding, :expected_head, :next_head,
  :producer, 'associate_boot_evidence', :request_hash);

INSERT INTO lifecycle_events
 (epoch, operation_id, sequence, generation, binding_sha256, action, claim_id)
VALUES (:epoch, :op, :next_head, :gen, :binding, 'Consume', :claim);
```

6. Verify insertion counts and postconditions; `:next_head` must be the checked
   integer successor, never an unchecked expression from the caller. COMMIT.
   No success is returned until COMMIT succeeds and statements are finalized.
   No intermediate insert/RETURNING row is a committed receipt. Deferred FK
   failure at COMMIT is still failure; explicitly roll back the open transaction.

SQLite serializes writers; the losing distinct claim either waits and then
fails the new head/state predicates, or receives BUSY. Same-key retries return
one original receipt. CAS is meaningful only inside this shared transaction;
a process mutex or an earlier SELECT alone is insufficient. Claim/revoke races
follow commit order. No external action is executed by this transaction.

## Idempotent query and error contract

Authenticated status lookup uses a new read transaction AFTER recovery and
authority/freshness checks, never an old read snapshot. Select the stored claim
with its matching event/Consumed reference using exact composite keys and
restrict producer_id to the authenticated principal. Verify immutable request
and receipt linkage; only then return fixed `claim_recorded` and committed head.
Absence yields neutral `claim_not_recorded` only under a healthy current store;
it is neither a failure verdict nor reuse permission. Return no raw envelope,
producer secrets, database paths, SQL text or engine diagnostics to callers.

| Condition | Public behavior |
| --- | --- |
| Existing identical authorized request | Original claim_recorded receipt; no new event/head update. |
| Existing key with changed request | claim_conflict; no writes. |
| Unauthorized principal or purpose | claim_unauthorized; do not disclose foreign claim existence. |
| Stale head/generation/binding/ineligible state | Existing history_* fixed codes, with no identifying details. |
| SQLITE_BUSY before write admission | claim_busy; no success; a bounded explicit same-key retry starts fresh. |
| Constraint failure before commit | Whole transaction rollback; claim_rejected/conflict if fully resolved, otherwise indeterminate. |
| BUSY at COMMIT | Transaction can remain open; bounded commit handling or explicit rollback, never report success. |
| FULL, IOERR, lost connection, interrupted commit | claim_indeterminate, close admission and enter recovery; inspect engine transaction state only internally. |
| CORRUPT, NOTADB, invalid schema or replay inconsistency | claim_store_unavailable; quarantine, no recreate-empty fallback. |

The minimal v1 schema stores committed claim receipts only, matching the pure
model. Rejections are sanitized responses, not durable receipts; no rejection
claims to reserve a key forever. Adding durable rejection receipts would need
a separate schema and retry contract. Consumption tombstones are not pruned
by the existing runtime seven-day cleanup.

## Crash, reboot and recovery

Before durable commit, changes are not grants. After commit but before response,
the same authenticated key retrieves the committed result. After a commit-time
error, the caller must not infer which state won. Block new admissions, recover
the store, verify all linkage/freshness, then query the SAME key. A missing row
after trusted complete recovery means not recorded; it does not automatically
reactivate anything or justify a new claim ID. No automatic privileged retry.

Every service start and reboot enters the closed gate, regardless of persisted
READY. Recovery may need writes for SQLite journal handling and must be owned
by the sole trusted service. Disk-full or I/O failure while setting recovery
state cannot be treated as a successful quarantine write; block in memory and
stop serving writes, with closed-by-default startup semantics after a crash.
Once indeterminate, the old process must not continue with cached connections
or prepared selections. No replay protection is inferred from reopening a DB.

## File security and deployment prerequisites

Proposed owner root:root, private directory mode 0700, DB/journal/coordination
files mode 0600, restrictive creation umask. Verify every parent, regular file
type, expected owner/mode and link count 1. Reject symlinks/hardlinks, group or
world write, device/FIFO files and unexpected sidecars. Provision with exclusive
creation and safe directory traversal; fsync DB and newly created directory
entries before declaring provisioning durable. No follow-on open with CREATE
if an expected provisioned database is missing.

SQLite opens its path and sidecars itself: an application lstat followed by
sqlite3_open is not a race-proof atomic open. A reviewed VFS/open strategy
(including supported no-follow behavior), stable root-only parent namespace,
and single-owner lifetime coordination are implementation prerequisites.
Compare path/open-file device/inode identity as additional checks, not as a
complete substitute. Never pass an arbitrary `/proc/self/fd` path as a shortcut
without analyzing sidecar names and SQLite locking. Administrative replacement,
backup and migration must exclude all runtime connections. Hostile root is
outside the protection of file permissions.

## Migration and rollback policy

Provisioning alone may create an empty v1 database. Runtime open must not create
or migrate an unknown, missing, unversioned or newer store automatically.
Future supported migrations run with admission closed, exclusive maintenance
ownership and no runtime connections. Use a reviewed explicit version ladder,
transactional DDL/data copy, unchanged claim IDs and consumption tombstones,
then full integrity/FK/semantic checks; bump user_version and schema metadata
in the same commit. Failure leaves the old version or an unavailable store,
never partially admitted state. No downgrade over live receipts.

Backups include a consistent database AND the independently managed authority
epoch context; a raw file copy during writes is not an approved backup method.
Restoring an old DB can restore Active state even when all SQL checks pass.
Neither transaction isolation, EXTRA, integrity_check nor a hash chain/counter
inside that DB prevents this. Independent freshness/epoch continuity is still
required; recovery gates stay closed when it cannot be established. Restoring
or cloning must follow a separately reviewed epoch retirement/re-enrollment
procedure, not accept the restored READY/head as authoritative.

## Later verification plan

- Two processes and separate connections claim the same reference: one commit;
  same ID returns one receipt, changed request conflicts, second consumption
  fails UNIQUE even if application checks are omitted.
- Exercise every deferred circular FK: missing/mismatched claim, event or
  Consumed reference must fail COMMIT. Verify failed COMMIT is rolled back.
- Stale head/generation/binding, inactive references and uncommitted envelope
  claims reject without partial updates; zero affected rows abort everything.
- Inject failures between each UPDATE/INSERT/COMMIT, and kill before/after
  commit/response. Check exact old or complete new state and idempotent lookup.
- BUSY at begin/commit, FULL, IOERR and sync faults block success and exercise
  uncertainty gates; corrupt DB/journal and integrity/FK errors quarantine.
- Migration crash/unsupported version/tampered trigger and schema tests cannot
  open admission; no automatic CREATE, reset, REPLACE or bypass pragma.
- Validate integer extremes, duplicate Active rows, missing events, broken
  active pointer, unsupported producers and confidential error responses.
- Rootless temporary fixtures exercise symlink, hardlink, path swap, writable
  parents and sidecar manipulation. Actual root deployment requires separate
  disposable-VM review, not privileged tests on the user's installed system.
- Restored snapshot/head/READY remains untrusted without external freshness;
  no claim receipt grants cleanup, reboot, install or new source trust.

## Review status and primary semantics

This draft was manually reviewed for STRICT types, NOT NULL, exact composite
parent keys, NULL FK behavior, deferred commit checks, partial-index uniqueness,
integer overflow and affected-row ordering. Task 17 now parses and executes it only in disposable test fixtures; this does
not establish production readiness. See the executable-fixture review below.

- [STRICT tables](https://sqlite.org/stricttables.html)
- [Composite and deferred foreign keys](https://sqlite.org/foreignkeys.html)
- [Transactions and error state](https://sqlite.org/lang_transaction.html)
- [PRAGMA configuration and checks](https://sqlite.org/pragma.html)

The schema extraction and negative transaction fixtures are now test-only
artifacts. Next, review admission/recovery coordination and database schema
identity validation before implementing a production adapter. No production
store or authority integration has been introduced.


## Task 17 executable fixture review

The schema and claim statements are mirrored verbatim under `tests/fixtures/`;
fixture tests compare both SQL files with this document to prevent drift.
The first executable check found that omitting the head UPDATE could otherwise
commit claim/Consume/reference together while leaving an old head/Active pointer.
`consume_matches_head` now rejects that omission at Consume insertion. No
constraint was removed or weakened. Deferred missing claim/event/reference
relations still fail COMMIT, which leaves the transaction open until rollback.

Test-only Python stdlib sqlite3 uses the installed SQLite library; a Cargo test
harness runs it and checks the basic outcome against `atomic_claim.rs`. No Rust
SQLite dependency, unsafe FFI wrapper or production linkage is added. Python
and a STRICT-capable SQLite build are required test tools; absence is a failure,
not a skipped pass. This keeps native SQLite binding maintenance in the standard
library, but does not select the future production Rust SQLite dependency.

Each fixture owns a TemporaryDirectory and private DB, closes connections and
removes the directory. Tests cover actual two-connection contention, SQLITE_BUSY,
SQLITE_FULL via a fixture page limit, subprocess exit around COMMIT, response
loss and reopen. I/O uncertainty is an explicit injected exception, not a claim
of real VFS fault injection or power-loss durability. A semantic counterexample
passes integrity_check/foreign_key_check but fails domain replay; those PRAGMAs
are not a substitute for semantic recovery or independent freshness.


## Task 18 schema identity, migration and recovery fixtures

Only the Python test adapter and Cargo test harness are extended. Test databases
now receive user_version=1 and application_id=0x5A434C4D at fixture provisioning;
no production format is provisioned or changed. Expected schema identity is
constructed from the checked-in canonical SQL in a separate in-memory template,
not a hash or version supplied by the candidate database. Compare the full
sqlite_schema object/SQL inventory, table_xinfo, index_list/index_xinfo and
foreign_key_list, including autoindexes, CHECK text and all critical triggers.
This deliberately rejects even unapproved semantically equivalent rewrites.
It is a test strategy on the selected SQLite engine, not an engine-independent
schema canonicalization or source-authentication guarantee.

Recovery and claim admission check versions, metadata, connection configuration
(including enabled triggers and constraints), exact schema identity, SQLite
integrity/FK results and bounded fixture-specific semantic replay. Adapter
construction is closed by default. Freshness is still an explicit TEST
assumption; even a restored perfectly matching schema cannot establish it.
Overflow is rejected before allocating a sequence based on the head value.
Every failed recovery leaves claim/query admission closed and does not reset,
repair, recreate or silently migrate the target database.

Migration tests use an intentionally synthetic v0 stamp format: the exact v1
claim data layout plus fixture_legacy_marker and user_version=0. The only
supported test migration drops that known marker and stamps user_version=1
inside one BEGIN IMMEDIATE transaction. Metadata's data-layout schema_version
remains 1. This is NOT a claimed migration from any existing production broker
record or deployed database. No FK/CHECK/trigger is disabled during migration,
no records are backfilled, and no unknown v0 schema is accepted. Failure or
connection close rolls back both DDL and version; repeat on a valid current
schema is a no-op. Partial stamps, unknown/newer versions and changed objects
are rejected without automatic repair.

Negative corruption tests deliberately disable protections only while creating
bad fixture data, then restore them before admission. This tests rejection,
not a runtime bypass policy. Coverage includes missing/altered/no-op/disabled
consume_matches_head, dropped UNIQUE/CHECK/FK constraints, unknown objects,
missing columns/tables, foreign keys and claim/event/projection inconsistencies,
CHECK corruption detected by integrity_check, and semantic contradictions that
both SQLite integrity PRAGMAs miss. No production SQLite dependency or actual
migration service is introduced.
