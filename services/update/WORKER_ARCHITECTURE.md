# Worker architecture — source implementation, not production deployment

This change builds on the approved host A/B and systemd 262.1 source reviews.
No installed unit, running service, package, snapshot or rollback was changed.
It is not approval to deploy or execute a real transaction.

## Worker profile and trust boundary

The worker source uses `ProtectSystem=no`, an empty `ReadWritePaths`, and
`ProtectKernelTunables=no`. Trusted package payloads, sysctl/binfmt, EFI hooks,
atomic file replacement and arbitrary package state cannot be supported by a
permanent finite path list. This does not remount a genuinely read-only host
filesystem. The broker unit source is unchanged.

Retained worker controls: root user/group, fixed UUID-only entry point,
`Restart=no`, `KillMode=control-group`, `NoNewPrivileges=yes`, `PrivateTmp=yes`,
`ProtectHome=yes`, `ProtectControlGroups=yes`, `LockPersonality=yes`, and
AF_UNIX/AF_INET/AF_INET6/AF_NETLINK only. Device/module/SUID metadata restrictions
remain off. stdout/stderr go to the journal; libalpm messages, script output,
hook starts and final results are logged with the operation ID. Logs are
supporting diagnostics, not proof of hook exit success.

This is a short-lived privileged executor for trusted repositories, not an
isolation boundary for malicious root scripts. PolicyKit sender binding,
server-held one-use nonce, exact canonical plan matching, worker lease, and
pacman transaction lock remain in force. Their implementations were compared
with the pre-change working-tree copies; authorization, consume, claim, lease
and flock functions are unchanged. Nothing accepts a caller-supplied command,
UID or package list.

## Independent execution evidence

`execution_evidence.rs` models package commit, hook verification and boot
preparation independently:

| Evidence | Values | Current production behavior |
|---|---|---|
| package_commit | not-started / in-progress / committed / failed-may-be-partial / unknown | Based on durable checkpoint and libalpm return |
| hooks | unknown / failed / verified | Explicit TransHookFailed records failed; commit Ok leaves unknown |
| boot_preparation | unknown / failed / verified | Aggregate of scoped read-only kernel/DKMS/initramfs/Limine checks; see BOOT_VERIFICATION.md |
| new_kernel_boot | unknown / failed / verified | Always unknown before a separate post-reboot observation |
| pre_snapshot / post_snapshot | positive snapshot ID or absent | Only successful Snapper output is recorded |

There is no current path that certifies hook exit success or a successful new kernel boot. Read-only artifact checks can certify only their documented local scope; see [BOOT_VERIFICATION.md](BOOT_VERIFICATION.md). A normal
completed package commit returns terminal `completed-with-warnings` and D-Bus
`ok_with_warnings`. A post-snapshot failure remains a warning with an explicit
message, not a false rollback or boot-readiness claim.

An atomic/fsynced record checkpoint precedes commit; failure to save it aborts
before commit. The return outcome is saved before lock release. Post-snapshot
is still attempted after a failed commit. Final checkpoint failures report an
error without automatically retrying. Interruption changes an inflight commit
to unknown; it preserves an already confirmed commit and existing snapshot IDs.
Normal ExecStopPost is idempotent for completed-with-warnings records.

Record format 5 carries the full plan, artifact and hook evidence, package
archive identities, repository metadata digests and policy fingerprint. The
reader supports formats 1–4, assigning
unknown evidence even to old successful records. GetOperationState keeps its
`a{ss}` ABI and adds component states, kernel identities and bounded diagnostic fields. Qt preserves these fields, handles
missing keys conservatively, shows package/script/boot results separately,
and does not automatically replace an unverified completion warning with a
fresh “system up to date” scan. Tests block the real system-bus address before
constructing UpdateService, preventing accidental broker activation.

## One root snapshot chain, without silently weakening recovery

Only this worker's unit environment sets `SNAP_PAC_SKIP=1`; there is no global
environment modification and no installed hook edit/mask. Zethropol's direct
Snapper root pre-snapshot remains mandatory before commit, with a paired
post-snapshot attempt afterward, including commit failure. The environment
variable belongs to snap-pac, not Snapper, so it does not skip those commands.

`snapshot_policy.rs` permits suppression only when all inspected conditions
match the audited contract:

- The snap-pac script SHA256 is
  `746eacf0785fd40c59750dd9821f894c6b1c41bb1ec892af6872b94093447dc6`.
- The two vendor hook Exec commands call that script with `pre`/`post`.
- There is no local override of those hooks and no custom HookDir beyond the
  normal `/etc/pacman.d/hooks` directory.
- `/etc/conf.d/snapper` names exactly root. The ini policy is default or the
  supported explicit root snapshot=true setting. Custom cleanup, metadata,
  descriptions or extra configurations require integration instead of being
  silently discarded.
- The transaction is not installing/upgrading snap-pac. Its incoming skip
  contract must be reviewed first; checking only the old installed script
  cannot establish how the new post hook behaves.

If snap-pac is absent and its hooks are absent, there is no chain to suppress.
Unknown or unreadable policies fail before snapshot/commit. The repository DB
may already have been refreshed by that point; “no commit” does not mean no
cache/DB writes. No additional snapshot configuration is silently skipped.

The digest deliberately bounds compatibility to the audited script. Supporting
another version requires review and tests, not automatic acceptance. This is
not an audit of all future packages that might add their own snapshot hooks.
Archive/hook pinning and VM validation remain deployment gates. Root pre/post
snapshots still do not cover ESP/NVRAM, all separate subvolumes, or prove boot
recovery. Automatic rollback remains disabled.

## Replacement-aware approved plans

Explicit libalpm replacement edges are now supported in source. PreviewPlan
returns additions, versioned removals/reasons/replacement names, and the exact
canonical SHA-256 plan hash persisted for the authenticated sender. Qt displays
INSTALL/UPGRADE and REMOVE/REPLACED BY entries and requires an unchecked explicit
removal checkbox. TriggerUpdate accepts the reviewed hash and an explicit removal
approval boolean; it retains PolicyKit, sender/session ownership, the one-use
nonce and atomic consumption. The worker reconstructs the full plan under the
pacman lock and rejects any removal, version or edge drift before snapshots/commit.

Conflict-driven removal, unresolved dependency target dropping, ambiguous
providers/replacements and critical removals remain blocked. The libalpm
RemovePkgs question means dropping unresolved transaction targets; it must not
be mislabeled as proof of installed dependency removal. Any final installed
removal without an explicit replacement edge is rejected. Kernel/boot file
ownership and a conservative critical package policy also block removal.

Record format 5 includes the removal set and package-content identity; readers
accept legacy records 1–4 without inventing content identity. Old binaries
cannot consume format-5 records.
Deploy the matching broker, worker, Qt client and source D-Bus policy together;
older clients lack the required hash/consent arguments and fail closed.
See [REPLACEMENT_MODEL.md](REPLACEMENT_MODEL.md) for support boundaries, tests
and production gates. Format 5 binds official archive checksums, repository
database/signature digests, and effective pacman/hook policy to the plan. These
checks are source-implemented but have not been exercised against a real signed
repository transaction.

## Validation performed

- `cargo fmt -- --check`: passed.
- `cargo clippy --offline --all-targets -- -D warnings`: passed.
- `cargo test --offline`: **93 passed, 0 failed, 2 ignored** (95 total; includes content identity, drift, replacement and authorization tests).
  The ignored entries are the opt-in live KDE/policy test and a subprocess
  fixture entry invoked only by isolated integration tests.
- `cargo build --offline --release`: passed.
- Full Control Center CMake build including QML compilation and Update tests:
  passed. Existing Qt CMake policy/Vulkan-header notices do not fail the build.
- CTest: **3/3 suites passed**, including peer D-Bus transport of the new
  evidence map, legacy missing evidence, warning retention and unrelated ID
  isolation. No real broker was activated.
- `systemd-analyze verify` on the source worker unit: passed; no service started.

Sandbox socket restrictions initially blocked private D-Bus transport tests and
systemd's static verifier. The same commands passed outside the sandbox using
only isolated fixtures/static analysis. Live host/policy tests were not enabled.
Tests cover pre-snapshot failure preventing commit, evidence persistence/reopen,
interruption ambiguity, corrupt records, legacy migration, snapshot policy
rejection, consent gates, nonce replay, identity, plan equality, lock/lease
collisions and terminal idempotency. No real package or snapshot executor was
run. Pure tests do not establish real ALPM hook/Snapper/firmware integration.

## Remaining production blockers and VM matrix

- No complete hook-exit observer or post-reboot verifier exists. The new read-only artifact checks have explicitly limited scope; boot success remains unknown. See BOOT_VERIFICATION.md for unsupported layouts and VM gates.
- Replacement-aware source flow passes metadata/authorization/UI fixtures. Real replacement commits, hooks, recovery and deployed PolicyKit require disposable VM validation.
- Additional snap-pac policies/versions and incoming hook changes need explicit
  support. Validate exactly one root pre/post pair and continued normal
  snap-pac behavior for transactions outside this worker.
- Validate download-only behavior, trusted checksum extraction, libalpm archive
  signature enforcement and precommit cache verification in a disposable VM
  using the configured signed repositories. No failure path may weaken the
  existing SigLevel policy.
- Deploy broker/worker together with their matching unit environment. Old
  binaries cannot read format 5; do not downgrade while live format-5 operations
  exist. Records remain under /run and do not survive reboot.
- Keep the shared root-owned 0700 runtime store through worker completion and
  broker restart. Do not add a worker RuntimeDirectory that deletes shared
  operation records on stop.
- Test filesystem, glibc, systemd/PID1 reexec, keyring, DKMS, initramfs, Limine,
  EFI/NVRAM, Secure Boot, root pre/post and failed post hooks in a disposable
  Btrfs VM. Verify ProtectHome (including filesystem package directory metadata
  and signing keys), PrivateTmp/daemon interactions, AF restrictions and cgroup
  protection against the real configured hooks.
- Exercise disk full, failed commit, checkpoint failure, worker interruption,
  broker restart, stop/shutdown and the existing 30-second stop timeout. No
  automatic retry should replay a possibly applied transaction.
- Restore and boot the VM; separately verify kernel/initramfs, root subvolume,
  ESP and firmware state. A successful snapshot call is not this validation.

The installed production binary and broker/worker unit hashes remained equal
to the prior host baseline. Existing unrelated working-tree files and backups
in the pre-edit hash inventory were preserved. Production deployment is still
blocked on the explicit functional and VM gates above.
