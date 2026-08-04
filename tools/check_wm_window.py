#!/usr/bin/env python3
"""Asserts that a ring-3 process's window is actually on screen.

Milestone 36. The serial transcript can say that /dev/wm opened, that a
window was created, that its pixels were mapped and a rectangle damaged -
and every one of those can be true with nothing visible, because the last
step is a compositor blit that no syscall reports on. The picture is the
only place that fact lives.

So this reads the PPM `make test-wm` leaves behind and looks for what
userland/wm_test.c draws, which was chosen to be unmistakable:

  - a blue-green vertical gradient, dark at the top and lighter at the
    bottom, which no kernel app draws (they are flat fills and chrome);
  - a white 2px frame around it;
  - a red marker block inside.

Finding all three in one rectangular region is not something a mis-blit,
a stale back buffer or a wallpaper can do by accident. Deliberately not a
pixel-exact comparison against a golden image: the marker walks, the
window cascades, and a test that fails when the marker is six pixels
further right is a test that gets deleted.
"""

import sys


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()

    # P6 <w> <h> <maxval>, whitespace-separated, comments start with '#'.
    fields = []
    i = 0
    while len(fields) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i:i + 1] != b"\n":
                i += 1
            continue
        start = i
        while i < len(data) and not data[i:i + 1].isspace():
            i += 1
        fields.append(data[start:i])
    i += 1  # the single whitespace byte after maxval

    if fields[0] != b"P6":
        raise ValueError("not a binary PPM: %r" % fields[0])
    w, h, maxval = int(fields[1]), int(fields[2]), int(fields[3])
    if maxval != 255:
        raise ValueError("unexpected maxval %d" % maxval)
    return w, h, data[i:i + w * h * 3]


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: check_wm_window.py <screenshot.ppm>\n")
        return 2
    path = argv[1]

    try:
        w, h, px = read_ppm(path)
    except (OSError, ValueError) as exc:
        sys.stderr.write("check_wm_window: cannot read %s: %s\n" % (path, exc))
        return 1

    def at(x, y):
        o = (y * w + x) * 3
        return px[o], px[o + 1], px[o + 2]

    # The gradient's own definition is the test, and the discriminating
    # channel is red: wm_test.c holds it at exactly 0x20 for every pixel
    # of the background while green and blue climb. The desktop wallpaper
    # is also a blue gradient - that is why this has to be exact rather
    # than a range - but its red channel runs 5..7, so the two do not
    # overlap anywhere.
    gradient = 0
    white = 0
    red = 0
    per_col = [0] * w
    per_row = [0] * h

    for y in range(h):
        for x in range(w):
            r, g, b = at(x, y)
            if r == 0x20 and 35 <= g <= 165 and 55 <= b <= 215 and b > g + 15:
                gradient += 1
                per_col[x] += 1
                per_row[y] += 1
            elif r > 240 and g > 240 and b > 240:
                white += 1
            elif r > 200 and g < 90 and b < 90:
                red += 1

    # The extent of the window, as the run of rows and columns that are
    # *mostly* gradient, rather than the bounding box of every matching
    # pixel. A window icon or an anti-aliased glyph elsewhere on screen
    # can contribute a handful of matching pixels and stretch a bounding
    # box across the display; it cannot make a column 100 pixels deep.
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

    x0, bw = extent(per_col, 100)
    y0, bh = extent(per_row, 100)

    # The window is 480x320 of client area. The bounding box is allowed to
    # be a little smaller (the marker and the input band cover part of it)
    # but not larger: a gradient wider than the window means the test is
    # matching something that is not the window.
    checks = [
        ("the window's gradient is on screen", gradient > 80000),
        ("it is exactly one window wide", 440 <= bw <= 480),
        ("and one window tall", 280 <= bh <= 320),
        ("its white frame is there", white > 500),
        ("the red marker is drawn", red > 500),
    ]

    ok = True
    print("check_wm_window: %s (%dx%d)" % (path, w, h))
    print("  gradient pixels: %d, a solid %dx%d block at (%d,%d)"
          % (gradient, bw, bh, x0, y0))
    print("  white pixels: %d   red pixels: %d" % (white, red))
    for label, passed in checks:
        print("  %s %s" % ("[ok]  " if passed else "[FAIL]", label))
        ok = ok and passed

    if not ok:
        sys.stderr.write("\nNo ring-3 window found in the screenshot.\n")
        return 1
    print("\nA window drawn by a ring-3 process is on the desktop.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
