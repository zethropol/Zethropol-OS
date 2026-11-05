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
