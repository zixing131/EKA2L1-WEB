#!/usr/bin/env bash
# ============================================================================
# EKA2L1 — HarmonyOS (HarmonyOS NEXT) NATIVE build.
#
# This builds the *native* HarmonyOS app (ArkTS/ArkUI front-end + the EKA2L1
# C++ emulator core cross-compiled with the OpenHarmony NDK and packaged as a
# NAPI module), NOT the WebView-wrapped WASM build (that is build_hos.sh).
#
# Project:  src/emu/hos  (DevEco Studio Stage-model project)
#   - ArkUI UI lives in   src/emu/hos/entry/src/main/ets   (mirrors the Android UI)
#   - NAPI bridge in      src/emu/hos/entry/src/main/cpp/napi_init.cpp
#   - C++ frontend in     src/emu/hos/native               (mirrors src/emu/android)
#   - The entry CMake adds the EKA2L1 repo root, so the whole core builds with
#     the OHOS toolchain (the root detects CMAKE_SYSTEM_NAME=OHOS -> EKA2L1_OHOS).
#
# Output: src/emu/hos/entry/build/default/outputs/default/entry-default-*.hap
#
# Usage:
#   ./build_hos_native.sh                 # ohpm install + hvigorw assembleHap (debug)
#   ./build_hos_native.sh release         # release build mode
#   ./build_hos_native.sh --configure     # configure the native CMake only (fast
#                                         # iteration on C++ compile errors; no HAP)
#   ./build_hos_native.sh --native        # configure + compile native libs only
#   ./build_hos_native.sh clean           # remove generated build dirs
#
# Environment overrides:
#   DEVECO_HOME   DevEco-Studio.app/Contents dir (auto-detected on macOS)
#   OHOS_SDK      OpenHarmony native SDK dir (.../sdk/default/openharmony/native)
#   ABI           Native ABI for --configure/--native (default: arm64-v8a)
#   JOBS          Parallel build jobs (default: number of CPUs)
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOS_PROJECT="$ROOT/src/emu/hos"
ENTRY_CPP="$HOS_PROJECT/entry/src/main/cpp"
ABI="${ABI:-arm64-v8a}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

log()  { printf '\033[1;36m[hos-native]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[hos-native]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[hos-native] error:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Locate DevEco Studio toolchain (macOS app bundle by default).
# ---------------------------------------------------------------------------
detect_deveco() {
    if [ -n "${DEVECO_HOME:-}" ] && [ -d "$DEVECO_HOME" ]; then
        return
    fi
    local candidates=(
        "/Applications/DevEco-Studio.app/Contents"
        "$HOME/Applications/DevEco-Studio.app/Contents"
    )
    for c in "${candidates[@]}"; do
        if [ -d "$c" ]; then
            DEVECO_HOME="$c"
            return
        fi
    done
    die "DevEco Studio not found. Set DEVECO_HOME to .../DevEco-Studio.app/Contents"
}

# ---------------------------------------------------------------------------
# Native-only configure/compile (fast C++ iteration without the full HAP build).
# Mirrors the exact CMake invocation DevEco/hvigor uses for the entry module.
# ---------------------------------------------------------------------------
configure_native() {
    detect_deveco

    local sdk_root="${OHOS_SDK:-$DEVECO_HOME/sdk/default/openharmony/native}"
    local hms_native="$DEVECO_HOME/sdk/default/hms/native"
    local cmake_bin="$sdk_root/build-tools/cmake/bin/cmake"
    local ninja_bin="$sdk_root/build-tools/cmake/bin/ninja"
    local toolchain="$hms_native/build/cmake/hmos.toolchain.bisheng.cmake"

    [ -x "$cmake_bin" ]   || die "OHOS cmake not found at $cmake_bin (check DEVECO_HOME / OHOS_SDK)"
    [ -f "$toolchain" ]   || toolchain="$sdk_root/build/cmake/ohos.toolchain.cmake"
    [ -f "$toolchain" ]   || die "OHOS cmake toolchain not found under $hms_native or $sdk_root"

    local build_dir="$ENTRY_CPP/.cxx-native/$ABI"
    mkdir -p "$build_dir"

    log "Configuring native ($ABI) with OHOS toolchain..."
    log "  SDK:       $sdk_root"
    log "  toolchain: $toolchain"

    "$cmake_bin" \
        -H"$ENTRY_CPP" \
        -B"$build_dir" \
        -GNinja \
        -DOHOS_ARCH="$ABI" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_SYSTEM_NAME=OHOS \
        -DCMAKE_OHOS_ARCH_ABI="$ABI" \
        -DOHOS_SDK_NATIVE="$sdk_root" \
        -DHMOS_SDK_NATIVE="$hms_native" \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_MAKE_PROGRAM="$ninja_bin" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        --no-warn-unused-cli

    echo "$build_dir"   # last line = build dir for the caller
}

compile_native() {
    local build_dir
    build_dir="$(configure_native | tail -n 1)"

    detect_deveco
    local sdk_root="${OHOS_SDK:-$DEVECO_HOME/sdk/default/openharmony/native}"
    local cmake_bin="$sdk_root/build-tools/cmake/bin/cmake"

    log "Compiling native ($ABI), target: entry, jobs: $JOBS..."
    "$cmake_bin" --build "$build_dir" --target entry -j"$JOBS"
    log "Native build complete -> $build_dir"
}

# ---------------------------------------------------------------------------
# Stage the emulator's runtime assets (GLES shaders, default sound banks,
# Symbian DLL patches, compat DB) into entry/src/main/resources/rawfile/assets
# so the HAP bundles them; the app copies them into its sandbox on first run.
# The patches come from a native build (bin/patch); the rest live in the tree.
# ---------------------------------------------------------------------------
stage_assets() {
    local rawfile="$HOS_PROJECT/entry/src/main/resources/rawfile/assets"
    log "Staging runtime assets into rawfile/assets ..."

    mkdir -p "$rawfile/resources/upscale" "$rawfile/compat" "$rawfile/patch" "$rawfile/scripts"

    # GLES shaders + default sound banks
    cp -f "$ROOT/src/emu/drivers/resources/gles/"* "$rawfile/resources/" 2>/dev/null || true
    cp -fR "$ROOT/src/emu/drivers/resources/upscale/." "$rawfile/resources/upscale/" 2>/dev/null || true
    cp -f "$ROOT/src/emu/drivers/resources/defaultbank.hsb" "$rawfile/resources/" 2>/dev/null || true
    cp -f "$ROOT/src/emu/drivers/resources/defaultbank.sf2" "$rawfile/resources/" 2>/dev/null || true

    # Compatibility database
    cp -f "$ROOT/miscs/compat/"*.yml "$rawfile/compat/" 2>/dev/null || true

    # Symbian DLL patches: prefer freshly built ones, else any previously built.
    # The patches are prebuilt .dll binaries copied to <build>/bin/patch by the
    # add_symbian_patch CMake step. Search the script's own native build dir
    # first, then DevEco's hvigor build dirs.
    local patch_src=""
    for cand in \
        "$ENTRY_CPP/.cxx-native/$ABI/bin/patch" \
        "$(find "$ENTRY_CPP/.cxx-native" -type d -name patch 2>/dev/null | head -n 1)" \
        "$(find "$HOS_PROJECT/entry/.cxx" -type d -name patch 2>/dev/null | head -n 1)"; do
        if [ -n "$cand" ] && [ -d "$cand" ]; then
            patch_src="$cand"
            break
        fi
    done
    if [ -n "$patch_src" ]; then
        cp -f "$patch_src/"* "$rawfile/patch/" 2>/dev/null || true
        log "  patches from $patch_src"
    else
        warn "No built Symbian patches found. Run './build_hos_native.sh --native' once"
        warn "before the HAP build so patch/*.dll get staged (avkonfep, goommonitor, ...)."
    fi

    log "  assets staged under $rawfile"
}

# ---------------------------------------------------------------------------
# Full HAP build via hvigorw (DevEco drives the native CMake internally).
# ---------------------------------------------------------------------------
build_hap() {
    detect_deveco

    stage_assets

    local node_bin="$DEVECO_HOME/tools/node/bin"
    local hvigorw="$DEVECO_HOME/tools/hvigor/bin/hvigorw.js"
    local ohpm="$DEVECO_HOME/tools/ohpm/bin/ohpm"
    local mode="${1:-debug}"

    [ -x "$node_bin/node" ] || die "DevEco node not found at $node_bin/node"
    [ -f "$hvigorw" ]       || die "hvigorw not found at $hvigorw"

    export PATH="$node_bin:$DEVECO_HOME/tools/ohpm/bin:$PATH"
    export DEVECO_SDK_HOME="${DEVECO_SDK_HOME:-$DEVECO_HOME/sdk}"

    cd "$HOS_PROJECT"

    # local.properties must point at the SDK for headless builds.
    if ! grep -q "sdk.dir" local.properties 2>/dev/null; then
        log "Writing sdk.dir to local.properties"
        printf 'sdk.dir=%s\n' "$DEVECO_HOME/sdk" >> local.properties
    fi

    log "Installing ohpm dependencies..."
    if [ -x "$ohpm" ]; then
        "$ohpm" install --all || warn "ohpm install reported issues; continuing"
    fi

    local target="assembleHap"
    log "Running hvigorw $target (mode=$mode, jobs=$JOBS)..."
    "$node_bin/node" "$hvigorw" \
        --mode module \
        -p product=default \
        -p buildMode="$mode" \
        "$target" \
        --no-daemon --analyze=normal

    log "HAP build complete. Look under:"
    log "  $HOS_PROJECT/entry/build/default/outputs/default/*.hap"
}

clean() {
    log "Cleaning native + HAP build dirs..."
    rm -rf "$ENTRY_CPP/.cxx-native" \
           "$HOS_PROJECT/entry/.cxx" \
           "$HOS_PROJECT/entry/build" \
           "$HOS_PROJECT/.hvigor" \
           "$HOS_PROJECT/build"
    log "Done."
}

case "${1:-}" in
    --configure)  configure_native >/dev/null; log "Configure done." ;;
    --native)     compile_native ;;
    clean)        clean ;;
    release)      build_hap release ;;
    ""|debug)     build_hap debug ;;
    *)            die "Unknown argument: $1 (use: <empty>|release|--configure|--native|clean)" ;;
esac
