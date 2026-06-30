#!/usr/bin/env bash
# ============================================================================
# EKA2L1-WEB Release build.
#
# Output: build_wasm_release/  (full protection)
# EKA2L1_DEBUG_BUILD is OFF: copyright notice, file-integrity checks, domain
# whitelist and the version watermark are all active. The POST_BUILD step
# (gen_integrity.py) seals the asset hashes into eka2l1.wasm and writes
# js/integrity.js.
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build_wasm_release"
JOBS="${JOBS:-4}"

if ! command -v emcmake >/dev/null 2>&1; then
    if [ -n "${EMSDK:-}" ] && [ -f "$EMSDK/emsdk_env.sh" ]; then
        # shellcheck disable=SC1091
        source "$EMSDK/emsdk_env.sh"
    elif [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
        # shellcheck disable=SC1091
        source "$HOME/emsdk/emsdk_env.sh"
    else
        echo "error: emcmake not found and emsdk_env.sh not located. Activate emsdk first." >&2
        exit 1
    fi
fi

emcmake cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DEKA2L1_DEBUG_BUILD=OFF \
    -DEKA2L1_BUILD_TESTS=OFF \
    -DEKA2L1_BUILD_TOOLS=OFF \
    -DEKA2L1_BUILD_PATCH=OFF \
    -DEKA2L1_ENABLE_SCRIPTING_ABILITY=OFF

cmake --build "$BUILD_DIR" --target eka2l1_wasm -j"$JOBS"

echo
echo "Release build complete -> $BUILD_DIR/bin"
echo "Integrity sealed (gen_integrity.py); serve from an authorized domain"
echo "(*.zixing.fun / *.iniche.cn) — localhost is rejected in Release."
