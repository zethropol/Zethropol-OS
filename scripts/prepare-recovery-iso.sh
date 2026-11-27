#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PROFILE_DIR="${REPO_ROOT}/build/recovery"
LOCAL_REPO_DIR="${REPO_ROOT}/packages/repo/x86_64"
STAGE_DIR="/tmp/zethropol-recovery-iso"
SKIP_REPO_BUILD=0

usage() {
    echo "Usage: $0 [--stage-dir <dir>] [--skip-repo-build]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --stage-dir)
            STAGE_DIR="$2"
            shift 2
            ;;
        --skip-repo-build)
            SKIP_REPO_BUILD=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

for cmd in mkarchiso xorriso mksquashfs mformat mkfs.fat pacman pacman-conf fish; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "ERROR: Required tool missing: $cmd" >&2
        exit 1
    fi
done

if [[ "$SKIP_REPO_BUILD" -eq 0 ]]; then
    bash "${REPO_ROOT}/scripts/build-local-repo.sh"
fi

if [[ ! -f "${LOCAL_REPO_DIR}/zethropol-local.db.tar.gz" ]]; then
    echo "ERROR: Local repository database missing: ${LOCAL_REPO_DIR}/zethropol-local.db.tar.gz" >&2
    exit 1
fi

echo "==> Validating ArchISO profile syntax under ${PROFILE_DIR}"
bash -n "${PROFILE_DIR}/profiledef.sh"
fish --no-execute "${PROFILE_DIR}/airootfs/usr/local/bin/zethropol-recovery"

# Verify zero hardcoded developer paths in build/recovery/
if grep -rn '/home/' "${PROFILE_DIR}" >/dev/null 2>&1; then
    echo "ERROR: Hardcoded /home/ path found in ${PROFILE_DIR}!" >&2
    grep -rn '/home/' "${PROFILE_DIR}" >&2
    exit 1
fi

# Verify mandatory recovery packages are declared
for required in snapper fish limine limine-snapper-sync zethropol-recovery btrfs-progs; do
    if ! grep -qx "$required" "${PROFILE_DIR}/packages.x86_64"; then
        echo "ERROR: Required package '$required' missing from ${PROFILE_DIR}/packages.x86_64" >&2
        exit 1
    fi
done

mkdir -p "${STAGE_DIR}/out" "${STAGE_DIR}/repo"
chmod 755 "${STAGE_DIR}" "${STAGE_DIR}/out" "${STAGE_DIR}/repo"
rm -f "${STAGE_DIR}/repo"/*
cp -a "${LOCAL_REPO_DIR}"/. "${STAGE_DIR}/repo/"
chmod 644 "${STAGE_DIR}/repo"/*.pkg.tar.zst "${STAGE_DIR}/repo"/*.tar.gz "${STAGE_DIR}/repo/SHA256SUMS"
RUNTIME_PACMAN_CONF="${STAGE_DIR}/pacman.conf"

# Inject [zethropol-local] ahead of [core] pointing to world-readable staged repo
# so pacman/alpm works regardless of $HOME directory permissions.
awk -v repo_dir="${STAGE_DIR}/repo" '
    /^\[core\]/ && !inserted {
        print "[zethropol-local]"
        print "SigLevel = Optional TrustAll"
        print "Server = file://" repo_dir
        print ""
        inserted = 1
    }
    { print }
' "${PROFILE_DIR}/pacman.conf" > "$RUNTIME_PACMAN_CONF"
chmod 644 "$RUNTIME_PACMAN_CONF"

pacman-conf --config "$RUNTIME_PACMAN_CONF" >/dev/null
pacman-conf --config "$RUNTIME_PACMAN_CONF" --repo=zethropol-local >/dev/null

# Verify every package in packages.x86_64 resolves in host sync DB or [zethropol-local]
TMP_DB="$(mktemp -d /tmp/zethropol-iso-pkgcheck.XXXXXX)"
trap 'rm -rf "$TMP_DB"' EXIT
mkdir -p "${TMP_DB}/sync"
cp -f /var/lib/pacman/sync/*.db "${TMP_DB}/sync/" 2>/dev/null || true
cp -f "${LOCAL_REPO_DIR}/zethropol-local.db.tar.gz" "${TMP_DB}/sync/zethropol-local.db"

missing_pkgs=()
while read -r pkg; do
    if [[ -z "$pkg" || "${pkg:0:1}" == "#" ]]; then
        continue
    fi
    if ! pacman --config "$RUNTIME_PACMAN_CONF" --dbpath "$TMP_DB" -Si "$pkg" >/dev/null 2>&1; then
        missing_pkgs+=("$pkg")
    fi
done < "${PROFILE_DIR}/packages.x86_64"

if [[ ${#missing_pkgs[@]} -gt 0 ]]; then
    echo "ERROR: Unresolved packages in build/recovery/packages.x86_64: ${missing_pkgs[*]}" >&2
    exit 1
fi

rec_ver="$(pacman --config "$RUNTIME_PACMAN_CONF" --dbpath "$TMP_DB" -Si zethropol-local/zethropol-recovery | awk -F': ' '/^(Version|Sürüm)/ {print $2; exit}')"
echo "==> Profile validation PASS (zethropol-recovery resolved as: ${rec_ver})"
echo "==> Runtime pacman.conf ready: ${RUNTIME_PACMAN_CONF}"
echo ""
echo "Single root command (Fish-compatible) to build the ISO:"
echo "  sudo mkarchiso -v -C ${RUNTIME_PACMAN_CONF} -w ${STAGE_DIR}/work -o ${STAGE_DIR}/out ${PROFILE_DIR}"
