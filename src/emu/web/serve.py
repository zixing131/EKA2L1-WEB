#!/usr/bin/env python3
"""
Simple HTTP server that adds the COOP/COEP headers required for SharedArrayBuffer
(which is needed for Emscripten pthreads/ASYNCIFY in the browser).

Usage:
    python3 serve.py <port> <directory>

Example:
    cd build_wasm_test/bin
    python3 ../../src/emu/web/serve.py 8080 .
"""

import sys
import os
from http.server import HTTPServer, SimpleHTTPRequestHandler


class COOPCOEPHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()

    def log_message(self, format, *args):
        print(f"[serve] {self.address_string()} - {format % args}")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    directory = sys.argv[2] if len(sys.argv) > 2 else "."

    os.chdir(directory)

    server = HTTPServer(("", port), COOPCOEPHandler)
    print(f"Serving '{os.getcwd()}' at http://localhost:{port}/")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServer stopped.")


if __name__ == "__main__":
    main()
