#!/usr/bin/env python3
"""guitest.py - graphical programs on NovarisOS, photographed (Milestone 113).

    tools/guitest.py <iso> <build-dir> <memory> [proxy-port]

Boots an image built with a list of graphical programs
(/usr/share/novaris/gui-apps.txt) and GUI64_SHOTS, with the serial line on
a socket. For each program the kernel says whether the window it was
meant to open is on the desktop with something drawn in it -

    NOVARIS64: gui <label> = window shown: "<title>"

- and then prints `[host] SHOT <label>` and waits: this takes a QEMU
screendump into <build-dir>/gui/<label>.ppm and lets it go on. Passes
when every program on the list showed its window.

With a proxy port the guest goes out through the build machine's HTTP
proxy, the only way out of this machine (Milestone 99) - for Steam. QEMU's
user network already makes 10.0.2.2 this machine, so the image names
http://10.0.2.2:<port> and needs no forwarding rule (a guestfwd rule serves
one connection and stops). Names are resolved too, because Steam looks up
its hosts before it asks for a proxy: QEMU's DNS forwarder (10.0.2.3, the
guest's /etc/resolv.conf) is pointed by tools/qemu_net64.sh at
tools/dohdns.py on 127.0.0.1, which asks DNS-over-HTTPS through the proxy.
"""
import os
import re
import socket
import subprocess
import sys
import time


def monitor(path, command):
    s = socket.socket(socket.AF_UNIX)
    s.connect(path)
    s.settimeout(30)
    buf = b""
    while b"(qemu)" not in buf:
        buf += s.recv(4096)
    s.sendall(command.encode() + b"\n")
    buf = b""
    while b"(qemu)" not in buf:
        buf += s.recv(4096)
    s.close()


def main():
    iso, build, memory = sys.argv[1], sys.argv[2], sys.argv[3]
    proxy_port = sys.argv[4] if len(sys.argv) > 4 else ""
    shots = os.path.join(build, "gui")
    os.makedirs(shots, exist_ok=True)
    ser = os.path.join(build, "gui.ser")
    mon = os.path.join(build, "gui.mon")
    log = os.path.join(build, "serial-gui.log")
    for p in (ser, mon):
        if os.path.exists(p):
            os.unlink(p)

    # The boot's own network layer (userland/inet64.c) expects the servers
    # tools/net64_helper.py runs, on the ports make test uses, and has the
    # host connect to the guest's port 8080: without them it waits there.
    here = os.path.dirname(os.path.abspath(__file__))
    ports = dict(http=18780, dns=18781, fwd=18782, https=18783)
    helper = subprocess.Popen(
        [sys.executable, os.path.join(here, "net64_helper.py"), "--http", str(ports["http"]),
         "--https", str(ports["https"]), "--dns", str(ports["dns"]), "--connect", str(ports["fwd"]),
         "--status", os.path.join(build, "gui-net64.status")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    if helper.poll() is not None:
        print("FAIL: tools/net64_helper.py did not start - is another one holding ports %s?"
              % " ".join(str(p) for p in sorted(ports.values())))
        sys.exit(1)
    net = ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:8080" % ports["fwd"],
           "-device", "rtl8139,netdev=n0"]
    wrap, dns = [], None
    if proxy_port:
        # The guest reaches it as 10.0.2.2:<port>; nothing answering here
        # means an image that will spend an hour discovering it is offline.
        probe = socket.socket()
        probe.settimeout(5)
        try:
            probe.connect(("127.0.0.1", int(proxy_port)))
        except OSError:
            print("FAIL: nothing answers on the proxy port %s here - see $HTTPS_PROXY" % proxy_port)
            helper.kill()
            sys.exit(1)
        finally:
            probe.close()
        resolv = os.path.join(build, "gui-resolv.conf")
        with open(resolv, "w") as f:
            f.write("nameserver 127.0.0.1\n")
        wrap = ["sh", os.path.join(here, "qemu_net64.sh"), resolv]
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            probe.bind(("127.0.0.1", 53))
            probe.close()
            dns = subprocess.Popen([sys.executable, "-I", os.path.join(here, "dohdns.py")])
        except OSError:
            probe.close()   # one is already answering there
    qemu = subprocess.Popen(
        wrap + ["qemu-system-x86_64", "-cdrom", iso, "-m", memory, "-display", "none"] +
        os.environ.get("GUITEST_QEMU_ARGS", "").split() + net +
        ["-chardev", "socket,id=s0,path=%s,server=on,wait=on" % ser, "-serial", "chardev:s0",
         "-monitor", "unix:%s,server,nowait" % mon, "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(ser):
            break
        time.sleep(0.1)
    con = socket.socket(socket.AF_UNIX)
    con.connect(ser)
    con.settimeout(3600)

    verdicts, taken, pending, done = {}, [], b"", False
    out = open(log, "wb")
    deadline = time.time() + int(os.environ.get("GUITEST_TIMEOUT", "7200"))
    try:
        while not done and time.time() < deadline:
            chunk = con.recv(65536)
            if not chunk:
                break
            out.write(chunk)
            out.flush()
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                text = line.decode("latin-1").strip()
                m = re.search(r"NOVARIS64: gui (\S+) = (window shown|no window|ran|failed)", text)
                if m:
                    verdicts[m.group(1)] = m.group(2) in ("window shown", "ran")
                    print(text[text.index("NOVARIS64:"):], flush=True)
                if "[host] done" in text or "bring-up complete" in text:
                    con.sendall(b"k")
                    done = True
                    break
                if "[host] SHOT " in text:
                    label = text.split("[host] SHOT ", 1)[1].split()[0]
                    path = os.path.join(shots, label + ".ppm")
                    monitor(mon, "screendump " + path)
                    time.sleep(0.3)
                    taken.append(path)
                    con.sendall(b"k")
    finally:
        out.close()
        qemu.kill()
        qemu.wait()
        if dns:
            dns.kill()
        helper.kill()

    failed = [k for k, v in verdicts.items() if not v]
    print("--- %d screendumps in %s" % (len(taken), shots))
    if not done:
        print("FAIL: the run never finished (see %s)" % log)
        sys.exit(1)
    if not verdicts:
        print("FAIL: no program on the list was run")
        sys.exit(1)
    if failed:
        print("FAIL: no window from: %s" % " ".join(failed))
        sys.exit(1)
    print("PASS: %d graphical program(s) opened their windows: %s"
          % (len(verdicts), " ".join(verdicts)))


main()
