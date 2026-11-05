# Update cache cleanup

The worker considers cleanup only after commit, successful lock release and
post-transaction recovery/evidence checkpoints. `cache_cleanup::eligible` also
requires verified commit, hooks, boot preparation and boot artifacts, a verified
new-kernel boot, and recorded pre/post snapshot IDs. Failed or uncertain updates
never open a cleanup ALPM handle. Existing execution paths leave hooks/new boot
unverified, so these paths report `skipped-unverified`; this change does not
invent evidence or relax that gate. No post-reboot retry is scheduled here.

An eligible cleanup creates a fresh ALPM handle to read installed versions and
takes the normal pacman transaction lock. It never deletes `db.lck`. Only the
configured `/var/cache/pacman/pkg` directory qualifies; unsafe/custom paths are
retained. Current-version archives and the highest lower ALPM version of each
installed package/architecture are retained. Future versions, packages lacking
current/previous coverage, uninstalled packages, kernel/firmware/recovery/system
critical package archives, and unknown/partial downloads are retained.

Only regular files with the expected owner, safe mode and one hardlink qualify.
Directory descriptors pin paths; file identities are rechecked. A Linux write
lease must be held for an archive and its optional signature before unlinking.
Open files and unsupported/denied leases are retained with warnings; pending
lease breaks are refused. No system scripts, hooks or package transactions are
executed to inspect old archives. Production signature/content verification for
installation is unchanged.

Worker downloads in the shared pacman cache are not treated as operation-owned
temporary files. In particular, `.part` files are never swept. Private preview
metadata already has a UUID-scoped `TemporaryPlanDatabase` RAII owner; its
existing Drop removes only that private directory, including on preview errors.
No global `/tmp`, snapshot or recovery cleanup is added.

Broker record format 6 persists cleanup status, removed file count (archives
plus signatures), removed logical bytes, observed filesystem available-space
increase and bounded warning details. Formats 1–5 remain readable with cleanup
`not-run`; missing results are never fabricated. D-Bus exposes these as string
keys in the existing `GetOperationState` map. Cleanup/persistence errors remain
separate warnings and do not reverse confirmed package commit evidence. Free
space is an observation: CoW and asynchronous allocation can delay reclamation,
and unrelated filesystem activity can affect the reading.

All deletion tests use fresh `zethropol-cleanup-test-*` directories under the
system temporary directory. No host cleanup, service deployment or package
upgrade is part of this development change.
