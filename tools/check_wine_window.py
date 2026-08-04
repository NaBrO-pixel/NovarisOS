#!/usr/bin/env python3
"""Asserts that a Windows program is on screen, under real Wine.

Milestone 37. The serial transcript can say a great deal about a Wine run
and nothing at all about whether a window appeared: every call between
GDI and the framebuffer succeeds silently, and the one line that used to
prove the *absence* of a window - "err:winediag:nodrv_CreateWindow" - only
proves that no driver loaded, not that one worked.

So this reads the screendump and looks for what a Win32 window is made
of: a large solid block of 0xD4D0C8. That is COLOR_3DFACE, the grey every
dialog, toolbar and control on Windows has been painted since 1995, and
it is the background of a default Wine window. Nothing else on the
Novaris desktop is that colour - the wallpaper is blue, the chrome is
near-black, the terminal is near-black - so a block of it several hundred
pixels on a side is a Windows program and nothing else.

Deliberately not looking for the *title*: reading "Untitled - Notepad"
out of a screenshot means OCR, and a test that needs OCR to decide
whether it passed is a test nobody trusts.
"""

import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from check_wm_window import read_ppm     # the PPM reader, not the checks


# COLOR_3DFACE, with a little tolerance for nothing in particular - the
# blit is exact, but a future compositor that dims inactive windows would
# not be.
FACE = (212, 208, 200)
TOLERANCE = 4


def main(argv):
    if len(argv) not in (2, 3):
        sys.stderr.write("usage: check_wine_window.py <screenshot.ppm> [min-side]\n")
        return 2
    path = argv[1]
    min_side = int(argv[2]) if len(argv) > 2 else 300

    try:
        w, h, px = read_ppm(path)
    except (OSError, ValueError) as exc:
        sys.stderr.write("check_wine_window: cannot read %s: %s\n" % (path, exc))
        return 1

    per_col = [0] * w
    per_row = [0] * h
    total = 0

    for y in range(h):
        base = y * w * 3
        for x in range(w):
            o = base + x * 3
            if (abs(px[o] - FACE[0]) <= TOLERANCE and
                    abs(px[o + 1] - FACE[1]) <= TOLERANCE and
                    abs(px[o + 2] - FACE[2]) <= TOLERANCE):
                total += 1
                per_col[x] += 1
                per_row[y] += 1

    # The longest run of rows and of columns that are mostly this colour,
    # rather than the bounding box of every matching pixel: a single grey
    # button somewhere else must not stretch the answer across the screen.
    def extent(counts, floor):
        best = run = start = best_start = 0
        for i, n in enumerate(counts):
            if n >= floor:
                if run == 0:
                    start = i
                run += 1
                if run > best:
                    best, best_start = run, start
            else:
                run = 0
        return best_start, best

    x0, bw = extent(per_col, min_side // 2)
    y0, bh = extent(per_row, min_side // 2)

    checks = [
        ("a Win32 window background is on screen", total > min_side * min_side),
        ("it is a solid block, at least %dpx wide" % min_side, bw >= min_side),
        ("and at least %dpx tall" % min_side, bh >= min_side),
    ]

    ok = True
    print("check_wine_window: %s (%dx%d)" % (path, w, h))
    print("  COLOR_3DFACE pixels: %d, a solid %dx%d block at (%d,%d)"
          % (total, bw, bh, x0, y0))
    for label, passed in checks:
        print("  %s %s" % ("[ok]  " if passed else "[FAIL]", label))
        ok = ok and passed

    if not ok:
        sys.stderr.write("\nNo Windows program window found in the screenshot.\n")
        return 1
    print("\nA Windows program is on the desktop, drawn by Wine through /dev/wm.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
