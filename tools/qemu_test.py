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

    def mouse_to(self, x, y):
        """Puts the pointer at an absolute screen position.

        QEMU's `mouse_move` is *relative* for a PS/2 mouse, which is the
        only kind Novaris has a driver for, so absolute positioning means
        slamming the pointer into the top-left corner first and then
        moving out by the coordinates. The corner slam is one big negative
        move; the driver clamps, so overshooting is how you get there.

        The moves are split into chunks because a PS/2 packet carries a
        signed 9-bit delta per axis, and a single huge move would be
        truncated rather than clamped."""
        for _ in range(4):
            self.send("mouse_move -400 -400")
        time.sleep(0.15)
        remaining_x, remaining_y = x, y
        while remaining_x > 0 or remaining_y > 0:
            dx = min(120, remaining_x)
            dy = min(120, remaining_y)
            self.send("mouse_move %d %d" % (dx, dy))
            remaining_x -= dx
            remaining_y -= dy
        time.sleep(0.25)

    def click(self, double=False):
        """A click, and optionally a second one close enough to be a
        double. Novaris's window manager decides that by the gap between
        the two presses, so the pause here is deliberately short."""
        self.send("mouse_button 1")
        time.sleep(0.05)
        self.send("mouse_button 0")
        if double:
            time.sleep(0.08)
            self.send("mouse_button 1")
            time.sleep(0.05)
            self.send("mouse_button 0")
        time.sleep(0.4)

    def screendump(self, path):
        """Writes the framebuffer to a PPM.

        QEMU answers `screendump` before the file is complete, so the
        caller that wants to look at the image has to wait for it rather
        than for the monitor."""
        self.send("screendump " + path)
        deadline = time.time() + 10.0
        size = -1
        while time.time() < deadline:
            time.sleep(0.4)
            try:
                now = os.path.getsize(path)
            except OSError:
                continue
            if now > 0 and now == size:
                return
            size = now

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


def normalize(raw):
    """The serial capture as a user would have seen it on screen.

    The guest emits CRLF; normalize that, and apply backspaces, since the
    shell's line editing is real and a transcript full of "\b" would not
    match what the screen said."""
    text = raw.decode("utf-8", errors="replace").replace("\r\n", "\n")
    out = []
    for ch in text:
        if ch == "\b":
            if out:
                out.pop()
        else:
            out.append(ch)
    return "".join(out)


def read_transcript(path):
    with open(path, "rb") as fh:
        return normalize(fh.read())


def wait_for(serial_path, seconds, patterns):
    """Sleeps up to `seconds`, stopping early once every pattern has shown up.

    Without this a settle is a fixed cost paid in full on every run, so it
    has to be set for the slowest plausible machine and then everybody
    waits that long. Wine's real startup path takes minutes and varies by
    a lot between runs, and the difference between "generous" and "slow"
    should not have to be guessed in a Makefile.

    Only the --expect patterns are waited for. Stopping as soon as they
    have all appeared means the --reject patterns are only checked over
    the window actually observed - which is the right trade when the last
    thing expected is the program's own exit line, and the reason this is
    opt-in rather than the default."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        time.sleep(min(2.0, max(0.0, deadline - time.time())))
        if not patterns:
            continue
        try:
            text = read_transcript(serial_path)
        except OSError:
            continue
        if all(re.search(p, text, re.MULTILINE) for p in patterns):
            return


def run(iso, commands, boot_wait, key_delay, settle, timeout, keep_serial=None,
        memory=128, disk=None, setup=(), stop_when=(), clicks=(),
        click_settle=1.5, post=(), screenshot=None, post_clicks=(),
        http_forward=None):
    serial_path = keep_serial or tempfile.mktemp(suffix=".log", prefix="novaris-serial-")
    port = 45000 + (os.getpid() % 10000)

    argv = [
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
        # Milestone 38. A network card, and QEMU's own user-mode stack
        # behind it: a DHCP server at 10.0.2.2 that hands out 10.0.2.15, a
        # DNS forwarder at 10.0.2.3, and the host reachable at 10.0.2.2.
        # Always present rather than opt-in, because "the OS boots with a
        # network card in it" is now part of what every test covers.
        # QEMU's user-mode stack: DHCP hands out 10.0.2.15, the host is
        # 10.0.2.2, DNS forwards from 10.0.2.3.
        #
        # 10.0.2.2 is both the guest's default gateway and this machine,
        # so a server bound on the host is reachable from the guest with
        # no forwarding rule at all - which matters, because QEMU's
        # `guestfwd` proxy serves one connection and then stops, and a
        # test that fetches twice would fail the second time for reasons
        # that have nothing to do with the OS.
        #
        # Binding the server to 0.0.0.0 rather than loopback is what makes
        # 10.0.2.2 answer: slirp connects out from the host's stack.
        "-netdev", "user,id=n0",
        "-device", "rtl8139,netdev=n0",
    ]
    if disk:
        # Milestone 32. QEMU's -cdrom is IDE index 2 (secondary master),
        # so the disk goes at index 0 (primary master) - which is the
        # first slot the kernel's ATA probe looks at.
        #
        # `format=raw` is not optional: without it QEMU guesses the format
        # from the contents, prints a warning about doing so, and would
        # guess wrong on an image whose first sector resembled something
        # else.
        #
        # `-boot d` goes with it. A FAT32 volume has 0x55AA in its boot
        # sector - that is part of the format - so SeaBIOS considers the
        # disk bootable and tries it before the CD, and the machine stops
        # on a boot sector that contains no boot code. The ISO is what
        # boots; the disk is only storage.
        argv += ["-drive",
                 "file=%s,format=raw,if=ide,index=0,media=disk" % disk,
                 "-boot", "d"]

    qemu = subprocess.Popen(
        argv,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    try:
        mon = Monitor(port)
        # GRUB counts down before handing control to the kernel; the kernel
        # then runs its whole boot self-test before the shell prompts.
        time.sleep(boot_wait)

        # Setup commands configure the machine before the run proper and
        # are not what is being measured, so they get a short fixed pause
        # rather than the settle the real commands need. Without the
        # split, a `symlinks off` in front of a Wine run would cost the
        # same three minutes the Wine run does.
        for cmd in setup:
            mon.type(cmd + "\n", key_delay)
            time.sleep(3.0)

        # Pointer work before anything is typed. "340,300,double" means
        # move there and double-click.
        def do_clicks(specs):
            for spec in specs:
                parts = spec.split(",")
                x, y = int(parts[0]), int(parts[1])
                what = parts[2].strip() if len(parts) > 2 else ""
                mon.mouse_to(x, y)
                if what != "move":
                    mon.click(what == "double")
                time.sleep(click_settle)

        do_clicks(clicks)

        if not commands:
            # Clicks only. The settle is still needed - what was clicked
            # may take minutes to say anything.
            if stop_when:
                wait_for(serial_path, settle, stop_when)
            else:
                time.sleep(settle)

        for i, cmd in enumerate(commands):
            mon.type(cmd + "\n", key_delay)
            last = (i == len(commands) - 1)
            # Pointer work aimed at the command that is *running*, not at
            # the shell before it. A program that draws a window and reads
            # input can only be sent input while it is up, which is after
            # the line that starts it and before the settle that waits on
            # it - here.
            if last:
                do_clicks(post_clicks)
            if last and stop_when:
                wait_for(serial_path, settle, stop_when)
            else:
                time.sleep(settle)

        # Typed after the waiting is over rather than before it, so that a
        # `sync` at the end of a run does not cost the run's whole settle.
        # --stop-when-matched only shortens the *last* command's wait, and
        # the last command is rarely the interesting one.
        for cmd in post:
            mon.type(cmd + "\n", key_delay)
            time.sleep(4.0)

        # Last, so the picture is of the machine in the state the
        # assertions were made about - and taken before `quit`, which is
        # the only reason this is inside the try.
        if screenshot:
            mon.screendump(os.path.abspath(screenshot))

        mon.close()
    finally:
        try:
            qemu.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            qemu.kill()
            qemu.wait()

    transcript = read_transcript(serial_path)
    if keep_serial is None:
        os.unlink(serial_path)
    return transcript


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", default="novaris.iso")
    ap.add_argument("--cmd", action="append", default=[],
                    help="a shell command to type (repeatable)")
    ap.add_argument("--setup", action="append", default=[],
                    help="a shell command to type before the run proper, "
                         "with a short pause rather than the full settle "
                         "(repeatable)")
    ap.add_argument("--script", help="file with one shell command per line")
    ap.add_argument("--boot-wait", type=float, default=14.0)
    ap.add_argument("--key-delay", type=float, default=0.035)
    ap.add_argument("--settle", type=float, default=2.5,
                    help="seconds to wait after each command before the next")
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--serial-log", help="keep the raw serial capture at this path")
    ap.add_argument("--memory", type=int, default=128,
                    help="guest RAM in MB")
    ap.add_argument("--disk",
                    help="raw disk image to attach as an IDE hard disk. "
                         "Written to in place, so pass a copy when the "
                         "original matters.")
    ap.add_argument("--expect", action="append", default=[],
                    help="regex that must appear in the transcript (repeatable)")
    ap.add_argument("--reject", action="append", default=[],
                    help="regex that must NOT appear in the transcript (repeatable)")
    ap.add_argument("--click", action="append", default=[],
                    help="X,Y[,double] - move the pointer there and click, "
                         "before the --cmd lines are typed (repeatable). "
                         "This is how the desktop itself is driven: "
                         "double-clicking a .exe in the File Explorer is a "
                         "thing no amount of typing can test.")
    ap.add_argument("--post-click", action="append", default=[],
                    help="X,Y[,double|move] - the same, but done after the "
                         "last --cmd has been typed, so it reaches the "
                         "program that command started rather than the "
                         "shell that started it (repeatable)")
    ap.add_argument("--click-settle", type=float, default=1.5,
                    help="seconds to wait after each --click")
    ap.add_argument("--post-cmd", action="append", default=[],
                    help="a shell command typed after the settle has "
                         "finished, with a short fixed pause (repeatable)")
    ap.add_argument("--screenshot",
                    help="write the framebuffer to this PPM at the end of "
                         "the run. Some things are only true on screen: a "
                         "window drawn by a ring-3 process leaves no trace "
                         "in a serial transcript.")
    ap.add_argument("--http-dir",
                    help="serve this directory to the guest. The port is "
                         "chosen per run and substituted for $HTTP in every "
                         "--cmd, so a test writes "
                         "'fetch http://10.0.2.2:$HTTP/x'. The guest has no "
                         "route to the outside world here, and a test that "
                         "needed one would fail when the outside world did - "
                         "so the server it fetches from is this machine.")
    ap.add_argument("--stop-when-matched", action="store_true",
                    help="end the last command's settle as soon as every "
                         "--expect pattern has appeared, instead of always "
                         "waiting it out")
    args = ap.parse_args()

    # The server the guest fetches from, if the test asked for one.
    http_proc = None
    http_forward = None
    if args.http_dir:
        http_forward = 34000 + (os.getpid() % 20000)
        http_proc = subprocess.Popen(
            [sys.executable, "-m", "http.server", str(http_forward),
             "--bind", "0.0.0.0", "--directory", args.http_dir],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)

    commands = list(args.cmd)
    if args.script:
        with open(args.script) as fh:
            commands += [ln.strip() for ln in fh
                         if ln.strip() and not ln.startswith("#")]

    if http_forward:
        commands = [c.replace("$HTTP", str(http_forward)) for c in commands]
        args.post_cmd = [c.replace("$HTTP", str(http_forward))
                         for c in args.post_cmd]

    transcript = run(args.iso, commands, args.boot_wait, args.key_delay,
                     args.settle, args.timeout, args.serial_log, args.memory,
                     args.disk, args.setup,
                     args.expect if args.stop_when_matched else (),
                     args.click, args.click_settle, args.post_cmd,
                     args.screenshot, args.post_click, http_forward)
    if http_proc:
        http_proc.terminate()
        http_proc.wait()

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
