#!/usr/bin/env bash
# ============================================================================
# EKA2L1-WEB HarmonyOS (HOS) build.
#
# Output: build_wasm_hos/  (full protection, local-origin allowed)
# Like the Release build but with EKA2L1_HOS_BUILD=ON:
#   - Copyright notice, file-integrity checks and version watermark are active.
#   - Domain whitelist is relaxed to allow empty / local hosts so the wasm can
#     run inside a HarmonyOS WebView (file://, resource://, custom scheme).
#   - Channel watermark shows "HOS" instead of "Release".
# The POST_BUILD step (gen_integrity.py) still seals the asset hashes.
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT/build_wasm_hos"
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
    -DEKA2L1_HOS_BUILD=ON \
    -DEKA2L1_BUILD_TESTS=OFF \
    -DEKA2L1_BUILD_TOOLS=OFF \
    -DEKA2L1_BUILD_PATCH=OFF \
    -DEKA2L1_ENABLE_SCRIPTING_ABILITY=OFF

cmake --build "$BUILD_DIR" --target eka2l1_wasm -j"$JOBS"

echo
echo "HOS build complete -> $BUILD_DIR/bin"
echo "Integrity sealed (gen_integrity.py); local origins (file://, resource://) are allowed."
