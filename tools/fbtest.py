#!/usr/bin/env python3
"""Boots the 64-bit ISO and checks what is actually on the screen.

Every other display assertion in this project is the kernel reading back
what the kernel just wrote. That catches a broken mapping and nothing
else: a framebuffer mapped over ordinary RAM would pass all of them and
show a black screen.

This asks QEMU instead. `screendump` writes the emulated display device's
own surface to a PPM, which is the far side of the driver - if the
picture is there, the pixels reached the hardware.

The pattern is drawn by kernel/kmain64.c, layer 6c. The coordinates and
colours below have to match it, and are deliberately flat blocks at
round numbers so that verifying them is a compare rather than a
judgement.
"""

import os
import socket
import subprocess
import sys
import time

WIDTH, HEIGHT = 1024, 768

# (x, y, r, g, b, what it is)
EXPECTED = [
    (200, 200, 0xFF, 0x00, 0x00, "the red block"),
    (500, 200, 0x00, 0xFF, 0x00, "the green block"),
    (200, 500, 0x00, 0x00, 0xFF, "the blue block"),
    (500, 500, 0xFF, 0xFF, 0xFF, "the white block"),
    (800, 700, 0x10, 0x20, 0x30, "the background"),
    # Just outside the red block on each side: if the rectangle were
    # drawn at the wrong offset or the pitch were wrong, these would be
    # red and the checks above would still pass.
    (99, 200, 0x10, 0x20, 0x30, "the column left of the red block"),
    (300, 200, 0x10, 0x20, 0x30, "the column right of the red block"),
    (200, 99, 0x10, 0x20, 0x30, "the row above the red block"),
    (200, 300, 0x10, 0x20, 0x30, "the row below the red block"),

    # The band userland/fbdraw64.c drew, through a MAP_SHARED mapping of
    # /dev/fb0 it obtained with the ordinary fbdev ioctls. This is the
    # one check here that no kernel-side assertion can substitute for:
    # it says a ring-3 process reached the actual display hardware.
    (500, 650, 0xFF, 0x00, 0xFF, "the magenta band a process drew"),
    (150, 630, 0xFF, 0x00, 0xFF, "the left end of the band"),
    (850, 690, 0xFF, 0x00, 0xFF, "the right end of the band"),
    (500, 619, 0x10, 0x20, 0x30, "the row above the band"),
    (500, 700, 0x10, 0x20, 0x30, "the row below the band"),
    (99, 650, 0x10, 0x20, 0x30, "the column left of the band"),
    (900, 650, 0x10, 0x20, 0x30, "the column right of the band"),
]


def read_ppm(path):
    """QEMU writes a binary P6 with a maxval of 255."""
    with open(path, "rb") as f:
        data = f.read()

    fields = []
    i = 0
    while len(fields) < 4:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":                  # a comment runs to EOL
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        start = i
        while i < len(data) and not data[i : i + 1].isspace():
            i += 1
        fields.append(data[start:i])
    i += 1                                            # the single whitespace
                                                      # byte after maxval

    magic, w, h, maxval = fields[0], int(fields[1]), int(fields[2]), int(fields[3])
    if magic != b"P6":
        raise ValueError("not a binary PPM: %r" % magic)
    if maxval != 255:
        raise ValueError("unexpected maxval %d" % maxval)
    return w, h, data[i:]


def pixel(pix, w, x, y):
    off = (y * w + x) * 3
    return pix[off], pix[off + 1], pix[off + 2]


def monitor_command(sock_path, command, timeout=20.0):
    deadline = time.time() + timeout
    while True:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(sock_path)
            break
        except (FileNotFoundError, ConnectionRefusedError):
            if time.time() > deadline:
                raise
            time.sleep(0.1)
    with s:
        s.settimeout(5.0)
        try:
            s.recv(4096)                              # the banner
        except socket.timeout:
            pass
        s.sendall(command.encode() + b"\n")
        time.sleep(1.0)
        try:
            return s.recv(65536).decode(errors="replace")
        except socket.timeout:
            return ""


def main():
    if len(sys.argv) != 4:
        print("usage: fbtest.py <iso> <ram> <build-dir>", file=sys.stderr)
        return 2
    iso, ram, build = sys.argv[1], sys.argv[2], sys.argv[3]

    serial = os.path.join(build, "fbtest.serial.log")
    shot = os.path.abspath(os.path.join(build, "screen.ppm"))
    monsock = os.path.join(build, "fbtest.mon")

    for path in (serial, shot, monsock):
        if os.path.exists(path):
            os.unlink(path)

    qemu = subprocess.Popen(
        ["qemu-system-x86_64", "-cdrom", iso, "-m", ram, "-display", "none",
         "-serial", "file:" + serial, "-no-reboot",
         "-monitor", "unix:%s,server,nowait" % monsock],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        # The kernel halts after kernel_main, so the machine stays up and
        # the last thing drawn stays on the screen. Wait for it to say so
        # rather than sleeping a guessed number of seconds.
        deadline = time.time() + 90
        while time.time() < deadline:
            if qemu.poll() is not None:
                print("FAIL: qemu exited before the kernel finished booting")
                return 1
            try:
                with open(serial, "rb") as f:
                    if b"bring-up complete" in f.read():
                        break
            except FileNotFoundError:
                pass
            time.sleep(0.25)
        else:
            print("FAIL: the kernel never reached the end of kernel_main")
            return 1

        reply = monitor_command(monsock, "screendump %s" % shot)

        deadline = time.time() + 20
        while time.time() < deadline:
            if os.path.exists(shot) and os.path.getsize(shot) > 0:
                break
            time.sleep(0.25)
        else:
            print("FAIL: qemu wrote no screenshot; monitor said: %r" % reply)
            return 1
        time.sleep(0.5)                               # let the write finish
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()

    w, h, pix = read_ppm(shot)
    if (w, h) != (WIDTH, HEIGHT):
        print("FAIL: the screen is %dx%d, expected %dx%d" % (w, h, WIDTH, HEIGHT))
        return 1

    bad = 0
    for x, y, r, g, b, what in EXPECTED:
        got = pixel(pix, w, x, y)
        if got != (r, g, b):
            print("FAIL: %s at (%d,%d) is #%02x%02x%02x, expected #%02x%02x%02x"
                  % (what, x, y, got[0], got[1], got[2], r, g, b))
            bad += 1

    if bad:
        return 1

    print("PASS: the screen really shows the pattern (%d pixels checked "
          "on a %dx%d screendump)" % (len(EXPECTED), w, h))
    return 0


if __name__ == "__main__":
    sys.exit(main())
