# Disposable VM acceptance plan — NOT executed

Status: plan and passive inventory prepared. No VM/image was created, booted or
installed; no real package, snapshot or boot operation ran on this host.
Production AUR remains CLOSED. Unit/private-root success is not system acceptance.

## Available and missing infrastructure

`VM_HOST_INVENTORY.json` is the passive, outside-runner-sandbox observation:
CPU `svm` and KVM module visible; `/dev/kvm` exists and passive read/write access
checks succeed. No KVM device was opened/ioctl'd, so acceleration is untested.
QEMU system binary, qemu-img, virsh/libvirt tooling, virt-install and cloud-localds
are absent from PATH. xorriso, systemd-nspawn, bwrap, gpgv and timeout are visible.
Containers are not substitutes for firmware/real reboot and recovery acceptance.
Mount namespace is recorded; PID-1 namespace comparison was denied, so no claim
of equality is made. Earlier sandbox-only absence of `/dev/kvm` is superseded.

This task installed/updated nothing and made no daemon connection. An operator
must provide an approved VM runner elsewhere or separately provision the host
under another task. Also missing: an authorized disposable image, its expected
SHA256, guest disk/layout manifest, and actual approved signed production inputs.

The integration crate is a non-installed library, with no service binary or
installer. A reviewed VM-only broker/worker harness/package assembly is still
needed to execute the real system-bus methods. The production gate has no enable
flag: the positive-acceptance driver must be a separately reviewed guest-only
build/capability, with recorded source/binary hashes and an explicit disposable
VM identity. It must not weaken or replace the shipping gate/source/config.
No positive case is executable merely by running this document against the host.

## Required guest and authority manifest

Before scheduling a positive case, capture and approve:

- VM UUID, base-image SHA256, disposable disk paths, firmware choice and a private
  serial/QMP endpoint. No host physical disks, production EFI vars, host folders,
  host system bus, production journal or private keyring may be passed through.
- Guest Zethropol/Arch package/config versions matching the ALPM/boot verifier,
  real KDE/logind active-local graphical session and actual existing Update
  D-Bus/PolicyKit rules. No unconditional allow-all fixture PolicyKit authority.
- A supported Btrfs/Snapper root target and supported real boot/Limine layout;
  guest kernel/initramfs/ESP evidence must be observable by the EXISTING verifier.
- Dedicated persistent journal storage at `/var/lib/zethropol/aur-transactions`,
  outside package-root rollback. Reviewed ext4/Btrfs/XFS, root:root directory
  0700/files 0600, writable in the REAL service/worker mount namespace, fsync and
  atomic no-replace supported. Test scratch writes only INSIDE the disposable
  guest; a ReadWritePaths property alone is not proof of write permission.
- A guest-only source/binary/unit hash manifest and actual namespace/lease/
  syscall permissions. See existing `services/update/WORKER_VALIDATION.md`:
  the prior mount-permission probe was not proof of a real commit-capable worker.
- Three approved PUBLIC project role keyrings with SHA256/primary fingerprints,
  root-owned fixed-path policy; distribution/native keyring policy separately.
  Do not inherit Arch signers as receipt/build-root authorities.
- Exact signed build-root manifest and distribution dependency closure, reviewed
  recipe/source plan and selected outputs; a trusted independently attested
  detached receipt and matching signed archives. Reserve/sign promptly within
  the existing five-minute handoff, after long unprivileged builds complete.
- Owner-approved harmless bounded fixture payloads permitting partial extraction
  observation. No client command/PKGBUILD may run in the root broker. Fixture
  build occurs only in the existing unprivileged sandbox.

Verification uses only public keys. Role authorization and artifact signing must
be done by their owner/approved attestor. If the owner's policy requires the
physical developer token, only that owner performs the signing/unlock step.
This agent must not create a replacement private key, enroll a test key into
production policy or request/upload a private key. VM-only mechanism policies
are not evidence that a production signer policy was authorized. Record which
scope every result establishes; production remains closed for either scope
until release acceptance and actual production inputs are approved.

## Isolation and evidence procedure

1. Verify image/artefact hashes and the manifest above before any guest mutation.
   Provision guest-only marker UUID and assert it from the harness. Refuse a host
   path, missing guest marker, shared production resource or ambiguous target.
2. Run passive guest preflight and `GetAurReadiness`. Valid public role pins do
   NOT certify signed artifacts, bootability or installation readiness. Baseline
   `InstallAur` must still return closed on the shipping build.
3. Validate the separately reviewed VM test capability; bind it to this VM UUID,
   boot ID, image hash and explicit operator scope. No production deployment or
   source gate removal is part of this task.
4. Establish each case from a known disposable baseline. Keep the authoritative
   journal through reboot/fault/recovery. For independent cases clone a fresh
   guest identity; do NOT erase accepted nonce evidence then reuse that identity.
5. Capture protected raw journal copies (including envelopes/boot anchor),
   canonical/receipt/signature hashes, ALPM local/sync fingerprints, package-file
   inventory, system-bus unique sender/UID/PID/session observations, actual
   PolicyKit decision/action, worker lease/cgroup, Snapper IDs, boot artefacts and
   serial logs. Do not equate hook lifecycle events with exit-status evidence.
6. Compare after state. Every denial before commit requires zero package/DB
   mutations; a created pre-snapshot is recorded separately. No old consent,
   auto retry, automatic rollback or invented success may hide uncertainty.

## Required cases and pass criteria

| ID | Action INSIDE disposable VM | Required evidence/result |
|---|---|---|
| BASE | Shipping build, even with valid keys | Install stays closed; readiness never says acceptance passed |
| AUTH-OK | Real active local user, original Preview then Install actions, correct receipt/hash | Distinct real PolicyKit decisions, original UID/PID/sender/session binding, one-use consent; final native plan equals preview |
| SUCCESS | Real signed payload commit through VM-only driver | Actual files/local DB match selected versions, actual pre/post snapshot IDs persist, native Committed; warnings remain for unverified hooks/new boot |
| DENY | User cancels auth; foreign UID/peer, remote/inactive session, no Install grant | No consume/commit, package/DB fingerprints unchanged, original correct receipt remains subject to normal one-use rules |
| TRUST | Wrong role/signer/keyring hash, bad/expired receipt/root, changed archive/signature/source plan | Broker independently rejects; no package mutation; no root test-key enrollment |
| PLAN | Change installed/sync/config state or introduce official upgrade/dependency/conflict between preview and final prepare | Full locked resolver/equality rejects terminally; no partial upgrade or automatic fresh consent |
| SNAP-FAIL | Make guest snapshot target invalid/read-only or fail Snapper in the reviewed VM fixture | Precondition fails before commit; NotStarted and any observed recovery ID persist |
| BOOT-BLOCK | Unsupported/unknown boot layout or missing/mismatched required guest artefact | Existing preparation verifier blocks native commit; no disabled check/fabricated boot flag |
| CANCEL-BEFORE | Cancel while snapshot creation is pending, before durable commit-start wins | Exactly one winner; NotStarted; any later real snapshot ID retained; no revival |
| CANCEL-AFTER | Cancel after fsynced commit start | Cancellation refused; commit result retained independently; no rollback/retry |
| CRASH-BEFORE | Kill broker before commit-start publication, restart service | No native commit; queued/running abandoned work terminal; old grant cannot resume |
| CRASH-ADMIT | Kill after receipt tombstone publish, before final preview publish | Tombstone retained and same receipt/UUID cannot be admitted again; incomplete admission is not consent |
| PARTIAL-KILL | Wait for durable commit-start AND at least one inert fixture payload file while full payload is incomplete; kill broker | Native outcome Unknown/may be partial, real DB/files inspected, pre-snapshot retained; no retry on broker restart |
| POWER-LOSS | Repeat partial window, abruptly power off THIS disposable guest, reboot same journal volume | Old previews dead despite clock reset/bus-name reuse; receipt remains spent; Unknown and recovery IDs retained; actual fsync/media behavior checked |
| POST-SNAP | Native succeeds but guest post-snapshot fails | Committed plus warning; no second native commit to repair postcondition |
| POST-BOOT | Completed commit, real guest reboot into chosen boot entry | Compare running kernel/root and artefacts, serial/boot logs, persistent result via fresh read authorization; do not rewrite native evidence from boot alone |
| CLOCK-REPLAY | Reboot and roll guest wall clock backward, replay old exact receipt/hash/identity claims | Boot epoch invalidates grants; monotonic deadline not rebased; retained UUID cannot be recycled |
| DAMAGE | On guest copies: bad checksum, truncate/remove marker/op/anchor, wrong owner/mode, symlink/hardlink | Store blocks; no chmod/blank-journal repair, silent evidence removal or automatic retry |
| SPACE | Cross logical limits and separately exhaust guest journal space | New work refused with checkpoint headroom where possible; fsync/write errors fail closed; unresolved native outcome stays Unknown |
| RECOVER | Inspect partial guest state; operator performs separately authorized package-root snapshot recovery | Journal/receipt evidence survives root rollback, actual boot/recovery verified; original Unknown operation never reruns |
| REGRESSION | Existing official update, Build Control Center and Flatpak tests in guest | Existing source/result/consent separation preserved; AUR journal is not official consent |

Use a serial/guest watcher on the fsynced journal and inert fixture extraction
state to find the partial-commit window. A process kill or VM poweroff must be
positively observed at that window; a kill after completion is NOT a partial
commit test. Do not count cfg(test) crash hooks or private-root simulations as
real media, PolicyKit, snapshot or boot acceptance. If deterministic timing
cannot be established, mark that case BLOCKED/NOT-RUN rather than passed.

## Exit/retention rules

All required cases need signed-off raw evidence; deviations/Unknown observations
remain explicit. Preserve the protected journal and replay proofs; redact public
summaries without discarding original protected evidence. Real operator recovery
is a new reviewed action, never automatic repeat of the failed operation.

Before release approval, resolve any corruption/retention/mount-write/boot gaps,
verify actual production role inputs, and separately review service/worker/UI
packaging and the acceptance gate. Mechanism-only VM tests, a verified keyring,
valid native return or a bootable container do not enable production AUR.
