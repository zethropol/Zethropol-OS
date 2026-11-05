# AUR acceptance infrastructure — 2026-11-06

**Production acceptance BLOCKED. Installation CLOSED.** No broker/ALPM,
SQLite/post-reboot, Flatpak, ZCI, installed UI or host package changes.

## Public artifact and trust observation

The real fixed-path read-only release preflight ran outside the runner sandbox.
Manifest, Archive, receipt and distribution role keyring digest/allowlist loading
passed; receipt role independence passed. This establishes the existing public
policy's integrity checks, not independent release approval of its signers.

`/var/lib/zethropol/aur-provider-inputs` was empty. The six retained local build
root candidates were unsigned, expired schema-2 manifests; none is a current
approved production artifact. No private signing/approval store was searched.
Provider configuration `/etc/zethropol-update/aur-providers.json` and fixed native
Archive trust directory `/etc/zethropol-update/aur-native-trust` were absent.
The runtime `/run/zethropol-update/aur-artifacts` was also absent; it is not a
user-supplied artifact and is provisioned only in an authorized target deployment.

[ACCEPTANCE_INPUTS.md](../../desktop/control-center/aur/release/ACCEPTANCE_INPUTS.md)
now records the exact public policy, signer/provider configuration, manifest,
approval, distribution pairs, reviewed build request and receipt contract, fixed
locations and separate trust preparation steps. It distinguishes signer config
schema 2 from the current broker's schema-1/64-package root manifest contract.
Provider schema-2 roots are not broker acceptance inputs. No fake manifest,
owner approval, signer, native trustdb or guest identity was generated.

## Disposable guest observation and preparation

Outside-sandbox passive inventory found `/dev/kvm` as a character device with
read/write access. It was NOT opened and acceleration was NOT tested. Earlier
sandbox-local absence is not host absence. QEMU system binaries, qemu-img,
virsh/libvirt, virt-install, VBoxManage and cloud-localds were not available in
`/usr/bin`. xorriso, nspawn and bubblewrap were present; containers cannot certify
boot/power-loss/recovery.

No repository image or supplied guest identity was identified. The standard
libvirt image directory was absent; `/var/lib/machines` was inaccessible, not
empty or proven absent. No daemon was contacted, guest booted or host package
installed. There is no verified disposable guest on which to run positive tests.

`release/acceptance_inventory.py` reproduces bounded passive metadata inventory,
distinguishes inaccessible/missing, never opens a device or executes helpers and
always returns exit 2 with acceptance blocked. The updated
[VM_ACCEPTANCE_PLAN.md](../../desktop/control-center/aur/VM_ACCEPTANCE_PLAN.md)
and input runbook specify image hashes/format, isolated overlay, fixed guest
identity, independent journal layout, exact guest-only provisioning/fault scope
and the existing acceptance matrix. Exact launch/provision argv cannot safely be
created without those concrete inputs. No guessed launch script or image download
was attempted. A separately approved external VM runner is sufficient; no host
virtualizer installation is required by this task.

## Tests actually executed

| Check | Result / scope |
| --- | --- |
| Actual fixed public-policy preflight | BLOCKED overall, expected exit 2; role integrity/independence PASS, deployment/artifact prerequisites missing |
| New passive inventory | BLOCKED, expected exit 2; metadata only, never acceptance |
| New inventory boundary tests | 5 PASS: symlink refusal, bounded enumeration, inaccessible vs absent, no device/helper calls, CLI rejects guest selection |
| Python AUR regression | 486 PASS, 60 historical cases SKIPPED; 546 discovered |
| Update Rust regression | 239 PASS, 6 ignored |
| AUR Rust regression | 178 PASS, 6 ignored |
| Existing CTest | 9/9 PASS, opt-in live Qt methods remain skipped |
| Actual noninteractive PolicyKit denial | PASS; no Update method or positive grant requested |
| Live LAB namespace/build/signature/cgroup cancel-timeout-crash | Three drivers PASS; private fixtures only, no host package commit |
| Changed Python syntax / links / diff / credential-pattern scan | PASS |
| Real guest positive PolicyKit / D-Bus/native transaction / snapshots / boot / power-loss / recovery | NOT RUN: no verified guest or approved production artifact/trust set |

Rust shipping code was unchanged; no Clippy warning was suppressed and existing
main-service dead-code diagnostics remain as previously recorded. LAB successes
are not production acceptance. No test failure was relabeled as production PASS.

## Concrete external inputs still required

1. Release-owner-approved current signed schema-1 PUBLIC root manifest and exact
   signed distribution artifact closure, reviewed build/source expectations and
   independent Archive/receipt public role authorization. Existing owners must
   authorize any actual UID949/951/950 signing operation; no private key/password
   is requested. See the runbook for protected approval/configuration paths.
2. Separately approved PUBLIC native Archive keybox/trustdb/config snapshot,
   exact hashes and primaries matching the current policy. Provision only under
   target-specific authorization; never enroll it in official pacman trust.
3. Approved VM runner endpoint/tooling, concrete disposable image with expected
   SHA-256/format, guest UUID/layout/resource limits, and exact guest-only
   provisioning/install/fault/recovery authority. Positive acceptance needs a
   separately reviewed guest-only capability; shipping production gates remain
   closed and fixture hooks must not be deployed as that capability.

Only these real prerequisites block further production execution. Preparation
and independent regression are complete within the available resources.
