# Required external production trust inputs

Status: **dış bağımlılık bekliyor**. No production key, signature or policy is
invented by this change. No key is generated/imported into a host keyring.

The existing trust loader and signed root extractor are already implemented;
they are not rewritten here. The missing input is authorization of an actual
production signing authority and the exact signed release artifacts.

Required from the project/release owner through an authenticated channel:

1. The approved production manifest signer's public keyring and primary-key
   fingerprint allowlist. State who authorized this signing role. Do not infer
   it from an AUR maintainer key, a TLS download, the host's package keyring or
   a newly generated test key. If an existing authorized developer/release key
   must sign the root manifest, the owner performs that signing privately.
   **The private key must not be uploaded or supplied to this agent.**
2. The expected public keyring SHA256 and the reviewed local policy for existing
   `TrustPolicy.load`: `keyring` (local basename), `keyring_sha256`, `signers`.
   Provisioning root-owned immutable policy files is a separate reviewed
   deployment step; this task does not write them to the host.
3. An actual signed manifest in the existing schema:
   `schema=1`, `purpose=zethropol-aur-build-root`, architecture, expiry no more
   than one day ahead and exact package name/version/SHA256 list (at most 64).
   Supply its detached signature and the matching signed packages from approved
   distribution repositories. Runtime/build/check dependency closure and real
   repository metadata/signatures must validate under the approved policy.
4. The approved trust policy for built AUR archives/signatures. A user-owned
   builder signing its own output does not itself grant root installation
   authority. It must be consistent with the configured native package
   signature policy, independent broker intake, approved recipe/source plan
   and server-owned receipt. Distribution dependency signers and project root
   manifest authority are distinct roles; a test key satisfies neither.

No production artifact set or authorized signer policy was supplied with this
request or found in the reviewed project trust inputs. This is not a claim that
no private signing key exists elsewhere; private-key stores were not searched.
Until the owner supplies/approves the public bootstrap and signed artifacts,
production provenance and dependency-root closure cannot be certified and AUR
installation must remain disabled. A signed copy of host tools using an
ephemeral fixture key is still only a fixture.

## Read-only distribution evidence from this integration

Public distribution keyrings and detached-signed cached packages DO exist.
The statement above concerns the absence of an authorized PROJECT manifest/
AUR-output bootstrap, not the absence of distribution public keys.

`PUBLIC_TRUST_AUDIT.json` records a read-only verification of cached
`base-devel-1-2-any.pkg.tar.zst` against existing Arch/CachyOS public keyrings:

- gpgv exit status **0**, with VALIDSIG primary fingerprint
  `9D74DF6F91B7BDABD5815CA84AC5588F941C2A25`.
- Package SHA256:
  `412a2effe23751e1c11b2fa68155f1a29ecea5d9dc8ea8cef4e87b7b429d3201`.
- Public ring SHA256 values and detached signature digest are in that audit.

The public rings are armored. They were dearmored into a disposable private
directory for gpgv; no host import/keyring update, package refresh or private-key
read was performed. The reproducible probe is
`tests/read_only_public_trust.py`. A single valid cached distribution package
is not proof of current complete build dependency closure, current repository
metadata, signer revocation policy or project build-root/AUR-output authority.

## Inputs still required and the exact closed boundary

All four owner-provided roles/artifact requirements above remain required.
The owner must supply the PUBLIC keyring, approved primary fingerprints and
keyring digest through an authenticated release channel, identify the authority
approving each role, and supply matching signed manifest/packages. The manifest
schema, lifetime, architecture and package count remain exactly as specified
above; no substitute test manifest/key is accepted for production.

The built-output policy must additionally establish who may attest a build,
what reviewed recipe/source/build-root identity is covered, which output
signer(s) are accepted by native ALPM, and how the root-owned one-use receipt
binds these to the server preview and operation. A builder's self-signature,
user JSON or an official-update preview cannot supply this authority.

Receipt verification, receipt-derived preview and durable one-use admission are
now implemented in the non-installed development library. The fixed production
policy is `/etc/zethropol-update/aur-trust/policy.json`; it was NOT provisioned.
Its schema/purpose and separately pinned `manifest`, `receipt`, `archive` roles
are specified in `RECEIPT_PROTOCOL.md`. Every role requires its own explicitly
authorized local binary PUBLIC keyring digest and primary fingerprint allowlist.
Role grants are not inherited from native Arch trust or from one another.

The approved RECEIPT authority must be an independent trusted build attestor,
not an arbitrary worker signing its own report. Before signing it must establish
the measured/provisioned root, reviewed recipe/source plan and exact outputs.
The broker independently verifies signatures, context, root identity, archive
bytes/native metadata and locked resolution; it cannot prove actual execution
or reproduce a build merely from signed JSON. This attestation contract and the
owner's explicit role approval are production trust inputs, not worker claims.

The development install method and worker still unconditionally reject after
loading/verifying policy because real PolicyKit/snapshot/boot/recovery acceptance
is incomplete. Missing public policy/artifacts reject earlier. Python install
also remains closed. There is no enable flag. Valid keys/receipts alone do not
open installation; no test fingerprint is included in a production allowlist.

In the earlier receipt revision, LAB signing keys were generated only in
disposable test directories to verify
mechanics. Signed native tests run in separate processes per trust scope because
libalpm/GPGME cache process-global GPG initialization. Production must keep its
configured trust domain fixed and must never switch to a client/test GPGDir.

Before production rollout, validate real PolicyKit admission and actual
snapshot/boot/crash recovery in a disposable VM before a separate reviewed
service/UI deployment. This host was not used for any AUR package installation.


## Final deployment constraints

The source-review handoff must reserve server expectations before attestation;
raw user/worker JSON must never become trusted expectations. Keep long builds
outside the existing five-minute privileged preview/receipt window and obtain a
fresh explicit handoff if it expires. The selected outputs must be uniquely
name-sorted and exactly match the signed receipt and native package metadata.

Receipt/keyring files and signatures are capped and snapshotted; no private
key/hardware token is needed by verification. Output signer trust must ALSO be
provisioned in native ALPM's configured package trust policy by a separately
reviewed deployment; native trust is not expanded from a worker receipt.

AUR companions and operation evidence now use the original BrokerStore at the
fixed persistent path `/var/lib/zethropol/aur-transactions`, with checksummed
boot epochs, durable admission and no automatic proof cleanup. This location
was NOT provisioned on the host. Simulated reboot/process-crash results are
separate from actual power-loss, mount/retention/rollback and boot acceptance;
see `PERSISTENT_RECORDS.md` and `VM_ACCEPTANCE_PLAN.md`.

`GetAurReadiness` is the development read-only verification interface: it
checks the fixed public policy/keyring pins, reports each explicit role's primary
fingerprints/digest if valid, and ALWAYS reports installation closed and real
acceptance incomplete. It does not generate/import keys or attest missing
signed artifacts. The original received-FD receipt/manifest/archive verification
path remains the authoritative artifact check. No installed D-Bus service was
changed or queried to enable AUR.

No private key was generated in this persistence task. Twelve pre-existing
signed-LAB Rust cases that would generate keys were deliberately excluded; their
prior crypto evidence is retained separately. No test fingerprint entered any
production trust list.

| Step | Required input/operator | Physical/private-key boundary |
|---|---|---|
| Verify approved role policy/public rings | Public bytes, pinned digest, explicitly authorized primary fingerprints | No private key or hardware device needed; verifier implemented |
| Verify root manifest, receipt and output signatures | Actual detached signatures, exact artifacts and reviewed context | Public verification only; implemented |
| Authorize signing roles | Project/release authority through authenticated channel | Agent cannot invent this authorization |
| Produce missing manifest/output/attestation signatures | Authorized release owner/independent build attestor | If owner policy requires its physical developer token, only the owner unlocks/signs; no replacement key or private-key upload |
| Native output signer enrollment | Separately reviewed guest/production native package policy | Never inferred/imported from worker-provided material |

Actual approved project role grants/public keyrings and a matching complete
signed production root/dependency/receipt/output set have still NOT been
supplied. A cached distribution signature is not a substitute. Owner-operated
signing/provisioning and real disposable-system acceptance remain external
requirements; neither persistence nor valid keyring pins enables installation.
