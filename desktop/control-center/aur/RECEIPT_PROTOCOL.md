# AUR receipt admission protocol v1

> The non-installed production candidate now also recognizes **build-root
> manifest schema 2** with fixed 256-package/2-GiB-expanded/65,536-entry bounds.
> Schema 1 retains its original limits. Receipt schema, 32 signer and 64 output
> limits are unchanged. This does not enable installation or bypass signatures.
> See [root capacity and remaining acceptance](YAY_OFFLINE_ROOT_PROGRESS.md).

Status: implemented in the non-installed Update reuse library. **Installation
remains closed**, even with valid keys/receipts, until real PolicyKit, snapshot,
boot and recovery acceptance is complete. No production key or policy ships in
this repository. The signed receipt schema is `receipt-v1.schema.json`;
Rust typed parsing additionally rejects duplicate JSON fields at every level.

## Trust roles

Arch/distribution signature trust remains configured native ALPM package and
repository policy. It does not grant Zethropol build-root or receipt authority.
`ReceiptTrust::load` reads only the fixed broker-local path
`/etc/zethropol-update/aur-trust/policy.json`. This task does not provision it.
All policy/keyring ancestors must be root-owned, non-symlink directories without
group/world write. Policy and keyrings must be root-owned regular files without
group/world write, opened without following symlinks. Size limits: policy 64 KiB,
each binary public keyring 16 MiB. Keyrings are hashed and copied into sealed
memfds before verification; caller paths, keys and policy flags are never used.

Policy schema has exactly these required fields:

- `schema`: integer `1`.
- `purpose`: `zethropol-aur-trust`.
- `manifest`, `receipt`, `archive`: separately configured role objects, each
  containing only `keyring` (policy-local basename), `keyring_sha256` (64 lowercase
  hex characters), and `signers` (1–32 approved uppercase 40/64-character PRIMARY
  fingerprints). Binary public keyrings are required; dearmor/provision publicly
  authorized rings separately. No key discovery/import, inherited allowlist,
  TLS/AUR-maintainer trust or test-key fallback occurs.

Each role verifies one detached signature using fixed gpgv, an isolated private
home and held sealed FDs, under a 15-second timeout. Exactly one VALIDSIG primary
fingerprint must be allowed for THAT role. Bad, expired/revoked or multiple
signature results fail closed. Signing subkeys require the pinned primary.
Output archives ALSO pass native REQUIRED signatures, with optional, marginal
and unknown-key acceptance removed for AUR. The configured native GPG domain
is fixed; native GPGME contexts are not switched between client trust domains.

**The receipt signer must be an authorized independent build attestor**, not
an arbitrary user worker signing its own claims. It must establish that the
reviewed recipe/sources and approved measured/provisioned build-root produced
the exact archives before signing. Signatures authenticate that attestation;
they do not mathematically prove a build was executed or reproduce its output.
This verifier does not promote a raw worker result to trusted build evidence.
The same key can hold multiple project roles only with explicit owner approval;
roles are never inferred from distribution trust or one another.

## Server reservation and worker receipt

At the build-to-attestation handoff, the existing source review stage supplies
`ReviewedBuild` to the internal
`reserve_reviewed_aur_message` adapter; this is deliberately not a client/worker
D-Bus expectation parameter. It binds source package/version, target architecture,
SHA256 of the reviewed source plan, SHA256 of the exact signed build-root
manifest bytes and an exact, uniquely name-sorted list of selected output
name/version/filename triples. Recipe/source/build/check/runtime selection is
covered by the existing reviewed source-plan digest. Only trusted server review
code may supply these expectations; worker assertions cannot define them.

The adapter uses the existing received-header identity/logind resolution and
Preview PolicyKit action, then the SAME BrokerStore creates a preview reservation
with the original server-generated UUID/token and `aur-build-reservation`
purpose. It is not installable. Its five-minute preview window is unchanged: perform
long unprivileged builds before this privileged receipt handoff, reserve the
server nonce, then obtain the attestation promptly. Expiry requires a fresh
review/authorization and reservation; no automatic lifetime extension occurs.
The reviewed expectations are written/fsynced to
a private companion in that store. Restart reloads them and checks their digest
against the original record. A missing/changed companion fails closed.

The worker/authorized attestor returns a detached-signed JSON receipt containing
all required schema fields:

| Field | Required meaning |
|---|---|
| `schema`, `purpose` | `1`, `zethropol-aur-build-receipt` |
| `operation_id` | Existing broker UUID; also the one-use receipt nonce |
| `uid`, `bus_name`, `session_id` | Exact broker-resolved owner of that reservation |
| `source_package`, `source_version` | Reviewed source package/base and version |
| `architecture` | Reviewed `x86_64` or `aarch64` target |
| `source_plan_sha256` | Digest supplied by the server's reviewed source-plan stage |
| `build_root_sha256` | SHA256 of exact separately signed build-root manifest bytes |
| `issued_at`, `expires_at` | Unix seconds; issued no later than now, expires after now and issued, lifetime <=300 seconds |
| `packages` | Exact selected outputs, sorted as in the server review; 1–64 |
| each output | `name`, `version`, `architecture`, `filename`, `sha256`, `signature_sha256` |

Output architecture must equal the target or `any`, AND the native archive
architecture. Native `.PKGINFO` package base must equal the reviewed source
package, and native name/version/filename must match the receipt. Hashes use
lowercase SHA256; signature hashes cover the exact detached signature bytes.
Unknown, duplicate, missing, malformed, extra, dropped or repeated output data
is rejected. Split packages require each selected output to be listed explicitly.

The separately signed build-root manifest preserves the existing schema:
`schema=1`, `purpose=zethropol-aur-build-root`, architecture, `expires`, and
1–64 unique `{name,version,sha256}` packages. It must match the expected digest,
be unexpired and expire no more than one day ahead. Distribution dependency
closure validation/provisioning remains the existing build-root pipeline; it is
not replaced by receipt JSON or by a copy of host executables.

## Receipt to explicit consent to commit

`PreviewAurReceipt` is added only to the DEVELOPMENT build's existing
`org.zethropol.Update.v1` interface. It receives the operation ID plus owned,
sealed receipt/signature, manifest/signature and archive/signature FDs. It takes
no caller UID, trusted expectation, keyring, path, shell command or snapshot ID.
The original message and Preview PolicyKit action determine the caller. It
loads expectations from the server reservation and independently verifies:

1. Immutable memfd seals and bounds; receipt/manifest and each signature <=1 MiB.
2. Context, operation, source plan, build-root identity and both lifetimes.
3. Root/receipt/output detached signatures under their separate project roles.
4. Actual staged archive SHA256, native REQUIRED signature and native metadata.
5. Existing locked full ALPM solver: official full-upgrade barrier, dependencies,
   architecture, conflicts, additions/removals and database/policy drift guards.

The existing CanonicalPlan is constructed from observed native transaction
metadata and installed versions. Its package provenance hash is the exact signed
receipt SHA256. Policy/state stamping additionally covers the production trust
policy and its pinned public keyrings, alongside existing host policy/metadata.

Admission uses the original BrokerStore lock in the AUR-only persistent store
`/var/lib/zethropol/aur-transactions`. It atomically publishes an exclusive, fsynced
`aur-receipt-UUID.spent` companion BEFORE atomically publishing the final plan
in the original preview record. The companion binds the UUID, receipt hash,
full canonical hash, reviewed provenance, signature hashes, trust-policy hash,
Unix expiry and a bounded boottime deadline. A crash between these writes burns
the admission; it never allows replay. Malformed/partial markers also fail closed.
Repeated admission, a replaced reservation or a different owner/operation fails.
Receipt companions are bounded to 4096 files and at most 64 MiB; quota exhaustion
refuses new admission/reservation without silently deleting replay evidence.

The original broker record format and five-minute lifetime are unchanged.
Receipt expiry is an ADDITIONAL check under both clocks, so wall-clock rollback
cannot extend the admitted lifetime. The preview method returns observed
packages, removals (always empty for this AUR policy) and the full plan hash.
It does NOT consume Install authorization or start commit.

`InstallAur` requires the separate original Install PolicyKit grant and exact
approved hash, then consumes the original one-use preview. AUR-only admission
checks remain distinct from official previews. The worker rechecks the persisted
receipt, policy/expiry and delivered detached-signature hashes. It rehashes and
natively verifies received archive bytes and resolves again under the native
lock. Any preview/final plan mismatch is terminal. Immediately before commit,
the receipt deadline/trust stamp is rechecked after the existing real pre-snapshot
boundary. No client snapshot evidence is accepted.

Existing cancellation, worker lease, durable commit-start/outcome, snapshot,
boot and no-retry rules remain in force. An inflight crash becomes Unknown;
explicit native failure may be partial. Hook success and a successful new boot
are never inferred from a native success return. No root adapter executes
PKGBUILD, builds, or client-selected commands.

## Scope and outstanding acceptance

Verification and native commit tests use disposable roots and ephemeral LAB keys.
No key is added to the fixed production policy. Live PolicyKit and real
snapshot/boot recovery are NOT certified. `require_aur_production_authority`
loads/validates trust and still unconditionally rejects installation for missing
real-system acceptance. There is no runtime enable switch; the Python install
boundary also stays closed. No installed interface or service was changed.

AUR reservations, accepted receipt/nonce proofs and commit/outcome records now
use the original BrokerStore in `/var/lib/zethropol/aur-transactions`, with
checksummed boot-tagged envelopes and startup recovery. Official runtime state
and transient FDs/archives remain under `/run`. Old live receipt/review epochs
and previews cannot carry consent across a boot change. No proof is automatically
pruned; a missing/corrupt accepted record/marker/anchor fails closed.

`GetAurOperationState` supports fresh authorized same-UID history reads without
inheriting old grants; `GetAurReadiness` validates public policy pins while
always reporting installation closed and system acceptance incomplete. Neither
query proves package artefacts or boot recovery. Exact durability, retention and
cutover rules are in `PERSISTENT_RECORDS.md`; actual VM/media/PolicyKit/boot
acceptance is in `VM_ACCEPTANCE_PLAN.md`. No host persistent directory was
provisioned and no host ALPM transaction or private key generation ran in this
persistence task. Earlier signing-fixture evidence remains historical.
