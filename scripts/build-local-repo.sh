#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
MANIFEST="${REPO_ROOT}/packages/repo/manifest.txt"
OUT_DIR="${REPO_ROOT}/packages/repo/x86_64"
REPO_NAME="zethropol-local"
VERIFY_PACMAN=1

usage() {
    echo "Usage: $0 [--out <dir>] [--manifest <file>] [--no-pacman-verify]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --out)
            OUT_DIR="$2"
            shift 2
            ;;
        --manifest)
            MANIFEST="$2"
            shift 2
            ;;
        --no-pacman-verify)
            VERIFY_PACMAN=0
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

if [[ ! -f "$MANIFEST" ]]; then
    echo "ERROR: Manifest not found: $MANIFEST" >&2
    exit 1
fi

for cmd in repo-add sha256sum pacman tar; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "ERROR: Required tool missing: $cmd" >&2
        exit 1
    fi
done

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd -- "$OUT_DIR" && pwd)"

# Remove stale generated repo files/packages before staging manifest entries
rm -f "${OUT_DIR}"/*.pkg.tar.zst \
      "${OUT_DIR}/${REPO_NAME}".db* \
      "${OUT_DIR}/${REPO_NAME}".files* \
      "${OUT_DIR}/SHA256SUMS"

declare -a STAGED_PKGS=()
while read -r pkg_name rel_src _extra; do
    if [[ -z "${pkg_name:-}" || "${pkg_name:0:1}" == "#" ]]; then
        continue
    fi
    if [[ -z "${rel_src:-}" ]]; then
        echo "ERROR: Invalid manifest entry for '$pkg_name'" >&2
        exit 1
    fi
    src_path="${REPO_ROOT}/${rel_src}"
    if [[ ! -f "$src_path" ]]; then
        echo "ERROR: Missing package artifact: $src_path" >&2
        exit 1
    fi
    # Basic integrity check via pacman metadata reader
    pacman -Qip "$src_path" >/dev/null
    dst_path="${OUT_DIR}/${pkg_name}"
    cp -f "$src_path" "$dst_path"
    STAGED_PKGS+=("$dst_path")
done < "$MANIFEST"

if [[ ${#STAGED_PKGS[@]} -eq 0 ]]; then
    echo "ERROR: Manifest produced 0 packages." >&2
    exit 1
fi

echo "==> Indexing ${#STAGED_PKGS[@]} packages into ${OUT_DIR}/${REPO_NAME}.db.tar.gz"
LC_ALL=C repo-add --new --remove "${OUT_DIR}/${REPO_NAME}.db.tar.gz" "${STAGED_PKGS[@]}"

# Generate deterministic SHA256SUMS for packages + repository databases
(
    cd "$OUT_DIR"
    LC_ALL=C sha256sum \
        *.pkg.tar.zst \
        "${REPO_NAME}.db.tar.gz" \
        "${REPO_NAME}.files.tar.gz" > SHA256SUMS
    sha256sum -c SHA256SUMS
)

if [[ "$VERIFY_PACMAN" -eq 1 ]]; then
    TMP_VERIFY="$(mktemp -d /tmp/zethropol-repo-verify.XXXXXX)"
    trap 'rm -rf "$TMP_VERIFY"' EXIT
    mkdir -p "${TMP_VERIFY}/db/sync" "${TMP_VERIFY}/cache"
    cp -f "${OUT_DIR}/${REPO_NAME}.db.tar.gz" "${TMP_VERIFY}/db/sync/${REPO_NAME}.db"
    {
        echo "[options]"
        echo "Architecture = x86_64"
        echo "DBPath = ${TMP_VERIFY}/db"
        echo "CacheDir = ${TMP_VERIFY}/cache"
        echo "LogFile = ${TMP_VERIFY}/pacman.log"
        echo ""
        echo "[${REPO_NAME}]"
        echo "SigLevel = Optional TrustAll"
        echo "Server = file://${OUT_DIR}"
    } > "${TMP_VERIFY}/pacman.conf"

    echo "==> Verifying isolated rootless libalpm/pacman query against [${REPO_NAME}]"
    pacman-conf --config "${TMP_VERIFY}/pacman.conf" >/dev/null
    listed_output="$(pacman --config "${TMP_VERIFY}/pacman.conf" --dbpath "${TMP_VERIFY}/db" -Sl "${REPO_NAME}")"
    listed_count="$(printf '%s\n' "$listed_output" | grep -c .)"
    if [[ "$listed_count" -ne "${#STAGED_PKGS[@]}" ]]; then
        echo "ERROR: Expected ${#STAGED_PKGS[@]} indexed packages, got ${listed_count}" >&2
        exit 1
    fi
    printf '%s\n' "$listed_output"
    pacman --config "${TMP_VERIFY}/pacman.conf" --dbpath "${TMP_VERIFY}/db" -Si "${REPO_NAME}/zethropol-recovery" "${REPO_NAME}/zethropol-control-center" >/dev/null
fi

echo "==> Local repository ready: ${OUT_DIR} (${#STAGED_PKGS[@]} packages)"
