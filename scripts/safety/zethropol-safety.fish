#!/usr/bin/env fish

# Zethropol Safety Layer
# System update safety, snapshot and recovery operations.

set command $argv[1]

if test "$command" = status
    echo "=== Zethropol Safety Status ==="
    echo ""
    echo "Snapper:"
    sudo snapper -c root get-config | grep -E "FSTYPE|SUBVOLUME|NUMBER_CLEANUP|NUMBER_LIMIT|NUMBER_LIMIT_IMPORTANT"
    echo ""
    echo "Latest snapshots:"
    sudo snapper -c root list | tail -n 8
    echo ""
    echo "Limine snapshot integration:"
    limine-snapper-info | grep -E "Last snapshot|Snapshot entries|Boot usage|Snapshot files|Unused files|Missing files|Corrupted files"
    exit 0
end

if test "$command" = snapshot
    echo "Creating Zethropol safety snapshot..."
    set snapshot_id (sudo snapper -c root create --type single --cleanup-algorithm number --description "Zethropol Safety Snapshot" --userdata "zethropol=safety" --print-number)
    if test $status -eq 0
        echo "Safety snapshot created: #$snapshot_id"
        exit 0
    end
    echo "ERROR: Safety snapshot creation failed."
    exit 1
end

if test "$command" = rollback
    set snapshot_id $argv[2]

    if test -z "$snapshot_id"
        echo "Available snapshots:"
        sudo snapper -c root list
        echo ""
        echo "Usage: zethropol-safety.fish rollback <snapshot-id>"
        exit 1
    end

    if not sudo snapper -c root list | grep -qE "^ *$snapshot_id "
        echo "ERROR: Snapshot #$snapshot_id was not found."
        exit 1
    end

    set cmdline (string join " " < /proc/cmdline)

    if string match -q "*rootflags=subvol=*/.snapshots/$snapshot_id/snapshot*" -- $cmdline
        echo "Snapshot #$snapshot_id is currently booted."
        echo "Starting Zethropol restore..."
        sudo limine-snapper-restore
        exit $status
    end

    echo "Rollback target: #$snapshot_id"
    echo ""
    echo "The target snapshot is not currently booted."
    echo "Reboot and select snapshot #$snapshot_id from the Limine menu."
    echo "Then run:"
    echo "  zethropol-safety.fish rollback $snapshot_id"
    exit 0
end

echo "Usage: zethropol-safety.fish status"
echo "       zethropol-safety.fish snapshot"
echo "       zethropol-safety.fish rollback <snapshot-id>"
