/* icons.c - the shell's pictograms, drawn from primitives. See
 * include/icons.h for why there are no image files involved. */

#include "icons.h"
#include "apps.h"
#include "gfx.h"

/* Colors picked to read as Windows: a saturated system blue, a manila
 * folder, a near-black console. */
#define ICON_BLUE       GFX_RGB(0x00, 0x78, 0xD4)
#define ICON_BLUE_DEEP  GFX_RGB(0x00, 0x5A, 0x9E)
#define ICON_BLUE_LIGHT GFX_RGB(0x4C, 0xC2, 0xFF)
#define ICON_FOLDER     GFX_RGB(0xFF, 0xC4, 0x3D)
#define ICON_FOLDER_DIM GFX_RGB(0xE0, 0xA1, 0x1E)
#define ICON_CONSOLE    GFX_RGB(0x0C, 0x0C, 0x0C)
#define ICON_PAPER      GFX_RGB(0xFC, 0xFC, 0xFC)
#define ICON_INK        GFX_RGB(0x4A, 0x4A, 0x52)
#define ICON_RED        GFX_RGB(0xE8, 0x11, 0x23)
#define ICON_STEEL      GFX_RGB(0x9A, 0xA5, 0xB4)

void icon_window_logo(int x, int y, int size, uint32_t color) {
    /* Four panes and a cross-shaped gutter - the shape the Start button
     * has carried since Windows 8 flattened the flag. */
    int gap = size / 8;
    if (gap < 2) gap = 2;
    int a = (size - gap) / 2;      /* left/top pane */
    int b = size - gap - a;        /* right/bottom pane, absorbs the odd pixel */
    gfx_fill(x, y, a, a, color);
    gfx_fill(x + a + gap, y, b, a, color);
    gfx_fill(x, y + a + gap, a, b, color);
    gfx_fill(x + a + gap, y + a + gap, b, b, color);
}

/* --- app icons ---------------------------------------------------------- */

static void icon_terminal(int x, int y, int size) {
    int r = size / 8;
    gfx_round_rect(x, y, size, size, r, ICON_CONSOLE);
    gfx_round_frame(x, y, size, size, r, 1, GFX_ARGB(0x66, 0xFF, 0xFF, 0xFF));
    /* The title strip, so it reads as a console window rather than a
     * black square. */
    gfx_fill(x + 2, y + size / 6, size - 4, 1, GFX_ARGB(0x50, 0xFF, 0xFF, 0xFF));

    /* A chevron and a caret, scaled by hand: the mono font's 8x16 cell is
     * the wrong size for anything but the largest icon. */
    int px = x + size * 26 / 100, py = y + size * 42 / 100;
    int t = size / 12;
    if (t < 1) t = 1;
    for (int i = 0; i < size / 6; i++) {
        gfx_fill(px + i, py + i, t, t, GFX_RGB(0xFF, 0xFF, 0xFF));
        gfx_fill(px + i, py + size / 3 - i - t, t, t, GFX_RGB(0xFF, 0xFF, 0xFF));
    }
    gfx_fill(x + size * 52 / 100, y + size * 66 / 100, size / 4, t,
             GFX_RGB(0xFF, 0xFF, 0xFF));
}

static void icon_explorer(int x, int y, int size) {
    /* A manila folder, open: back panel, tab, lighter front panel. */
    int fx = x + size / 12, fw = size - size / 6;
    int top = y + size / 5;
    gfx_round_rect(fx, top, fw * 45 / 100, size / 8 + 2, 2, ICON_FOLDER_DIM);
    gfx_round_rect(fx, top + size / 10, fw, size * 55 / 100, 3, ICON_FOLDER_DIM);
    gfx_round_rect_gradient(fx + size / 16, top + size * 18 / 100,
                            fw - size / 8, size * 47 / 100, 3,
                            GFX_RGB(0xFF, 0xD9, 0x76), ICON_FOLDER);
    gfx_round_frame(fx, top + size / 10, fw, size * 55 / 100, 3, 1,
                    GFX_ARGB(0x40, 0, 0, 0));
}

static void icon_task_manager(int x, int y, int size) {
    int r = size / 10;
    gfx_round_rect(x, y, size, size, r, GFX_RGB(0xF7, 0xF9, 0xFC));
    gfx_round_frame(x, y, size, size, r, 1, GFX_ARGB(0x50, 0x00, 0x3A, 0x66));
    gfx_fill(x + 2, y + 2, size - 4, size / 7, ICON_BLUE);

    /* Four bars, one of them in the red the CPU graph turns under load. */
    static const int bars[4] = { 34, 62, 46, 80 };
    int bw = size / 8;
    int base = y + size * 82 / 100;
    for (int i = 0; i < 4; i++) {
        int bh = size * bars[i] / 200;
        gfx_fill(x + size / 6 + i * (bw + size / 14), base - bh, bw, bh,
                 i == 3 ? ICON_RED : ICON_BLUE);
    }
}

static void icon_about(int x, int y, int size) {
    int r = size / 8;
    gfx_round_rect_gradient(x, y, size, size, r, ICON_BLUE_LIGHT, ICON_BLUE_DEEP);
    gfx_round_frame(x, y, size, size, r, 1, GFX_ARGB(0x60, 0xFF, 0xFF, 0xFF));
    gfx_sparkle(x + size / 2, y + size / 2, size * 34 / 100,
                GFX_RGB(0xFF, 0xFF, 0xFF));
}

void icon_document(int x, int y, int size) {
    int w = size * 3 / 4, h = size;
    int px = x + (size - w) / 2;
    int fold = size / 3;

    gfx_fill(px, y, w, h, ICON_PAPER);
    gfx_frame(px, y, w, h, GFX_ARGB(0x55, 0, 0, 0));
    /* The dog-ear, drawn as a shaded triangle in the corner rather than a
     * cut-out: there is no "erase" here, only paint, and the icon doesn't
     * know what color the background behind it is. */
    for (int i = 0; i < fold; i++) {
        gfx_fill(px + w - fold + i, y + 1, fold - i - 1, 1,
                 GFX_RGB(0xDD, 0xE3, 0xEC));
        gfx_pixel(px + w - fold + i, y + i, GFX_ARGB(0x77, 0, 0, 0));
    }
    gfx_fill(px + w - fold, y, 1, fold, GFX_ARGB(0x55, 0, 0, 0));

    for (int i = 0; i < 4; i++) {
        int ly = y + fold + size / 8 + i * (size / 7);
        if (ly > y + h - 3) break;
        gfx_fill(px + size / 8, ly, w - size / 4 - (i == 3 ? size / 5 : 0), 1,
                 ICON_INK);
    }
}

void icon_folder(int x, int y, int size) { icon_explorer(x, y, size); }

static void icon_generic(int x, int y, int size) {
    int r = size / 8;
    gfx_round_rect_gradient(x, y, size, size, r, GFX_RGB(0xD8, 0xDE, 0xE8),
                            GFX_RGB(0x9E, 0xA8, 0xB8));
    gfx_round_frame(x, y, size, size, r, 1, GFX_ARGB(0x50, 0, 0, 0));
}

void icon_app(const app_t* app, int x, int y, int size) {
    if (app == &app_terminal)      icon_terminal(x, y, size);
    else if (app == &app_files)    icon_explorer(x, y, size);
    else if (app == &app_monitor)  icon_task_manager(x, y, size);
    else if (app == &app_about)    icon_about(x, y, size);
    else if (app == &app_textview) icon_document(x, y, size);
    else if (app == &app_alert)    icon_about(x, y, size);
    else                           icon_generic(x, y, size);
}

/* --- shell icons -------------------------------------------------------- */

void icon_computer(int x, int y, int size) {
    /* A monitor on a stand - "This PC", near enough. */
    int w = size, h = size * 7 / 10;
    gfx_round_rect(x, y, w, h, 2, GFX_RGB(0x3A, 0x42, 0x50));
    gfx_round_rect(x + 2, y + 2, w - 4, h - 5, 1, ICON_BLUE_DEEP);
    gfx_round_rect_gradient(x + 3, y + 3, w - 6, h - 7, 1, ICON_BLUE_LIGHT,
                            ICON_BLUE);
    gfx_fill(x + w / 2 - size / 10, y + h, size / 5, size / 8, ICON_STEEL);
    gfx_round_rect(x + w / 4, y + h + size / 8, w / 2, size / 10, 2, ICON_STEEL);
}

void icon_recycle_bin(int x, int y, int size) {
    /* A translucent bin: lid, body, three ribs. Novaris has no writable
     * storage, so it is always empty and always drawn that way. */
    int w = size * 62 / 100, h = size * 72 / 100;
    int bx = x + (size - w) / 2, by = y + size - h;

    gfx_round_rect(bx - size / 12, by - size / 8, w + size / 6, size / 10, 2,
                   GFX_ARGB(0xCC, 0xC8, 0xD4, 0xE2));
    gfx_round_rect(bx + w / 3, by - size * 20 / 100, w / 3, size / 12, 2,
                   GFX_ARGB(0xCC, 0xC8, 0xD4, 0xE2));
    gfx_round_rect(bx, by, w, h, 3, GFX_ARGB(0xB8, 0xDA, 0xE4, 0xF0));
    gfx_round_frame(bx, by, w, h, 3, 1, GFX_ARGB(0x66, 0x40, 0x50, 0x66));
    for (int i = 1; i < 3; i++) {
        gfx_fill(bx + w * i / 3, by + size / 12, 1, h - size / 6,
                 GFX_ARGB(0x66, 0x50, 0x60, 0x78));
    }
}

/* --- glyphs ------------------------------------------------------------- */

void icon_search(int x, int y, int size, uint32_t color) {
    int cx = x + size * 42 / 100, cy = y + size * 42 / 100;
    int r = size * 30 / 100;
    gfx_ring(cx, cy, r, 1, color);
    for (int i = 0; i < size / 3; i++) {
        gfx_pixel(cx + r * 7 / 10 + i, cy + r * 7 / 10 + i, color);
        gfx_pixel(cx + r * 7 / 10 + i + 1, cy + r * 7 / 10 + i, color);
    }
}

void icon_power(int x, int y, int size, uint32_t color) {
    int cx = x + size / 2, cy = y + size / 2 + size / 12;
    int r = size * 32 / 100;
    /* A ring with the vertical stroke drawn straight through its top. The
     * real glyph breaks the ring at the stroke; nothing here can erase a
     * pixel, so the stroke simply overprints it, which at 16px is a
     * difference nobody can see. */
    gfx_ring(cx, cy, r, 1, color);
    gfx_fill(cx, cy - r - size / 6, 1, r + size / 6, color);
}

void icon_network(int x, int y, int size, uint32_t color) {
    /* Four rising bars, the signal-strength shape. */
    int bw = size / 6;
    if (bw < 2) bw = 2;
    for (int i = 0; i < 4; i++) {
        int bh = size * (3 + i * 2) / 12;
        gfx_fill(x + i * (bw + 1), y + size - bh, bw, bh, color);
    }
}

void icon_volume(int x, int y, int size, uint32_t color) {
    /* A speaker: a box, a horn widening away from it, and one sound wave.
     * The wave is a bare arc rather than a ring - at the 14px the system
     * tray draws this at, a full ring is indistinguishable from a blob. */
    int cy = y + size / 2;
    int bh = size / 3;
    int bx = x + 1, bw = size / 5;
    if (bw < 2) bw = 2;
    gfx_fill(bx, cy - bh / 2, bw, bh, color);

    int cone = size / 3;
    for (int i = 0; i < cone; i++) {
        int h = bh + (size - bh) * (i + 1) / cone;
        gfx_fill(bx + bw + i, cy - h / 2, 1, h, color);
    }

    int r = size * 34 / 100;
    int ax = bx + bw + cone + 1;
    for (int dy = -r; dy <= r; dy++) {
        int d2 = r * r - dy * dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) <= d2) dx++;
        gfx_pixel(ax + dx, cy + dy, color);
    }
}

void icon_chevron_up(int x, int y, int size, uint32_t color) {
    int cx = x + size / 2, cy = y + size / 2 + size / 6;
    for (int i = 0; i < size / 3; i++) {
        gfx_pixel(cx - i, cy - i, color);
        gfx_pixel(cx - i, cy - i + 1, color);
        gfx_pixel(cx + i, cy - i, color);
        gfx_pixel(cx + i, cy - i + 1, color);
    }
}
