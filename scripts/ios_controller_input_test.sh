#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_DIR="$(mktemp -d /tmp/eka2l1-controller-tests.XXXXXX)"
trap 'rm -rf "$TEST_DIR"' EXIT

# The UIKit mapping editor and this macOS harness share the same host-input enum.
python3 - "$ROOT_DIR" "$TEST_DIR" <<'PY'
from pathlib import Path
import sys
root, out = map(Path, sys.argv[1:])
source = (root / 'src/emu/ios/App/ControllerMapping.swift').read_text()
enum = source[source.index('enum HostButton:'):source.index('// Hardware-keyboard host inputs')]
(out / 'HostButton.swift').write_text('import Foundation\nimport GameController\n' + enum)
PY

swiftc -swift-version 6 -O \
    "$TEST_DIR/HostButton.swift" \
    "$ROOT_DIR/src/emu/ios/App/ControllerPointer.swift" \
    "$ROOT_DIR/src/emu/ios/Tests/ControllerPointerTests.swift" \
    -o "$TEST_DIR/controller-pointer-tests"
"$TEST_DIR/controller-pointer-tests"
