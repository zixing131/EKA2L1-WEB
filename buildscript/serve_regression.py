#!/usr/bin/env python3
"""Serve the Web regression page on localhost (default port: 18091)."""

import argparse
from functools import partial
from http.server import ThreadingHTTPServer
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src/emu/web"))
from serve import COOPCOEPHandler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", type=int, nargs="?", default=18091)
    parser.add_argument("--directory", type=Path, default=ROOT / "build_wasm_release/bin")
    args = parser.parse_args()
    if not (args.directory / "smoke.html").is_file():
        parser.error("smoke.html is missing; run bash buildscript/build_release.sh first")
    handler = partial(COOPCOEPHandler, directory=str(args.directory.resolve()))
    with ThreadingHTTPServer(("127.0.0.1", args.port), handler) as server:
        print(f"Regression page: http://127.0.0.1:{args.port}/smoke.html", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
