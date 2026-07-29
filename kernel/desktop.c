/* desktop.c - the desktop environment: wallpaper, menu bar, Dock,
 * Spotlight, and the event loop that drives all of it.
 *
 * The frame loop is the heart of the file, and it is deliberately not
 * "redraw everything at 60Hz". Nothing here has a GPU; a full 1280x800
 * recomposite is a megapixel of work. So every source of change - a
 * window moving, the clock ticking over, the pointer sliding two pixels -
 * reports the rectangle it affected, and each frame recomposites only the
 * union of those rectangles:
 *
 *     wallpaper -> windows (back to front) -> menu bar -> Dock -> cursor
 *
 * always in that order, always from scratch within the damaged region.
 * That's why translucency works: the menu bar can blend with whatever is
 * genuinely underneath it, because whatever is underneath it was just
 * drawn. It's also why the pointer can slide over a window without
 * corrupting it - there is no "save the pixels underneath" anywhere.
 */

#include "desktop.h"
#include "apps.h"
#include "wm.h"
#include "gfx.h"
#include "uikit.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "mouse.h"
#include "kheap.h"
#include "kstring.h"
#include "pit.h"
#include "rtc.h"
#include "vfs.h"
#include "io.h"
#include "console.h"
#include "shell.h"

/* --- layout ------------------------------------------------------------- */

#define MENUBAR_H     26
#define DOCK_ICON     42
#define DOCK_GAP      14
#define DOCK_PAD      10
#define DOCK_BOTTOM   8
#define DOCK_H        (DOCK_ICON + DOCK_PAD * 2)
#define DOCK_MAX_SCALE 130   /* percent, at the pointer */
#define DOCK_INFLUENCE 90    /* pixels of magnification falloff */
#define DOCK_MAX_ITEMS (8 + WM_MAX_WINDOWS)

#define MENU_ITEM_H   24
#define MENU_SEP_H    9
#define MENU_MAX_ITEMS 20
#define MENU_PAD_Y    6
#define MENU_TEXT_X   30   /* label inset: leaves room for a check mark */
#define MENU_RIGHT_PAD 14

#define SPOT_W        560
#define SPOT_FIELD_H  60
#define SPOT_ROW_H    42
#define SPOT_MAX_ROWS 5

static gfx_surface_t* screen;      /* the frame being assembled */
static gfx_surface_t* wallpaper;   /* rendered once at startup */
static int screen_w, screen_h;

static int cursor_x, cursor_y;
static int prev_cursor_x, prev_cursor_y;

static uint32_t last_tick;
static int last_minute = -1;
static uint32_t last_generation;

/* --- menus -------------------------------------------------------------- */

enum {
    ACT_NONE, ACT_ABOUT, ACT_SYSTEM_REPORT, ACT_SOFTWARE_UPDATE,
    ACT_RESTART, ACT_SHUT_DOWN, ACT_NEW_TERMINAL, ACT_OPEN_FILES,
    ACT_CLOSE_WINDOW, ACT_MINIMIZE, ACT_ZOOM, ACT_BRING_ALL_FRONT,
    ACT_HIDE_APP, ACT_QUIT_APP, ACT_HELP, ACT_SELECT_WINDOW, ACT_SPOTLIGHT,
};

typedef struct {
    char label[44];
    char shortcut[10];
    uint8_t separator;
    uint8_t enabled;
    uint8_t checked;
    int action;
    int arg;
} menu_item_t;

static menu_item_t menu_items[MENU_MAX_ITEMS];
static int menu_item_count;
static int open_menu = -1;         /* index into the menu bar titles */
static int menu_hover = -1;        /* hovered item within the open menu */
static gfx_rect_t menu_rect;

/* Menu bar titles, laid out left to right. Index 0 is the logo. */
#define MENU_TITLE_COUNT 5
static const char* menu_titles[MENU_TITLE_COUNT] = {
    "", "App", "File", "Window", "Help"
};
static int title_x[MENU_TITLE_COUNT];
static int title_w[MENU_TITLE_COUNT];

/* --- dock --------------------------------------------------------------- */

enum { DOCK_APP, DOCK_WINDOW, DOCK_TRASH };

typedef struct {
    int kind;
    const app_t* app;
    window_t* win;      /* for minimized windows */
    const char* label;
    int cx;             /* center x, after magnification */
    int size;
    int running;
} dock_item_t;

static dock_item_t dock_items[DOCK_MAX_ITEMS];
static int dock_count;
static gfx_rect_t dock_rect;
static int dock_hover = -1;
static int dock_bounce_item = -1;
static uint32_t dock_bounce_start;

/* --- spotlight ---------------------------------------------------------- */

static int spotlight_open;
static char spotlight_query[48];
static int spotlight_len;
static int spotlight_selected;
static gfx_rect_t spotlight_rect;

typedef struct {
    char name[64];
    char subtitle[40];
    int action;      /* ACT_* or -1 for "open this file" */
    char file[64];
} spot_result_t;

static spot_result_t spot_results[SPOT_MAX_ROWS];
static int spot_result_count;

/* --- wallpaper ---------------------------------------------------------- */

static uint32_t mix(uint32_t a, uint32_t b, int t /* 0-255 */) {
    int it = 255 - t;
    return GFX_RGB((GFX_RED(a) * it + GFX_RED(b) * t) / 255,
                   (GFX_GREEN(a) * it + GFX_GREEN(b) * t) / 255,
                   (GFX_BLUE(a) * it + GFX_BLUE(b) * t) / 255);
}

/* An aurora: a vertical base gradient with two big soft light sources
 * bled into it. Computed per pixel once at startup and then kept, because
 * every damaged region of every frame starts by copying from it. */
static void render_wallpaper(void) {
    const uint32_t top = GFX_RGB(0x0A, 0x10, 0x30);
    const uint32_t bottom = GFX_RGB(0x1A, 0x24, 0x54);
    const uint32_t glow_a = GFX_RGB(0x7B, 0x4B, 0xE0);   /* violet */
    const uint32_t glow_b = GFX_RGB(0x1E, 0x8F, 0xD9);   /* cyan-blue */

    int ax = screen_w * 30 / 100, ay = screen_h * 22 / 100;
    int bx = screen_w * 76 / 100, by = screen_h * 72 / 100;
    int ra = screen_h * 75 / 100, rb = screen_h * 62 / 100;

    for (int y = 0; y < screen_h; y++) {
        uint32_t* row = &wallpaper->pixels[y * wallpaper->pitch];
        uint32_t base = mix(top, bottom, y * 255 / (screen_h - 1));
        for (int x = 0; x < screen_w; x++) {
            uint32_t color = base;

            int dx = x - ax, dy = y - ay;
            int d2 = dx * dx / 64 + dy * dy / 64;   /* scaled to stay in range */
            int r2 = ra * ra / 64;
            if (d2 < r2) {
                /* Quadratic falloff, so the light has a soft shoulder
                 * rather than a visible circular edge. */
                int t = (r2 - d2) * 255 / r2;
                color = mix(color, glow_a, t * t / 255 * 88 / 255);
            }
            dx = x - bx;
            dy = y - by;
            d2 = dx * dx / 64 + dy * dy / 64;
            r2 = rb * rb / 64;
            if (d2 < r2) {
                int t = (r2 - d2) * 255 / r2;
                color = mix(color, glow_b, t * t / 255 * 70 / 255);
            }
            row[x] = 0xFF000000u | color;
        }
    }

    /* A vignette, which is what stops the corners from looking flat. */
    gfx_set_target(wallpaper);
    for (int i = 0; i < 90; i++) {
        gfx_frame(i, i, screen_w - i * 2, screen_h - i * 2,
                  GFX_ARGB(2, 0, 0, 0));
    }
    gfx_set_target(screen);
}

/* --- icons -------------------------------------------------------------- */

/* Every Dock icon is drawn from scratch at whatever size magnification
 * asked for, which is what lets them scale smoothly under the pointer. */
static void draw_app_icon(const app_t* app, int x, int y, int size) {
    int r = size * 22 / 100;

    if (app == &app_terminal) {
        gfx_round_rect_gradient(x, y, size, size, r, GFX_RGB(0x3A, 0x3A, 0x42),
                                GFX_RGB(0x1A, 0x1A, 0x1F));
        gfx_round_frame(x, y, size, size, r, 1, GFX_ARGB(0x50, 0xFF, 0xFF, 0xFF));
        gfx_fill(x + size / 8, y + size / 6, size * 6 / 8, 1,
                 GFX_ARGB(0x60, 0xFF, 0xFF, 0xFF));
        int gx = x + size / 4, gy = y + size / 2 - 8;
        gfx_mono_text(gx, gy, ">", GFX_RGB(0x7C, 0xF6, 0x8A));
        gfx_fill(gx + 12, gy + 13, 10, 2, GFX_RGB(0xE8, 0xE8, 0xEE));
    } else if (app == &app_files) {
        /* a folder: back panel, tab, front panel */
        int fx = x + size / 10, fw = size * 8 / 10;
        gfx_round_rect(fx, y + size * 3 / 10, fw / 2, size * 2 / 10, 3,
                       GFX_RGB(0x5A, 0xB6, 0xF5));
        gfx_round_rect_gradient(fx, y + size * 7 / 20, fw, size * 45 / 100, 5,
                                GFX_RGB(0x63, 0xC2, 0xFF), GFX_RGB(0x1F, 0x84, 0xE0));
        gfx_round_frame(fx, y + size * 7 / 20, fw, size * 45 / 100, 5, 1,
                        GFX_ARGB(0x40, 0, 0, 0));
    } else if (app == &app_monitor) {
        gfx_round_rect(x, y, size, size, r, GFX_RGB(0xFA, 0xFA, 0xFD));
        gfx_round_frame(x, y, size, size, r, 1, GFX_ARGB(0x40, 0, 0, 0));
        static const int bars[5] = { 30, 55, 40, 75, 60 };
        int bw = size / 9;
        for (int i = 0; i < 5; i++) {
            int bh = size * bars[i] / 160;
            gfx_round_rect(x + size / 6 + i * (bw + 3), y + size * 3 / 4 - bh,
                           bw, bh, 1,
                           i == 3 ? GFX_RGB(0xFF, 0x6B, 0x5B)
                                  : GFX_RGB(0x33, 0x9A, 0xF5));
        }
    } else if (app == &app_about) {
        gfx_round_rect_gradient(x, y, size, size, r, GFX_RGB(0x4F, 0x7D, 0xFF),
                                GFX_RGB(0x21, 0x3E, 0xB8));
        gfx_round_frame(x, y, size, size, r, 1, GFX_ARGB(0x50, 0xFF, 0xFF, 0xFF));
        gfx_sparkle(x + size / 2, y + size / 2, size * 35 / 100,
                    GFX_RGB(0xFF, 0xFF, 0xFF));
    } else {
        gfx_round_rect_gradient(x, y, size, size, r, GFX_RGB(0xC8, 0xC8, 0xD2),
                                GFX_RGB(0x92, 0x92, 0x9E));
    }
}

static void draw_trash_icon(int x, int y, int size) {
    int w = size * 3 / 5, h = size * 7 / 10;
    int tx = x + (size - w) / 2, ty = y + size - h - size / 12;
    gfx_round_rect(tx - 3, ty - 6, w + 6, 4, 2, GFX_ARGB(0xCC, 0xE8, 0xE8, 0xF0));
    gfx_round_rect(tx + w / 3, ty - 10, w / 3, 5, 2,
                   GFX_ARGB(0xCC, 0xE8, 0xE8, 0xF0));
    gfx_round_rect(tx, ty, w, h, 4, GFX_ARGB(0xB0, 0xDC, 0xDC, 0xE6));
    for (int i = 1; i < 3; i++) {
        gfx_fill(tx + w * i / 3, ty + 5, 1, h - 10, GFX_ARGB(0x60, 0x50, 0x50, 0x60));
    }
}

/* --- dock layout -------------------------------------------------------- */

/* Magnification: an icon's size falls off quadratically with its distance
 * from the pointer, which is what makes the Dock feel like it's being
 * pushed rather than switched. */
static int magnified_size(int center_x) {
    if (dock_hover < 0) return DOCK_ICON;
    int d = cursor_x - center_x;
    if (d < 0) d = -d;
    if (d >= DOCK_INFLUENCE) return DOCK_ICON;
    int t = (DOCK_INFLUENCE - d) * 255 / DOCK_INFLUENCE;
    int scale = 100 + (DOCK_MAX_SCALE - 100) * (t * t / 255) / 255;
    return DOCK_ICON * scale / 100;
}

static void dock_add_app(const app_t* app, const char* label) {
    if (dock_count >= DOCK_MAX_ITEMS) return;
    dock_items[dock_count].kind = DOCK_APP;
    dock_items[dock_count].app = app;
    dock_items[dock_count].win = 0;
    dock_items[dock_count].label = label;
    dock_items[dock_count].running = wm_app_window_count(app) > 0;
    dock_count++;
}

static void dock_layout(void) {
    dock_count = 0;
    dock_add_app(&app_terminal, "Terminal");
    dock_add_app(&app_files, "Files");
    dock_add_app(&app_monitor, "Activity Monitor");
    dock_add_app(&app_about, "About This Novaris");

    /* Minimized windows collect on the right, as they do on a Mac. */
    for (int i = 0; i < wm_count() && dock_count < DOCK_MAX_ITEMS - 1; i++) {
        window_t* win = wm_at(i);
        if (!win || !win->open || !win->minimized) continue;
        dock_items[dock_count].kind = DOCK_WINDOW;
        dock_items[dock_count].app = win->app;
        dock_items[dock_count].win = win;
        dock_items[dock_count].label = win->title;
        dock_items[dock_count].running = 0;
        dock_count++;
    }

    dock_items[dock_count].kind = DOCK_TRASH;
    dock_items[dock_count].app = 0;
    dock_items[dock_count].win = 0;
    dock_items[dock_count].label = "Trash";
    dock_items[dock_count].running = 0;
    dock_count++;

    int slot = DOCK_ICON + DOCK_GAP;
    int total = dock_count * slot - DOCK_GAP + DOCK_PAD * 2;
    dock_rect.w = total;
    /* The panel is tall enough to hold a magnified icon without clipping. */
    dock_rect.h = DOCK_H + (DOCK_ICON * (DOCK_MAX_SCALE - 100) / 100);
    dock_rect.x = (screen_w - total) / 2;
    dock_rect.y = screen_h - DOCK_BOTTOM - dock_rect.h;

    int x = dock_rect.x + DOCK_PAD;
    for (int i = 0; i < dock_count; i++) {
        int cx = x + DOCK_ICON / 2;
        dock_items[i].cx = cx;
        dock_items[i].size = magnified_size(cx);
        x += slot;
    }
}

static int dock_item_at(int x, int y) {
    if (y < dock_rect.y || y >= dock_rect.y + dock_rect.h) return -1;
    for (int i = 0; i < dock_count; i++) {
        int half = dock_items[i].size / 2 + 2;
        if (x >= dock_items[i].cx - half && x < dock_items[i].cx + half) return i;
    }
    return -1;
}

static void draw_dock(void) {
    dock_layout();

    /* Frosted panel: blur what's behind it, then wash it with white. The
     * blur is why a window sliding under the Dock shows through as color
     * rather than as a legible rectangle. */
    int base_y = dock_rect.y + dock_rect.h - DOCK_H;
    gfx_blur(dock_rect.x, base_y, dock_rect.w, DOCK_H, 6);
    gfx_round_rect(dock_rect.x, base_y + 3, dock_rect.w, DOCK_H, 20,
                   GFX_ARGB(0x40, 0, 0, 0));
    gfx_round_rect(dock_rect.x, base_y, dock_rect.w, DOCK_H, 20,
                   GFX_ARGB(0x4D, 0xFF, 0xFF, 0xFF));
    gfx_round_frame(dock_rect.x, base_y, dock_rect.w, DOCK_H, 20, 1,
                    GFX_ARGB(0x40, 0xFF, 0xFF, 0xFF));

    int bottom = base_y + DOCK_H - DOCK_PAD;
    for (int i = 0; i < dock_count; i++) {
        dock_item_t* item = &dock_items[i];
        int size = item->size;
        int x = item->cx - size / 2;
        int y = bottom - size;

        /* A launch bounce: a half-second hop, the way an app starting up
         * announces itself. */
        if (i == dock_bounce_item) {
            uint32_t age = pit_get_ticks() - dock_bounce_start;
            if (age > 50) {
                dock_bounce_item = -1;
            } else {
                int t = (int)age * 100 / 50;             /* 0-100 */
                int hop = (100 - (t - 50) * (t - 50) / 25) * 22 / 100;
                if (hop > 0) y -= hop;
            }
        }

        if (item->kind == DOCK_TRASH) {
            gfx_fill(item->cx - DOCK_ICON / 2 - DOCK_GAP / 2, base_y + 8, 1,
                     DOCK_H - 16, GFX_ARGB(0x40, 0xFF, 0xFF, 0xFF));
            draw_trash_icon(x, y, size);
        } else if (item->kind == DOCK_WINDOW) {
            /* A live thumbnail of the minimized window, scaled down - the
             * window's backing surface is right there, so there's no
             * reason to draw a generic icon instead. Keep its aspect
             * ratio: a squashed window reads as a rendering bug. */
            int sw = item->win->w, sh = item->win->h;
            int tw = size, th = size;
            if (sw > sh) th = size * sh / sw;
            else tw = size * sw / sh;
            int tx = item->cx - tw / 2, ty = y + size - th;
            gfx_blit_scaled(item->win->surface, tx, ty, tw, th);
            gfx_round_frame(tx, ty, tw, th, 3, 1, GFX_ARGB(0x66, 0xFF, 0xFF, 0xFF));
        } else {
            draw_app_icon(item->app, x, y, size);
        }

        if (item->running) {
            gfx_circle(item->cx, base_y + DOCK_H - 5, 2,
                       GFX_ARGB(0xCC, 0xFF, 0xFF, 0xFF));
        }
    }

    /* The hovered item's name, in a little tooltip above the Dock. */
    if (dock_hover >= 0 && dock_hover < dock_count) {
        const char* label = dock_items[dock_hover].label;
        int tw = gfx_text_width(&uifont_regular, label);
        int bw = tw + 24;
        int bx = dock_items[dock_hover].cx - bw / 2;
        if (bx < 4) bx = 4;
        if (bx + bw > screen_w - 4) bx = screen_w - 4 - bw;
        int by = dock_rect.y - 12;
        gfx_round_rect(bx, by, bw, 26, 8, GFX_ARGB(0xE6, 0x2E, 0x2E, 0x36));
        gfx_round_frame(bx, by, bw, 26, 8, 1, GFX_ARGB(0x40, 0xFF, 0xFF, 0xFF));
        gfx_text_center(&uifont_regular, bx, by + 5, bw, label,
                        GFX_RGB(0xFF, 0xFF, 0xFF));
    }
}

/* --- menu bar ----------------------------------------------------------- */

static const char* active_app_name(void) {
    const app_t* app = wm_active_app();
    return app ? app->name : "Finder";
}

static void month_name(char* out, int month) {
    static const char* names[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    kstrcpy(out, (month >= 1 && month <= 12) ? names[month - 1] : "???");
}

static void weekday_name(char* out, int weekday) {
    static const char* names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    kstrcpy(out, (weekday >= 0 && weekday < 7) ? names[weekday] : "???");
}

static void format_clock(char* out) {
    rtc_time_t now;
    rtc_read(&now);

    char part[16], num[12];
    weekday_name(part, now.weekday);
    kstrcpy(out, part);
    kstrcat(out, " ");
    ku32_to_dec(now.day, num);
    kstrcat(out, num);
    kstrcat(out, " ");
    month_name(part, now.month);
    kstrcat(out, part);
    kstrcat(out, "  ");
    ku32_to_dec(now.hour, num);
    if (now.hour < 10) kstrcat(out, "0");
    kstrcat(out, num);
    kstrcat(out, ":");
    ku32_to_dec(now.minute, num);
    if (now.minute < 10) kstrcat(out, "0");
    kstrcat(out, num);
}

static void draw_magnifier(int cx, int cy, uint32_t color) {
    gfx_ring(cx - 1, cy - 1, 5, 1, color);
    for (int i = 0; i < 4; i++) gfx_pixel(cx + 3 + i, cy + 3 + i, color);
}

static void draw_menubar(void) {
    gfx_blur(0, 0, screen_w, MENUBAR_H, 5);
    gfx_fill(0, 0, screen_w, MENUBAR_H, GFX_ARGB(0x66, 0xF2, 0xF2, 0xF8));
    gfx_fill(0, MENUBAR_H - 1, screen_w, 1, GFX_ARGB(0x28, 0, 0, 0));

    uint32_t text = GFX_RGB(0xFF, 0xFF, 0xFF);
    int y = (MENUBAR_H - uifont_regular.line_height) / 2;

    /* Titles are measured every frame because the second one is the name
     * of whichever app is frontmost, and that changes. */
    int x = 12;
    title_x[0] = x;
    title_w[0] = 24;
    x += title_w[0] + 4;

    const char* names[MENU_TITLE_COUNT];
    names[0] = "";
    names[1] = active_app_name();
    names[2] = menu_titles[2];
    names[3] = menu_titles[3];
    names[4] = menu_titles[4];

    for (int i = 1; i < MENU_TITLE_COUNT; i++) {
        const uifont_t* font = (i == 1) ? &uifont_bold : &uifont_regular;
        title_w[i] = gfx_text_width(font, names[i]) + 20;
        title_x[i] = x;
        x += title_w[i];
    }

    for (int i = 0; i < MENU_TITLE_COUNT; i++) {
        if (i == open_menu) {
            gfx_round_rect(title_x[i] - (i == 0 ? 4 : 0), 2,
                           title_w[i] + (i == 0 ? 8 : 0), MENUBAR_H - 5, 5,
                           GFX_ARGB(0x40, 0xFF, 0xFF, 0xFF));
        }
        if (i == 0) {
            gfx_sparkle(title_x[0] + 10, MENUBAR_H / 2 - 1, 9, text);
        } else {
            const uifont_t* font = (i == 1) ? &uifont_bold : &uifont_regular;
            gfx_text(font, title_x[i] + 10, y, names[i], text);
        }
    }

    /* Status items, laid out right to left. */
    char clock[48];
    format_clock(clock);
    int cw = gfx_text_width(&uifont_regular, clock);
    gfx_text(&uifont_regular, screen_w - 14 - cw, y, clock, text);

    draw_magnifier(screen_w - 14 - cw - 26, MENUBAR_H / 2, text);

    /* A control-centre style pair of pills. */
    int px = screen_w - 14 - cw - 58;
    gfx_round_rect(px, MENUBAR_H / 2 - 5, 8, 4, 2, GFX_ARGB(0xE0, 0xFF, 0xFF, 0xFF));
    gfx_round_rect(px, MENUBAR_H / 2 + 1, 8, 4, 2, GFX_ARGB(0xE0, 0xFF, 0xFF, 0xFF));
}

/* --- menu contents ------------------------------------------------------ */

static void add_item(const char* label, const char* shortcut, int action,
                     int arg, int enabled) {
    if (menu_item_count >= MENU_MAX_ITEMS) return;
    menu_item_t* item = &menu_items[menu_item_count++];
    kstrlcpy(item->label, label, sizeof(item->label));
    kstrlcpy(item->shortcut, shortcut ? shortcut : "", sizeof(item->shortcut));
    item->separator = 0;
    item->enabled = (uint8_t)enabled;
    item->checked = 0;
    item->action = action;
    item->arg = arg;
}

static void add_separator(void) {
    if (menu_item_count >= MENU_MAX_ITEMS) return;
    menu_item_t* item = &menu_items[menu_item_count++];
    item->label[0] = '\0';
    item->shortcut[0] = '\0';
    item->separator = 1;
    item->enabled = 0;
    item->checked = 0;
    item->action = ACT_NONE;
}

static void build_menu(int index) {
    menu_item_count = 0;
    window_t* focused = wm_focused();

    switch (index) {
        case 0:
            add_item("About This Novaris", 0, ACT_ABOUT, 0, 1);
            add_separator();
            add_item("System Report" UI_ELLIPSIS, 0, ACT_SYSTEM_REPORT, 0, 1);
            add_item("Software Update" UI_ELLIPSIS, 0, ACT_SOFTWARE_UPDATE, 0, 1);
            add_separator();
            add_item("Restart" UI_ELLIPSIS, 0, ACT_RESTART, 0, 1);
            add_item("Shut Down" UI_ELLIPSIS, 0, ACT_SHUT_DOWN, 0, 1);
            break;
        case 1: {
            char label[44];
            kstrcpy(label, "Hide ");
            kstrlcpy(label + 5, active_app_name(), sizeof(label) - 5);
            add_item("About This Novaris", 0, ACT_ABOUT, 0, 1);
            add_separator();
            add_item(label, UI_CMD "H", ACT_HIDE_APP, 0, focused != 0);
            kstrcpy(label, "Quit ");
            kstrlcpy(label + 5, active_app_name(), sizeof(label) - 5);
            add_item(label, UI_CMD "Q", ACT_QUIT_APP, 0, focused != 0);
            break;
        }
        case 2:
            add_item("Terminal Window", UI_CMD "N", ACT_NEW_TERMINAL, 0, 1);
            add_item("Files Window", UI_CMD "O", ACT_OPEN_FILES, 0, 1);
            add_separator();
            add_item("Close Window", UI_CMD "W", ACT_CLOSE_WINDOW, 0, focused != 0);
            break;
        case 3: {
            add_item("Minimize", UI_CMD "M", ACT_MINIMIZE, 0, focused != 0);
            add_item("Zoom", 0, ACT_ZOOM, 0, focused && focused->resizable);
            add_separator();
            add_item("Bring All to Front", 0, ACT_BRING_ALL_FRONT, 0, wm_count() > 0);
            int windows = 0;
            for (int i = wm_count() - 1; i >= 0; i--) {
                window_t* win = wm_at(i);
                if (!win || !win->open) continue;
                if (!windows) add_separator();
                add_item(win->title, 0, ACT_SELECT_WINDOW, i, 1);
                menu_items[menu_item_count - 1].checked = (win == focused);
                windows++;
            }
            break;
        }
        case 4:
            add_item("Novaris Help", 0, ACT_HELP, 0, 1);
            add_item("Search" UI_ELLIPSIS, UI_CMD "Space", ACT_SPOTLIGHT, 0, 1);
            break;
        default: break;
    }
}

static void reboot(void) {
    /* Pulse the 8042's reset line - the classic PC way to reboot. */
    while (inb(0x64) & 0x02) { }
    outb(0x64, 0xFE);
    for (;;) __asm__ __volatile__("hlt");
}

static void shut_down(void) {
    gfx_set_target(screen);
    gfx_clip_reset();
    gfx_clear(GFX_RGB(0x08, 0x0A, 0x14));
    gfx_sparkle(screen_w / 2, screen_h / 2 - 70, 30, GFX_RGB(0x5A, 0x7C, 0xFF));
    gfx_text_center(&uifont_title, 0, screen_h / 2 - 16, screen_w,
                    "It is now safe to power off", GFX_RGB(0xE8, 0xE8, 0xF0));
    gfx_text_center(&uifont_regular, 0, screen_h / 2 + 22, screen_w,
                    "Novaris has shut down.", GFX_RGB(0x8A, 0x8A, 0x9A));
    gfx_present(screen, 0, 0, screen_w, screen_h);

    /* QEMU and most virtual machines honor one of these ACPI ports; real
     * hardware generally won't, which is what the message above is for. */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    __asm__ __volatile__("cli");
    for (;;) __asm__ __volatile__("hlt");
}

static void spotlight_show(void);

static void run_action(int action, int arg) {
    window_t* focused = wm_focused();
    switch (action) {
        case ACT_ABOUT: about_open(); break;
        case ACT_SYSTEM_REPORT: monitor_open(); break;
        case ACT_SOFTWARE_UPDATE:
            alert_open("Novaris is up to date",
                       "There is no software update service. This is the newest "
                       "version, because it is the only version.");
            break;
        case ACT_RESTART: reboot(); break;
        case ACT_SHUT_DOWN: shut_down(); break;
        case ACT_NEW_TERMINAL: terminal_open(); break;
        case ACT_OPEN_FILES: files_open(); break;
        case ACT_CLOSE_WINDOW: if (focused) wm_close(focused); break;
        case ACT_MINIMIZE: if (focused) wm_minimize(focused); break;
        case ACT_ZOOM: if (focused) wm_zoom(focused); break;
        case ACT_HIDE_APP:
            if (focused) wm_minimize(focused);
            break;
        case ACT_QUIT_APP:
            if (focused && focused->app) wm_close_all_of(focused->app);
            break;
        case ACT_BRING_ALL_FRONT:
            for (int i = 0; i < wm_count(); i++) {
                window_t* win = wm_at(i);
                if (win && win->open && !win->minimized) wm_focus(win);
            }
            break;
        case ACT_SELECT_WINDOW: {
            window_t* win = wm_at(arg);
            if (win && win->open) wm_focus(win);
            break;
        }
        case ACT_HELP:
            if (!textview_open("readme.txt")) {
                alert_open("Novaris Help",
                           "Click the Dock to open apps. Drag a window by its "
                           "title bar, resize it from the bottom-right corner, "
                           "and press Command-Space to search.");
            }
            break;
        case ACT_SPOTLIGHT: spotlight_show(); break;
        default: break;
    }
}

static void close_menu(void) {
    if (open_menu < 0) return;
    wm_damage(menu_rect.x - 4, menu_rect.y - 4, menu_rect.w + 40, menu_rect.h + 40);
    wm_damage(0, 0, screen_w, MENUBAR_H);
    open_menu = -1;
    menu_hover = -1;
}

static void show_menu(int index) {
    if (index < 0 || index >= MENU_TITLE_COUNT) return;
    close_menu();
    open_menu = index;
    menu_hover = -1;
    build_menu(index);

    /* Wide enough for the longest item drawn at its natural width: the
     * 30px check-mark gutter, the label, a gap, the shortcut, and the
     * right margin. Nothing in a menu should ever be ellipsized. */
    int width = 180;
    for (int i = 0; i < menu_item_count; i++) {
        int w = MENU_TEXT_X + gfx_text_width(&uifont_regular, menu_items[i].label) +
                MENU_RIGHT_PAD;
        if (menu_items[i].shortcut[0]) {
            w += 24 + gfx_text_width(&uifont_regular, menu_items[i].shortcut);
        }
        if (w > width) width = w;
    }
    int height = MENU_PAD_Y * 2;
    for (int i = 0; i < menu_item_count; i++) {
        height += menu_items[i].separator ? MENU_SEP_H : MENU_ITEM_H;
    }

    menu_rect.x = title_x[index] - 6;
    menu_rect.y = MENUBAR_H + 2;
    menu_rect.w = width;
    menu_rect.h = height;
    if (menu_rect.x + width > screen_w - 8) menu_rect.x = screen_w - 8 - width;
    wm_damage(menu_rect.x - 4, menu_rect.y - 4, width + 40, height + 40);
    wm_damage(0, 0, screen_w, MENUBAR_H);
}

static int menu_item_at(int x, int y) {
    if (!gfx_rect_contains(&menu_rect, x, y)) return -1;
    int cy = menu_rect.y + MENU_PAD_Y;
    for (int i = 0; i < menu_item_count; i++) {
        int h = menu_items[i].separator ? MENU_SEP_H : MENU_ITEM_H;
        if (y >= cy && y < cy + h) return menu_items[i].separator ? -1 : i;
        cy += h;
    }
    return -1;
}

static void draw_menu(void) {
    if (open_menu < 0) return;

    gfx_blur(menu_rect.x, menu_rect.y, menu_rect.w, menu_rect.h, 5);
    gfx_round_rect(menu_rect.x + 1, menu_rect.y + 4, menu_rect.w, menu_rect.h, 8,
                   GFX_ARGB(0x50, 0, 0, 0));
    gfx_round_rect(menu_rect.x, menu_rect.y, menu_rect.w, menu_rect.h, 8,
                   GFX_ARGB(0xE8, 0xF7, 0xF7, 0xFA));
    gfx_round_frame(menu_rect.x, menu_rect.y, menu_rect.w, menu_rect.h, 8, 1,
                    GFX_ARGB(0x30, 0, 0, 0));

    int y = menu_rect.y + MENU_PAD_Y;
    for (int i = 0; i < menu_item_count; i++) {
        menu_item_t* item = &menu_items[i];
        if (item->separator) {
            gfx_fill(menu_rect.x + 10, y + MENU_SEP_H / 2, menu_rect.w - 20, 1,
                     GFX_ARGB(0x30, 0, 0, 0));
            y += MENU_SEP_H;
            continue;
        }
        uint32_t color = item->enabled ? UI_TEXT : UI_TEXT_FAINT;
        if (i == menu_hover && item->enabled) {
            gfx_round_rect(menu_rect.x + 4, y, menu_rect.w - 8, MENU_ITEM_H, 5,
                           UI_ACCENT);
            color = GFX_RGB(0xFF, 0xFF, 0xFF);
        }
        if (item->checked) {
            gfx_text(&uifont_regular, menu_rect.x + 12,
                     y + (MENU_ITEM_H - uifont_regular.line_height) / 2,
                     UI_CHECK, color);
        }
        int text_y = y + (MENU_ITEM_H - uifont_regular.line_height) / 2;
        int avail = menu_rect.w - MENU_TEXT_X - MENU_RIGHT_PAD;
        if (item->shortcut[0]) {
            avail -= 24 + gfx_text_width(&uifont_regular, item->shortcut);
        }
        gfx_text_ellipsized(&uifont_regular, menu_rect.x + MENU_TEXT_X, text_y,
                            avail, item->label, color);
        if (item->shortcut[0]) {
            gfx_text_right(&uifont_regular, menu_rect.x, text_y,
                           menu_rect.w - MENU_RIGHT_PAD, item->shortcut,
                           i == menu_hover && item->enabled
                               ? GFX_ARGB(0xCC, 0xFF, 0xFF, 0xFF)
                               : UI_TEXT_DIM);
        }
        y += MENU_ITEM_H;
    }
}

/* --- spotlight ---------------------------------------------------------- */

static int matches(const char* haystack, const char* needle) {
    if (!needle[0]) return 1;
    for (const char* h = haystack; *h; h++) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) break;
            a++;
            b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

static void spotlight_search(void) {
    static const struct { const char* name; const char* subtitle; int action; }
    apps[] = {
        { "Terminal", "Application", ACT_NEW_TERMINAL },
        { "Files", "Application", ACT_OPEN_FILES },
        { "Activity Monitor", "Application", ACT_SYSTEM_REPORT },
        { "About This Novaris", "System Information", ACT_ABOUT },
        { "Shut Down", "System Command", ACT_SHUT_DOWN },
        { "Restart", "System Command", ACT_RESTART },
    };

    spot_result_count = 0;
    for (uint32_t i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
        if (spot_result_count >= SPOT_MAX_ROWS) break;
        if (!matches(apps[i].name, spotlight_query)) continue;
        spot_result_t* r = &spot_results[spot_result_count++];
        kstrlcpy(r->name, apps[i].name, sizeof(r->name));
        kstrlcpy(r->subtitle, apps[i].subtitle, sizeof(r->subtitle));
        r->action = apps[i].action;
        r->file[0] = '\0';
    }

    if (vfs_root && spotlight_query[0]) {
        uint32_t idx = 0;
        vfs_node_t* node;
        while ((node = vfs_readdir(vfs_root, idx)) && spot_result_count < SPOT_MAX_ROWS) {
            if (matches(node->name, spotlight_query)) {
                spot_result_t* r = &spot_results[spot_result_count++];
                kstrlcpy(r->name, node->name, sizeof(r->name));
                kstrlcpy(r->subtitle, "File on the initrd", sizeof(r->subtitle));
                r->action = -1;
                kstrlcpy(r->file, node->name, sizeof(r->file));
            }
            idx++;
        }
    }
    if (spotlight_selected >= spot_result_count) spotlight_selected = 0;
}

static void spotlight_geometry(void) {
    spotlight_rect.w = SPOT_W;
    spotlight_rect.h = SPOT_FIELD_H + (spot_result_count ? 8 : 0) +
                       spot_result_count * SPOT_ROW_H;
    spotlight_rect.x = (screen_w - SPOT_W) / 2;
    spotlight_rect.y = screen_h / 4;
}

static void spotlight_damage(void) {
    wm_damage(spotlight_rect.x - 30, spotlight_rect.y - 30,
              spotlight_rect.w + 60, spotlight_rect.h + 60);
}

static void spotlight_show(void) {
    spotlight_open = 1;
    spotlight_len = 0;
    spotlight_query[0] = '\0';
    spotlight_selected = 0;
    spotlight_search();
    spotlight_geometry();
    spotlight_damage();
}

static void spotlight_hide(void) {
    if (!spotlight_open) return;
    spotlight_damage();
    spotlight_open = 0;
}

static void spotlight_activate(void) {
    if (spotlight_selected < 0 || spotlight_selected >= spot_result_count) {
        spotlight_hide();
        return;
    }
    spot_result_t r = spot_results[spotlight_selected];
    spotlight_hide();

    if (r.action >= 0) {
        run_action(r.action, 0);
        return;
    }
    if (kstr_ends_with_ci(r.file, ".txt")) {
        textview_open(r.file);
    } else {
        char command[96];
        kstrcpy(command, "run ");
        kstrcat(command, r.file);
        terminal_run(command);
    }
}

static void draw_spotlight(void) {
    if (!spotlight_open) return;
    spotlight_geometry();
    gfx_rect_t r = spotlight_rect;

    gfx_blur(r.x, r.y, r.w, r.h, 8);
    gfx_round_rect(r.x, r.y + 8, r.w, r.h, 14, GFX_ARGB(0x60, 0, 0, 0));
    gfx_round_rect(r.x, r.y, r.w, r.h, 14, GFX_ARGB(0xE0, 0x2A, 0x2A, 0x32));
    gfx_round_frame(r.x, r.y, r.w, r.h, 14, 1, GFX_ARGB(0x40, 0xFF, 0xFF, 0xFF));

    draw_magnifier(r.x + 30, r.y + SPOT_FIELD_H / 2, GFX_RGB(0xC8, 0xC8, 0xD4));

    if (spotlight_len) {
        gfx_text(&uifont_title, r.x + 56, r.y + 16, spotlight_query,
                 GFX_RGB(0xFF, 0xFF, 0xFF));
        int cx = r.x + 58 + gfx_text_width(&uifont_title, spotlight_query);
        gfx_fill(cx, r.y + 16, 2, 26, GFX_ARGB(0xCC, 0x7A, 0xA8, 0xFF));
    } else {
        gfx_text(&uifont_title, r.x + 56, r.y + 16, "Spotlight Search",
                 GFX_RGB(0x86, 0x86, 0x94));
    }

    if (!spot_result_count) return;
    gfx_fill(r.x + 12, r.y + SPOT_FIELD_H, r.w - 24, 1,
             GFX_ARGB(0x30, 0xFF, 0xFF, 0xFF));

    int y = r.y + SPOT_FIELD_H + 8;
    for (int i = 0; i < spot_result_count; i++) {
        if (i == spotlight_selected) {
            gfx_round_rect(r.x + 8, y, r.w - 16, SPOT_ROW_H - 2, 7, UI_ACCENT);
        }
        uint32_t primary = GFX_RGB(0xF0, 0xF0, 0xF6);
        uint32_t secondary = i == spotlight_selected
                                 ? GFX_ARGB(0xCC, 0xFF, 0xFF, 0xFF)
                                 : GFX_RGB(0x9A, 0x9A, 0xA6);
        gfx_text(&uifont_regular, r.x + 30, y + 4, spot_results[i].name, primary);
        gfx_text(&uifont_small, r.x + 30, y + 21, spot_results[i].subtitle,
                 secondary);
        y += SPOT_ROW_H;
    }
}

/* --- cursor ------------------------------------------------------------- */

/* The classic arrow, as a tiny bitmap: 'X' is the outline, '.' the fill. */
#define CURSOR_W 12
#define CURSOR_H 19
static const char* const cursor_bitmap[CURSOR_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X......XXXXX",
    "X...X..X    ",
    "X..X X..X   ",
    "X.X   X..X  ",
    "XX    X..X  ",
    "X      X..X ",
    "        X..X",
    "        XXXX",
};

static void draw_cursor(int x, int y) {
    /* A soft shadow first, offset a pixel, then the arrow itself. */
    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            char c = cursor_bitmap[row][col];
            if (c == ' ' || c == '\0') continue;
            gfx_pixel(x + col + 1, y + row + 1, GFX_ARGB(0x40, 0, 0, 0));
        }
    }
    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            char c = cursor_bitmap[row][col];
            if (c == 'X') gfx_pixel(x + col, y + row, GFX_RGB(0x10, 0x10, 0x14));
            else if (c == '.') gfx_pixel(x + col, y + row, GFX_RGB(0xFF, 0xFF, 0xFF));
        }
    }
}

static void damage_cursor(int x, int y) {
    wm_damage(x - 1, y - 1, CURSOR_W + 4, CURSOR_H + 4);
}

/* --- compositing -------------------------------------------------------- */

int desktop_menubar_height(void) { return MENUBAR_H; }
int desktop_dock_reserved(void) { return DOCK_H + DOCK_BOTTOM + 4; }

/* Panels read the pixels around them (that's what a blur is), so any
 * frame that touches part of one has to recomposite all of it - otherwise
 * the blur would sample whatever the previous frame happened to leave
 * behind. */
static void expand_for_panels(gfx_rect_t* r) {
    gfx_rect_t menubar = { 0, 0, screen_w, MENUBAR_H };
    if (gfx_rect_intersects(r, &menubar)) gfx_rect_union(r, &menubar);

    gfx_rect_t dock = dock_rect;
    if (dock.w > 0 && gfx_rect_intersects(r, &dock)) {
        gfx_rect_t padded = { dock.x - 2, dock.y - 40, dock.w + 4, dock.h + 44 };
        gfx_rect_union(r, &padded);
    }
    if (open_menu >= 0) {
        gfx_rect_t m = { menu_rect.x - 2, menu_rect.y - 2, menu_rect.w + 8,
                         menu_rect.h + 10 };
        if (gfx_rect_intersects(r, &m)) gfx_rect_union(r, &m);
    }
    if (spotlight_open) {
        gfx_rect_t s = { spotlight_rect.x - 2, spotlight_rect.y - 2,
                         spotlight_rect.w + 6, spotlight_rect.h + 14 };
        if (gfx_rect_intersects(r, &s)) gfx_rect_union(r, &s);
    }

    if (r->x < 0) { r->w += r->x; r->x = 0; }
    if (r->y < 0) { r->h += r->y; r->y = 0; }
    if (r->x + r->w > screen_w) r->w = screen_w - r->x;
    if (r->y + r->h > screen_h) r->h = screen_h - r->y;
}

static void compose(gfx_rect_t damage) {
    expand_for_panels(&damage);
    if (damage.w <= 0 || damage.h <= 0) return;

    gfx_set_target(screen);
    gfx_copy_region(screen, wallpaper, damage.x, damage.y, damage.w, damage.h);

    gfx_clip_reset();
    gfx_clip(damage.x, damage.y, damage.w, damage.h);

    wm_composite(&damage);

    gfx_set_target(screen);
    gfx_clip(damage.x, damage.y, damage.w, damage.h);
    draw_menubar();
    draw_menu();
    draw_dock();
    draw_spotlight();
    draw_cursor(cursor_x, cursor_y);

    gfx_present(screen, damage.x, damage.y, damage.w, damage.h);
}

void desktop_pump_output(void) {
    gfx_rect_t damage;
    if (wm_take_damage(&damage)) compose(damage);
}

/* --- input -------------------------------------------------------------- */

static void handle_menubar_click(int x, int y) {
    (void)y;
    for (int i = 0; i < MENU_TITLE_COUNT; i++) {
        if (x >= title_x[i] - (i == 0 ? 4 : 0) &&
            x < title_x[i] + title_w[i]) {
            if (open_menu == i) close_menu();
            else show_menu(i);
            return;
        }
    }
    /* The magnifier in the status area opens Spotlight. */
    if (x > screen_w - 260) {
        if (spotlight_open) spotlight_hide();
        else spotlight_show();
    }
}

static void handle_dock_click(int index) {
    if (index < 0 || index >= dock_count) return;
    dock_item_t* item = &dock_items[index];

    if (item->kind == DOCK_TRASH) {
        alert_open("The Trash is empty",
                   "Novaris keeps its files on a read-only initrd, so there is "
                   "nothing here to throw away.");
        return;
    }
    if (item->kind == DOCK_WINDOW) {
        wm_unminimize(item->win);
        return;
    }

    window_t* existing = wm_find_by_app(item->app);
    if (existing) {
        wm_focus(existing);
        return;
    }
    dock_bounce_item = index;
    dock_bounce_start = pit_get_ticks();
    if (item->app == &app_terminal) terminal_open();
    else if (item->app == &app_files) files_open();
    else if (item->app == &app_monitor) monitor_open();
    else if (item->app == &app_about) about_open();
}

static void handle_mouse(const mouse_event_t* ev) {
    prev_cursor_x = cursor_x;
    prev_cursor_y = cursor_y;
    cursor_x = ev->x;
    cursor_y = ev->y;
    if (cursor_x != prev_cursor_x || cursor_y != prev_cursor_y) {
        damage_cursor(prev_cursor_x, prev_cursor_y);
        damage_cursor(cursor_x, cursor_y);
    }

    /* Dock hover, which drives both magnification and the tooltip. */
    int over_dock = dock_item_at(cursor_x, cursor_y);
    if (over_dock != dock_hover ||
        (dock_hover >= 0 && cursor_x != prev_cursor_x)) {
        dock_hover = over_dock;
        wm_damage(dock_rect.x - 4, dock_rect.y - 40, dock_rect.w + 8,
                  dock_rect.h + 48);
    }

    /* An open menu swallows everything until it closes. */
    if (open_menu >= 0) {
        int item = menu_item_at(cursor_x, cursor_y);
        if (item != menu_hover) {
            menu_hover = item;
            wm_damage(menu_rect.x, menu_rect.y, menu_rect.w, menu_rect.h);
        }
        /* Sliding along the menu bar with a menu open switches menus, the
         * way it does on a Mac. */
        if (cursor_y < MENUBAR_H) {
            for (int i = 0; i < MENU_TITLE_COUNT; i++) {
                if (cursor_x >= title_x[i] && cursor_x < title_x[i] + title_w[i] &&
                    i != open_menu) {
                    show_menu(i);
                    break;
                }
            }
        }
        if (ev->pressed & MOUSE_BUTTON_LEFT) {
            if (cursor_y < MENUBAR_H) {
                handle_menubar_click(cursor_x, cursor_y);
            } else if (item < 0 && !gfx_rect_contains(&menu_rect, cursor_x, cursor_y)) {
                close_menu();
            }
            return;
        }
        if (ev->released & MOUSE_BUTTON_LEFT) {
            if (item >= 0 && menu_items[item].enabled) {
                int action = menu_items[item].action;
                int arg = menu_items[item].arg;
                close_menu();
                run_action(action, arg);
            }
            return;
        }
        return;
    }

    if (ev->pressed & MOUSE_BUTTON_LEFT) {
        if (spotlight_open &&
            !gfx_rect_contains(&spotlight_rect, cursor_x, cursor_y)) {
            spotlight_hide();
        }
        if (cursor_y < MENUBAR_H) {
            handle_menubar_click(cursor_x, cursor_y);
            return;
        }
        if (over_dock >= 0) {
            handle_dock_click(over_dock);
            return;
        }
    }

    wm_handle_mouse(ev);
}

static void handle_key(const key_event_t* ev) {
    if (!ev->pressed) return;

    if (spotlight_open) {
        if (ev->code == KEY_ESCAPE) { spotlight_hide(); return; }
        if (ev->code == KEY_ENTER) { spotlight_activate(); return; }
        if (ev->code == KEY_UP || ev->code == KEY_DOWN) {
            spotlight_selected += (ev->code == KEY_DOWN) ? 1 : -1;
            if (spotlight_selected < 0) spotlight_selected = 0;
            if (spotlight_selected >= spot_result_count) {
                spotlight_selected = spot_result_count - 1;
            }
            spotlight_damage();
            return;
        }
        if (ev->code == KEY_BACKSPACE) {
            if (spotlight_len > 0) spotlight_query[--spotlight_len] = '\0';
            spotlight_search();
            spotlight_damage();
            spotlight_geometry();
            spotlight_damage();
            return;
        }
        if (ev->ascii >= 32 && ev->ascii < 127 &&
            spotlight_len < (int)sizeof(spotlight_query) - 1) {
            spotlight_query[spotlight_len++] = (char)ev->ascii;
            spotlight_query[spotlight_len] = '\0';
            spotlight_search();
            spotlight_damage();
            spotlight_geometry();
            spotlight_damage();
            return;
        }
        return;
    }

    if (open_menu >= 0) {
        if (ev->code == KEY_ESCAPE) { close_menu(); return; }
    }

    if (ev->mods & KEY_MOD_COMMAND) {
        switch (ev->ascii) {
            case ' ': spotlight_show(); return;
            case 'n': case 'N': run_action(ACT_NEW_TERMINAL, 0); return;
            case 'o': case 'O': run_action(ACT_OPEN_FILES, 0); return;
            case 'q': case 'Q': run_action(ACT_QUIT_APP, 0); return;
            case 'h': case 'H': run_action(ACT_HIDE_APP, 0); return;
            default: break;
        }
    }

    wm_handle_key(ev);
}

/* --- startup ------------------------------------------------------------ */

void desktop_start(void) {
    screen_w = (int)fb_width();
    screen_h = (int)fb_height();

    screen = gfx_surface_create(screen_w, screen_h);
    wallpaper = gfx_surface_create(screen_w, screen_h);
    if (!screen || !wallpaper) {
        /* Nothing sensible to fall back to but the text shell, and the
         * caller knows how to run it. */
        console_set_sink(0);
        terminal_writestring("\nDesktop: not enough memory for a framebuffer "
                             "back buffer.\n");
        return;
    }

    gfx_set_target(screen);
    render_wallpaper();

    mouse_set_bounds(screen_w, screen_h);
    cursor_x = prev_cursor_x = screen_w / 2;
    cursor_y = prev_cursor_y = screen_h / 2;

    wm_init(screen_w, screen_h, MENUBAR_H, desktop_dock_reserved());
    dock_layout();

    terminal_app_init();
    terminal_open();
    shell_init();   /* banner and first prompt, now inside the window */

    wm_damage_all();
    last_tick = pit_get_ticks();

    for (;;) {
        mouse_event_t mev;
        while (mouse_poll_event(&mev)) handle_mouse(&mev);

        key_event_t kev;
        while (keyboard_poll_event(&kev)) handle_key(&kev);

        /* A window opening, closing or minimizing changes what the Dock
         * and the menu bar show, and neither of them is anywhere near the
         * window that changed - so window damage alone would leave both
         * stale. Damage the whole bottom strip rather than the Dock's
         * current rectangle: the Dock's width depends on the very item
         * count that just changed, so the rectangle to erase is the old
         * one, which is exactly what the strip covers. */
        if (wm_generation() != last_generation) {
            last_generation = wm_generation();
            wm_damage(0, screen_h - DOCK_H - DOCK_BOTTOM -
                             (DOCK_ICON * (DOCK_MAX_SCALE - 100) / 100) - 40,
                      screen_w, screen_h);
            wm_damage(0, 0, screen_w, MENUBAR_H);
        }

        uint32_t now = pit_get_ticks();
        if (now - last_tick >= 10) {       /* ~10 Hz housekeeping */
            last_tick = now;
            wm_tick();

            rtc_time_t clock;
            rtc_read(&clock);
            if (clock.minute != last_minute) {
                last_minute = clock.minute;
                wm_damage(0, 0, screen_w, MENUBAR_H);
            }
            if (dock_bounce_item >= 0) {
                wm_damage(dock_rect.x, dock_rect.y - 30, dock_rect.w,
                          dock_rect.h + 30);
            }
        }

        gfx_rect_t damage;
        if (wm_take_damage(&damage)) {
            compose(damage);
        } else {
            /* Nothing to draw: sleep until the next interrupt rather than
             * spinning. The PIT alone wakes us 100 times a second. */
            __asm__ __volatile__("hlt");
        }
    }
}
