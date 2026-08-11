/* desktop.c - the Windows-style shell: wallpaper, desktop icons, the
 * taskbar, the Start menu, context menus, the Alt+Tab switcher, and the
 * event loop that drives all of it.
 *
 * The frame loop is the heart of the file, and it is deliberately not
 * "redraw everything at 60Hz". Nothing here has a GPU; a full 1280x800
 * recomposite is a megapixel of work. So every source of change - a
 * window moving, the clock ticking over, the pointer sliding two pixels -
 * reports the rectangle it affected, and each frame recomposites only the
 * union of those rectangles:
 *
 *     wallpaper -> desktop icons -> windows -> taskbar -> menus -> cursor
 *
 * always in that order, always from scratch within the damaged region.
 * That's why translucency works: the taskbar can blend with whatever is
 * genuinely underneath it, because whatever is underneath it was just
 * drawn. It's also why the pointer can slide over a window without
 * corrupting it - there is no "save the pixels underneath" anywhere.
 *
 * The shell is Windows-shaped: one bar along the bottom holding Start, a
 * search field, a button per window and a clock; a Start menu that both
 * launches things and searches; icons on the desktop itself. The one
 * structural consequence of that layout is that the taskbar is the *only*
 * reserved edge - there is no menu bar, so a window's own title bar is
 * the only chrome above it, and the work area starts at y=0.
 */

#include "desktop.h"
#include "apps.h"
#include "wm.h"
#include "gfx.h"
#include "uikit.h"
#include "icons.h"
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
#include "wmdev.h"

/* --- layout ------------------------------------------------------------- */

#define TASKBAR_H       40
#define START_BTN_W     48
#define SEARCH_W        208
#define TASK_BTN_W      170
#define TASK_BTN_ICON_W 44
#define TRAY_GLYPH_W    26
#define TRAY_CLOCK_W    86
#define SHOW_DESKTOP_W  6
#define TASKBAR_MAX_ITEMS (8 + WM_MAX_WINDOWS)

#define DESK_ICON_CELL_W 88
#define DESK_ICON_CELL_H 86
#define DESK_ICON_SIZE   40
#define DESK_ICON_TOP    18
#define DESK_ICON_LEFT   16

#define MENU_ITEM_H     28
#define MENU_SEP_H      9
#define MENU_MAX_ITEMS  20
#define MENU_PAD_Y      5
#define MENU_TEXT_X     34   /* label inset: leaves room for a check mark */
#define MENU_RIGHT_PAD  16

#define START_W         356
#define START_H         500
#define START_PAD       16
#define START_SEARCH_H  36
#define START_FOOTER_H  54
#define START_TILE_W    108
#define START_TILE_H    88
#define START_TILE_COLS 3
#define START_ROW_H     44
#define START_MAX_ENTRIES 16

#define THUMB_W         200
#define THUMB_H         150
#define SWITCH_TILE     104

/* --- the Windows palette ------------------------------------------------ */

#define TASKBAR_FILL    GFX_ARGB(0xE6, 0x1C, 0x1C, 0x20)
#define TASKBAR_RULE    GFX_ARGB(0x30, 0xFF, 0xFF, 0xFF)
#define TASKBAR_TEXT    GFX_RGB(0xF2, 0xF2, 0xF5)
#define TASKBAR_TEXT_DIM GFX_ARGB(0xB0, 0xFF, 0xFF, 0xFF)
#define TASKBAR_HOVER   GFX_ARGB(0x22, 0xFF, 0xFF, 0xFF)
#define TASKBAR_ACTIVE  GFX_ARGB(0x33, 0xFF, 0xFF, 0xFF)
#define TASKBAR_PRESSED GFX_ARGB(0x44, 0xFF, 0xFF, 0xFF)

#define FLYOUT_FILL     GFX_ARGB(0xF2, 0xF7, 0xF7, 0xFA)
#define FLYOUT_BORDER   GFX_ARGB(0x40, 0x00, 0x00, 0x00)
#define FLYOUT_HOVER    GFX_RGB(0xE6, 0xE9, 0xEE)
#define FLYOUT_RADIUS   7

static gfx_surface_t* screen;      /* the frame being assembled */
static gfx_surface_t* wallpaper;   /* rendered once at startup */
static int screen_w, screen_h;

static int cursor_x, cursor_y;
static int prev_cursor_x, prev_cursor_y;

static uint32_t last_tick;
static int last_minute = -1;
static uint32_t last_generation;

/* --- actions ------------------------------------------------------------ */

enum {
    ACT_NONE, ACT_ABOUT, ACT_TASK_MANAGER, ACT_TERMINAL, ACT_EXPLORER,
    ACT_HELP, ACT_RESTART, ACT_SHUT_DOWN, ACT_RECYCLE_BIN, ACT_REFRESH,
    ACT_SHOW_DESKTOP, ACT_OPEN_FILE,
    /* Window operations - these all act on `menu_target`. */
    ACT_WIN_RESTORE, ACT_WIN_MINIMIZE, ACT_WIN_MAXIMIZE, ACT_WIN_CLOSE,
    ACT_WIN_FOCUS,
};

/* How to draw a thing that isn't a window: either an app's icon, or one
 * of the shell's own. */
enum { PIC_APP, PIC_COMPUTER, PIC_RECYCLE, PIC_DOCUMENT };

static void draw_pictogram(int pic, const app_t* app, int x, int y, int size) {
    switch (pic) {
        case PIC_COMPUTER: icon_computer(x, y, size); break;
        case PIC_RECYCLE:  icon_recycle_bin(x, y, size); break;
        case PIC_DOCUMENT: icon_document(x, y, size); break;
        default:           icon_app(app, x, y, size); break;
    }
}

/* --- popup menus -------------------------------------------------------- */
/* One menu at a time, anchored anywhere: the Start button's power flyout,
 * a window's system menu, a taskbar button's menu and the desktop's
 * right-click menu are all the same widget with different contents. */

typedef struct {
    char label[44];
    char shortcut[12];
    uint8_t separator;
    uint8_t enabled;
    uint8_t checked;
    int action;
    int arg;
} menu_item_t;

static menu_item_t menu_items[MENU_MAX_ITEMS];
static int menu_item_count;
static int menu_open;
static int menu_hover = -1;
static gfx_rect_t menu_rect;
static window_t* menu_target;   /* the window a system menu belongs to */

/* --- taskbar ------------------------------------------------------------ */

enum { TB_START, TB_SEARCH, TB_PINNED, TB_WINDOW, TB_TRAY, TB_SHOW_DESKTOP };

typedef struct {
    int kind;
    const app_t* app;
    window_t* win;          /* for TB_WINDOW */
    const char* label;
    int action;             /* for TB_PINNED */
    int x, w;
} task_item_t;

static task_item_t task_items[TASKBAR_MAX_ITEMS];
static int task_count;
static gfx_rect_t taskbar_rect;
static int taskbar_hover = -1;
static int taskbar_pressed = -1;
static uint32_t hover_since;    /* when the pointer arrived, for the preview */
static int thumb_shown;         /* is the preview currently on screen */

/* The apps pinned to the taskbar and listed first in the Start menu. */
static const struct { const app_t* app; const char* label; int action; }
pinned_apps[] = {
    { &app_terminal, "Terminal",      ACT_TERMINAL },
    { &app_files,    "File Explorer", ACT_EXPLORER },
    { &app_monitor,  "Task Manager",  ACT_TASK_MANAGER },
    { &app_about,    "About Novaris", ACT_ABOUT },
};
#define PINNED_COUNT ((int)(sizeof(pinned_apps) / sizeof(pinned_apps[0])))

/* --- Start menu --------------------------------------------------------- */

typedef struct {
    char name[64];
    char sub[40];
    char file[64];
    const app_t* app;
    int pic;
    int action;
    int tile;          /* 1 = pinned grid tile, 0 = list row */
    gfx_rect_t r;
} start_entry_t;

static int start_open;
static char start_query[48];
static int start_len;
static int start_selected;
static int start_hover = -1;
static gfx_rect_t start_rect;
static start_entry_t start_entries[START_MAX_ENTRIES];
static int start_entry_count;
static gfx_rect_t start_search_rect, start_power_rect;

/* --- desktop icons ------------------------------------------------------ */

typedef struct {
    const char* label;
    const app_t* app;
    int pic;
    int action;
    gfx_rect_t r;
} desk_icon_t;

static desk_icon_t desk_icons[] = {
    { "This Novaris",  0,            PIC_COMPUTER, ACT_ABOUT,       {0,0,0,0} },
    { "Recycle Bin",   0,            PIC_RECYCLE,  ACT_RECYCLE_BIN, {0,0,0,0} },
    { "File Explorer", &app_files,   PIC_APP,      ACT_EXPLORER,    {0,0,0,0} },
    { "Terminal",      &app_terminal,PIC_APP,      ACT_TERMINAL,    {0,0,0,0} },
};
#define DESK_ICON_COUNT ((int)(sizeof(desk_icons) / sizeof(desk_icons[0])))
static int desk_selected = -1;

/* --- Alt+Tab switcher --------------------------------------------------- */

static int switcher_open;
static window_t* switcher_list[WM_MAX_WINDOWS];
static int switcher_count;
static int switcher_index;
static gfx_rect_t switcher_rect;

/* The Windows key toggles Start only if nothing else was pressed while it
 * was held - otherwise Win+E would open the Start menu on the way to
 * opening File Explorer. */
static int super_alone;

static void run_action(int action, int arg);
static void close_menu(void);
static void start_hide(void);

/* --- wallpaper ---------------------------------------------------------- */

static uint32_t mix(uint32_t a, uint32_t b, int t /* 0-255 */) {
    int it = 255 - t;
    return GFX_RGB((GFX_RED(a) * it + GFX_RED(b) * t) / 255,
                   (GFX_GREEN(a) * it + GFX_GREEN(b) * t) / 255,
                   (GFX_BLUE(a) * it + GFX_BLUE(b) * t) / 255);
}

/* A deep blue field with a bloom of light behind where the Start menu
 * opens, plus a cooler one high and right - the shape of every Windows
 * default wallpaper since 8. Computed per pixel once at startup and then
 * kept, because every damaged region of every frame starts by copying
 * from it. */
static void render_wallpaper(void) {
    const uint32_t top = GFX_RGB(0x04, 0x1C, 0x3C);
    const uint32_t bottom = GFX_RGB(0x02, 0x0C, 0x22);
    const uint32_t glow_a = GFX_RGB(0x22, 0x8C, 0xF0);   /* system blue */
    const uint32_t glow_b = GFX_RGB(0x11, 0xC8, 0xD6);   /* teal */

    int ax = screen_w * 34 / 100, ay = screen_h * 66 / 100;
    int bx = screen_w * 74 / 100, by = screen_h * 26 / 100;
    int ra = screen_h * 80 / 100, rb = screen_h * 58 / 100;

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
                color = mix(color, glow_a, t * t / 255 * 96 / 255);
            }
            dx = x - bx;
            dy = y - by;
            d2 = dx * dx / 64 + dy * dy / 64;
            r2 = rb * rb / 64;
            if (d2 < r2) {
                int t = (r2 - d2) * 255 / r2;
                color = mix(color, glow_b, t * t / 255 * 58 / 255);
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

/* --- desktop icons ------------------------------------------------------ */

static void desk_icons_layout(void) {
    int x = DESK_ICON_LEFT, y = DESK_ICON_TOP;
    for (int i = 0; i < DESK_ICON_COUNT; i++) {
        if (y + DESK_ICON_CELL_H > screen_h - TASKBAR_H) {
            /* Icons fill a column and then start the next one, which is
             * what a Windows desktop does when it runs out of height. */
            y = DESK_ICON_TOP;
            x += DESK_ICON_CELL_W;
        }
        desk_icons[i].r.x = x;
        desk_icons[i].r.y = y;
        desk_icons[i].r.w = DESK_ICON_CELL_W;
        desk_icons[i].r.h = DESK_ICON_CELL_H;
        y += DESK_ICON_CELL_H;
    }
}

static int desk_icon_at(int x, int y) {
    for (int i = 0; i < DESK_ICON_COUNT; i++) {
        if (gfx_rect_contains(&desk_icons[i].r, x, y)) return i;
    }
    return -1;
}

static void draw_desk_icons(void) {
    for (int i = 0; i < DESK_ICON_COUNT; i++) {
        gfx_rect_t r = desk_icons[i].r;
        if (i == desk_selected) {
            gfx_round_rect(r.x, r.y, r.w, r.h, 4, GFX_ARGB(0x59, 0x00, 0x78, 0xD4));
            gfx_round_frame(r.x, r.y, r.w, r.h, 4, 1,
                            GFX_ARGB(0x99, 0x6C, 0xC0, 0xFF));
        }
        draw_pictogram(desk_icons[i].pic, desk_icons[i].app,
                       r.x + (r.w - DESK_ICON_SIZE) / 2, r.y + 10,
                       DESK_ICON_SIZE);
        /* Icon labels get a shadow rather than a plate: a wallpaper this
         * dark doesn't need the plate, but the text still has to survive
         * a bright patch of the bloom. */
        gfx_text_center(&uifont_small, r.x + 1, r.y + 59, r.w,
                        desk_icons[i].label, GFX_ARGB(0xAA, 0, 0, 0));
        gfx_text_center(&uifont_small, r.x, r.y + 58, r.w, desk_icons[i].label,
                        GFX_RGB(0xFF, 0xFF, 0xFF));
    }
}

/* --- clock -------------------------------------------------------------- */

static void append_uint(char* out, uint32_t value, int pad2) {
    char num[12];
    ku32_to_dec(value, num);
    if (pad2 && value < 10) kstrcat(out, "0");
    kstrcat(out, num);
}

static void format_time(char* out) {
    rtc_time_t now;
    rtc_read(&now);
    int hour = now.hour % 12;
    if (hour == 0) hour = 12;
    out[0] = '\0';
    append_uint(out, (uint32_t)hour, 0);
    kstrcat(out, ":");
    append_uint(out, now.minute, 1);
    kstrcat(out, now.hour < 12 ? " AM" : " PM");
}

static void format_date(char* out) {
    rtc_time_t now;
    rtc_read(&now);
    out[0] = '\0';
    append_uint(out, now.month, 0);
    kstrcat(out, "/");
    append_uint(out, now.day, 0);
    kstrcat(out, "/");
    append_uint(out, now.year, 0);
}

/* --- taskbar ------------------------------------------------------------ */

static void taskbar_add(int kind, const app_t* app, window_t* win,
                        const char* label, int action, int x, int w) {
    if (task_count >= TASKBAR_MAX_ITEMS) return;
    task_item_t* item = &task_items[task_count++];
    item->kind = kind;
    item->app = app;
    item->win = win;
    item->label = label;
    item->action = action;
    item->x = x;
    item->w = w;
}

/* Lays the bar out left to right, then the tray right to left, and gives
 * whatever is left over to the window buttons - which is why opening a
 * tenth window narrows the other nine instead of pushing them off. */
static void taskbar_layout(void) {
    task_count = 0;
    taskbar_rect.x = 0;
    taskbar_rect.y = screen_h - TASKBAR_H;
    taskbar_rect.w = screen_w;
    taskbar_rect.h = TASKBAR_H;

    taskbar_add(TB_START, 0, 0, "Start", ACT_NONE, 0, START_BTN_W);
    taskbar_add(TB_SEARCH, 0, 0, "Type here to search", ACT_NONE,
                START_BTN_W + 2, SEARCH_W);

    int tray_x = screen_w - SHOW_DESKTOP_W - TRAY_CLOCK_W - TRAY_GLYPH_W * 3;
    taskbar_add(TB_TRAY, 0, 0, "", ACT_NONE, tray_x,
                TRAY_CLOCK_W + TRAY_GLYPH_W * 3);
    taskbar_add(TB_SHOW_DESKTOP, 0, 0, "Show desktop", ACT_SHOW_DESKTOP,
                screen_w - SHOW_DESKTOP_W, SHOW_DESKTOP_W);

    int x = START_BTN_W + 2 + SEARCH_W + 8;
    int avail = tray_x - 8 - x;

    /* Count the buttons first so they can share the space evenly. A
     * pinned app with no windows gets an icon-only button; a pinned app
     * with windows gets one labelled button per window, which is exactly
     * how Windows behaves with grouping turned off. */
    int labelled = 0, icons = 0;
    for (int i = 0; i < PINNED_COUNT; i++) {
        int n = wm_app_window_count(pinned_apps[i].app);
        if (n) labelled += n;
        else icons++;
    }
    for (int i = 0; i < wm_count(); i++) {
        window_t* win = wm_at(i);
        if (!win || !win->open) continue;
        int pinned = 0;
        for (int j = 0; j < PINNED_COUNT; j++) {
            if (pinned_apps[j].app == win->app) pinned = 1;
        }
        if (!pinned) labelled++;
    }

    int btn_w = TASK_BTN_W;
    if (labelled > 0) {
        int room = avail - icons * TASK_BTN_ICON_W;
        if (room < labelled * TASK_BTN_W) btn_w = room / labelled;
        if (btn_w < TASK_BTN_ICON_W) btn_w = TASK_BTN_ICON_W;
    }

    for (int i = 0; i < PINNED_COUNT; i++) {
        const app_t* app = pinned_apps[i].app;
        int n = wm_app_window_count(app);
        if (!n) {
            taskbar_add(TB_PINNED, app, 0, pinned_apps[i].label,
                        pinned_apps[i].action, x, TASK_BTN_ICON_W);
            x += TASK_BTN_ICON_W;
            continue;
        }
        for (int j = 0; j < wm_count(); j++) {
            window_t* win = wm_at(j);
            if (!win || !win->open || win->app != app) continue;
            taskbar_add(TB_WINDOW, app, win, win->title, ACT_NONE, x, btn_w);
            x += btn_w;
        }
    }
    for (int i = 0; i < wm_count(); i++) {
        window_t* win = wm_at(i);
        if (!win || !win->open) continue;
        int pinned = 0;
        for (int j = 0; j < PINNED_COUNT; j++) {
            if (pinned_apps[j].app == win->app) pinned = 1;
        }
        if (pinned) continue;
        taskbar_add(TB_WINDOW, win->app, win, win->title, ACT_NONE, x, btn_w);
        x += btn_w;
    }
}

static int taskbar_item_at(int x, int y) {
    if (y < taskbar_rect.y) return -1;
    for (int i = 0; i < task_count; i++) {
        if (task_items[i].kind == TB_TRAY) continue;
        if (x >= task_items[i].x && x < task_items[i].x + task_items[i].w) return i;
    }
    return -1;
}

static void draw_tray(const task_item_t* item) {
    int y = taskbar_rect.y;
    int gx = item->x;
    icon_chevron_up(gx + 6, y + 12, 14, TASKBAR_TEXT_DIM);
    icon_network(gx + TRAY_GLYPH_W + 6, y + 13, 14, TASKBAR_TEXT_DIM);
    icon_volume(gx + TRAY_GLYPH_W * 2 + 6, y + 13, 14, TASKBAR_TEXT_DIM);

    char text[24];
    int cx = gx + TRAY_GLYPH_W * 3;
    format_time(text);
    gfx_text_right(&uifont_small, cx, y + 5, TRAY_CLOCK_W - 12, text,
                   TASKBAR_TEXT);
    format_date(text);
    gfx_text_right(&uifont_small, cx, y + 20, TRAY_CLOCK_W - 12, text,
                   TASKBAR_TEXT);
}

static void draw_taskbar(void) {
    taskbar_layout();
    int y = taskbar_rect.y;

    /* Frosted, then washed near-black: the blur is why a window sliding
     * under the taskbar shows through as color rather than as a legible
     * rectangle. */
    gfx_blur(0, y, screen_w, TASKBAR_H, 6);
    gfx_fill(0, y, screen_w, TASKBAR_H, TASKBAR_FILL);
    gfx_fill(0, y, screen_w, 1, TASKBAR_RULE);

    window_t* front = wm_focused();

    for (int i = 0; i < task_count; i++) {
        task_item_t* item = &task_items[i];
        int hovered = (taskbar_hover == i);
        int down = (taskbar_pressed == i);

        switch (item->kind) {
            case TB_START: {
                if (start_open || down) {
                    gfx_fill(item->x, y, item->w, TASKBAR_H, TASKBAR_PRESSED);
                } else if (hovered) {
                    gfx_fill(item->x, y, item->w, TASKBAR_H, TASKBAR_HOVER);
                }
                icon_window_logo(item->x + (item->w - 18) / 2, y + 11, 18,
                                 start_open ? GFX_RGB(0x4C, 0xC2, 0xFF)
                                            : GFX_RGB(0xFF, 0xFF, 0xFF));
                break;
            }
            case TB_SEARCH: {
                int sy = y + 6, sh = TASKBAR_H - 12;
                gfx_round_rect(item->x, sy, item->w, sh, sh / 2,
                               hovered ? GFX_ARGB(0x33, 0xFF, 0xFF, 0xFF)
                                       : GFX_ARGB(0x1F, 0xFF, 0xFF, 0xFF));
                gfx_round_frame(item->x, sy, item->w, sh, sh / 2, 1,
                                GFX_ARGB(0x33, 0xFF, 0xFF, 0xFF));
                icon_search(item->x + 10, sy + (sh - 14) / 2, 14, TASKBAR_TEXT_DIM);
                gfx_text_ellipsized(&uifont_small, item->x + 32,
                                    sy + (sh - uifont_small.line_height) / 2,
                                    item->w - 42, item->label, TASKBAR_TEXT_DIM);
                break;
            }
            case TB_PINNED: {
                if (down) gfx_fill(item->x, y, item->w, TASKBAR_H, TASKBAR_PRESSED);
                else if (hovered) gfx_fill(item->x, y, item->w, TASKBAR_H, TASKBAR_HOVER);
                icon_app(item->app, item->x + (item->w - 24) / 2, y + 8, 24);
                break;
            }
            case TB_WINDOW: {
                int active = (item->win == front && !item->win->minimized);
                if (down) gfx_fill(item->x, y, item->w, TASKBAR_H, TASKBAR_PRESSED);
                else if (active) gfx_fill(item->x, y, item->w, TASKBAR_H, TASKBAR_ACTIVE);
                else if (hovered) gfx_fill(item->x, y, item->w, TASKBAR_H, TASKBAR_HOVER);

                /* The running indicator: a bar along the bottom edge, in
                 * the accent color, wider and brighter for the window
                 * you're actually in. */
                int bar_w = active ? item->w - 12 : item->w / 3;
                gfx_round_rect(item->x + (item->w - bar_w) / 2, y + TASKBAR_H - 3,
                               bar_w, 3, 1,
                               active ? GFX_RGB(0x60, 0xCD, 0xFF)
                                      : GFX_ARGB(0xB0, 0x4C, 0xA6, 0xE0));

                icon_app(item->app, item->x + 10, y + 12, 16);
                if (item->w > TASK_BTN_ICON_W + 20) {
                    gfx_text_ellipsized(&uifont_small, item->x + 32,
                                        y + (TASKBAR_H - uifont_small.line_height) / 2 - 1,
                                        item->w - 42, item->label,
                                        item->win->minimized ? TASKBAR_TEXT_DIM
                                                             : TASKBAR_TEXT);
                }
                break;
            }
            case TB_TRAY:
                draw_tray(item);
                break;
            case TB_SHOW_DESKTOP:
                gfx_fill(item->x, y + 3, 1, TASKBAR_H - 6, TASKBAR_RULE);
                if (hovered || down) {
                    gfx_fill(item->x + 1, y, item->w - 1, TASKBAR_H,
                             TASKBAR_HOVER);
                }
                break;
            default: break;
        }
    }
}

/* The live preview a hovered taskbar button shows after a moment, the way
 * Windows has since Aero. The window's backing surface is right there, so
 * there's no reason to draw anything less than the real thing. */
static int thumbnail_target(gfx_rect_t* out) {
    if (taskbar_hover < 0 || taskbar_hover >= task_count) return 0;
    task_item_t* item = &task_items[taskbar_hover];
    if (item->kind != TB_WINDOW || !item->win || !item->win->surface) return 0;
    if (pit_get_ticks() - hover_since < 30) return 0;   /* ~300ms dwell */

    out->w = THUMB_W;
    out->h = THUMB_H;
    out->x = item->x + item->w / 2 - THUMB_W / 2;
    out->y = taskbar_rect.y - THUMB_H - 8;
    if (out->x < 6) out->x = 6;
    if (out->x + out->w > screen_w - 6) out->x = screen_w - 6 - out->w;
    return 1;
}

static void draw_thumbnail(void) {
    gfx_rect_t r;
    if (!thumbnail_target(&r)) return;
    window_t* win = task_items[taskbar_hover].win;

    gfx_blur(r.x, r.y, r.w, r.h, 6);
    gfx_round_rect(r.x, r.y + 3, r.w, r.h, 8, GFX_ARGB(0x50, 0, 0, 0));
    gfx_round_rect(r.x, r.y, r.w, r.h, 8, GFX_ARGB(0xEE, 0x24, 0x24, 0x2A));
    gfx_round_frame(r.x, r.y, r.w, r.h, 8, 1, GFX_ARGB(0x44, 0xFF, 0xFF, 0xFF));

    icon_app(win->app, r.x + 10, r.y + 8, 16);
    gfx_text_ellipsized(&uifont_small, r.x + 32, r.y + 9, r.w - 42, win->title,
                        GFX_RGB(0xF0, 0xF0, 0xF4));

    /* Keep the window's aspect ratio: a squashed preview reads as a
     * rendering bug rather than as a small window. */
    int bw = r.w - 20, bh = r.h - 40;
    int tw = bw, th = win->h * bw / win->w;
    if (th > bh) { th = bh; tw = win->w * bh / win->h; }
    int tx = r.x + (r.w - tw) / 2, ty = r.y + 32 + (bh - th) / 2;
    gfx_blit_scaled(win->surface, tx, ty, tw, th);
    gfx_frame(tx, ty, tw, th, GFX_ARGB(0x66, 0xFF, 0xFF, 0xFF));
}

/* --- Start menu --------------------------------------------------------- */

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

static start_entry_t* start_add(const char* name, const char* sub,
                                const app_t* app, int pic, int action, int tile) {
    if (start_entry_count >= START_MAX_ENTRIES) return 0;
    start_entry_t* e = &start_entries[start_entry_count++];
    kstrlcpy(e->name, name, sizeof(e->name));
    kstrlcpy(e->sub, sub ? sub : "", sizeof(e->sub));
    e->file[0] = '\0';
    e->app = app;
    e->pic = pic;
    e->action = action;
    e->tile = tile;
    e->r.x = e->r.y = e->r.w = e->r.h = 0;
    return e;
}

/* Everything the Start menu can offer, in one table so search and the
 * pinned grid can't disagree about what exists. */
static const struct { const char* name; const char* sub; const app_t* app;
                      int pic; int action; }
start_apps[] = {
    { "Terminal",      "App",             &app_terminal, PIC_APP,     ACT_TERMINAL },
    { "File Explorer", "App",             &app_files,    PIC_APP,     ACT_EXPLORER },
    { "Task Manager",  "App",             &app_monitor,  PIC_APP,     ACT_TASK_MANAGER },
    { "About Novaris", "System",          &app_about,    PIC_APP,     ACT_ABOUT },
    { "Novaris Help",  "System",          0,             PIC_DOCUMENT,ACT_HELP },
    { "Recycle Bin",   "System folder",   0,             PIC_RECYCLE, ACT_RECYCLE_BIN },
    { "Restart",       "Power",           0,             PIC_COMPUTER,ACT_RESTART },
    { "Shut down",     "Power",           0,             PIC_COMPUTER,ACT_SHUT_DOWN },
};
#define START_APP_COUNT ((int)(sizeof(start_apps) / sizeof(start_apps[0])))

/* Builds the entry list and gives every entry a rectangle. Draw and hit
 * test both read those rectangles, so they can't drift apart. */
static void start_layout(void) {
    start_entry_count = 0;

    start_rect.w = START_W;
    start_rect.h = START_H;
    start_rect.x = 12;
    start_rect.y = screen_h - TASKBAR_H - 10 - START_H;
    if (start_rect.y < 12) {
        start_rect.y = 12;
        start_rect.h = screen_h - TASKBAR_H - 22;
    }

    start_search_rect.x = start_rect.x + START_PAD;
    start_search_rect.y = start_rect.y + START_PAD;
    start_search_rect.w = START_W - START_PAD * 2;
    start_search_rect.h = START_SEARCH_H;

    start_power_rect.w = 36;
    start_power_rect.h = 36;
    start_power_rect.x = start_rect.x + START_W - START_PAD - 36;
    start_power_rect.y = start_rect.y + start_rect.h - START_FOOTER_H / 2 - 18;

    int y = start_search_rect.y + START_SEARCH_H + 16;
    int content_bottom = start_rect.y + start_rect.h - START_FOOTER_H - 8;

    if (start_len == 0) {
        /* Pinned grid, then whatever the initrd has to recommend. */
        int gx = start_rect.x + START_PAD;
        int col = 0;
        int row_y = y + 22;
        for (int i = 0; i < START_APP_COUNT && i < 6; i++) {
            start_entry_t* e = start_add(start_apps[i].name, start_apps[i].sub,
                                         start_apps[i].app, start_apps[i].pic,
                                         start_apps[i].action, 1);
            if (!e) break;
            e->r.x = gx + col * START_TILE_W;
            e->r.y = row_y;
            e->r.w = START_TILE_W;
            e->r.h = START_TILE_H;
            if (++col == START_TILE_COLS) { col = 0; row_y += START_TILE_H; }
        }
        if (col) row_y += START_TILE_H;

        int list_y = row_y + 34;
        if (vfs_root) {
            uint32_t idx = 0;
            vfs_node_t* node;
            while ((node = vfs_readdir(vfs_root, idx)) &&
                   list_y + START_ROW_H <= content_bottom) {
                start_entry_t* e = start_add(node->name, "Recently added", 0,
                                             PIC_DOCUMENT, ACT_OPEN_FILE, 0);
                if (!e) break;
                kstrlcpy(e->file, node->name, sizeof(e->file));
                e->r.x = start_rect.x + START_PAD;
                e->r.y = list_y;
                e->r.w = START_W - START_PAD * 2;
                e->r.h = START_ROW_H;
                list_y += START_ROW_H;
                idx++;
            }
        }
    } else {
        int list_y = y;
        for (int i = 0; i < START_APP_COUNT; i++) {
            if (list_y + START_ROW_H > content_bottom) break;
            if (!matches(start_apps[i].name, start_query)) continue;
            start_entry_t* e = start_add(start_apps[i].name, start_apps[i].sub,
                                         start_apps[i].app, start_apps[i].pic,
                                         start_apps[i].action, 0);
            if (!e) break;
            e->r.x = start_rect.x + START_PAD;
            e->r.y = list_y;
            e->r.w = START_W - START_PAD * 2;
            e->r.h = START_ROW_H;
            list_y += START_ROW_H;
        }
        if (vfs_root) {
            uint32_t idx = 0;
            vfs_node_t* node;
            while ((node = vfs_readdir(vfs_root, idx)) &&
                   list_y + START_ROW_H <= content_bottom) {
                if (matches(node->name, start_query)) {
                    start_entry_t* e = start_add(node->name, "File on this PC", 0,
                                                 PIC_DOCUMENT, ACT_OPEN_FILE, 0);
                    if (!e) break;
                    kstrlcpy(e->file, node->name, sizeof(e->file));
                    e->r.x = start_rect.x + START_PAD;
                    e->r.y = list_y;
                    e->r.w = START_W - START_PAD * 2;
                    e->r.h = START_ROW_H;
                    list_y += START_ROW_H;
                }
                idx++;
            }
        }
    }

    if (start_selected >= start_entry_count) start_selected = 0;
    if (start_entry_count == 0) start_selected = -1;
}

static void start_damage(void) {
    wm_damage(start_rect.x - 24, start_rect.y - 24, start_rect.w + 48,
              start_rect.h + 48);
}

static void start_show(void) {
    if (start_open) return;
    close_menu();
    start_open = 1;
    start_len = 0;
    start_query[0] = '\0';
    start_selected = start_len ? 0 : -1;
    start_hover = -1;
    start_layout();
    start_damage();
    wm_damage(0, screen_h - TASKBAR_H, screen_w, TASKBAR_H);
}

static void start_hide(void) {
    if (!start_open) return;
    start_damage();
    start_open = 0;
    start_hover = -1;
    wm_damage(0, screen_h - TASKBAR_H, screen_w, TASKBAR_H);
}

static void start_toggle(void) {
    if (start_open) start_hide();
    else start_show();
}

static int start_entry_at(int x, int y) {
    for (int i = 0; i < start_entry_count; i++) {
        if (gfx_rect_contains(&start_entries[i].r, x, y)) return i;
    }
    return -1;
}

static void start_activate(int index) {
    if (index < 0 || index >= start_entry_count) return;
    start_entry_t e = start_entries[index];
    start_hide();

    if (e.action == ACT_OPEN_FILE) {
        /* The same route a double-click in the File Explorer takes - see
         * app_open_path(). A search hit that ran a .exe through the
         * built-in Win32 layer while double-clicking the same file ran it
         * under Wine would be two answers to one question. */
        app_open_path(e.file);
        return;
    }
    run_action(e.action, 0);
}

static void draw_start_menu(void) {
    if (!start_open) return;
    start_layout();
    gfx_rect_t r = start_rect;

    gfx_blur(r.x, r.y, r.w, r.h, 9);
    gfx_round_rect(r.x, r.y + 6, r.w, r.h, 10, GFX_ARGB(0x55, 0, 0, 0));
    gfx_round_rect(r.x, r.y, r.w, r.h, 10, GFX_ARGB(0xF2, 0xF6, 0xF6, 0xFA));
    gfx_round_frame(r.x, r.y, r.w, r.h, 10, 1, GFX_ARGB(0x3A, 0, 0, 0));

    /* Search field. */
    gfx_rect_t s = start_search_rect;
    gfx_round_rect(s.x, s.y, s.w, s.h, s.h / 2, GFX_RGB(0xFF, 0xFF, 0xFF));
    gfx_round_frame(s.x, s.y, s.w, s.h, s.h / 2, 1, GFX_ARGB(0x35, 0, 0, 0));
    icon_search(s.x + 12, s.y + (s.h - 15) / 2, 15, UI_TEXT_DIM);
    if (start_len) {
        int tx = s.x + 36;
        gfx_text(&uifont_regular, tx,
                 s.y + (s.h - uifont_regular.line_height) / 2, start_query,
                 UI_TEXT);
        gfx_fill(tx + gfx_text_width(&uifont_regular, start_query) + 2,
                 s.y + 9, 1, s.h - 18, UI_ACCENT);
    } else {
        gfx_text(&uifont_regular, s.x + 36,
                 s.y + (s.h - uifont_regular.line_height) / 2,
                 "Type here to search", UI_TEXT_FAINT);
    }

    /* Headings, but only in the no-query layout - a search result list
     * doesn't get section titles. */
    if (start_len == 0 && start_entry_count) {
        gfx_text(&uifont_small, r.x + START_PAD, s.y + s.h + 18, "Pinned",
                 UI_TEXT_DIM);
        for (int i = 0; i < start_entry_count; i++) {
            if (start_entries[i].tile) continue;
            gfx_text(&uifont_small, r.x + START_PAD,
                     start_entries[i].r.y - 20, "Recommended", UI_TEXT_DIM);
            break;
        }
    }

    for (int i = 0; i < start_entry_count; i++) {
        start_entry_t* e = &start_entries[i];
        int hot = (i == start_hover || i == start_selected);
        if (hot) {
            gfx_round_rect(e->r.x, e->r.y, e->r.w, e->r.h, 6,
                           i == start_selected ? GFX_ARGB(0x2E, 0x00, 0x78, 0xD4)
                                               : FLYOUT_HOVER);
        }
        if (e->tile) {
            draw_pictogram(e->pic, e->app, e->r.x + (e->r.w - 32) / 2,
                           e->r.y + 14, 32);
            gfx_text_center(&uifont_small, e->r.x + 4, e->r.y + 56, e->r.w - 8,
                            e->name, UI_TEXT);
        } else {
            draw_pictogram(e->pic, e->app, e->r.x + 10, e->r.y + 6, 28);
            gfx_text_ellipsized(&uifont_regular, e->r.x + 48, e->r.y + 6,
                                e->r.w - 58, e->name, UI_TEXT);
            gfx_text_ellipsized(&uifont_small, e->r.x + 48, e->r.y + 23,
                                e->r.w - 58, e->sub, UI_TEXT_DIM);
        }
    }

    if (start_len && start_entry_count == 0) {
        gfx_text_center(&uifont_regular, r.x, s.y + s.h + 60, r.w,
                        "No results found", UI_TEXT_DIM);
    }

    /* Footer: who's signed in on the left, power on the right. */
    int fy = r.y + r.h - START_FOOTER_H;
    gfx_fill(r.x + 1, fy, r.w - 2, 1, GFX_ARGB(0x22, 0, 0, 0));
    gfx_circle(r.x + START_PAD + 14, fy + START_FOOTER_H / 2, 14, UI_ACCENT);
    gfx_text(&uifont_small, r.x + START_PAD + 12, fy + START_FOOTER_H / 2 - 7,
             "N", GFX_RGB(0xFF, 0xFF, 0xFF));
    gfx_text(&uifont_regular, r.x + START_PAD + 38,
             fy + (START_FOOTER_H - uifont_regular.line_height) / 2, "Novaris",
             UI_TEXT);

    int power_hot = gfx_rect_contains(&start_power_rect, cursor_x, cursor_y);
    if (power_hot) {
        gfx_round_rect(start_power_rect.x, start_power_rect.y,
                       start_power_rect.w, start_power_rect.h, 5, FLYOUT_HOVER);
    }
    icon_power(start_power_rect.x + 9, start_power_rect.y + 9, 18, UI_TEXT);
}

/* --- menus -------------------------------------------------------------- */

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
    item->arg = 0;
}

static void close_menu(void) {
    if (!menu_open) return;
    wm_damage(menu_rect.x - 6, menu_rect.y - 6, menu_rect.w + 24,
              menu_rect.h + 24);
    menu_open = 0;
    menu_hover = -1;
    menu_target = 0;
}

/* Places the menu at (x, y), flipping it up or left when it would run off
 * the screen - which is the whole reason a context menu needs its own
 * placement pass rather than just being drawn where the click was. */
static void show_menu_at(int x, int y) {
    int width = 170;
    for (int i = 0; i < menu_item_count; i++) {
        int w = MENU_TEXT_X + gfx_text_width(&uifont_regular, menu_items[i].label) +
                MENU_RIGHT_PAD;
        if (menu_items[i].shortcut[0]) {
            w += 26 + gfx_text_width(&uifont_regular, menu_items[i].shortcut);
        }
        if (w > width) width = w;
    }
    int height = MENU_PAD_Y * 2;
    for (int i = 0; i < menu_item_count; i++) {
        height += menu_items[i].separator ? MENU_SEP_H : MENU_ITEM_H;
    }

    menu_rect.x = x;
    menu_rect.y = y;
    menu_rect.w = width;
    menu_rect.h = height;
    if (menu_rect.x + width > screen_w - 6) menu_rect.x = screen_w - 6 - width;
    if (menu_rect.x < 6) menu_rect.x = 6;
    if (menu_rect.y + height > screen_h - 6) menu_rect.y = y - height;
    if (menu_rect.y < 6) menu_rect.y = 6;

    menu_open = 1;
    menu_hover = -1;
    wm_damage(menu_rect.x - 6, menu_rect.y - 6, width + 24, height + 24);
}

static void show_desktop_menu(int x, int y) {
    menu_item_count = 0;
    menu_target = 0;
    add_item("Refresh", 0, ACT_REFRESH, 0, 1);
    add_separator();
    add_item("Open Terminal", 0, ACT_TERMINAL, 0, 1);
    add_item("Open File Explorer", 0, ACT_EXPLORER, 0, 1);
    add_separator();
    add_item("Show desktop", 0, ACT_SHOW_DESKTOP, 0, 1);
    add_item("Task Manager", 0, ACT_TASK_MANAGER, 0, 1);
    add_separator();
    add_item("About Novaris", 0, ACT_ABOUT, 0, 1);
    show_menu_at(x, y);
}

/* A window's system menu - what Alt+Space, a right-click on the title bar
 * and a click on the window icon all open. */
static void show_system_menu(window_t* win, int x, int y) {
    if (!win) return;
    menu_item_count = 0;
    menu_target = win;
    int floating = !win->maximized && win->snapped == WM_SNAP_NONE;
    add_item("Restore", 0, ACT_WIN_RESTORE, 0, !floating);
    add_item("Minimize", 0, ACT_WIN_MINIMIZE, 0, !win->minimized);
    add_item("Maximize", 0, ACT_WIN_MAXIMIZE, 0, win->resizable && !win->maximized);
    add_separator();
    add_item("Close", "Alt+F4", ACT_WIN_CLOSE, 0, 1);
    show_menu_at(x, y);
}

static void show_taskbar_menu(int index, int x, int y) {
    task_item_t* item = &task_items[index];
    menu_item_count = 0;
    menu_target = 0;
    if (item->kind == TB_WINDOW) {
        menu_target = item->win;
        add_item(item->win->app ? item->win->app->name : "Window", 0, ACT_NONE, 0, 0);
        add_separator();
        add_item("Restore", 0, ACT_WIN_RESTORE, 0,
                 item->win->minimized || item->win->maximized ||
                 item->win->snapped != WM_SNAP_NONE);
        add_item("Minimize", 0, ACT_WIN_MINIMIZE, 0, !item->win->minimized);
        add_item("Maximize", 0, ACT_WIN_MAXIMIZE, 0,
                 item->win->resizable && !item->win->maximized);
        add_separator();
        add_item("Close window", "Alt+F4", ACT_WIN_CLOSE, 0, 1);
    } else if (item->kind == TB_PINNED) {
        add_item(item->label, 0, ACT_NONE, 0, 0);
        add_separator();
        add_item("Open", 0, item->action, 0, 1);
    } else {
        add_item("Task Manager", 0, ACT_TASK_MANAGER, 0, 1);
        add_separator();
        add_item("Show desktop", 0, ACT_SHOW_DESKTOP, 0, 1);
    }
    show_menu_at(x, y);
}

static void show_power_menu(void) {
    menu_item_count = 0;
    menu_target = 0;
    add_item("Restart", 0, ACT_RESTART, 0, 1);
    add_item("Shut down", 0, ACT_SHUT_DOWN, 0, 1);
    show_menu_at(start_power_rect.x - 40,
                 start_power_rect.y + start_power_rect.h + 4);
}

static int menu_item_at(int x, int y) {
    if (!menu_open || !gfx_rect_contains(&menu_rect, x, y)) return -1;
    int cy = menu_rect.y + MENU_PAD_Y;
    for (int i = 0; i < menu_item_count; i++) {
        int h = menu_items[i].separator ? MENU_SEP_H : MENU_ITEM_H;
        if (y >= cy && y < cy + h) return menu_items[i].separator ? -1 : i;
        cy += h;
    }
    return -1;
}

static void draw_menu(void) {
    if (!menu_open) return;

    gfx_blur(menu_rect.x, menu_rect.y, menu_rect.w, menu_rect.h, 6);
    gfx_round_rect(menu_rect.x + 1, menu_rect.y + 4, menu_rect.w, menu_rect.h,
                   FLYOUT_RADIUS, GFX_ARGB(0x4A, 0, 0, 0));
    gfx_round_rect(menu_rect.x, menu_rect.y, menu_rect.w, menu_rect.h,
                   FLYOUT_RADIUS, FLYOUT_FILL);
    gfx_round_frame(menu_rect.x, menu_rect.y, menu_rect.w, menu_rect.h,
                    FLYOUT_RADIUS, 1, FLYOUT_BORDER);

    int y = menu_rect.y + MENU_PAD_Y;
    for (int i = 0; i < menu_item_count; i++) {
        menu_item_t* item = &menu_items[i];
        if (item->separator) {
            gfx_fill(menu_rect.x + 10, y + MENU_SEP_H / 2, menu_rect.w - 20, 1,
                     GFX_ARGB(0x28, 0, 0, 0));
            y += MENU_SEP_H;
            continue;
        }
        uint32_t color = item->enabled ? UI_TEXT : UI_TEXT_FAINT;
        if (i == menu_hover && item->enabled) {
            gfx_round_rect(menu_rect.x + 4, y, menu_rect.w - 8, MENU_ITEM_H, 4,
                           FLYOUT_HOVER);
        }
        if (item->checked) {
            gfx_text(&uifont_regular, menu_rect.x + 13,
                     y + (MENU_ITEM_H - uifont_regular.line_height) / 2,
                     UI_CHECK, color);
        }
        int text_y = y + (MENU_ITEM_H - uifont_regular.line_height) / 2;
        int avail = menu_rect.w - MENU_TEXT_X - MENU_RIGHT_PAD;
        if (item->shortcut[0]) {
            avail -= 26 + gfx_text_width(&uifont_regular, item->shortcut);
        }
        gfx_text_ellipsized(&uifont_regular, menu_rect.x + MENU_TEXT_X, text_y,
                            avail, item->label, color);
        if (item->shortcut[0]) {
            gfx_text_right(&uifont_regular, menu_rect.x, text_y,
                           menu_rect.w - MENU_RIGHT_PAD, item->shortcut,
                           UI_TEXT_DIM);
        }
        y += MENU_ITEM_H;
    }
}

/* --- Alt+Tab switcher --------------------------------------------------- */

static void switcher_geometry(void) {
    int cols = switcher_count > 0 ? switcher_count : 1;
    int w = cols * SWITCH_TILE + 24;
    int max_w = screen_w - 80;
    if (w > max_w) w = max_w;
    switcher_rect.w = w;
    switcher_rect.h = SWITCH_TILE + 24 + 26;
    switcher_rect.x = (screen_w - w) / 2;
    switcher_rect.y = (screen_h - switcher_rect.h) / 2;
}

static void switcher_damage(void) {
    wm_damage(switcher_rect.x - 20, switcher_rect.y - 20,
              switcher_rect.w + 40, switcher_rect.h + 40);
}

/* Most-recently-used order, which is the reverse of the z-order the
 * window manager keeps. Minimized windows are included: on Windows they
 * are exactly what Alt+Tab is for. */
static int switcher_show(void) {
    switcher_count = 0;
    for (int i = wm_count() - 1; i >= 0; i--) {
        window_t* win = wm_at(i);
        if (win && win->open) switcher_list[switcher_count++] = win;
    }
    if (switcher_count < 2) return 0;
    switcher_open = 1;
    switcher_index = 1;    /* Alt+Tab alone goes to the previous window */
    switcher_geometry();
    switcher_damage();
    return 1;
}

static void switcher_step(int delta) {
    if (!switcher_open || switcher_count == 0) return;
    switcher_index = (switcher_index + delta + switcher_count) % switcher_count;
    switcher_damage();
}

static void switcher_commit(int apply) {
    if (!switcher_open) return;
    switcher_damage();
    switcher_open = 0;
    if (apply && switcher_index >= 0 && switcher_index < switcher_count) {
        window_t* win = switcher_list[switcher_index];
        if (win && win->open) wm_focus(win);
    }
}

static void draw_switcher(void) {
    if (!switcher_open) return;
    switcher_geometry();
    gfx_rect_t r = switcher_rect;

    gfx_blur(r.x, r.y, r.w, r.h, 8);
    gfx_round_rect(r.x, r.y + 6, r.w, r.h, 10, GFX_ARGB(0x60, 0, 0, 0));
    gfx_round_rect(r.x, r.y, r.w, r.h, 10, GFX_ARGB(0xE8, 0x22, 0x22, 0x28));
    gfx_round_frame(r.x, r.y, r.w, r.h, 10, 1, GFX_ARGB(0x40, 0xFF, 0xFF, 0xFF));

    int tile = (r.w - 24) / (switcher_count ? switcher_count : 1);
    if (tile > SWITCH_TILE) tile = SWITCH_TILE;
    int x = r.x + (r.w - tile * switcher_count) / 2;

    for (int i = 0; i < switcher_count; i++) {
        window_t* win = switcher_list[i];
        int tx = x + i * tile, ty = r.y + 12;
        if (i == switcher_index) {
            gfx_round_rect(tx + 2, ty - 2, tile - 4, tile + 4, 6,
                           GFX_ARGB(0x40, 0xFF, 0xFF, 0xFF));
            gfx_round_frame(tx + 2, ty - 2, tile - 4, tile + 4, 6, 1,
                            GFX_ARGB(0xCC, 0x60, 0xCD, 0xFF));
        }
        int bw = tile - 22, bh = tile - 34;
        int tw = bw, th = win->h * bw / win->w;
        if (th > bh) { th = bh; tw = win->w * bh / win->h; }
        if (tw > 0 && th > 0 && win->surface) {
            gfx_blit_scaled(win->surface, tx + (tile - tw) / 2,
                            ty + 6 + (bh - th) / 2, tw, th);
        }
        icon_app(win->app, tx + tile / 2 - 8, ty + tile - 24, 16);
    }

    window_t* sel = (switcher_index >= 0 && switcher_index < switcher_count)
                        ? switcher_list[switcher_index] : 0;
    if (sel) {
        gfx_text_center(&uifont_regular, r.x + 10, r.y + r.h - 24, r.w - 20,
                        sel->title, GFX_RGB(0xF0, 0xF0, 0xF6));
    }
}

/* --- system actions ----------------------------------------------------- */

static void reboot(void) {
    /* Pulse the 8042's reset line - the classic PC way to reboot. */
    while (inb(0x64) & 0x02) { }
    outb(0x64, 0xFE);
    for (;;) __asm__ __volatile__("hlt");
}

static void shut_down(void) {
    gfx_set_target(screen);
    gfx_clip_reset();
    gfx_clear(GFX_RGB(0x00, 0x55, 0xA5));
    icon_window_logo(screen_w / 2 - 26, screen_h / 2 - 130, 52,
                     GFX_RGB(0xFF, 0xFF, 0xFF));
    gfx_text_center(&uifont_title, 0, screen_h / 2 - 40, screen_w,
                    "It's now safe to turn off your computer",
                    GFX_RGB(0xFF, 0xFF, 0xFF));
    gfx_text_center(&uifont_regular, 0, screen_h / 2 + 4, screen_w,
                    "Novaris has shut down.", GFX_ARGB(0xCC, 0xFF, 0xFF, 0xFF));
    gfx_present(screen, 0, 0, screen_w, screen_h);

    /* QEMU and most virtual machines honor one of these ACPI ports; real
     * hardware generally won't, which is what the message above is for. */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    __asm__ __volatile__("cli");
    for (;;) __asm__ __volatile__("hlt");
}

static void run_action(int action, int arg) {
    (void)arg;
    window_t* target = menu_target ? menu_target : wm_focused();
    switch (action) {
        case ACT_ABOUT: about_open(); break;
        case ACT_TASK_MANAGER: monitor_open(); break;
        case ACT_TERMINAL: terminal_open(); break;
        case ACT_EXPLORER: files_open(); break;
        case ACT_RESTART: reboot(); break;
        case ACT_SHUT_DOWN: shut_down(); break;
        case ACT_REFRESH: wm_damage_all(); break;
        case ACT_SHOW_DESKTOP: wm_toggle_show_desktop(); break;
        case ACT_RECYCLE_BIN:
            alert_open("Recycle Bin is empty",
                       "Novaris keeps its files on a read-only initrd, so there "
                       "is nothing here to throw away.");
            break;
        case ACT_HELP:
            if (!textview_open("readme.txt")) {
                alert_open("Novaris Help",
                           "Click Start or a taskbar button to open an app. "
                           "Drag a window by its title bar, resize it from any "
                           "edge, drag it against a screen edge to snap it, and "
                           "press Alt+Tab to switch windows.");
            }
            break;
        case ACT_WIN_RESTORE:
            if (target) {
                if (target->minimized) wm_unminimize(target);
                else wm_restore(target);
            }
            break;
        case ACT_WIN_MINIMIZE: if (target) wm_minimize(target); break;
        case ACT_WIN_MAXIMIZE: if (target) wm_maximize(target); break;
        case ACT_WIN_CLOSE: if (target) wm_close(target); break;
        case ACT_WIN_FOCUS: if (target) wm_focus(target); break;
        default: break;
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

#define CURSOR_PAD 12   /* the resize cursors are wider than the arrow */

static void draw_arrow_cursor(int x, int y) {
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

/* The resize pointers are a double-headed arrow along one of four axes.
 * One routine covers all four because the only thing that differs is the
 * step taken per pixel along the shaft; the arrowheads are laid out along
 * the perpendicular (-dy, dx), which is what keeps the diagonal ones from
 * degenerating into a thick line.
 *
 * Drawn twice: a fat dark pass first, then a thin white one on top, which
 * is how a cursor stays visible over both a black terminal and a white
 * document without any compositing tricks. */
static void draw_resize_cursor(int cx, int cy, int dx, int dy) {
    const int len = 9;
    const int px = -dy, py = dx;    /* perpendicular to the shaft */

    for (int pass = 0; pass < 2; pass++) {
        uint32_t color = pass ? GFX_RGB(0xFF, 0xFF, 0xFF)
                              : GFX_RGB(0x10, 0x10, 0x14);
        int grow = pass ? 0 : 1;    /* the dark pass is the outline */

        for (int i = -len; i <= len; i++) {
            for (int o = -grow; o <= grow; o++) {
                for (int p = -grow; p <= grow; p++) {
                    gfx_pixel(cx + dx * i + o, cy + dy * i + p, color);
                }
            }
        }
        for (int end = -1; end <= 1; end += 2) {
            int tipx = cx + dx * len * end, tipy = cy + dy * len * end;
            for (int i = 0; i <= 4 + grow; i++) {
                for (int j = -i; j <= i; j++) {
                    gfx_pixel(tipx - dx * i * end + px * j,
                              tipy - dy * i * end + py * j, color);
                }
            }
        }
    }
}

static void draw_cursor(int x, int y) {
    switch (wm_cursor_shape()) {
        case WM_CURSOR_SIZE_WE:   draw_resize_cursor(x + 1, y + 1, 1, 0); break;
        case WM_CURSOR_SIZE_NS:   draw_resize_cursor(x + 1, y + 1, 0, 1); break;
        case WM_CURSOR_SIZE_NWSE: draw_resize_cursor(x + 1, y + 1, 1, 1); break;
        case WM_CURSOR_SIZE_NESW: draw_resize_cursor(x + 1, y + 1, 1, -1); break;
        default: draw_arrow_cursor(x, y); break;
    }
}

static void damage_cursor(int x, int y) {
    wm_damage(x - CURSOR_PAD, y - CURSOR_PAD, CURSOR_W + CURSOR_PAD * 2,
              CURSOR_H + CURSOR_PAD * 2);
}

/* --- compositing -------------------------------------------------------- */

int desktop_taskbar_height(void) { return TASKBAR_H; }

/* Panels read the pixels around them (that's what a blur is), so any
 * frame that touches part of one has to recomposite all of it - otherwise
 * the blur would sample whatever the previous frame happened to leave
 * behind. */
static void expand_for_panels(gfx_rect_t* r) {
    gfx_rect_t bar = { 0, screen_h - TASKBAR_H, screen_w, TASKBAR_H };
    if (gfx_rect_intersects(r, &bar)) gfx_rect_union(r, &bar);

    if (start_open) {
        gfx_rect_t s = { start_rect.x - 2, start_rect.y - 2, start_rect.w + 6,
                         start_rect.h + 12 };
        if (gfx_rect_intersects(r, &s)) gfx_rect_union(r, &s);
    }
    if (menu_open) {
        gfx_rect_t m = { menu_rect.x - 2, menu_rect.y - 2, menu_rect.w + 8,
                         menu_rect.h + 10 };
        if (gfx_rect_intersects(r, &m)) gfx_rect_union(r, &m);
    }
    if (switcher_open) {
        gfx_rect_t s = { switcher_rect.x - 2, switcher_rect.y - 2,
                         switcher_rect.w + 6, switcher_rect.h + 12 };
        if (gfx_rect_intersects(r, &s)) gfx_rect_union(r, &s);
    }
    gfx_rect_t thumb;
    if (thumbnail_target(&thumb)) {
        gfx_rect_t t = { thumb.x - 2, thumb.y - 2, thumb.w + 6, thumb.h + 12 };
        if (gfx_rect_intersects(r, &t)) gfx_rect_union(r, &t);
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
    draw_desk_icons();

    wm_composite(&damage);

    gfx_set_target(screen);
    gfx_clip(damage.x, damage.y, damage.w, damage.h);
    draw_taskbar();
    draw_thumbnail();
    draw_start_menu();
    draw_menu();
    draw_switcher();
    draw_cursor(cursor_x, cursor_y);

    gfx_present(screen, damage.x, damage.y, damage.w, damage.h);
}

/* Compositing alone, with no input and no housekeeping: what a
 * long-running command's output needs so that it streams instead of
 * arriving all at once (see app_terminal.c). Kept separate from
 * desktop_pump() below on purpose - printing must not be a route by which
 * a keystroke gets dispatched. */
static int composing;   /* see desktop_pump() */

void desktop_pump_output(void) {
    if (composing) return;
    gfx_rect_t damage;
    composing = 1;
    if (wm_take_damage(&damage)) compose(damage);
    composing = 0;
}

/* --- input -------------------------------------------------------------- */

static void handle_taskbar_click(int index) {
    if (index < 0 || index >= task_count) return;
    task_item_t* item = &task_items[index];

    switch (item->kind) {
        case TB_START:
            start_toggle();
            break;
        case TB_SEARCH:
            if (!start_open) start_show();
            break;
        case TB_SHOW_DESKTOP:
            wm_toggle_show_desktop();
            break;
        case TB_PINNED:
            run_action(item->action, 0);
            break;
        case TB_WINDOW:
            /* Click the button of the window you're already in and it
             * minimizes - the one taskbar behavior everybody has muscle
             * memory for. */
            if (item->win->minimized) wm_unminimize(item->win);
            else if (item->win == wm_focused()) wm_minimize(item->win);
            else wm_focus(item->win);
            break;
        default: break;
    }
}

static void handle_mouse(const mouse_event_t* ev) {
    prev_cursor_x = cursor_x;
    prev_cursor_y = cursor_y;
    cursor_x = ev->x;
    cursor_y = ev->y;
    int moved = (cursor_x != prev_cursor_x || cursor_y != prev_cursor_y);
    if (moved) {
        damage_cursor(prev_cursor_x, prev_cursor_y);
        damage_cursor(cursor_x, cursor_y);
    }

    /* Taskbar hover, which drives both the button highlight and the
     * delayed thumbnail preview. */
    int over_task = taskbar_item_at(cursor_x, cursor_y);
    if (over_task != taskbar_hover) {
        gfx_rect_t old_thumb;
        if (thumbnail_target(&old_thumb)) {
            wm_damage(old_thumb.x - 4, old_thumb.y - 4, old_thumb.w + 8,
                      old_thumb.h + 16);
        }
        taskbar_hover = over_task;
        hover_since = pit_get_ticks();
        thumb_shown = 0;
        wm_damage(0, screen_h - TASKBAR_H, screen_w, TASKBAR_H);
    }

    /* An open menu swallows everything until it closes. */
    if (menu_open) {
        int item = menu_item_at(cursor_x, cursor_y);
        if (item != menu_hover) {
            menu_hover = item;
            wm_damage(menu_rect.x, menu_rect.y, menu_rect.w, menu_rect.h);
        }
        if (ev->pressed & (MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT)) {
            if (item < 0 && !gfx_rect_contains(&menu_rect, cursor_x, cursor_y)) {
                close_menu();
            }
            return;
        }
        if (ev->released & MOUSE_BUTTON_LEFT) {
            if (item >= 0 && menu_items[item].enabled) {
                int action = menu_items[item].action;
                int arg = menu_items[item].arg;
                window_t* target = menu_target;
                menu_open = 0;      /* keep menu_target for run_action */
                menu_hover = -1;
                wm_damage(menu_rect.x - 6, menu_rect.y - 6, menu_rect.w + 24,
                          menu_rect.h + 24);
                menu_target = target;
                run_action(action, arg);
                menu_target = 0;
            }
            return;
        }
        return;
    }

    if (start_open) {
        int entry = start_entry_at(cursor_x, cursor_y);
        if (entry != start_hover) {
            start_hover = entry;
            start_damage();
        }
        if (ev->pressed & MOUSE_BUTTON_LEFT) {
            if (gfx_rect_contains(&start_power_rect, cursor_x, cursor_y)) {
                show_power_menu();
                return;
            }
            if (entry >= 0) {
                start_activate(entry);
                return;
            }
            if (!gfx_rect_contains(&start_rect, cursor_x, cursor_y)) {
                /* Clicking Start again while it's open has to close it and
                 * not immediately reopen it, so the click is spent here. */
                int over = taskbar_item_at(cursor_x, cursor_y);
                start_hide();
                if (over >= 0 && task_items[over].kind != TB_START) {
                    handle_taskbar_click(over);
                }
                return;
            }
            return;
        }
        if (ev->released & MOUSE_BUTTON_LEFT) return;
    }

    if (ev->pressed & MOUSE_BUTTON_RIGHT) {
        if (over_task >= 0) {
            show_taskbar_menu(over_task, cursor_x, cursor_y - 4);
            return;
        }
        if (cursor_y >= taskbar_rect.y) {
            menu_item_count = 0;
            menu_target = 0;
            add_item("Task Manager", 0, ACT_TASK_MANAGER, 0, 1);
            add_separator();
            add_item("Show desktop", 0, ACT_SHOW_DESKTOP, 0, 1);
            show_menu_at(cursor_x, cursor_y - 4);
            return;
        }
        if (wm_handle_mouse(ev)) return;
        show_desktop_menu(cursor_x, cursor_y);
        return;
    }

    if (ev->pressed & MOUSE_BUTTON_LEFT) {
        if (over_task >= 0) {
            taskbar_pressed = over_task;
            wm_damage(0, screen_h - TASKBAR_H, screen_w, TASKBAR_H);
            return;
        }
        if (cursor_y >= taskbar_rect.y) return;
    }

    if (ev->released & MOUSE_BUTTON_LEFT) {
        if (taskbar_pressed >= 0) {
            int index = taskbar_pressed;
            taskbar_pressed = -1;
            wm_damage(0, screen_h - TASKBAR_H, screen_w, TASKBAR_H);
            if (over_task == index) handle_taskbar_click(index);
            return;
        }
    }

    if (wm_handle_mouse(ev)) {
        if (desk_selected >= 0 && (ev->pressed & MOUSE_BUTTON_LEFT)) {
            int old = desk_selected;
            desk_selected = -1;
            wm_damage(desk_icons[old].r.x, desk_icons[old].r.y,
                      desk_icons[old].r.w, desk_icons[old].r.h);
        }
        return;
    }

    /* Nothing above wanted it, so this is the desktop itself. */
    if (ev->pressed & MOUSE_BUTTON_LEFT) {
        int icon = desk_icon_at(cursor_x, cursor_y);
        static uint32_t last_icon_click;
        static int last_icon;
        uint32_t now = pit_get_ticks();
        if (icon >= 0 && icon == last_icon && now - last_icon_click < 50) {
            last_icon = -1;
            run_action(desk_icons[icon].action, 0);
            return;
        }
        last_icon = icon;
        last_icon_click = now;
        if (icon != desk_selected) {
            int old = desk_selected;
            desk_selected = icon;
            if (old >= 0) {
                wm_damage(desk_icons[old].r.x, desk_icons[old].r.y,
                          desk_icons[old].r.w, desk_icons[old].r.h);
            }
            if (icon >= 0) {
                wm_damage(desk_icons[icon].r.x, desk_icons[icon].r.y,
                          desk_icons[icon].r.w, desk_icons[icon].r.h);
            }
        }
    }
}

static void handle_start_key(const key_event_t* ev) {
    if (ev->code == KEY_ESCAPE) { start_hide(); return; }
    if (ev->code == KEY_ENTER) {
        if (start_selected >= 0) start_activate(start_selected);
        else if (start_entry_count) start_activate(0);
        else start_hide();
        return;
    }
    if (ev->code == KEY_UP || ev->code == KEY_DOWN) {
        if (!start_entry_count) return;
        start_selected += (ev->code == KEY_DOWN) ? 1 : -1;
        if (start_selected < 0) start_selected = start_entry_count - 1;
        if (start_selected >= start_entry_count) start_selected = 0;
        start_damage();
        return;
    }
    if (ev->code == KEY_BACKSPACE) {
        if (start_len > 0) start_query[--start_len] = '\0';
        start_selected = start_len ? 0 : -1;
        start_layout();
        start_damage();
        return;
    }
    if (ev->ascii >= 32 && ev->ascii < 127 &&
        start_len < (int)sizeof(start_query) - 1) {
        start_query[start_len++] = (char)ev->ascii;
        start_query[start_len] = '\0';
        start_selected = 0;
        start_layout();
        start_damage();
    }
}

static void handle_key(const key_event_t* ev) {
    /* Releases matter for exactly two things, and both are Windows
     * gestures: letting go of Alt commits an Alt+Tab, and tapping the
     * Windows key on its own opens Start. */
    if (!ev->pressed) {
        if (ev->code == KEY_LALT || ev->code == KEY_RALT) switcher_commit(1);
        if (ev->code == KEY_SUPER && super_alone) {
            super_alone = 0;
            start_toggle();
        }
        return;
    }

    if (ev->code == KEY_SUPER) { super_alone = 1; return; }
    if (ev->mods & KEY_MOD_SUPER) super_alone = 0;

    if (switcher_open) {
        if (ev->code == KEY_TAB) {
            switcher_step((ev->mods & KEY_MOD_SHIFT) ? -1 : 1);
            return;
        }
        if (ev->code == KEY_ESCAPE) { switcher_commit(0); return; }
        if (ev->code == KEY_LEFT) { switcher_step(-1); return; }
        if (ev->code == KEY_RIGHT) { switcher_step(1); return; }
        return;
    }

    if ((ev->mods & KEY_MOD_ALT) && ev->code == KEY_TAB) {
        if (switcher_show()) return;
    }

    /* Ctrl+Shift+Esc opens Task Manager, on Novaris as on Windows. */
    if ((ev->mods & KEY_MOD_CTRL) && (ev->mods & KEY_MOD_SHIFT) &&
        ev->code == KEY_ESCAPE) {
        monitor_open();
        return;
    }

    if (ev->mods & KEY_MOD_SUPER) {
        switch (ev->ascii) {
            case 'd': case 'D': wm_toggle_show_desktop(); return;
            case 'e': case 'E': files_open(); return;
            case 'r': case 'R': case 's': case 'S':
                start_show();
                return;
            default: break;
        }
    }

    if (menu_open && ev->code == KEY_ESCAPE) { close_menu(); return; }

    if (start_open) { handle_start_key(ev); return; }

    if (ev->code == KEY_ESCAPE && desk_selected >= 0) {
        int old = desk_selected;
        desk_selected = -1;
        wm_damage(desk_icons[old].r.x, desk_icons[old].r.y,
                  desk_icons[old].r.w, desk_icons[old].r.h);
        return;
    }

    wm_handle_key(ev);
}

/* --- the frame ----------------------------------------------------------
 *
 * One turn of the desktop: drain input, do the housekeeping that is due,
 * and composite whatever is damaged. Returns whether anything was drawn,
 * so the idle loop knows when it may `hlt`.
 *
 * This used to be the body of desktop_start()'s loop and nothing else
 * could reach it, which was fine while every window was drawn by kernel
 * code. Milestone 36 changed that. A ring-3 program that owns a window
 * (see kernel/wmdev.c) runs *inside* a shell command, and a shell command
 * runs inside this loop - so while such a program is drawing, the loop it
 * needs is sitting on the stack underneath it, blocked. Its window would
 * appear when the program exited, which is not a window.
 *
 * So the program's own ioctls turn the crank: "here is a new frame" and
 * "give me my input" each pump the desktop once. That is not a
 * concession, it is the right shape for a kernel with no preemption of
 * its own event loop - the process gets exactly the frames it asks for,
 * and a process that asks for none costs nothing.
 *
 * Reentrancy is the whole difficulty here, and it is worth being precise
 * about which kind. Nesting *is* the point - the outer call is dispatching
 * the keystroke that started the command whose syscall makes the inner
 * call - so a guard around the whole function would make every nested
 * pump a no-op, which is the same as not having one. What must not nest
 * is compositing: a repaint prints, printing pumps output
 * (app_terminal.c), and a compose inside a compose draws with another
 * frame's clip and origin. So `composing` guards exactly that, and input
 * dispatch is left free to nest.
 *
 * Which is safe because of two things that are not here. Every way the
 * desktop starts a program goes through shell_run_line(), and the shell
 * refuses to start a second command while one is running (shell_busy).
 * And the window manager's own state - drags, focus, z-order - is touched
 * only from these handlers, which run to completion before the syscall
 * that nested them returns.
 */
int desktop_pump(void) {
    mouse_event_t mev;
    while (mouse_poll_event(&mev)) handle_mouse(&mev);

    key_event_t kev;
    while (keyboard_poll_event(&kev)) handle_key(&kev);

    /* A window opening, closing or minimizing changes what the taskbar
     * shows, and the taskbar is nowhere near the window that changed - so
     * window damage alone would leave it stale. */
    if (wm_generation() != last_generation) {
        last_generation = wm_generation();
        wm_damage(0, screen_h - TASKBAR_H, screen_w, TASKBAR_H);
    }

    uint32_t now = pit_get_ticks();
    /* A heartbeat, so a run that takes five minutes says where the five
     * minutes went. Three measurements have now come back too small to
     * explain it - 32MB of mapping copies, no disk at all, under two
     * thousand socket operations - which means the next thing to look at
     * is the shape of the timeline rather than another suspect. */
    {
        extern uint32_t user_fault_count;
        static uint32_t last_beat;
        if (now - last_beat >= 500) {          /* every five seconds */
            last_beat = now;
            char num[12];
            terminal_writestring_color("[hb] ", VGA_COLOR_LIGHT_CYAN);
            ku32_to_dec(now / 100, num);
            terminal_writestring(num);
            terminal_writestring("s  faults ");
            ku32_to_dec(user_fault_count, num);
            terminal_writestring(num);
            terminal_writestring("\n");
        }
    }

    if (now - last_tick >= 10) {       /* ~10 Hz housekeeping */
        last_tick = now;
        wm_tick();

        rtc_time_t clock;
        rtc_read(&clock);
        if (clock.minute != last_minute) {
            last_minute = clock.minute;
            wm_damage(0, screen_h - TASKBAR_H, screen_w, TASKBAR_H);
        }
        /* The hover preview appears on a timer, so the one frame it
         * becomes due on has to be asked for - and only that one, or a
         * stationary pointer would repaint it ten times a second for as
         * long as it hovered. */
        gfx_rect_t thumb;
        int due = thumbnail_target(&thumb);
        if (due && !thumb_shown) {
            thumb_shown = 1;
            wm_damage(thumb.x, thumb.y, thumb.w, thumb.h);
        }
    }

    if (composing) return 0;

    gfx_rect_t damage;
    int drew = 0;
    composing = 1;
    if (wm_take_damage(&damage)) {
        compose(damage);
        drew = 1;
    }
    composing = 0;
    return drew;
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

    /* From here the framebuffer is the compositor's; see framebuffer.c. */
    fb_set_compositor_owned(1);

    mouse_set_bounds(screen_w, screen_h);
    cursor_x = prev_cursor_x = screen_w / 2;
    cursor_y = prev_cursor_y = screen_h / 2;

    /* No menu bar: the taskbar is the only reserved edge, so the work
     * area runs from the very top of the screen down to it. */
    wm_init(screen_w, screen_h, 0, TASKBAR_H);
    wm_set_sysmenu_handler(show_system_menu);

    /* /dev/wm only means anything once there is a window manager to open
     * a window in, so it is registered here rather than at boot: a
     * headless boot has no desktop and should not advertise one. */
    wmdev_init();

    desk_icons_layout();
    taskbar_layout();

    terminal_app_init();
    terminal_open();
    shell_init();   /* banner and first prompt, now inside the window */

    wm_damage_all();
    last_tick = pit_get_ticks();

    for (;;) {
        if (!desktop_pump()) {
            /* Nothing to draw: sleep until the next interrupt rather than
             * spinning. The PIT alone wakes us 100 times a second. */
            __asm__ __volatile__("hlt");
        }
    }
}
