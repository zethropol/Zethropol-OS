# Observable hook and boot evidence

This source change adds read-only artifact verification to the worker. It does
not run a transaction, rebuild anything, start services, create snapshots, load
modules, change firmware or deploy a unit. Historical host A/B evidence is not
reused as proof that these new checks pass on the host.

## Hook observation boundary

The used Rust `alpm` 5.0.2 binding exposes HookRunStart/HookRunDone with name,
description, position and total. The linked system's `/usr/include/alpm.h`
`alpm_event_hook_run_t` likewise contains no exit code. The reviewed upstream
pacman 54d9411 trans.c does not propagate the post-hook runner return value to
commit's return. That upstream review is not a fresh audit of all CachyOS fork
patches; no positive result depends on assuming the fork behaves identically.

The worker records a bounded lifecycle trace and `alpm::version()` alongside
its result. A returned event means only that a hook returned, **not** that it
exited successfully. `TransHookFailed` is explicit failure evidence. Generic
logs, an absence of errors, HookRunDone, commit success and valid boot files
never establish aggregate hook success. `hooks` remains `unknown` without a
reliable exit-status source. No log-text classifier, hook wrapper, ptrace/BPF
instrumentation or installed hook replacement was added.

## Read-only implementation

`boot_verifier.rs` obtains package ownership/version information from the same
libalpm local database handle as the transaction. It runs after commit returns,
while that worker still holds the pacman lock. The commit checkpoint is saved
first, then artifact evidence is saved before lock release. Post-snapshot
handling remains independent and is attempted as before.

The verifier uses only read-only file descriptors, metadata, directory reads,
canonical paths and an explicit helper enum. It does not source shell config,
run `dkms`, `mkinitcpio`, `limine`, `pacman`, `systemctl`, `snapper` or mount
commands. `lsinitcpio` is deliberately not used: even listing mode makes a
temporary directory. Archive extraction here means `bsdtar -xOf - -- <entry>`
from stdin to stdout only, never extraction into a filesystem directory.

Helpers are limited to bsdtar (listing/metadata/stdout reading), modinfo field
reading, gzip/xz/zstd stdout decompression, and BLAKE2/BLAKE3 checksum readers.
No shell is invoked. A timeout wrapper limits each helper to at most 15 seconds,
with a 60-second inspection deadline and 128-helper budget. Files are limited
to 512 MiB, helper output to 32 MiB, and persisted boot diagnostics to about
128 KiB. Exceeded limits, missing tools and denied reads yield unknown. A nonzero helper exit by itself also remains unknown: it does not distinguish unsupported decoder features/resource failure from corrupt content. These
are application limits, not a hard cancellation guarantee for stalled kernel
filesystem I/O. Nonblocking opens prevent FIFO outputs from hanging the reader.

## What each verified result means

| Component | Required observable evidence | What it does NOT prove |
|---|---|---|
| Kernel | Package-owned `/usr/lib/modules/<release>/vmlinuz` and pkgbase have consistent owner/version; changed kernel package matches approved new version; pkgbase matches package; x86 boot signature/HdrS and embedded release agree; modules.dep is present, nonempty and its listed files are present/nonempty | Cryptographic integrity of every installed file, every module's machine code, all depmod completeness or a successful boot |
| DKMS | Package-owned static dkms.conf explicitly declares autoinstall/name/version/modules; registration points to that version/kernel/architecture; built and installed module bytes agree after supported decompression; both vermagic values match; installed module is uniquely referenced by modules.dep | Loading, module signature/key acceptance, dynamic Bash policies or unregistered custom source requirements |
| Initramfs | Readable supported archive, bounded early-newc framing, init executable/symlink metadata and payload; shell-init interpreter is present/readable; module tree names identify the same kernel release | Actual init execution, ELF shared-library closure, correctness of every driver, root activation or hardware compatibility |
| Limine | Explicit literal ESP_PATH with an observed filesystem-root mount; local config entry paths resolve without traversal/volume escape; copied Linux bytes equal a uniquely package-owned installed source; referenced initramfs matches; UKI .linux/.initrd/.uname are coherent; x86-64 EFI headers are inspectable; deployed Limine bytes equal packaged source; supplied BLAKE2/BLAKE3 resource hashes match | Which config or loader firmware will select, shadow configs/SMBIOS overrides, signatures/Secure Boot, NVRAM order, command-line/root correctness or boot success |

Results are scoped assertions, not an “everything is valid” certificate. A
known missing/malformed required object or mismatch is failed. A missing
observation, unsupported layout, ambiguity or dynamic policy is unknown. Failed
dominates unknown in aggregation; empty coverage is unknown. No package-owned
or registered DKMS sources observed is explicitly recorded as the scope of a
verified no-DKMS result, not evidence that unknown custom modules work.

Supported paths are `boot():` relative to the explicitly configured mounted
volume, and uniquely mounted filesystem/partition UUID aliases through
`/dev/disk/by-uuid` or `by-partuuid`. UUID mapping requires an observed mount of
that filesystem root. Dynamic macros, network paths, ambiguous mounts,
subvolume-root mappings that cannot be established and complex symlink layouts
are not guessed. Unrelated non-Linux EFI applications are not certified.
Unowned/stale/snapshot kernel entries can keep Limine coverage unknown.

A signed or config-enrolled Limine binary may legitimately differ from the
packaged bytes. That difference is **unknown**, not assumed corruption and not
assumed safe. Supporting those transformations requires a dedicated verifier.
The current kernel and EFI header validators are scoped to x86/x86-64; other
architectures are not certified. Microcode-only archives cannot substitute for
a main initramfs, and built-in-only images without observable module identity
remain unknown.

## Running versus installed versus updated

`running_kernel` is read from `/proc/sys/kernel/osrelease`. It is never inferred
from the package database. `installed_kernels` comes from package-owned image
paths; `updated_kernels` identifies kernel packages in the approved transaction.
An installed package version differing from the approved target is reported as
a failure, including after a partially applied/failed commit.

`new_kernel_boot` is always unknown in this pre-reboot verifier. Even equal
release strings do not establish that the running image is the newly installed
binary. A separate post-reboot observation, image identity/boot context and VM
boot test are required. Fully verified overall success now also requires that
separate boot evidence, so local artifact verification cannot silently promote
an operation to reboot success.

## Persistence and Qt

Record format 3 stores per-component states, running/installed/updated releases,
multiline artifact details and hook lifecycle details. Formats 1 and 2 remain
readable with absent observations set to unknown. They never gain fabricated
boot evidence. New evidence is fsynced before lock release and again with the
post-snapshot outcome. A stop/reopen preserves already recorded observations.

`GetOperationState` remains `a{ss}`. New keys are `kernel_artifacts`,
`dkms_artifacts`, `initramfs_artifacts`, `limine_artifacts`, `new_kernel_boot`,
`running_kernel`, `installed_kernels`, `updated_kernels`, `boot_details` and
`hook_details`. Existing commit/hook/boot-preparation fields remain independent.
Artifact observations are point-in-time data (timestamped), not continuous
monitoring. Other privileged actors may change files despite the pacman lock.

Qt retains all fields and shows each component and release category separately.
Missing keys remain unverified. Even all verified local artifacts retain the
reboot warning. The worker's terminal completion can remain
completed-with-warnings after commit while a component is explicitly failed;
that does not erase the committed package result or imply rollback. Users see
the failed component and details, not a clean “system up to date” refresh.

## Tests and production gates

The suite includes coherent Linux and UKI layouts, wrong/missing/corrupt kernel
files, package ownership and target-version mismatch, empty or broken evidence,
DKMS registration/vermagic/byte mismatch and dynamic/unmanaged policies,
initramfs kernel mismatch/dangling/non-executable init, unsupported decoders and
formats, denied reads, missing/ambiguous entries, UUID ambiguity, traversal,
symlink escape, hash mismatch, PE truncation and helper limits. The read-only
archive test runs the real timeout/bsdtar reader on synthetic CPIO bytes through
pipes; it verifies both a good init chain and a missing target, without creating
boot files or invoking a package/snapshot operation.

Persistence tests cover format-3 round trips with failed and unknown component
states, normal stop/reopen, corrupt enums and formats 1/2. Rust tests ensure hook
return events cannot become exit-success proof, and verified artifacts cannot
become a successful new kernel boot. Qt tests cover successful/failed/unknown
artifact rows and real peer D-Bus transport of the added string-map fields.

Actual transaction integration is still a VM gate: real compressed images and
DKMS/signing policies, Limine config enrollment and firmware selection, root
command-line/driver requirements, snapshot interactions, interruption and
post-reboot boot/recovery tests. Global hook exit success remains unobservable
through the present API. No real update, package install, system service start,
snapshot/rollback or installed unit change was performed for these tests.
Broker/worker deployment must be coordinated because old binaries cannot read
format 5. Replacement consent, incoming package checksum/repository metadata
identity and effective hook/pacman-policy fingerprint are bound in the current
source plan. Their behavior against actual signed repositories and package
hooks remains a production/VM validation gate; the previous snapshot-policy
restrictions remain in force.

Sources inspected: local alpm 5.0.2 `unions.rs`, local `/usr/include/alpm.h`,
previously downloaded upstream pacman 54d9411 `lib/libalpm/trans.c`, installed
lsinitcpio/DKMS/Limine scripts and the existing architecture report. Limine path
and protocol semantics were checked against the upstream specification:
https://github.com/limine-bootloader/limine/blob/trunk/CONFIG.md .

## Validation result — 2026-10-27

- Rust: 81 passed, 0 failed, 2 ignored (83 total); 18 artifact-verifier tests.
- Qt: all 3 CTest suites passed, including the real peer D-Bus map and
  verified/failed/unknown UI rows.
- Rust release build, full Control Center build, cargo fmt check and Clippy
  all-targets with warnings denied passed.
- Source systemd unit static verification passed; its source was not changed.
- Six authorization/nonce/plan/lease/flock function bodies were unchanged;
  installed production unit/binary hashes matched the earlier host baseline.
- No real transaction or boot was performed. These results validate source and
  fixtures, not host boot readiness. Existing unrelated files/backups were
  preserved against the pre-edit hash inventory.
