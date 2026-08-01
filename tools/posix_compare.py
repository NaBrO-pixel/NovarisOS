#!/usr/bin/env python3
"""Run one binary on Linux and on Novaris, and diff the two transcripts.

This is the verification for ROADMAP.md Milestone 18. userland/posix_test.c
is written for Linux - raw `int $0x80`, Linux syscall numbers, built with
an ordinary `gcc -m32 -static -nostdlib` and linked against nothing. The
claim being tested is that Novaris implements that ABI rather than
something ABI-shaped, and the way to test it is to run the *same bytes*
both places and compare.

Exactly one line is expected to differ: uname()'s sysname, which says
Linux on the host and Novaris in QEMU. That the difference is there at all
is useful - it proves the QEMU transcript really came from Novaris and not
from a stale host run.

Usage:
    tools/posix_compare.py --iso novaris.iso --binary build/user/posixtest.elf
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Each test names the first and last line of its own output, plus any line
# that is *expected* to differ between the two systems, as
# (host substring, novaris substring). Everything else must match exactly.
TESTS = {
    "posixtest.elf": {
        "start": "posix syscall test",
        "end": "posix test done.",
        # uname is the one thing that should differ - and its differing is
        # useful, since it proves the QEMU transcript really came from
        # Novaris rather than from a stale host run.
        "expected": [("sysname = Linux", "sysname = Novaris")],
    },
    "glibc.elf": {
        "start": "hello from a real glibc-linked program",
        "end": "sorted:",
        "expected": [],   # a real C library, and nothing may differ
    },
    "pthtest.elf": {
        "start": "posix thread test",
        "end": "posix thread test done.",
        "expected": [],   # nothing at all may differ here either
    },
    "sigtest.elf": {
        "start": "signal test",
        "end": "signal test done.",
        "expected": [],   # nothing at all may differ here
    },
}


def extract(text, start, end):
    """The test's own output, from its banner to its last line."""
    lines = text.replace("\r", "").replace("\x00", "").split("\n")
    try:
        first = next(i for i, l in enumerate(lines) if start in l)
        last = next(i for i, l in enumerate(lines) if end in l)
    except StopIteration:
        return None
    return [l.rstrip() for l in lines[first:last + 1]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--name", default="posixtest.elf",
                    help="the binary's name on the initrd")
    args = ap.parse_args()

    spec = TESTS.get(args.name)
    if spec is None:
        print(f"FAIL: no comparison spec for {args.name}")
        return 1
    start, end, expected = spec["start"], spec["end"], spec["expected"]

    if not os.access(args.binary, os.X_OK):
        os.chmod(args.binary, 0o755)

    # The host run has to happen from a directory holding hello.txt, since
    # the test opens it by name.
    cwd = os.path.join(HERE, "..", "userland", "initrd_files")
    # stderr is merged into stdout rather than captured separately: the
    # test deliberately writes one line to fd 2, and on Novaris both
    # descriptors are the same console, so the host has to interleave them
    # the same way for the transcripts to line up.
    host = subprocess.run([os.path.abspath(args.binary)], cwd=cwd,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=60)
    host_lines = extract(host.stdout, start, end)
    if not host_lines:
        print("FAIL: the binary produced no recognisable output on the host")
        print(host.stdout)
        return 1

    guest = subprocess.run(
        [sys.executable, os.path.join(HERE, "qemu_test.py"),
         "--iso", args.iso, "--cmd", "run " + args.name],
        capture_output=True, text=True, timeout=900)
    guest_lines = extract(guest.stdout, start, end)
    if not guest_lines:
        print("FAIL: the binary produced no recognisable output on Novaris")
        print(guest.stdout[-3000:])
        return 1

    if len(host_lines) != len(guest_lines):
        print("FAIL: transcripts have different lengths "
              f"({len(host_lines)} host vs {len(guest_lines)} Novaris)")
        for h, g in zip(host_lines, guest_lines):
            if h != g:
                print(f"  host:    {h}\n  novaris: {g}")
        return 1

    failures = [l for l in guest_lines if "[FAIL]" in l]
    differences = [(h, g) for h, g in zip(host_lines, guest_lines) if h != g]

    def is_expected(h, g):
        return any(eh in h and eg in g for eh, eg in expected)

    for h, g in differences:
        print(("  expected difference: " if is_expected(h, g)
               else "  UNEXPECTED: ")
              + f"host {h.strip()!r} vs novaris {g.strip()!r}")

    unexpected = [(h, g) for h, g in differences if not is_expected(h, g)]

    checks = sum(1 for l in guest_lines if "[ok]" in l or "[FAIL]" in l)
    print(f"{len(host_lines)} lines compared, {checks} checks, "
          f"{len(failures)} failing, {len(unexpected)} unexpected difference(s)")

    if failures or unexpected:
        for l in failures:
            print("  " + l.strip())
        return 1

    print(f"PASS: {args.name} behaves identically on Linux and Novaris")
    return 0


if __name__ == "__main__":
    sys.exit(main())
