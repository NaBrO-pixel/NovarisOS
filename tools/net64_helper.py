#!/usr/bin/env python3
"""The build machine's side of the 64-bit kernel's inet layer.

QEMU's user-mode network puts this machine at 10.0.2.2 as the guest sees
it, so the guest can reach servers started here:

  * HTTP on --http and HTTPS on --https (a self-signed certificate for
    novaris.test, made at start-up), serving novaris.txt, page.html -
    a page whose script adds a paragraph, so a dump of the DOM says both
    that the page arrived and that JavaScript ran - and big.bin, 2MB of
    a known pattern for measuring TCP;
  * a DNS responder on --dns and, with --dns53, on 127.0.0.1:53 too.
    Names under .test answer 10.0.2.2, the host as the guest sees it,
    so novaris.test is this machine; example.com answers 192.0.2.1
    (RFC 5737's documentation range, so never mistaken for a real
    answer); any other name is NXDOMAIN, and a question that is not for
    an A record gets an empty answer;
  * and, the other way, a client that keeps connecting to --connect - a
    host forward to the guest's port 8080 - until the guest accepts,
    reads "hello from the host" and answers. That proves a connection
    made *to* NovarisOS works, which nothing inside the guest can show.

Port 53 is how the guest's own resolver reaches this responder: make
test runs QEMU with an /etc/resolv.conf of its own that names 127.0.0.1
(tools/qemu_net64.sh), and QEMU's DNS forwarder at 10.0.2.3 - the server
DHCP gives the guest - sends its questions there.

Runs until killed. --status collects "connected" once the guest has
answered and "dns53" if port 53 could be had; --log, every HTTP(S)
request and every DNS question.
"""

import argparse
import http.server
import os
import socket
import socketserver
import ssl
import subprocess
import tempfile
import threading
import time

LOG = None
LOCK = threading.Lock()


def log(line):
    if LOG:
        with LOCK, open(LOG, "a") as f:
            f.write(line + "\n")


PAGE = """<!doctype html>
<html><head><title>NovarisOS over TCP</title></head>
<body><p id="served">fetched over TCP from the build machine</p>
<script>
document.body.insertAdjacentHTML("beforeend",
    "<p id=\\"js\\">script ran: " + (6 * 7) + "</p>");
</script></body></html>
"""

BIG_LEN = 2 * 1024 * 1024


def big_pattern():
    # Byte i is (i * 7 + i // 251) & 0xFF, as userland/inet64.c checks.
    return bytes(((i * 7 + i // 251) & 0xFF) for i in range(BIG_LEN))


# --- DNS ---------------------------------------------------------------

def answer_for(name):
    name = name.lower().rstrip(".")
    if name == "test" or name.endswith(".test"):
        return bytes([10, 0, 2, 2])
    if name == "example.com":
        return bytes([192, 0, 2, 1])
    return None


def dns_reply(query):
    """The reply to one standard query, or None for anything else."""
    if len(query) < 12:
        return None
    if query[2] & 0x80 or ((query[4] << 8) | query[5]) != 1:
        return None
    labels, i = [], 12
    while i < len(query) and query[i]:
        if query[i] & 0xC0:
            return None
        labels.append(query[i + 1:i + 1 + query[i]].decode("ascii", "replace"))
        i += 1 + query[i]
    end = i + 1 + 4
    if end > len(query):
        return None
    qtype = (query[i + 1] << 8) | query[i + 2]
    name = ".".join(labels)
    addr = answer_for(name)
    question = query[12:end]
    rd = query[2] & 0x01
    if addr is None:
        flags = bytes([0x80 | rd, 0x83])          # NXDOMAIN
        counts = bytes([0, 1, 0, 0, 0, 0, 0, 0])
        body = b""
    elif qtype != 1:
        flags = bytes([0x80 | rd, 0x80])          # the name exists, no such record
        counts = bytes([0, 1, 0, 0, 0, 0, 0, 0])
        body = b""
    else:
        flags = bytes([0x80 | rd, 0x80])
        counts = bytes([0, 1, 0, 1, 0, 0, 0, 0])
        body = (bytes([0xC0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4]) + addr)
    log("dns %s type %d -> %s" % (name, qtype,
        "NXDOMAIN" if addr is None else ".".join(map(str, addr)) if qtype == 1 else "no data"))
    return query[0:2] + flags + counts + question + body


def serve_dns(host, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host, port))

    def loop():
        while True:
            try:
                query, peer = sock.recvfrom(512)
            except OSError:
                return
            reply = dns_reply(query)
            if reply:
                sock.sendto(reply, peer)

    threading.Thread(target=loop, daemon=True).start()
    return sock


# --- HTTP and HTTPS ----------------------------------------------------

class Handler(http.server.SimpleHTTPRequestHandler):
    scheme = "http"

    def do_PUT(self):
        """An upload to measure the guest's sending: the body has to be
        big.bin's pattern, and the reply says how much of it arrived
        intact."""
        n = int(self.headers.get("Content-Length", "0"))
        want = big_pattern()[:n] if n <= BIG_LEN else None
        got, good = b"", True
        while len(got) < n:
            chunk = self.rfile.read(min(65536, n - len(got)))
            if not chunk:
                break
            got += chunk
        good = want is not None and got == want
        body = ("%s %d\n" % ("intact" if good else "damaged", len(got))).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        log("%s %s %s" % (self.scheme, self.client_address[0], fmt % args))


class TlsHandler(Handler):
    scheme = "https"


def serve(port, directory, handler, tls=None):
    h = lambda *a, **k: handler(*a, directory=directory, **k)
    socketserver.TCPServer.allow_reuse_address = True
    server = socketserver.ThreadingTCPServer(("127.0.0.1", port), h)
    if tls:
        server.socket = tls.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def make_cert(directory):
    key = os.path.join(directory, "key.pem")
    cert = os.path.join(directory, "cert.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", key, "-out", cert, "-days", "2",
                    "-subj", "/CN=novaris.test",
                    "-addext", "subjectAltName=DNS:novaris.test,IP:10.0.2.2"],
                   check=True, capture_output=True)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    return ctx


def connect_loop(port, status):
    while True:
        try:
            with socket.create_connection(("127.0.0.1", port), 2.0) as c:
                c.sendall(b"hello from the host\n")
                # The reply may come late on a slow machine, or in pieces:
                # read until it has all come or 15 seconds have gone.
                reply, until = b"", time.time() + 15.0
                while b"hello from NovarisOS" not in reply and time.time() < until:
                    c.settimeout(max(0.1, until - time.time()))
                    try:
                        more = c.recv(128)
                    except socket.timeout:
                        break
                    if not more:
                        break
                    reply += more
                if b"hello from NovarisOS" in reply:
                    with LOCK, open(status, "a") as f:
                        f.write("connected\n")
                    return
        except OSError:
            pass
        time.sleep(1.0)


def main():
    global LOG
    ap = argparse.ArgumentParser()
    ap.add_argument("--http", type=int, required=True)
    ap.add_argument("--https", type=int)
    ap.add_argument("--dns", type=int, required=True)
    ap.add_argument("--dns53", action="store_true",
                    help="answer on 127.0.0.1:53 as well")
    ap.add_argument("--connect", type=int, required=True)
    ap.add_argument("--status", required=True)
    ap.add_argument("--log", help="append each request and question here")
    args = ap.parse_args()
    LOG = args.log

    www = tempfile.mkdtemp(prefix="net64www")
    with open(os.path.join(www, "novaris.txt"), "w") as f:
        f.write("This file was served to NovarisOS over TCP.\n")
    with open(os.path.join(www, "page.html"), "w") as f:
        f.write(PAGE)
    with open(os.path.join(www, "big.bin"), "wb") as f:
        f.write(big_pattern())

    serve(args.http, www, Handler)
    if args.https:
        serve(args.https, www, TlsHandler, make_cert(tempfile.mkdtemp(prefix="net64tls")))
    serve_dns("0.0.0.0", args.dns)
    if args.dns53:
        try:
            serve_dns("127.0.0.1", 53)
            with LOCK, open(args.status, "a") as f:
                f.write("dns53\n")
        except OSError as e:
            log("dns: cannot use 127.0.0.1:53: %s" % e)
    connect_loop(args.connect, args.status)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
