# Zethropol Linux Feature Map

## Feature 01 — Safe Update / Rollback / Recovery

### User Problem

> Güncelleme yaptım, sistem bozuldu. Şimdi ne olacak?

### Existing Linux Solutions

- openSUSE: Btrfs + Snapper
- Bazzite: OSTree deployments + rollback
- Vanilla OS: A/B root + atomic updates
- Pop!_OS: Recovery Partition
- CachyOS: Btrfs + Snapper + snap-pac + Limine integration

### Zethropol Decision

Zethropol will retain the mutable CachyOS/Arch architecture and use the existing Btrfs/Snapper infrastructure as the underlying safety mechanism.

The project will not adopt OSTree, A/B root partitions, or an immutable-root architecture solely for rollback functionality.

### Selected Foundation

- Btrfs
- Snapper
- snap-pac
- Limine
- limine-snapper-sync

### Zethropol Safety Layer

Zethropol will provide a higher-level safety layer above the existing components.

Concept:

Update → Safety Snapshot → Update → System Validation → PASS → Continue

FAIL → Rollback → Still Broken? → Recovery

### Safety Levels

#### 1. Prevention

Create a system snapshot before significant package transactions.

#### 2. Rollback

Allow the user to return the system root to a known previous state.

System rollback must not roll back user data stored in separate subvolumes such as @home.

#### 3. Recovery

Provide a recovery path when the normal installed system cannot boot or cannot provide a usable session.

### Current Infrastructure

pacman → snap-pac → Snapper pre snapshot → package transaction → Snapper post snapshot → Limine snapshot synchronization

### Zethropol-Specific Value

Zethropol should not duplicate Snapper or Limine functionality.

The differentiating layer is the integration and user experience:

- Detect important system changes
- Protect the system before risky operations
- Validate the resulting system
- Present recovery options without requiring Snapper knowledge
- Make rollback understandable to ordinary users
- Preserve user data independently from system rollback
- Provide a recovery path when normal boot is unavailable

### Important Constraints

- /boot is a separate VFAT filesystem and is not itself covered by Btrfs snapshots.
- Kernel and initramfs handling must therefore remain synchronized with snapshot rollback.
- limine-snapper-sync currently provides snapshot/kernel integration.
- Snapshots are not a replacement for independent backups.
- Zethropol should avoid architectural changes that break Arch/CachyOS package management or AUR compatibility.

### Implementation Status

🟢 Architectural foundation selected
🟡 Zethropol Safety Layer not implemented
⚪ GUI not implemented

### Decision

**KEEP**

Zethropol will build its update safety and recovery functionality on top of:

**Btrfs + Snapper + snap-pac + Limine + limine-snapper-sync**

rather than replacing the underlying operating-system architecture.

## Recovery

### User Problem

> Sistem normal şekilde boot edemiyor ve snapshot üzerinden doğrudan rollback mümkün değil.

### Existing Linux Foundations

- Arch Linux recovery environment
- arch-chroot
- Btrfs tools
- Snapper
- Limine / limine-snapper-sync

### Zethropol Decision

Zethropol will use an external recovery environment based on the Arch Linux recovery toolchain rather than creating a custom recovery operating system or duplicating existing filesystem tools.

The recovery layer will provide a Zethropol-oriented workflow over arch-chroot, Btrfs, Snapper and Limine.

### Recovery Levels

1. Snapshot boot: boot a known-good Snapper snapshot when the normal root is still bootable.
2. Snapshot restore: restore a selected snapshot to the normal root using limine-snapper-restore.
3. External recovery: boot a recovery environment when the normal system cannot boot.
4. Chroot repair: mount the installed system and use arch-chroot for package, configuration, kernel and bootloader repair.

### Important Constraint

Zethropol will not duplicate Btrfs Assistant, Snapper, Btrfs or arch-chroot functionality. Its value will be the recovery workflow, safety checks and Zethropol-specific user experience built around existing Linux tools.

### Decision

KEEP
