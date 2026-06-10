#!/usr/bin/env python3
"""
Simple HTTP(S) server that adds the COOP/COEP headers required for
SharedArrayBuffer (which is needed for Emscripten pthreads/ASYNCIFY in the
browser).

Usage:
    python3 serve.py [port] [directory] [--tls]

Example:
    cd build_wasm_test/bin
    python3 ../../src/emu/web/serve.py 8080 .

iPhone/iPad testing over LAN:
    SharedArrayBuffer only exists in a *secure context*. Opening
    http://<LAN-IP>:8080 from a phone is not one, so the emulator core cannot
    start. Run with --tls and open https://<LAN-IP>:8443 instead, then accept
    the self-signed certificate warning (Safari: Show Details -> visit this
    website). The certificate is generated once via the openssl CLI and cached
    next to this script.

    python3 ../../src/emu/web/serve.py 8443 . --tls
"""

import os
import socket
import subprocess
import sys
from http.server import HTTPServer, SimpleHTTPRequestHandler


class COOPCOEPHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.send_header("Pragma", "no-cache")
        super().end_headers()

    def log_message(self, format, *args):
        print(f"[serve] {self.address_string()} - {format % args}")


def lan_ip():
    """Best-effort LAN IP (no traffic is actually sent)."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return None


def ensure_self_signed_cert():
    """Generate (once) and return paths to a self-signed cert/key pair."""
    cert_dir = os.path.dirname(os.path.abspath(__file__))
    cert = os.path.join(cert_dir, "dev-cert.pem")
    key = os.path.join(cert_dir, "dev-key.pem")
    if os.path.exists(cert) and os.path.exists(key):
        return cert, key

    san = "DNS:localhost,IP:127.0.0.1"
    ip = lan_ip()
    if ip:
        san += f",IP:{ip}"

    cmd = [
        "openssl", "req", "-x509", "-newkey", "rsa:2048",
        "-keyout", key, "-out", cert, "-days", "3650", "-nodes",
        "-subj", "/CN=eka2l1-dev",
        "-addext", f"subjectAltName={san}",
    ]
    try:
        subprocess.run(cmd, check=True, capture_output=True)
    except FileNotFoundError:
        sys.exit("openssl CLI not found; install it or provide dev-cert.pem/dev-key.pem manually")
    except subprocess.CalledProcessError:
        # Old openssl without -addext: retry without SAN (still usable after
        # accepting the warning).
        subprocess.run(cmd[:-2], check=True)
    print(f"[serve] generated self-signed certificate: {cert}")
    return cert, key


def main():
    args = [a for a in sys.argv[1:] if a != "--tls"]
    use_tls = "--tls" in sys.argv[1:]

    port = int(args[0]) if len(args) > 0 else 8080
    directory = args[1] if len(args) > 1 else "."

    scheme = "https" if use_tls else "http"
    server = HTTPServer(("", port), COOPCOEPHandler)

    if use_tls:
        import ssl

        cert, key = ensure_self_signed_cert()
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        server.socket = ctx.wrap_socket(server.socket, server_side=True)

    os.chdir(directory)

    print(f"Serving '{os.getcwd()}' at {scheme}://localhost:{port}/")
    ip = lan_ip()
    if ip:
        print(f"[serve] LAN: {scheme}://{ip}:{port}/")
        if not use_tls:
            print("[serve] note: phones need a secure context for SharedArrayBuffer;"
                  " use --tls when testing from iPhone/iPad over LAN")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServer stopped.")


if __name__ == "__main__":
    main()
