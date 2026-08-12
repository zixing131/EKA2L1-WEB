#!/usr/bin/env python3
"""Serve the WASM build with the COOP/COEP headers required by SharedArrayBuffer."""
import http.server
import socketserver
import sys
import os

class COOPHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        super().end_headers()

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    directory = sys.argv[2] if len(sys.argv) > 2 else None
    if directory:
        os.chdir(directory)
    with socketserver.TCPServer(("", port), COOPHandler) as httpd:
        print(f"Serving on http://localhost:{port}/  (COOP/COEP headers enabled)")
        print(f"Directory: {os.getcwd()}")
        print("Press Ctrl+C to stop.")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nStopped.")
