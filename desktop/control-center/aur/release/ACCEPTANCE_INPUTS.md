# Production acceptance inputs and guest preparation — 2026-11-06

**NOT ACCEPTED / installation CLOSED.** This is an operator preparation contract,
not trust enrollment, signing consent or permission to mutate the host. Existing
broker/ALPM code is unchanged. No sample below is an approved artifact.

## Reproduce the passive inventory

From the source checkout, run these read-only commands in the intended execution
namespace. Both deliberately exit **2** and report installation closed, even if
all public metadata checks succeed:

```sh
python3 -I -B desktop/control-center/aur/release/preflight.py
python3 -I -B desktop/control-center/aur/release/acceptance_inventory.py
```

The second tool opens no KVM device, connects to no daemon, scans at most 257
entries per named public directory, reads no private store, and makes no guest
or signing call. An inaccessible directory is unknown, not absent. Tool presence,
file metadata and KVM read/write access are not acceleration or guest evidence.
Run outside the runner sandbox with its required authorization when host
inventory is needed; namespace-local absence must not be reported as host absence.
Do not commit personal raw inventory, image or key material.

## Exact public file and approval contract

| Input | Existing expected location / representation |
| --- | --- |
| Project role policy | `/etc/zethropol-update/aur-trust/policy.json`, schema 1, purpose `zethropol-aur-trust`, exactly `manifest`, `archive`, `receipt` objects |
| Each project role | Exactly `keyring`, `keyring_sha256`, `signers`; local basename of PUBLIC OpenPGP keyring, lowercase 64-digit SHA-256, 1–32 unique uppercase 40/64-digit primary fingerprints |
| Distribution policy | `/etc/zethropol-update/aur-trust/distribution.json`, exactly the same three role fields without the project wrapper; `distribution.gpg` is PUBLIC distro trust for build-root dependencies |
| Public keyrings | Policy-local `manifest.gpg`, `archive.gpg`, `receipt.gpg`, `distribution.gpg`; pinned digest and independently approved role primaries; never private material |
| Provider configuration | `/etc/zethropol-update/aur-providers.json`, exact `schema:1`, purpose `zethropol-aur-providers`, `archive`/`receipt` each with `user`/`signer`; account names from `aur-providers.json.example`, UID 951/950 respectively |
| Manifest signer configuration | `/etc/zethropol-update/aur-manifest-signer.json`, exact schema 2 existing-key-owner contract in `providers/aur-manifest-signer.json.example`, UID 949; existing owner performs authorized signing |
| Exact manifest approval | `/var/lib/zethropol/aur-manifest-approvals/<raw-manifest-SHA256>.json`; exact `schema:1`, purpose `zethropol-approved-root-manifest`, `manifest_sha256`, integer `expires` |
| Published public manifest pair | Existing publisher creates `/var/lib/zethropol/aur-provider-inputs/manifest-<raw-manifest-SHA256>/manifest.json` and `manifest.json.sig`; no replace of an existing publication |
| Distribution archive pairs | Exact paths below `/var/lib/zethropol/aur-provider-inputs` in the approved build request; each entry has `archive` and `signature`; no invented default package path |
| Reviewed build request | `/var/lib/zethropol/aur-approved-reviews/<review-hash>.build.json`; existing root-owned publisher consumes it; never create an approval merely from this document |
| Provisioned build root | `/var/lib/zethropol/aur-build-roots/<raw-manifest-SHA256>`; existing verifier/provisioner alone constructs and checks provenance; do not hand-populate its database |
| Broker receipt handoff | `/var/lib/zethropol/aur-deliveries/<operation-UUID>` created by existing providers; signed receipt, manifest and signed output FDs go through the actual peer-bound broker preview/consent |

Root-owned policy/config/keyring files and ancestors must reject symlinks and
untrusted writes. Public inputs must also be readable/traversable by the separate
build accounts. The existing publisher uses public files 0444 and published
directory 0555. Protected approval/review files remain root-only under their
existing provisioned directories. Existing runtime validators remain authoritative;
these paths are requirements, not claims that those files have been supplied.

Templates are `providers/policy.json.template`, `distribution.json.template`,
`aur-providers.json.example` and `aur-manifest-signer.json.example`. Their nulls
and empty signers intentionally fail validation. Do not fill them with LAB keys,
copy a test approval, extend expiry of a previously signed manifest or enable a
service simply to make preflight pass.

A broker-admissible root manifest is UTF-8 JSON with exactly:

- `schema`: integer **1**; `purpose`: `zethropol-aur-build-root`.
- `architecture`: `x86_64` or `aarch64`, matching the reviewed target.
- `expires`: integer Unix seconds, later than verification time and at most
  86400 seconds ahead. Recheck at publication and receipt admission.
- `packages`: 1–64 unique entries, each exactly `name`, `version`, `sha256`.
  Name/version must pass `trust.approved_manifest`; hash is lowercase SHA-256
  of the exact distribution package bytes, not a filename or extracted tree.

**Do not confuse schema numbers:** the UID949 signer *configuration* is schema 2.
The provider also understands schema-2 build roots (up to 256 entries), but the
current broker receipt parser accepts schema-1 root manifests only. Such schema-2
root inputs must remain blocked at broker admission; use a newly reviewed,
freshly signed schema-1 set within the actual broker contract for acceptance.
No parser or limit is changed by this infrastructure task.

The detached OpenPGP signature authenticates exact raw manifest bytes. Whitespace
changes require a new signature and raw-byte hash. The provenance manifest ID
also uses the existing `digest(canonical(plan))`; do not substitute that digest
for the raw signed-byte hash used in publication, root path and broker review.
Every root archive has a separate distribution signature and must match the
manifest order/identity/hash. Actual extraction validation still checks package
metadata, safe paths, quotas and dependency/root expectations.

The existing build request has exactly `store`, `recipe`, `plan`, `plan_sha256`,
`manifest`, `manifest_signature`, `packages`, `build_root`. Its plan is the actual
reviewed source/checksum/dependency/root plan from the current Control Center
workflow, not a placeholder reconstructed here. The independent receipt contains
exactly `schema:1`, purpose `zethropol-aur-build-receipt`, `operation_id`, `uid`,
`bus_name`, `session_id`, `source_package`, `source_version`, `architecture`,
`build_root_sha256`, `source_plan_sha256`, `issued_at`, `expires_at`, `packages`.
Each output is exactly `name`, `version`, `architecture`, `filename`, `sha256`,
`signature_sha256`. It binds the original broker reservation, sorted reviewed
outputs and actual signatures; lifetime is at most **300 seconds**. A stored
receipt cannot be fabricated in advance or reused across boots/operations.

The release owner approves the exact manifest raw hash and expiry through the
existing protected approval workflow; approval cannot outlive its manifest.
`manifest_workflow.sign` runs only as the configured owner after explicit signing
authorization; publication rechecks the signature, owner policy and approval.
Presence of a public key, root ownership or a valid LAB signature is not that
approval. Archive UID951 signs built output; independent UID950 rebuilds/attests.
Receipt primaries must differ from both manifest and archive roles. Keep each
actual signing/install action subject to its existing authority mechanism. No
private key/password is an acceptance input to this agent.

## Prepare the separate PUBLIC Archive native snapshot

Target is **only** `/etc/zethropol-update/aur-native-trust`. The official pacman
GPG directory must remain unchanged. There is no authorization to enroll trust
on this host and no installation command is executed by these instructions.

1. The release authority must approve the exact Archive primary set out of band,
   its role policy/keyring hashes and a public-only native snapshot. Record exact
   `pubring.kbx`, `trustdb.gpg`, `gpg.conf` digests in the deployment review. A
   document here or successful key import does not grant that authority.
2. Have that authority supply/build the public native snapshot in an isolated,
   unprivileged preparation environment using only approved PUBLIC Archive keys.
   Its trust certification must be explicitly reviewed; do not derive ownertrust
   from key presence, copy official trustdb, blindly assign ultimate trust, or
   allow keyserver retrieval/import. This task creates no trust database.
3. Use [the restricted config](../../../../services/update/deployment/aur/native-gpg.conf.example).
   Only `pubring.kbx`, `trustdb.gpg`, `gpg.conf`, optional public `pubring.kbx~`
   and an optional empty `private-keys-v1.d` are accepted. No secret keys,
   agents, sockets, extra keyring/config redirections or discovery directives.
4. Review the snapshot separately from production activation. On an identified
   disposable guest only, the authorized provisioner copies this exact reviewed
   snapshot to the fixed target, root owned with no group/other write; public
   files single-link regular files, no symlinks, safe ancestors. Standard target
   directory mode for preflight is 0755; native files at most 16 MiB/config 4 KiB.
5. Fresh native child verification must report exactly the Archive pinned public
   primaries, no official repositories and Required/TrustedOnly policy. Exercise
   valid, foreign-domain, missing/tampered/expired/revoked signatures against the
   approved inputs; retain before/after official domain hashes. Failed native
   trust is a rejection, never a reason to add the Archive key to official trust.

Changing a domain/policy invalidates old plans; require new peer-bound preview
and consent. Even a complete public snapshot cannot open production admission.

## Reproducible disposable guest preparation boundary

The available passive metadata is not a VM. An operator may provide an already
approved runner on another machine rather than install anything on this host.
Record all required values **before** producing a launch/provision command:

| Required owner-supplied value | Review requirement |
| --- | --- |
| Runner/guest identity | Exact endpoint and VM UUID; separately authorized guest-only capability and source/binary hashes |
| Base image | Concrete public image path/source, expected SHA-256, verified format/architecture, firmware; immutable base plus fresh disposable writable overlay |
| Guest layout | Btrfs/Snapper root, supported actual boot layout and separate durable journal volume outside root rollback |
| Isolation | Private serial/QMP endpoints; no physical host disks/EFI, host folders, host system bus, host keys or journal; reviewed network allowlist/offline inputs |
| Resources | Owner-approved RAM/CPU/disk limits; software emulation allowed only if explicitly selected and practical; KVM access alone is not proof |
| Public provisioning scope | Exact reviewed trust/artifact/binary/unit set, guest marker and expected hash inventory; root/PolicyKit approval only inside the identified guest |
| Fault/recovery scope | Separate operation-bound consent for snapshot, reboot, guest kill/poweroff and restoration; evidence retained outside rollback |

Once those concrete inputs exist, verify the base digest/format without booting,
create a new overlay at an operator-approved disposable path with an explicitly
specified base format, then generate and review the exact runner argv. Do not
invent paths/UUIDs, auto-download an unapproved image, attach host devices or
publish a generic root launch script with guessed layout. With no runner/image,
no overlay, guest configuration, launch or provisioning can yet be validated.

Use [the existing acceptance matrix](../VM_ACCEPTANCE_PLAN.md), beginning with
shipping readiness/Install denial. The shipping gate is deliberately unconditional;
positive tests need a separately reviewed guest-only capability, bound to guest
identity/boot/image and operation scope. Existing Rust fixture hooks are not this
capability and must not be deployed or promoted. No source gate removal or new
production enable flag is authorized here.

Real PolicyKit/session grants, deployed D-Bus/native transactions, snapshot
creation/restoration, partial-commit power loss, reboot and failed-update recovery
remain **NOT RUN** until those prerequisites exist. Store every result as
PASS/FAIL/BLOCKED with raw guest evidence and hashes; a plan or JSON assertion
cannot mark a case passed. LAB mechanism tests remain separately labelled LAB.
