#!/usr/bin/env bash
# ============================================================================
# EKA2L1-WEB Test build.
#
# Output: build_wasm_test/  (debug web build)
# Defines EKA2L1_DEBUG_BUILD -> all copyright / integrity / domain protection
# is compiled out. localhost works, tampered files still run, any domain runs.
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build_wasm_test"
JOBS="${JOBS:-4}"

# Make emcmake / emcc available (sourced from emsdk if not already on PATH).
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
    -DEKA2L1_DEBUG_BUILD=ON \
    -DEKA2L1_BUILD_TESTS=OFF \
    -DEKA2L1_BUILD_TOOLS=OFF \
    -DEKA2L1_BUILD_PATCH=OFF \
    -DEKA2L1_ENABLE_SCRIPTING_ABILITY=OFF

cmake --build "$BUILD_DIR" --target eka2l1_wasm -j"$JOBS"

echo
echo "Test build complete -> $BUILD_DIR/bin"
echo "Serve: python3 $ROOT/src/emu/web/serve.py 8080 $BUILD_DIR/bin"
