#!/usr/bin/env bash
#
# Build the `screentext` OCR helper used by the iOS regression suites, and print
# the path to the binary. Rebuilds only when the source is newer, so callers can
# invoke it unconditionally.

set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SRC_DIR/../.." && pwd)"
SRC="$SRC_DIR/ScreenText.swift"
OUT_DIR="${EKA2L1_SCREENTEXT_OUTDIR:-$REPO_ROOT/build/tools}"
OUT="$OUT_DIR/screentext"

if [ ! -x "$OUT" ] || [ "$SRC" -nt "$OUT" ]; then
    mkdir -p "$OUT_DIR"
    xcrun swiftc -O -target "$(uname -m)-apple-macosx14.0" -o "$OUT" "$SRC" >&2
fi

echo "$OUT"
