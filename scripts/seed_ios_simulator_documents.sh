#!/usr/bin/env bash
# Stage the booted iOS simulator's EKA2L1 sandbox with the user's ROM
# bundle. The actual <storage>/data/{drives,roms,devices.yml} layout the
# emulator runs against is built inside the app (see IosEmulator.mm) —
# that has to be done from inside the iOS sandbox because the macOS host
# APFS volume is case-insensitive while the iOS app data container is
# case-sensitive, and several emulator code paths expect both casings
# of the firmcode to resolve. This script just copies the source folders
# into Documents/roms (visible to the ROM picker) and any top-level .sis
# packages into Documents/sis (visible to the install picker).
#
# Usage:
#   scripts/seed_ios_simulator_documents.sh              # copy everything
#   scripts/seed_ios_simulator_documents.sh --dry-run    # show what would copy
#
# Env:
#   EKA2L1_IOS_BUNDLE_ID   default com.eka2l1.emulator
#   EKA2L1_IOS_SEED_SRC    default <repo>/roms

set -euo pipefail

BUNDLE_ID="${EKA2L1_IOS_BUNDLE_ID:-com.eka2l1.emulator}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_ROOT="${EKA2L1_IOS_SEED_SRC:-$REPO_ROOT/roms}"
DRY_RUN=0

for arg in "$@"; do
    case "$arg" in
        --dry-run|-n) DRY_RUN=1 ;;
        *) echo "Unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [[ ! -d "$SRC_ROOT" ]]; then
    echo "Source directory does not exist: $SRC_ROOT" >&2
    exit 1
fi

booted_count="$(xcrun simctl list devices booted | awk '/Booted/{n++} END{print n+0}')"
if [[ "$booted_count" -eq 0 ]]; then
    echo "No booted simulator. Boot one (open -a Simulator) and rerun." >&2
    exit 1
fi

if ! container="$(xcrun simctl get_app_container booted "$BUNDLE_ID" data 2>/dev/null)"; then
    echo "Could not locate EKA2L1 data container for $BUNDLE_ID." >&2
    echo "Install the app first: scripts/build_ios.sh smoke" >&2
    exit 1
fi

DOCS="$container/Documents"
echo "==> Simulator EKA2L1 sandbox: $DOCS"

run() {
    if [[ "$DRY_RUN" -eq 1 ]]; then
        echo "[dry-run] $*"
    else
        eval "$@"
    fi
}

run mkdir -p \"$DOCS/roms\" \"$DOCS/sis\"

shopt -s nullglob
for entry in "$SRC_ROOT"/*; do
    base="$(basename "$entry")"
    if [[ -d "$entry" ]]; then
        echo "    rom  $base"
        run rsync -a --delete \"$entry/\" \"$DOCS/roms/$base/\"
    else
        case "$base" in
            *.sis|*.sisx|*.SIS|*.SISX)
                echo "    sis  $base"
                run cp -f \"$entry\" \"$DOCS/sis/$base\"
                ;;
            *) ;;
        esac
    fi
done

echo "==> Done. Launch the app to let IosEmulator stage data/ from these bundles."
