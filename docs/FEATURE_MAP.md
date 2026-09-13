
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
