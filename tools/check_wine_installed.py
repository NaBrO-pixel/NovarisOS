#!/usr/bin/env python3
"""Refuses to run a Wine test against an OS image that has no Wine in it.

Since Milestone 35 there is one ISO, and whether Wine is in it depends on
whether WINE_BUILD was set when it was built. Without this check a
`make test-wine` on an image built without Wine fails fifteen minutes
later with "No Wine installed" buried in a serial transcript, which is a
bad way to be told to pass a variable.

Usage: check_wine_installed.py <initrd-staging-dir>
"""
import os
import sys

REQUIRED = [
    "usr/bin/wine",
    "usr/lib/wine/i386-unix/ntdll.so",
    "usr/lib/wine/i386-windows/ntdll.dll",
    "usr/share/wine/wine.inf",
]


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <initrd-staging-dir>", file=sys.stderr)
        return 2

    root = sys.argv[1]
    missing = [p for p in REQUIRED if not os.path.isfile(os.path.join(root, p))]
    if not missing:
        return 0

    print("This OS image was built without Wine, so there is nothing to test.",
          file=sys.stderr)
    print("", file=sys.stderr)
    print("  make WINE_BUILD=/path/to/a/built/wine", file=sys.stderr)
    print("", file=sys.stderr)
    print("installs it. ROADMAP.md has the two commands that build one.",
          file=sys.stderr)
    if len(missing) != len(REQUIRED):
        print("", file=sys.stderr)
        print("(Partially installed - missing: %s)" % ", ".join(missing),
              file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
