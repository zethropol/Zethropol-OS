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

echo "Usage: zethropol-safety.fish status"
echo "       zethropol-safety.fish snapshot"
