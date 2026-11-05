# AUR production acceptance follow-up — 2026-11-05

**INCOMPLETE. Production installation remains CLOSED.**

## Implemented and verified in this follow-up

* The locked AUR resolver rejects package downgrades using native version
  comparison, including epochs, before package addition/claim. Denial leaves
  the private fixture database unchanged and releases its transaction lock.
* After successful native commit, a fresh local DB handle verifies every
  planned installed version. Failure is recorded as a failed operation while
  retaining the actual Committed evidence; it never invents rollback or retry.
  Hook/boot uncertainty remains separately reported. No evidence schema changed.
* Rust and Python signature verification reject expiry/revocation/missing-key/
  failure status even alongside VALIDSIG, require one structurally complete
  signature status and retain pinned-primary validation. Rust rejects duplicate
  role allowlists; Python project-policy schema rejects boolean/float versions.
  Real signature-success cases remain exercised, not just mocked status cases.
* The source release preflight resolves its own modules correctly. It always
  reports installation=closed and ready_to_install=false, separating successful
  public prerequisite checks from actual production authorization/acceptance.
* New actual native transactions run in two independent PUBLIC LAB trust homes,
  one per fresh process with Required/TrustedOnly signature policy. Each domain
  commits only its own signed inert package. Cross-domain signatures, changed
  sealed artifact bytes/signatures, mismatched name/version/architecture and
  missing runtime dependencies are rejected with unchanged local DB/no payload.
  Hostile inherited GPG settings cannot select the child's trust domain.

The native transaction probe is test-only, requires an explicit LAB compile
macro, refuses root and accepts only private disposable /tmp LAB directories.
It uses sealed FDs, no registered sync repositories, no host hooks, no trust
import from a caller and no signature/dependency/conflict bypass. Its CLI is
not a broker entrypoint or production installation helper. Only ephemeral LAB
keys exist in temporary signing homes; only their public keys enter the
separate temporary native domains. None are production artifacts or authority.

## Final validation

| Check | Current result |
| --- | --- |
| Main Update Rust | 227 passed, 5 ignored; includes the 50-case SQLite fixture adapter |
| AUR candidate Rust | 166 passed, 5 ignored |
| Main and AUR cargo check, offline/locked | PASS |
| AUR all-target Clippy -D warnings | PASS |
| Main all-target Clippy -D warnings | FAIL: same inherited dead-code diagnostics documented in AUR_WIP_STATUS.md; no new suppression |
| Qt/QML build | PASS |
| CTest | 9/9 targets PASS; opt-in live Qt methods remain skipped |
| Python through CTest | 481 passed, 60 historical capture cases skipped; 541 discovered |
| Actual two-domain native transaction cases | PASS, isolated LAB only; two positive and eight negative transactions across three tests |
| Signed-root/makepkg/native signature driver | PASS, ephemeral LAB only; no host commit |
| rustfmt / diff whitespace / changed-file credential-pattern scan | PASS |

The first version-readback test changed only the desc VERSION text; native ALPM
uses the local package directory version too. The fixture was corrected to
model an actual version change by renaming that private directory. The source
preflight import failure was a code/layout bug, fixed and tested separately
from intentionally blocked prerequisite results. A later normal parallel Rust run exposed an existing lease test's immediate
reacquisition assumption: another test can fork while its CLOEXEC File is open,
retaining the open description briefly until exec. The exact test passed alone;
it now runs the same assertions in a single-test child process with no sleeps,
retries, removed assertions or production lock changes. Final full-suite results
are recorded above. No failed test is counted as production acceptance.

## Current unavailable inputs and uncompleted integration

The search was limited to public candidates, documented provider input paths
and repository VM image names; no private-key store was searched. Six local
Schema 2 root Manifest candidates remain expired and unsigned. No matching,
current owner-approved signed production artifact set was verified. Existing
public role keyrings/account names alone cannot establish it. The documented
provider inputs/build-root directories were empty at the preceding check and
no provider configuration was supplied. Inaccessible protected approval storage
is not claimed absent.

Current passive infrastructure inventory again found no QEMU, qemu-img,
libvirt/virsh or VirtualBox tool, no /dev/kvm and no supplied/repository guest
image or guest identity. nspawn/bubblewrap are available for mechanism tests;
they cannot certify independent boot or power-loss/snapshot recovery.

**Still not completed:** the actual production broker's AUR preparation reuses
its official ALPM handle. The new separate-domain LAB transactions do not
replace that path. A reviewed fixed-domain production process/worker integration
with approved public trust/artifact inputs and guest validation is still needed.
No official keyring was expanded, no native signature check disabled and no
production gate opened to make this missing integration appear successful.

## Safe disposable-guest setup and acceptance plan

No guest creation/install/fault command was run. The existing full case matrix
is [VM_ACCEPTANCE_PLAN.md](../../desktop/control-center/aur/VM_ACCEPTANCE_PLAN.md).
Execution must proceed only after concrete prerequisites exist:

1. Identify the operator-approved disposable image digest and VM UUID/boot
   identity. Review its Btrfs root/snapshot layout and durable journal volume
   outside root rollback. Do not attach host disks, EFI devices, private signing
   homes or writable host directories. Missing identity/layout stops execution.
2. Review exact source/binary hashes, approved PUBLIC role fingerprints/rings,
   current signed Manifest/artifact set and bounded evidence destination. Keep
   LAB keys and historical expired manifests outside production role lists.
3. Authorize only the exact guest provisioning scope using the existing
   privileged authorization mechanism. Provision guest-only public trust and
   inert transaction fixtures, without host key import or signing. Provisioning
   approval is not installation/rollback/power-loss consent.
4. Verify fixed native domains, full official-upgrade barrier, original D-Bus
   peer/session binding, separate real PolicyKit decisions and fresh one-use
   receipt/plan consent. Capture denial DB fingerprints before testing success.
5. Under separately authorized guest transactions, test valid commit, changed
   artifacts/plan/trust, cancel before/after commit-start, timeout, broker/worker
   crash and snapshot failures. Keep Committed/Unknown/partial outcomes distinct;
   no automatic retry or rollback may hide them.
6. With explicit guest fault/recovery scope, exercise the observed partial-commit
   power-loss window, reboot and snapshot restoration. Preserve journal/replay
   evidence outside rollback and verify installed versions and boot identity
   after recovery. Host reboot is never part of this plan.
7. Independently review raw guest evidence for every required case, including
   failed-update recovery, before considering the production gate. Missing,
   synthetic, skipped or LAB-only evidence is NOT RUN/BLOCKED, never PASS.

The release owner must supply/approve only PUBLIC role/artifact inputs and the
identified disposable guest. Existing operation-bound approval must be used if
actual signing, guest privilege, install, fault or recovery actions become
necessary. Do not send private keys/passwords; do not invent artifact/VM paths.

## Delivery scope

Changes are developed in the existing isolated Git worktree based on the
published main commit 2eda8924b91f92d21b9c48d88d453af26304908d. The original dirty
workspace, Hardware, backups, package archives and installed Control Center are
preserved. Verified safety fixes and tests may be published as incomplete WIP;
this document does not assert a completed release or predict push success.
