# Provider absolute-deadline integration

Source implementation only. Installed UID949/four-public PASS state is unchanged.
No root job, build, signature, key/agent operation, deployment or installer was run.

The prior ready job carried no original broker deadline. Each provider build
therefore inherited the generic 1800-second pipeline timeout even though broker
admission still expired after 300 seconds. This is now fail-closed in source:

- `ReceiptContext` carries `Record.created_at/expires_at`, both existing broker
  boottime seconds; the original reservation lifetime is not restarted.
- Candidate `delivery.rs` creates ready-job **schema 2** with a server-generated
  budget frame. It checks canonical boot identity, checked nanosecond conversion,
  exact 300-second lifetime and current remaining boottime before publishing ready.
- The frame contains `issued_boottime_ns`, `deadline_boottime_ns`,
  `captured_boottime_ns`, `captured_wall_ns`, and `boot_id`, alongside a strict
  budget schema/purpose. It is read only from existing root-owned job files.
- `RuntimeBudget` derives a wall deadline from that server sample and the
  **remaining** original boottime interval. Each checkpoint rejects expiry under
  either clock, rollback/drift over one second and boot changes. CLOCK_BOOTTIME
  includes suspend. This is not retrospective wall-clock evidence before the
  server sample; the original boottime reservation still caps all elapsed work.
- Provider job schema 1 is refused; it is never silently upgraded with a new TTL.
  Actor/role identity, signatures, provenance and source-plan bindings remain.
- Prepare, rebuild, Archive signing/attachment, receipt signing and publisher
  boundaries check the same absolute deadline. The pipeline receives only the
  floor of remaining seconds. Its cancellation callback checks throughout worker
  execution and source/root staging; private signing timeout is capped at
  min(60 seconds, remaining whole seconds). Expired output cannot be published.

The initial client build and transfer already consume the original reservation;
their time is not erased at ready-job creation. If the entire path does not fit,
it fails. A prebuild-before-reservation architecture remains a separate reviewed
change; this implementation does not add it or extend a receipt/preview lifetime.

`rebuild_acceptance --probe-budget` now exercises the real local wall/boottime
sampler and deadline checkpoint around an unsigned hashing workload. It uses an
explicit synthetic reservation; it is a mechanism probe, **not** a provider
rebuild, signature, receipt or full five-minute production acceptance.

Tests: 53 affected Python tests (32 capture/runtime-budget, 11 provider, 10
pipeline) and 2 targeted Rust tests passed. The Rust tests cover original
persisted broker boundaries in a disposable BrokerStore and budget-frame expiry,
clock/UUID and TTL-extension rejection. No native transaction or private key was
used. Evidence: `../build/aur-provider-deadline-next-20261101/`.

Deployment delta: provider.py/pipeline.py, root-owned `rebuild_acceptance.py`
dependency, candidate server binary from updated ReceiptContext/broker/delivery
sources, plus packaging/import guards. These changes must be hash-reviewed and
deployed together under separate permission. The accepted installed 13 files and
four-public transaction are neither overwritten nor declared obsolete failures.

Real fresh signed-root, separate-role package outputs/metadata/cleanup, independent
signed receipt, full critical-path timing and sealed broker admission remain
**BLOCKED**. Code and local clock probes never supply those missing artifacts.
