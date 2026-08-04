#!/usr/bin/env bash
# Rebuild the ABI-preserving Belle SCDV binary patch used by EKA2L1.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYMBIAN_ROOT="${SYMBIAN_ROOT:-$HOME/Developer/symbian}"
KIT_ROOT="${SYMBIAN_DLL_KIT_ROOT:-$SYMBIAN_ROOT/symbian-dll-agent-kit}"
ELF2E32_SRC="${ELF2E32_SRC:-$KIT_ROOT/.cache/elf2e32_next}"
BASE_BLOB="${SCDV_BASE_BLOB:-ec2ffa212c3cc7e0a426adacb656e1b962db3e6a}"
BASE_SHA256="6ead71eb0538bf519a4a088a2efa4ccec711ed53acc5dad42d105f1b4418b4ee"
OUTPUT="${1:-/tmp/scdv_general_belle.dll}"
BUILD_DIR="$(mktemp -d /tmp/eka2l1-scdv.XXXXXX)"
trap 'rm -rf "$BUILD_DIR"' EXIT

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Missing required tool: $1" >&2
        exit 2
    }
}

need arm-none-eabi-gcc
need arm-none-eabi-ld
need arm-none-eabi-objcopy
need clang++
need git
need python3

for required in \
    "$ELF2E32_SRC/lib/e32/E32Pack_processor.cpp" \
    "$ELF2E32_SRC/lib/e32/e32compressor.h" \
    "$KIT_ROOT/tools/verify_e32.py"; do
    [ -f "$required" ] || {
        echo "Missing Symbian DLL kit file: $required" >&2
        exit 2
    }
done

if [ -n "${SCDV_BASE_DLL:-}" ]; then
    cp "$SCDV_BASE_DLL" "$BUILD_DIR/base.dll"
else
    git -C "$ROOT" cat-file blob "$BASE_BLOB" >"$BUILD_DIR/base.dll" \
        || { echo "Base SCDV blob is unavailable; set SCDV_BASE_DLL" >&2; exit 2; }
fi

actual_base_sha="$(shasum -a 256 "$BUILD_DIR/base.dll" | awk '{print $1}')"
[ "$actual_base_sha" = "$BASE_SHA256" ] || {
    echo "Unexpected base SCDV SHA-256: $actual_base_sha" >&2
    exit 2
}

arm-none-eabi-gcc -march=armv5t -mthumb \
    -c "$ROOT/src/patch/scdv/surface_stub.S" -o "$BUILD_DIR/surface_stub.o"
arm-none-eabi-ld -Ttext=0 --entry=patched_get_interface --oformat=elf32-littlearm \
    -o "$BUILD_DIR/surface_stub.elf" "$BUILD_DIR/surface_stub.o"
arm-none-eabi-objcopy -O binary --only-section=.text \
    "$BUILD_DIR/surface_stub.elf" "$BUILD_DIR/surface_stub.bin"

clang++ -std=c++14 -D__EABI__ \
    -I"$ELF2E32_SRC/include" -I"$ELF2E32_SRC/src" -I"$ELF2E32_SRC/lib/e32" \
    "$ROOT/scripts/tools/patch_scdv_e32.cpp" \
    "$ELF2E32_SRC/lib/e32/E32Pack_processor.cpp" \
    "$ELF2E32_SRC/lib/e32/bpe_manager.cpp" \
    "$ELF2E32_SRC/lib/e32/byte_pair.cpp" \
    "$ELF2E32_SRC/lib/e32/checksum.cpp" \
    "$ELF2E32_SRC/lib/e32/deflatecompress.cpp" \
    "$ELF2E32_SRC/lib/e32/huffman.cpp" \
    "$ELF2E32_SRC/lib/e32/inflate.cpp" \
    "$ELF2E32_SRC/src/logger.cpp" \
    "$ELF2E32_SRC/src/common.cpp" \
    -o "$BUILD_DIR/patch_scdv_e32"

mkdir -p "$(dirname "$OUTPUT")"
"$BUILD_DIR/patch_scdv_e32" "$BUILD_DIR/base.dll" \
    "$BUILD_DIR/surface_stub.bin" "$OUTPUT"

python3 "$KIT_ROOT/tools/verify_e32.py" "$OUTPUT" \
    --uid2 0x10003B19 --uid3 0xEE000002
echo "SCDV_BELLE_SHA256=$(shasum -a 256 "$OUTPUT" | awk '{print $1}')"
echo "SCDV_BELLE_DLL=$OUTPUT"
