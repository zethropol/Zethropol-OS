# Update service security and recovery boundary

## Privileged execution

The system D-Bus activated service runs as root. The preview method is gated by
`org.zethropol.update.preview`; `TriggerUpdate` checks the original system-bus
sender against `org.zethropol.update.install`, whose PolicyKit default requires
administrator authentication. The service accepts only a one-use preview grant
for that sender, refreshes official repository databases, locks and resolves the
pacman transaction, then compares the exact package name, version, and repository
set before commit.

The root broker authenticates the original D-Bus sender, persists a single-use
preview, and starts the fixed systemd worker with only a broker-generated UUID.
The worker owns a lease through result publication and claims the stored plan
under the pacman lock. It never trusts a caller-supplied package list or UID.

Identity resolution uses bus UID/PID and logind. If logind explicitly reports
NoSessionForPID for a user-manager process, only that PID's same-UID logind
User.Display association is accepted, with UID, membership, active/local
Wayland/X11 and user-class checks. A lingering manager without a display is
rejected. This association does not authorize the caller: PolicyKit uses the
original unique bus subject. Identity is checked again after authorization;
records never transfer to another bus connection or another user's session.

Pacman install scripts and hooks execute with root privileges. Package
signatures establish repository authenticity and integrity, not that every
script is harmless. The enabled official repositories and their signing keys
are therefore part of the trusted computing base. A compromised or malicious
trusted package can act as root during an update.

The broker vendor profile remains unchanged by the worker architecture change.
The worker **source** now uses `ProtectSystem=no` without a finite write-path
allowlist. This is a short-lived trusted root package transaction, not an
isolation boundary for untrusted packages. `ProtectHome`, `PrivateTmp`,
`NoNewPrivileges`, `ProtectControlGroups`, `LockPersonality` and restricted
address families remain. `ProtectKernelTunables=no` permits normal sysctl,
binfmt and EFI hook behavior. Device, kernel-module and SUID metadata
restrictions are not added. The installed production unit is not modified.
See [WORKER_ARCHITECTURE.md](WORKER_ARCHITECTURE.md) for snapshot policy,
execution evidence, replacement consent and the outstanding VM gates.

## Snapshots and recovery

Before commit, the service requires `/.snapshots` to be on a writable Btrfs
mount and then requires Snapper to create a root pre-snapshot. If either check
fails, the package transaction is not committed. On completion it attempts a
paired post-snapshot.

There is no automatic rollback. A failed libalpm commit may have changed some
files or packages; the service reports the pre/post snapshot results but does
not select or restore a snapshot. Snapshots do not by themselves prove that the
machine can boot from or restore them. Before production use, validate snapshot
creation and restoration in a disposable Btrfs system, confirm the bootloader
can select the intended snapshot, and verify whether `/boot`, EFI, and other
separate subvolumes are included. Keep a separate recovery path for cases where
the installed system cannot boot.

### Controlled snapshot and recovery validation

The current host must not be used to prove snapshot creation or restore: the
service must see `/.snapshots` on writable Btrfs, and its mount namespace may
differ from an interactive shell. Use a disposable Btrfs VM with the same
Snapper root configuration and packaged systemd unit. First compare
`systemctl cat zethropol-update.service` with the package unit and resolve any
local `/etc/systemd/system/zethropol-update.service` override only after
reviewing its complete diff. In the VM, verify `findmnt -T /.snapshots` from a
service with the packaged sandbox, then create uniquely named pre/post test
snapshots using the same root Snapper configuration and verify both with
`snapper -c root list`. Do not run `TriggerUpdate` for this check.

For recovery, use a disposable VM disk: record the Btrfs subvolume layout,
bootloader entries, `/boot` and EFI mount relationships; make a pre-snapshot;
apply a harmless controlled file change only inside the test root subvolume;
restore using the distribution's supported Snapper/root-subvolume procedure;
then reboot the VM from the intended boot entry and verify the restored file,
initramfs, kernel, bootloader and EFI state. Separately test the documented
recovery path when the installed root does not boot. A successful snapshot
creation alone is not evidence that restore or boot recovery works.

### Local systemd unit override transition

The package installs its vendor unit under `/usr/lib/systemd/system`; it does
not own or replace `/etc/systemd/system/zethropol-update.service`. A local file
there shadows the vendor unit and remains in force after package installation.
The install hook only warns. Before enabling transactions, an administrator
must inspect `systemctl cat zethropol-update.service` and compare the local
file with the packaged unit. Preserve a copy and its provenance, then remove or
revise the override only through an explicitly authorized maintenance change;
reload systemd and verify the effective properties afterward. Never remove or
overwrite it automatically during package installation.
