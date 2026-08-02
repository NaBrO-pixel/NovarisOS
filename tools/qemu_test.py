#!/usr/bin/env python3
"""Boot the Novaris ISO in QEMU, type shell commands, capture the transcript.

Previous milestones verified themselves by screenshotting the framebuffer
and OCR'ing it against the kernel's own font bitmap. That works, but it is
far too slow and brittle to lean on when the thing under test is a Win32
emulation layer with dozens of APIs and a pile of test .exe files. So the
kernel now mirrors its console output to COM1 (kernel/serial.c) and this
harness reads that instead: boot, drive the shell by injecting keystrokes
through the QEMU monitor, and hand back the plain-text transcript for
assertions.

Two things worth knowing about driving the shell this way:

  * `sendkey` speaks key *names*, not characters, and it is the layer that
    mangled punctuation in earlier milestones' testing. The KEYMAP below
    spells out every character we need explicitly (including the shifted
    ones) rather than hoping QEMU guesses right.
  * QEMU's monitor is line-oriented and asynchronous - it acks a sendkey
    long before the guest's IRQ1 handler has seen the scancode. Hence the
    small per-key delay; without it the guest drops keys.

Usage:
    tools/qemu_test.py --iso novaris.iso --cmd "ls" --cmd "run hellope.exe"
    tools/qemu_test.py --iso novaris.iso --script tools/tests/win32.txt
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

# Character -> QEMU monitor key name(s). Anything needing shift is sent as
# a "shift-x" combo, which QEMU understands as a single sendkey argument.
KEYMAP = {
    " ": "spc", "\n": "ret", "-": "minus", "=": "equal", "[": "bracket_left",
    "]": "bracket_right", ";": "semicolon", "'": "apostrophe", "`": "grave_accent",
    "\\": "backslash", ",": "comma", ".": "dot", "/": "slash",
    "_": "shift-minus", "+": "shift-equal", ":": "shift-semicolon",
    '"': "shift-apostrophe", "<": "shift-comma", ">": "shift-dot",
    "?": "shift-slash", "|": "shift-backslash", "~": "shift-grave_accent",
    "{": "shift-bracket_left", "}": "shift-bracket_right",
    "!": "shift-1", "@": "shift-2", "#": "shift-3", "$": "shift-4",
    "%": "shift-5", "^": "shift-6", "&": "shift-7", "*": "shift-8",
    "(": "shift-9", ")": "shift-0",
}
for _c in "abcdefghijklmnopqrstuvwxyz0123456789":
    KEYMAP[_c] = _c
for _c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    KEYMAP[_c] = "shift-" + _c.lower()


class Monitor:
    """QEMU monitor over a TCP socket."""

    def __init__(self, port, timeout=30.0):
        deadline = time.time() + timeout
        last_err = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 1.0)
                self.sock.settimeout(1.0)
                return
            except OSError as exc:  # QEMU may not have bound the port yet
                last_err = exc
                time.sleep(0.1)
        raise RuntimeError("could not connect to QEMU monitor: %s" % last_err)

    def send(self, line):
        self.sock.sendall((line + "\n").encode())
        time.sleep(0.02)
        try:
            self.sock.recv(65536)
        except socket.timeout:
            pass

    def sendkey(self, name, delay):
        self.send("sendkey " + name)
        time.sleep(delay)

    def type(self, text, delay):
        for ch in text:
            key = KEYMAP.get(ch)
            if key is None:
                raise ValueError("no QEMU key name for %r" % ch)
            self.sendkey(key, delay)

    def close(self):
        try:
            self.send("quit")
        except OSError:
            pass
        self.sock.close()


def run(iso, commands, boot_wait, key_delay, settle, timeout, keep_serial=None,
        memory=128):
    serial_path = keep_serial or tempfile.mktemp(suffix=".log", prefix="novaris-serial-")
    port = 45000 + (os.getpid() % 10000)

    qemu = subprocess.Popen(
        [
            "qemu-system-i386",
            # 128MB is plenty for novaris.iso. The Wine ISO carries an
            # initrd of PE builtins that is read into RAM whole, so those
            # runs ask for more.
            "-m", "%dM" % memory,
            "-cdrom", iso,
            "-display", "none",
            "-serial", "file:" + serial_path,
            "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
            "-no-reboot",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    try:
        mon = Monitor(port)
        # GRUB counts down before handing control to the kernel; the kernel
        # then runs its whole boot self-test before the shell prompts.
        time.sleep(boot_wait)

        for cmd in commands:
            mon.type(cmd + "\n", key_delay)
            time.sleep(settle)

        mon.close()
    finally:
        try:
            qemu.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            qemu.kill()
            qemu.wait()

    with open(serial_path, "rb") as fh:
        raw = fh.read()
    if keep_serial is None:
        os.unlink(serial_path)

    # The guest emits CRLF; normalize, and apply backspaces so the captured
    # text matches what a user would actually see on screen.
    text = raw.decode("utf-8", errors="replace").replace("\r\n", "\n")
    out = []
    for ch in text:
        if ch == "\b":
            if out:
                out.pop()
        else:
            out.append(ch)
    return "".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", default="novaris.iso")
    ap.add_argument("--cmd", action="append", default=[],
                    help="a shell command to type (repeatable)")
    ap.add_argument("--script", help="file with one shell command per line")
    ap.add_argument("--boot-wait", type=float, default=14.0)
    ap.add_argument("--key-delay", type=float, default=0.035)
    ap.add_argument("--settle", type=float, default=2.5,
                    help="seconds to wait after each command before the next")
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--serial-log", help="keep the raw serial capture at this path")
    ap.add_argument("--memory", type=int, default=128,
                    help="guest RAM in MB")
    ap.add_argument("--expect", action="append", default=[],
                    help="regex that must appear in the transcript (repeatable)")
    ap.add_argument("--reject", action="append", default=[],
                    help="regex that must NOT appear in the transcript (repeatable)")
    args = ap.parse_args()

    commands = list(args.cmd)
    if args.script:
        with open(args.script) as fh:
            commands += [ln.strip() for ln in fh
                         if ln.strip() and not ln.startswith("#")]

    transcript = run(args.iso, commands, args.boot_wait, args.key_delay,
                     args.settle, args.timeout, args.serial_log, args.memory)
    sys.stdout.write(transcript)

    failures = []
    for pattern in args.expect:
        if not re.search(pattern, transcript, re.MULTILINE):
            failures.append("MISSING: " + pattern)
    for pattern in args.reject:
        if re.search(pattern, transcript, re.MULTILINE):
            failures.append("PRESENT (should not be): " + pattern)

    if failures:
        sys.stderr.write("\n=== TRANSCRIPT ASSERTIONS FAILED ===\n")
        for f in failures:
            sys.stderr.write("  " + f + "\n")
        return 1

    if args.expect or args.reject:
        sys.stderr.write("\nAll %d transcript assertion(s) passed.\n"
                         % (len(args.expect) + len(args.reject)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
