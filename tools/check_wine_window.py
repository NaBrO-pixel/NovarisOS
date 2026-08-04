#!/usr/bin/env python3
"""Asserts that a Windows program is on screen, under real Wine.

Milestone 37. The serial transcript can say a great deal about a Wine run
and nothing at all about whether a window appeared: every call between
GDI and the framebuffer succeeds silently, and the one line that used to
prove the *absence* of a window - "err:winediag:nodrv_CreateWindow" - only
proves that no driver loaded, not that one worked.

So this reads the screendump and looks for what a Win32 window is made
of: a large block of pure white - COLOR_WINDOW, the document area - with
0xD4D0C8 around it, COLOR_3DFACE, the grey every menu bar, toolbar and
status bar on Windows has been painted since 1995. Nothing else on this
desktop is either colour: the wallpaper is blue, the terminal near-black,
and Novaris's own title bars are 250,250,250 rather than 255.

Both, rather than either. An earlier version looked for the grey alone
and passed while Notepad had no text in it at all - with no font engine
the whole window came out one flat grey, menu bar and document area
alike. Chrome *around* a document is the shape of a window that works.

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

# COLOR_WINDOW: the white a document area is painted. Pure white, and
# nothing else here is - Novaris's own title bars are 250,250,250 and its
# chrome is lighter still but never 255. Checked separately from the grey
# because the two together are what a *working* window looks like: chrome
# around a document. A window with no font engine behind it is all grey,
# which is how this test used to pass while Notepad had no text in it.
WINDOW = (255, 255, 255)


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
    face = 0

    for y in range(h):
        base = y * w * 3
        for x in range(w):
            o = base + x * 3
            if (px[o], px[o + 1], px[o + 2]) == WINDOW:
                total += 1
                per_col[x] += 1
                per_row[y] += 1
            elif (abs(px[o] - FACE[0]) <= TOLERANCE and
                    abs(px[o + 1] - FACE[1]) <= TOLERANCE and
                    abs(px[o + 2] - FACE[2]) <= TOLERANCE):
                face += 1

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
        ("a Win32 document area is on screen", total > min_side * min_side),
        ("it is a solid block, at least %dpx wide" % min_side, bw >= min_side),
        ("and at least %dpx tall" % min_side, bh >= min_side),
        ("with Win32 chrome around it", face > 2000),
    ]

    ok = True
    print("check_wine_window: %s (%dx%d)" % (path, w, h))
    print("  COLOR_WINDOW pixels: %d, a solid %dx%d block at (%d,%d)"
          % (total, bw, bh, x0, y0))
    print("  COLOR_3DFACE pixels: %d" % face)
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
