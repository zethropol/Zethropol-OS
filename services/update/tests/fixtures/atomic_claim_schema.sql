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
