#!/usr/bin/env python3
"""Boots the 64-bit ISO and types at it.

Every input assertion in kmain64.c feeds the decoder by hand. That tests
the decoder and nothing else: all of them pass on a kernel whose IRQ1 is
masked, whose handler is never registered, and whose PS/2 controller was
never told to enable the mouse. The kernel prints "irq1 0, irq12 0" next
to them to say so.

This sends real keystrokes and real mouse movement through the QEMU
monitor. They go through the emulated 8042, raise actual interrupts, and
come back out of the driver - which is the far side of everything the
synthetic tests skip. It is the same argument fbtest.py makes by taking
a screendump instead of asking the kernel what it drew.

The kernel's input watch loop (the end of kernel_main) prints a line per
event; the expectations below have to match it.
"""

import os
import socket
import subprocess
import sys
import time


def monitor_command(sock_path, command, settle=0.35, timeout=30.0):
    """One command on the QEMU monitor. Reconnects per call, which is
    slower than holding the socket open and far easier to reason about
    when a command produces no reply."""
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
        s.settimeout(3.0)
        try:
            s.recv(4096)                                  # the banner
        except socket.timeout:
            pass
        s.sendall(command.encode() + b"\n")
        time.sleep(settle)
        try:
            return s.recv(65536).decode(errors="replace")
        except socket.timeout:
            return ""


def wait_for(path, needle, timeout):
    """Waits for a line to appear in the serial log."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                if needle in f.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.25)
    return False


def main():
    if len(sys.argv) != 4:
        print("usage: inputtest.py <iso> <ram> <build-dir>", file=sys.stderr)
        return 2
    iso, ram, build = sys.argv[1], sys.argv[2], sys.argv[3]

    serial = os.path.join(build, "inputtest.serial.log")
    monsock = os.path.join(build, "inputtest.mon")
    for path in (serial, monsock):
        if os.path.exists(path):
            os.unlink(path)

    qemu = subprocess.Popen(
        ["qemu-system-x86_64", "-cdrom", iso, "-m", ram, "-display", "none",
         "-serial", "file:" + serial,
         "-monitor", "unix:" + monsock + ",server,nowait",
         "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    failures = []
    try:
        # The watch loop only exists after the whole bring-up, so there
        # is no point typing before it.
        if not wait_for(serial, "---- input watch ----", 60):
            print("FAIL: the kernel never reached the input watch")
            return 1

        # A moment for the loop to actually be looping.
        time.sleep(0.5)

        # Keys, by name. QEMU translates these to set-1 scancodes on the
        # emulated 8042, which is exactly the path a real keyboard uses.
        # 'a' is keycode 30, 'b' 48, 'esc' 1 - Linux's numbers, which
        # are the set-1 scancodes unchanged.
        for key in ("a", "b", "esc"):
            monitor_command(monsock, "sendkey " + key)

        # The mouse. mouse_move is relative for a PS/2 mouse, which is
        # the only kind this driver knows.
        monitor_command(monsock, "mouse_move 10 6")
        monitor_command(monsock, "mouse_button 1")
        monitor_command(monsock, "mouse_button 0")

        time.sleep(1.5)

        with open(serial, "r", errors="replace") as f:
            log = f.read()

        watch = log.split("---- input watch ----", 1)[1]

        # Press and release both, because sendkey does both and a driver
        # that only decoded makes would look correct until something
        # held a key.
        expected = [
            ("key 30 down", "the 'a' keypress"),
            ("key 30 up", "the 'a' release"),
            ("key 48 down", "the 'b' keypress"),
            ("key 48 up", "the 'b' release"),
            ("key 1 down", "the escape keypress"),
            ("mouse x 10", "the mouse moving right by 10"),
            # Positive, and it is worth saying why, because it looks
            # like the driver's Y inversion is missing.
            #
            # It is inverted twice. The monitor's mouse_move takes
            # screen deltas - positive is down - and QEMU negates on the
            # way into the PS/2 packet, because the hardware counts Y
            # upwards. The driver negates it back. The result is evdev's
            # convention, where REL_Y positive is down, and it is the
            # same number Linux reports for the same gesture.
            #
            # So this end of the round trip cannot see the inversion at
            # all, which is exactly why the kernel asserts it directly:
            # a PS/2 packet with dy=+3 fed to the decoder has to come
            # out as REL_Y=-3.
            ("mouse y 6", "the mouse moving down by 6"),
            ("button 272 down", "the left button going down"),
            ("button 272 up", "the left button coming up"),
        ]

        for needle, what in expected:
            if needle in watch:
                print("ok   %s reached the driver" % what)
            else:
                print("FAIL: %s never reached the driver (%r)" % (what, needle))
                failures.append(what)

        if failures:
            print("\n--- the input watch said ---")
            print(watch.strip()[:2000] or "(nothing at all)")
            print("----------------------------")

    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()

    if failures:
        print("FAIL: %d of %d input events never arrived"
              % (len(failures), len(expected)))
        return 1

    print("PASS: real keystrokes and real mouse movement reach the driver")
    return 0


if __name__ == "__main__":
    sys.exit(main())
