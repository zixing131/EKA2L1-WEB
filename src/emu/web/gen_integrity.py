#!/usr/bin/env python3
# ============================================================================
# EKA2L1-WEB Release integrity sealing (post-build).
#
# Runs only for Release builds (build_release.sh / -DEKA2L1_DEBUG_BUILD=OFF).
# It freezes the expected file hashes into the already-linked wasm so the
# in-wasm tamper checks (protection.cpp) have real values to compare against:
#
#   1. SHA-256 each protected JS/HTML asset (post page-stamp) and overwrite the
#      g_integ table inside eka2l1.wasm (located by its magic marker).
#   2. SHA-256 the *patched* eka2l1.wasm and write js/integrity.js, which the
#      pages load so the wasm can self-verify (deferred, in boot.js).
#
# The asset order here MUST match the name_id constants in boot.js and the
# index passed to wasm_verify_asset() in protection.cpp.
#
# Usage: gen_integrity.py --stage <STAGE_DIR>
# ============================================================================

import argparse
import hashlib
import os
import struct
import sys

# (relative path under STAGE_DIR, short label stored in the table). Order = name_id.
ASSETS = [
    ("eka2l1.js", "eka2l1.js"),
    ("js/boot.js", "boot.js"),
    ("js/index.js", "index.js"),
    ("js/run.js", "run.js"),
    ("index.html", "index.html"),
    ("run.html", "run.html"),
]

MAGIC = b"EKA2L1INTEGTBL\x00"   # 15 bytes; magic[16] field, last byte also 0
TABLE_HEADER = 16 + 4 + 4       # magic[16] + version(u32) + count(u32)
ENTRY_SIZE = 24 + 32            # name[24] + sha256[32]
NAME_LEN = 24
MAX_ENTRIES = 8


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.digest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", required=True, help="staged web output dir (bin/)")
    args = ap.parse_args()

    stage = args.stage
    wasm_path = os.path.join(stage, "eka2l1.wasm")
    if not os.path.exists(wasm_path):
        sys.exit("gen_integrity: eka2l1.wasm not found in %s" % stage)

    # ---- 1. hash the protected assets --------------------------------------
    digests = []
    for rel, label in ASSETS:
        p = os.path.join(stage, rel)
        if not os.path.exists(p):
            sys.exit("gen_integrity: protected asset missing: %s" % p)
        digests.append((label, sha256_file(p)))

    # ---- 2. patch the integrity table inside the wasm ----------------------
    with open(wasm_path, "rb") as f:
        wasm = bytearray(f.read())

    off = wasm.find(MAGIC)
    if off < 0:
        sys.exit("gen_integrity: integrity magic not found in eka2l1.wasm "
                 "(symbol stripped? check protection.cpp g_integ)")
    if wasm.find(MAGIC, off + 1) >= 0:
        sys.exit("gen_integrity: integrity magic found more than once")

    count = len(digests)
    if count > MAX_ENTRIES:
        sys.exit("gen_integrity: too many assets (%d > %d)" % (count, MAX_ENTRIES))

    # version (u32 LE) at off+16, count (u32 LE) at off+20
    struct.pack_into("<I", wasm, off + 16, 1)
    struct.pack_into("<I", wasm, off + 20, count)

    entries_off = off + TABLE_HEADER
    for i, (label, digest) in enumerate(digests):
        e = entries_off + i * ENTRY_SIZE
        name = label.encode("ascii")[:NAME_LEN - 1]
        name = name + b"\x00" * (NAME_LEN - len(name))
        wasm[e:e + NAME_LEN] = name
        wasm[e + NAME_LEN:e + NAME_LEN + 32] = digest

    with open(wasm_path, "wb") as f:
        f.write(wasm)

    # ---- 3. seal the patched wasm hash into integrity.js -------------------
    wasm_hash = hashlib.sha256(bytes(wasm)).hexdigest()
    integ_js = os.path.join(stage, "js", "integrity.js")
    os.makedirs(os.path.dirname(integ_js), exist_ok=True)
    with open(integ_js, "w") as f:
        f.write("window.EKA2L1_WASM_HASH='%s';\n" % wasm_hash)

    print("gen_integrity: sealed %d assets; wasm sha256=%s" % (count, wasm_hash))
    for label, digest in digests:
        print("  %-14s %s" % (label, digest.hex()))


if __name__ == "__main__":
    main()
