#!/usr/bin/env python3
"""gputest.py - is what the GPU driver set really on the screen? (Milestone 112)

    tools/gputest.py <iso> <build-dir> [rage128p|rv100]

Boots the ISO on QEMU's ATI Radeon (-device ati-vga) with the serial line
on a socket, so this script can answer it. The kernel's GPU layer sets a
mode, flips between buffers and puts the mode back, and after each step
prints

    NOVARIS64: [gpu-test] SHOT <name> <width> <height> [x,y=rrggbb ...]

and waits. This takes a QEMU screendump - the emulated display's own
picture, the far side of the driver - checks its size and the pixels
named, and sends one byte to let the kernel carry on. The kernel's own
assertions (register read-backs) are in the serial log as usual; both
have to hold.
"""
import os
import socket
import subprocess
import sys
import time


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1
    if parts[0] != b"P6":
        raise ValueError("not a P6 PPM")
    return int(parts[1]), int(parts[2]), data[pos:]


def monitor(path, command):
    s = socket.socket(socket.AF_UNIX)
    s.connect(path)
    s.settimeout(20)
    buf = b""
    while b"(qemu)" not in buf:
        buf += s.recv(4096)
    s.sendall(command.encode() + b"\n")
    buf = b""
    while b"(qemu)" not in buf:
        buf += s.recv(4096)
    s.close()


def main():
    iso, build = sys.argv[1], sys.argv[2]
    model = sys.argv[3] if len(sys.argv) > 3 else "rage128p"
    ser = os.path.join(build, "gpu.ser")
    mon = os.path.join(build, "gpu.mon")
    log = os.path.join(build, "serial-gpu.log")
    for p in (ser, mon):
        if os.path.exists(p):
            os.unlink(p)

    qemu = subprocess.Popen(
        ["qemu-system-x86_64", "-cdrom", iso, "-m", "1G", "-display", "none",
         "-vga", "none", "-device", "ati-vga,model=" + model,
         "-chardev", "socket,id=s0,path=%s,server=on,wait=on" % ser, "-serial", "chardev:s0",
         "-monitor", "unix:%s,server,nowait" % mon, "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(ser):
            break
        time.sleep(0.1)
    con = socket.socket(socket.AF_UNIX)
    con.connect(ser)
    con.settimeout(300)

    failures, shots, pending = 0, 0, b""
    out = open(log, "wb")
    deadline = time.time() + 600
    done = False
    try:
        while not done and time.time() < deadline:
            chunk = con.recv(65536)
            if not chunk:
                break
            out.write(chunk)
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                text = line.decode("latin-1").strip()
                if "NOVARIS64: FAIL" in text:
                    print(text)
                    failures += 1
                if "[gpu] " in text or "SKIP  no ATI" in text:
                    print(text[text.index("NOVARIS64:"):] if "NOVARIS64:" in text else text)
                if "[gpu-test] done" in text:
                    con.sendall(b"k")
                    done = True
                    break
                if "[gpu-test] SHOT " not in text:
                    continue
                words = text.split("[gpu-test] SHOT ", 1)[1].split()
                name, w, h, checks = words[0], int(words[1]), int(words[2]), words[3:]
                ppm = os.path.join(build, "gpu-%s.ppm" % name)
                monitor(mon, "screendump " + ppm)
                time.sleep(0.2)
                gw, gh, pix = read_ppm(ppm)
                ok = (gw, gh) == (w, h)
                print("%s  %s: the screen is %dx%d, wanted %dx%d"
                      % ("ok " if ok else "FAIL", name, gw, gh, w, h))
                failures += not ok
                for c in checks:
                    xy, want = c.split("=")
                    x, y = (int(v) for v in xy.split(","))
                    i = (y * gw + x) * 3
                    got = "%02x%02x%02x" % tuple(pix[i:i + 3]) if ok else "------"
                    good = ok and got == want
                    print("%s  %s: (%d,%d) is #%s, wanted #%s"
                          % ("ok " if good else "FAIL", name, x, y, got, want))
                    failures += not good
                shots += 1
                con.sendall(b"k")
    finally:
        out.close()
        qemu.kill()
        qemu.wait()

    if not done:
        print("FAIL: the GPU layer never finished (see %s)" % log)
        failures += 1
    if shots < 4:
        print("FAIL: %d screendumps taken, wanted 4" % shots)
        failures += 1
    if failures:
        print("FAIL: %d check(s) did not hold" % failures)
        sys.exit(1)
    print("PASS: the ATI driver's modes and flips are on the screen, on the %s (%d screendumps)"
          % (model, shots))


main()
