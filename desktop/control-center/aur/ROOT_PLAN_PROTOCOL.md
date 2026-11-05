# Reviewed source-plan root commitment

Source-plan schema 1 keeps the full `rootfs` tree unchanged. Source-plan schema 2
replaces only that field with an exact descriptor:

```json
{"sha256":"<64 lowercase hex digits>","entries":60393}
```

The digest is SHA-256 of `worker.canonical(full_tree)` (sorted keys, compact JSON,
ASCII escapes). The complete tree still commits every path, file mode and content
hash, directory mode and symlink target, including provisioning provenance.
`entries` is an integer in 1..65,536; bools, extra fields and caller-selected
paths or limits are rejected. The canonical tree has a fixed 16 MiB ceiling.

The descriptor is computed from the server-provisioned root, not from a caller
file or an unauthenticated detached document. `pipeline.run`, `provider.provision`
and `Worker.run` recompute the full trusted tree and compare it using
`worker.matches_root`. The worker also reconstructs the complete reviewed source
plan before executing anything. Its full tree is passed to `FrozenTree`; no hash
descriptor is treated as extraction or mount instructions.

Create a new plan with `worker.review(..., schema=2)` only after production root
provisioning. The existing default stays schema 1. A new schema changes the
source-plan digest and requires a new owner-approved review/template. Existing
review hashes cannot authorize the replacement. Receipt/native/recovery and
expiry requirements are unchanged.

The source-plan version and signed build-root manifest version are independent.
Only the authenticated manifest selects package/extraction capacities. Selecting
source-plan schema 2 cannot increase a schema 1 root's limits.

Rust delivery and Qt managed build transports already treat the plan as part of
the hash-bound JSON request, not as an inline-root-specific structure. Their
1 MiB limits stay unchanged. A real unsigned yay candidate prepared from the
historical rehearsal tree is 897 bytes for the complete plan instead of a
7,002,901-byte inline tree. This size result is not production root validation.

This change fixes the document-size obstacle only. `FrozenTree` still requires
one sealed FD and multiple bwrap arguments per regular file. Its FD and argument
preflights remain mandatory. Large roots still fail closed; the protocol does
not introduce a mutable-directory bind fallback or a new production enable flag.

Subsequent implementation: authenticated manifest schema 2 now selects the
single readonly tmpfs backend in `QuotaWorker`; legacy `FrozenTree` remains for
manifest schema 1 and laboratory callers. The plan commitment is unchanged.
See [STAGED_ROOT_ARCHITECTURE.md](STAGED_ROOT_ARCHITECTURE.md) for the finite
resource contract, real scale tests and remaining production requirements.
