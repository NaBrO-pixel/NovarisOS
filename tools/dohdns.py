#!/usr/bin/env python3
"""dohdns.py [port] - a DNS server that answers through DNS-over-HTTPS
(Milestone 113).

This build machine has no resolver of its own: every name is resolved by
the HTTPS proxy its traffic goes through. Most programs never notice,
because they hand the proxy a name. Steam does not: its updater calls
getaddrinfo on the update host before it asks for its proxy settings,
and stops at "http error 0" when the name does not resolve.

So this answers A queries on 127.0.0.1:<port> (53 by default) by asking
https://dns.google/resolve - through the proxy, like everything else -
and is what tools/qemu_net64.sh's private resolv.conf points QEMU's DNS
forwarder at, so a name looked up inside NovarisOS is resolved the same
way. AAAA and the rest get "no data", which sends a resolver back to A.
"""
import json
import socket
import sys
import threading
import time
import urllib.parse
import urllib.request

CACHE = {}
LOCK = threading.Lock()


def resolve(name):
    now = time.time()
    with LOCK:
        hit = CACHE.get(name)
        if hit and hit[1] > now:
            return hit[0]
    url = "https://dns.google/resolve?" + urllib.parse.urlencode({"name": name, "type": "A"})
    try:
        with urllib.request.urlopen(url, timeout=15) as r:
            data = json.load(r)
        addrs = [a["data"] for a in data.get("Answer", []) if a.get("type") == 1]
    except Exception:
        addrs = []
    with LOCK:
        CACHE[name] = (addrs, now + 60)
    return addrs


def reply(query):
    if len(query) < 12 or query[2] & 0x80 or ((query[4] << 8) | query[5]) != 1:
        return None
    labels, i = [], 12
    while i < len(query) and query[i]:
        if query[i] & 0xC0:
            return None
        labels.append(query[i + 1:i + 1 + query[i]].decode("ascii", "replace"))
        i += 1 + query[i]
    end = i + 5
    if end > len(query):
        return None
    qtype = (query[i + 1] << 8) | query[i + 2]
    name = ".".join(labels).lower()
    question = query[12:end]
    rd = query[2] & 1
    addrs = resolve(name) if qtype == 1 else None
    if qtype == 1 and not addrs:
        return query[:2] + bytes([0x80 | rd, 0x83, 0, 1, 0, 0, 0, 0, 0, 0]) + question
    if qtype != 1:
        return query[:2] + bytes([0x80 | rd, 0x80, 0, 1, 0, 0, 0, 0, 0, 0]) + question
    body = b""
    for a in addrs[:8]:
        body += bytes([0xC0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4]) + socket.inet_aton(a)
    return query[:2] + bytes([0x80 | rd, 0x80, 0, 1, 0, len(addrs[:8]), 0, 0, 0, 0]) + question + body


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 53
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", port))
    while True:
        query, peer = sock.recvfrom(512)

        def answer(q=query, p=peer):
            r = reply(q)
            if r:
                sock.sendto(r, p)
        threading.Thread(target=answer, daemon=True).start()


main()
