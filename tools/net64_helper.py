#!/usr/bin/env python3
"""The build machine's side of the 64-bit kernel's inet layer.

QEMU's user-mode network puts this machine at 10.0.2.2 as the guest sees
it, so userland/inet64.c --external can reach servers started here:

  * an HTTP server on --http, serving one file, novaris.txt;
  * a DNS responder on --dns that answers every A question with
    192.0.2.1 (tools/qemu_test.py's, shared);
  * and, the other way, a client that keeps connecting to --connect - a
    host forward to the guest's port 8080 - until the guest accepts,
    reads "hello from the host" and answers. That proves a connection
    made *to* NovarisOS works, which nothing inside the guest can show.

Runs until killed. Writes "connected" to --status once the guest has
answered, so make test can tell a guest that never listened from one
that did.
"""

import argparse
import http.server
import os
import socket
import socketserver
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_test import serve_dns  # noqa: E402


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def serve_http(port, directory):
    handler = lambda *a, **k: QuietHandler(*a, directory=directory, **k)
    socketserver.TCPServer.allow_reuse_address = True
    server = socketserver.ThreadingTCPServer(("127.0.0.1", port), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def connect_loop(port, status):
    while True:
        try:
            with socket.create_connection(("127.0.0.1", port), 2.0) as c:
                c.settimeout(5.0)
                c.sendall(b"hello from the host\n")
                reply = c.recv(128)
                if b"hello from NovarisOS" in reply:
                    with open(status, "w") as f:
                        f.write("connected\n")
                    return
        except OSError:
            pass
        time.sleep(1.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--http", type=int, required=True)
    ap.add_argument("--dns", type=int, required=True)
    ap.add_argument("--connect", type=int, required=True)
    ap.add_argument("--status", required=True)
    args = ap.parse_args()

    www = tempfile.mkdtemp(prefix="net64www")
    with open(os.path.join(www, "novaris.txt"), "w") as f:
        f.write("This file was served to NovarisOS over TCP.\n")

    serve_http(args.http, www)
    serve_dns(args.dns)
    connect_loop(args.connect, args.status)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
