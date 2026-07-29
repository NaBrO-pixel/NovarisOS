/* wm.c - the window manager. See include/wm.h for the design notes.
 *
 * Layout of a window in memory: each one owns a gfx surface the size of
 * its whole frame (title bar included). Apps draw into that surface only
 * when something changes; compositing a frame is then shadow + rounded
 * blit per window, which is why dragging a window full of text stays
 * smooth - the text isn't re-rendered, it's copied.
 *
 * The chrome is Windows'. Three things about that are load-bearing rather
 * than cosmetic:
 *
 *   - the caption buttons are hit-tested from the window's *right* edge,
 *     so they stay put when a window is resized, and they are flush with
 *     the corner - the rounding at composite time is what trims the close
 *     button's red hover fill to the corner curve, not any code here;
 *   - every edge and corner resizes, which means a press has to resolve
 *     to one of nine regions before anything else looks at it, and a drag
 *     has to remember which edges are anchored rather than just tracking
 *     a corner;
 *   - snapping needs a window to remember a frame it was never actually
 *     displayed at (the one it will restore to), which is why
 *     restore_x/y/w/h is saved on the way *into* a maximize or a snap and
 *     never on the way between two of them.
 */

#include "wm.h"
#include "gfx.h"
#include "icons.h"
#include "kstring.h"
#include "pit.h"

/* --- the look ---------------------------------------------------------- */

#define CAPTION_ACTIVE       GFX_RGB(0xF3, 0xF3, 0xF3)
#define CAPTION_IDLE         GFX_RGB(0xFA, 0xFA, 0xFA)
#define CAPTION_RULE_ACTIVE  GFX_RGB(0xE2, 0xE2, 0xE6)
#define CAPTION_RULE_IDLE    GFX_RGB(0xEE, 0xEE, 0xF1)
#define TITLE_TEXT_ACTIVE    GFX_RGB(0x1A, 0x1A, 0x1A)
#define TITLE_TEXT_IDLE      GFX_RGB(0x9A, 0x9A, 0xA0)
#define BORDER_ACTIVE        GFX_ARGB(0x8C, 0x2E, 0x2E, 0x34)
#define BORDER_IDLE          GFX_ARGB(0x44, 0x2E, 0x2E, 0x34)

#define BTN_HOVER            GFX_ARGB(0x1A, 0x00, 0x00, 0x00)
#define BTN_PRESSED          GFX_ARGB(0x2E, 0x00, 0x00, 0x00)
#define BTN_CLOSE_HOVER      GFX_RGB(0xC4, 0x2B, 0x1C)
#define BTN_CLOSE_PRESSED    GFX_RGB(0xA8, 0x24, 0x17)
#define GLYPH_ACTIVE         GFX_RGB(0x1A, 0x1A, 0x1A)
#define GLYPH_IDLE           GFX_RGB(0xA6, 0xA6, 0xAC)
#define GLYPH_ON_RED         GFX_RGB(0xFF, 0xFF, 0xFF)

#define SNAP_PREVIEW_FILL    GFX_ARGB(0x4D, 0x00, 0x78, 0xD4)

#define SHADOW_BLUR_ACTIVE  20
#define SHADOW_BLUR_IDLE    10
#define SHADOW_ALPHA_ACTIVE 100
#define SHADOW_ALPHA_IDLE   45
#define SHADOW_OFFSET_Y     6

#define ICON_X       10          /* window icon inset in the title bar */
#define ICON_SIZE    16
#define TITLE_X      (ICON_X + ICON_SIZE + 8)

#define RESIZE_BORDER  5         /* edge grab width */
#define RESIZE_CORNER 14         /* corner grab box */
#define SNAP_EDGE      6         /* how close to a screen edge snaps */
#define DRAG_UNSNAP    8         /* pixels of drag that unsticks a snap */

/* --- state ------------------------------------------------------------- */

static window_t windows[WM_MAX_WINDOWS];
static window_t* zorder[WM_MAX_WINDOWS];   /* back to front */
static int zcount;

static int screen_w = 640, screen_h = 480;
static int work_x, work_y, work_w, work_h;
static int cascade;

static window_t* focused;
static window_t* hover_win;
static int hover_button = -1;    /* caption button under the pointer, or -1 */
static int pressed_button = -1;
static int cursor_shape = WM_CURSOR_ARROW;

static void (*sysmenu_handler)(window_t* win, int x, int y);

/* Hit-test regions, in the order a press resolves them. */
enum {
    HIT_NONE, HIT_CLIENT, HIT_CAPTION, HIT_ICON,
    HIT_BTN_MIN, HIT_BTN_MAX, HIT_BTN_CLOSE,
    HIT_L, HIT_R, HIT_T, HIT_B, HIT_TL, HIT_TR, HIT_BL, HIT_BR,
};

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };
static int drag_mode = DRAG_NONE;
static window_t* drag_win;
static int drag_grab_x, drag_grab_y;     /* pointer offset inside the frame */
static int drag_edge;                    /* HIT_* of the edge being dragged */
static int drag_origin_x, drag_origin_y; /* where the press landed */
static int drag_anchor_r, drag_anchor_b; /* fixed right/bottom during resize */
static int drag_moved;                   /* has this drag passed the threshold */

static int pending_snap = WM_SNAP_NONE;
static gfx_rect_t pending_snap_rect;

/* Windows that "show desktop" minimized, so a second press can put back
 * exactly those and not everything that happens to be minimized. */
static window_t* hidden_by_show_desktop[WM_MAX_WINDOWS];
static int hidden_count;

static uint32_t last_click_tick;
static int last_click_x, last_click_y;
static window_t* last_click_win;

static gfx_rect_t damage;
static uint32_t generation;

static void set_hover(window_t* win, int button);

uint32_t wm_generation(void) { return generation; }

void wm_set_sysmenu_handler(void (*fn)(window_t* win, int x, int y)) {
    sysmenu_handler = fn;
}

/* --- damage ------------------------------------------------------------ */

void wm_damage(int x, int y, int w, int h) {
    gfx_rect_t r = { x, y, w, h };
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > screen_w) r.w = screen_w - r.x;
    if (r.y + r.h > screen_h) r.h = screen_h - r.y;
    if (r.w <= 0 || r.h <= 0) return;
    gfx_rect_union(&damage, &r);
}

void wm_damage_all(void) { wm_damage(0, 0, screen_w, screen_h); }

int wm_has_damage(void) { return damage.w > 0 && damage.h > 0; }

int wm_take_damage(gfx_rect_t* out) {
    if (damage.w <= 0 || damage.h <= 0) return 0;
    if (out) *out = damage;
    damage.x = damage.y = damage.w = damage.h = 0;
    return 1;
}

/* A window's footprint including its shadow. */
static void damage_window(const window_t* win) {
    int pad = SHADOW_BLUR_ACTIVE + 2;
    wm_damage(win->x - pad, win->y - pad, win->w + pad * 2,
              win->h + pad * 2 + SHADOW_OFFSET_Y);
}

/* Just the title bar - what a caption-button hover actually changes. */
static void damage_caption(const window_t* win) {
    wm_damage(win->x, win->y, win->w, WM_TITLEBAR_H);
}

/* --- z-order ----------------------------------------------------------- */

static void zorder_remove(window_t* win) {
    int dst = 0;
    for (int i = 0; i < zcount; i++) {
        if (zorder[i] != win) zorder[dst++] = zorder[i];
    }
    zcount = dst;
}

static void zorder_raise(window_t* win) {
    zorder_remove(win);
    if (zcount < WM_MAX_WINDOWS) zorder[zcount++] = win;
}

/* The frontmost window a user can actually see and click. */
static window_t* topmost_visible(void) {
    for (int i = zcount - 1; i >= 0; i--) {
        if (zorder[i]->open && !zorder[i]->minimized) return zorder[i];
    }
    return 0;
}

/* --- lifecycle --------------------------------------------------------- */

void wm_init(int w, int h, int top, int bottom) {
    screen_w = w;
    screen_h = h;
    work_x = 0;
    work_y = top;
    work_w = w;
    work_h = h - top - bottom;
    zcount = 0;
    focused = 0;
    hidden_count = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) windows[i].open = 0;
}

void wm_work_area(gfx_rect_t* out) {
    out->x = work_x;
    out->y = work_y;
    out->w = work_w;
    out->h = work_h;
}

window_t* wm_open(const app_t* app, const char* title, int w, int h, void* data) {
    window_t* win = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!windows[i].open) { win = &windows[i]; break; }
    }
    if (!win) return 0;

    if (w > work_w - 20) w = work_w - 20;
    if (h > work_h - 20) h = work_h - 20;

    win->surface = gfx_surface_create(w, h);
    if (!win->surface) return 0;   /* out of heap: the window simply doesn't open */

    win->app = app;
    win->data = data;
    win->w = w;
    win->h = h;
    win->min_w = 260;
    win->min_h = 140;
    win->resizable = 1;
    win->minimized = 0;
    win->maximized = 0;
    win->snapped = WM_SNAP_NONE;
    win->needs_paint = 1;
    win->bg = GFX_RGB(0xFF, 0xFF, 0xFF);
    kstrlcpy(win->title, title ? title : "Untitled", WM_TITLE_MAX);

    /* Cascade, the way a desktop does when you open a second window:
     * offset from the last one, wrapping before it walks off screen. */
    int slot = cascade++ % 6;
    win->x = work_x + (work_w - w) / 2 + slot * 26 - 65;
    win->y = work_y + 26 + slot * 24;
    if (win->x < work_x + 12) win->x = work_x + 12;
    if (win->x + w > work_x + work_w - 12) win->x = work_x + work_w - 12 - w;
    if (win->y + h > work_y + work_h) win->y = work_y + 12;

    win->restore_x = win->x;
    win->restore_y = win->y;
    win->restore_w = w;
    win->restore_h = h;

    win->open = 1;
    generation++;
    zorder_raise(win);
    focused = win;
    damage_window(win);
    return win;
}

void wm_close(window_t* win) {
    if (!win || !win->open) return;
    damage_window(win);
    if (win->app && win->app->closed) win->app->closed(win);
    gfx_surface_destroy(win->surface);
    win->surface = 0;
    win->open = 0;
    generation++;
    zorder_remove(win);
    if (focused == win) focused = topmost_visible();
    if (hover_win == win) { hover_win = 0; hover_button = -1; }
    if (drag_win == win) { drag_win = 0; drag_mode = DRAG_NONE; }
    if (last_click_win == win) last_click_win = 0;
    for (int i = 0; i < hidden_count; i++) {
        if (hidden_by_show_desktop[i] == win) hidden_by_show_desktop[i] = 0;
    }
}

void wm_close_all_of(const app_t* app) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].open && windows[i].app == app) wm_close(&windows[i]);
    }
}

void wm_focus(window_t* win) {
    if (!win || !win->open) return;
    if (win->minimized) wm_unminimize(win);
    if (focused == win && zcount > 0 && zorder[zcount - 1] == win) return;
    window_t* old = focused;
    focused = win;
    generation++;
    zorder_raise(win);
    if (old && old->open) { old->needs_paint = 1; damage_window(old); }
    win->needs_paint = 1;
    damage_window(win);
}

window_t* wm_focused(void) { return focused; }

const app_t* wm_active_app(void) {
    window_t* win = topmost_visible();
    return win ? win->app : 0;
}

void wm_set_title(window_t* win, const char* title) {
    if (!win) return;
    kstrlcpy(win->title, title, WM_TITLE_MAX);
    generation++;
    win->needs_paint = 1;
    damage_window(win);
}

void wm_move(window_t* win, int x, int y) {
    if (!win || !win->open) return;
    /* A window can hang off the sides and the bottom, but its title bar
     * can never go above the work area - it has to stay grabbable. */
    if (y < work_y) y = work_y;
    if (y > work_y + work_h - WM_TITLEBAR_H) y = work_y + work_h - WM_TITLEBAR_H;
    if (x + win->w < 80) x = 80 - win->w;
    if (x > screen_w - 80) x = screen_w - 80;
    if (x == win->x && y == win->y) return;

    damage_window(win);
    win->x = x;
    win->y = y;
    damage_window(win);
}

void wm_resize(window_t* win, int w, int h) {
    if (!win || !win->open) return;
    if (w < win->min_w) w = win->min_w;
    if (h < win->min_h) h = win->min_h;
    if (w > screen_w) w = screen_w;
    if (h > work_h) h = work_h;
    if (w == win->w && h == win->h) return;

    damage_window(win);
    if (!gfx_surface_resize(win->surface, w, h)) return; /* keep the old size */
    win->w = w;
    win->h = h;
    win->needs_paint = 1;
    damage_window(win);
}

/* Move and resize as one step, so a drag on the left or top edge doesn't
 * momentarily place the window somewhere it was never meant to be. */
static void set_frame(window_t* win, int x, int y, int w, int h) {
    if (w < win->min_w) w = win->min_w;
    if (h < win->min_h) h = win->min_h;
    if (w > screen_w) w = screen_w;
    if (h > work_h) h = work_h;
    if (y < work_y) y = work_y;

    damage_window(win);
    if ((w != win->w || h != win->h) && !gfx_surface_resize(win->surface, w, h)) {
        damage_window(win);
        return;
    }
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->needs_paint = 1;
    damage_window(win);
}

void wm_minimize(window_t* win) {
    if (!win || !win->open || win->minimized) return;
    damage_window(win);
    win->minimized = 1;
    generation++;
    if (focused == win) {
        focused = topmost_visible();
        if (focused) { focused->needs_paint = 1; damage_window(focused); }
    }
    set_hover(0, -1);
}

void wm_unminimize(window_t* win) {
    if (!win || !win->open || !win->minimized) return;
    win->minimized = 0;
    generation++;
    win->needs_paint = 1;
    zorder_raise(win);
    focused = win;
    damage_window(win);
}

/* Remembers where a floating window was, on the way into a maximize or a
 * snap. Deliberately does nothing if the window is already in one of
 * those states, so maximize-then-snap-then-restore lands back on the
 * original floating frame rather than on an intermediate one. */
static void save_restore_frame(window_t* win) {
    if (win->maximized || win->snapped != WM_SNAP_NONE) return;
    win->restore_x = win->x;
    win->restore_y = win->y;
    win->restore_w = win->w;
    win->restore_h = win->h;
}

static void snap_rect(int target, gfx_rect_t* out) {
    int hw = work_w / 2, hh = work_h / 2;
    switch (target) {
        case WM_SNAP_LEFT:  out->x = work_x;      out->y = work_y;      out->w = hw;           out->h = work_h; break;
        case WM_SNAP_RIGHT: out->x = work_x + hw; out->y = work_y;      out->w = work_w - hw;  out->h = work_h; break;
        case WM_SNAP_TL:    out->x = work_x;      out->y = work_y;      out->w = hw;           out->h = hh; break;
        case WM_SNAP_TR:    out->x = work_x + hw; out->y = work_y;      out->w = work_w - hw;  out->h = hh; break;
        case WM_SNAP_BL:    out->x = work_x;      out->y = work_y + hh; out->w = hw;           out->h = work_h - hh; break;
        case WM_SNAP_BR:    out->x = work_x + hw; out->y = work_y + hh; out->w = work_w - hw;  out->h = work_h - hh; break;
        default:            out->x = work_x;      out->y = work_y;      out->w = work_w;       out->h = work_h; break;
    }
}

void wm_maximize(window_t* win) {
    if (!win || !win->open || !win->resizable || win->maximized) return;
    save_restore_frame(win);
    gfx_rect_t r;
    snap_rect(WM_SNAP_MAXIMIZE, &r);
    set_frame(win, r.x, r.y, r.w, r.h);
    win->maximized = 1;
    win->snapped = WM_SNAP_NONE;
    generation++;
    set_hover(0, -1);
}

void wm_restore(window_t* win) {
    if (!win || !win->open) return;
    if (!win->maximized && win->snapped == WM_SNAP_NONE) return;
    win->maximized = 0;
    win->snapped = WM_SNAP_NONE;
    set_frame(win, win->restore_x, win->restore_y, win->restore_w,
              win->restore_h);
    generation++;
    /* The window just changed shape under a stationary pointer, so the
     * caption-button hover state is about to be a lie until the mouse
     * moves again. Drop it now. */
    set_hover(0, -1);
}

void wm_toggle_maximize(window_t* win) {
    if (!win || !win->open || !win->resizable) return;
    if (win->maximized) wm_restore(win);
    else wm_maximize(win);
}

void wm_snap(window_t* win, int target) {
    if (!win || !win->open) return;
    if (target == WM_SNAP_NONE) { wm_restore(win); return; }
    if (target == WM_SNAP_MAXIMIZE) { wm_maximize(win); return; }
    if (!win->resizable) return;

    save_restore_frame(win);
    gfx_rect_t r;
    snap_rect(target, &r);
    set_frame(win, r.x, r.y, r.w, r.h);
    win->maximized = 0;
    win->snapped = (uint8_t)target;
    generation++;
    set_hover(0, -1);
}

void wm_toggle_show_desktop(void) {
    /* Which way this goes is decided by what's on screen right now, not by
     * whether there's a remembered list. Deciding it from the list gets
     * this wrong the moment anything else touches a window in between -
     * close the windows it hid, open new ones, and the next press would
     * "restore" an empty list while leaving the new windows sitting
     * there. */
    int visible = 0;
    for (int i = 0; i < zcount; i++) {
        if (zorder[i]->open && !zorder[i]->minimized) { visible = 1; break; }
    }

    if (!visible) {
        for (int i = 0; i < hidden_count; i++) {
            window_t* win = hidden_by_show_desktop[i];
            if (win && win->open) wm_unminimize(win);
        }
        hidden_count = 0;
        return;
    }

    hidden_count = 0;
    for (int i = 0; i < zcount; i++) {
        window_t* win = zorder[i];
        if (!win->open || win->minimized) continue;
        if (hidden_count < WM_MAX_WINDOWS) {
            hidden_by_show_desktop[hidden_count++] = win;
        }
    }
    /* Front to back, so the frontmost window ends up first in the list and
     * is the last one put back - which is where focus lands. */
    for (int i = hidden_count - 1; i >= 0; i--) wm_minimize(hidden_by_show_desktop[i]);
}

void wm_invalidate(window_t* win) {
    if (!win || !win->open) return;
    win->needs_paint = 1;
    damage_window(win);
}

void wm_invalidate_rect(window_t* win, int x, int y, int w, int h) {
    if (!win || !win->open) return;
    win->needs_paint = 1;
    wm_damage(win->x + x, win->y + WM_TITLEBAR_H + y, w, h);
}

int wm_count(void) { return zcount; }
window_t* wm_at(int index) {
    return (index >= 0 && index < zcount) ? zorder[index] : 0;
}

window_t* wm_find_by_app(const app_t* app) {
    for (int i = zcount - 1; i >= 0; i--) {
        if (zorder[i]->open && zorder[i]->app == app) return zorder[i];
    }
    return 0;
}

int wm_app_window_count(const app_t* app) {
    int n = 0;
    for (int i = 0; i < zcount; i++) {
        if (zorder[i]->open && zorder[i]->app == app) n++;
    }
    return n;
}

void wm_client_rect(const window_t* win, gfx_rect_t* out) {
    out->x = win->x;
    out->y = win->y + WM_TITLEBAR_H;
    out->w = win->w;
    out->h = win->h - WM_TITLEBAR_H;
}

int wm_cursor_shape(void) { return cursor_shape; }

int wm_snap_preview(gfx_rect_t* out) {
    if (pending_snap == WM_SNAP_NONE) return 0;
    if (out) *out = pending_snap_rect;
    return 1;
}

/* --- chrome ------------------------------------------------------------ */

/* Caption buttons are laid out from the right edge inward: index 0 is
 * minimize, 1 maximize/restore, 2 close. */
static void caption_button_rect(const window_t* win, int index, gfx_rect_t* r) {
    r->w = WM_CAPTION_BTN_W;
    r->h = WM_TITLEBAR_H;
    r->x = win->w - (3 - index) * WM_CAPTION_BTN_W;
    r->y = 0;
}

static void draw_minimize_glyph(int cx, int cy, uint32_t color) {
    gfx_fill(cx - 5, cy, 10, 1, color);
}

static void draw_maximize_glyph(int cx, int cy, uint32_t color) {
    gfx_frame(cx - 5, cy - 5, 10, 10, color);
}

/* Two offset squares, the "this window is already maximized" mark. The
 * front square is filled with the caption color first, because nothing
 * here can erase the line of the square behind it. */
static void draw_restore_glyph(int cx, int cy, uint32_t color, uint32_t bg) {
    gfx_frame(cx - 3, cy - 6, 9, 9, color);
    gfx_fill(cx - 6, cy - 3, 9, 9, bg);
    gfx_frame(cx - 6, cy - 3, 9, 9, color);
}

static void draw_close_glyph(int cx, int cy, uint32_t color) {
    for (int i = -5; i <= 5; i++) {
        gfx_pixel(cx + i, cy + i, color);
        gfx_pixel(cx + i, cy - i, color);
        gfx_pixel(cx + i + 1, cy + i, GFX_WITH_ALPHA(color, 0x80));
        gfx_pixel(cx + i + 1, cy - i, GFX_WITH_ALPHA(color, 0x80));
    }
}

static void draw_caption_buttons(window_t* win, int active) {
    uint32_t caption_bg = active ? CAPTION_ACTIVE : CAPTION_IDLE;

    for (int i = 0; i < 3; i++) {
        gfx_rect_t r;
        caption_button_rect(win, i, &r);
        int enabled = (i != 1) || win->resizable;
        int hovered = (hover_win == win && hover_button == i && enabled);
        int down = (hovered && pressed_button == i);

        uint32_t glyph = active ? GLYPH_ACTIVE : GLYPH_IDLE;
        uint32_t behind = caption_bg;
        if (hovered) {
            if (i == 2) {
                uint32_t fill = down ? BTN_CLOSE_PRESSED : BTN_CLOSE_HOVER;
                gfx_fill(r.x, r.y, r.w, r.h, fill);
                glyph = GLYPH_ON_RED;
                behind = fill;
            } else {
                uint32_t fill = down ? BTN_PRESSED : BTN_HOVER;
                gfx_fill(r.x, r.y, r.w, r.h, fill);
                glyph = GLYPH_ACTIVE;
                /* The hover wash is translucent, so what the restore
                 * glyph has to paint over is the blend, not the wash. */
                behind = GFX_RGB(
                    (GFX_RED(caption_bg) * (255 - GFX_ALPHA(fill))) / 255,
                    (GFX_GREEN(caption_bg) * (255 - GFX_ALPHA(fill))) / 255,
                    (GFX_BLUE(caption_bg) * (255 - GFX_ALPHA(fill))) / 255);
            }
        }
        if (!enabled) glyph = GFX_ARGB(0x50, 0x60, 0x60, 0x66);

        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (i == 0) draw_minimize_glyph(cx, cy, glyph);
        else if (i == 1) {
            if (win->maximized) draw_restore_glyph(cx, cy, glyph, behind);
            else draw_maximize_glyph(cx, cy, glyph);
        } else {
            draw_close_glyph(cx, cy, glyph);
        }
    }
}

static void paint_window(window_t* win) {
    int active = (win == focused);
    gfx_set_target(win->surface);

    /* A flat caption: no gradient. Windows stopped shading title bars a
     * decade ago and the flat fill is most of what makes this read as
     * Windows rather than as something older. */
    gfx_fill(0, 0, win->w, WM_TITLEBAR_H,
             active ? CAPTION_ACTIVE : CAPTION_IDLE);
    gfx_fill(0, WM_TITLEBAR_H - 1, win->w, 1,
             active ? CAPTION_RULE_ACTIVE : CAPTION_RULE_IDLE);

    icon_app(win->app, ICON_X, (WM_TITLEBAR_H - ICON_SIZE) / 2, ICON_SIZE);

    /* The title reads from the left, and stops before the buttons. */
    int avail = win->w - TITLE_X - 3 * WM_CAPTION_BTN_W - 8;
    if (avail > 0) {
        gfx_text_ellipsized(&uifont_regular, TITLE_X,
                            (WM_TITLEBAR_H - uifont_regular.line_height) / 2,
                            avail, win->title,
                            active ? TITLE_TEXT_ACTIVE : TITLE_TEXT_IDLE);
    }

    draw_caption_buttons(win, active);

    gfx_save();
    gfx_origin(0, WM_TITLEBAR_H);
    if (gfx_clip(0, 0, win->w, wm_client_h(win))) {
        gfx_fill(0, 0, win->w, wm_client_h(win), win->bg);
        if (win->app && win->app->paint) win->app->paint(win);
    }
    gfx_restore();

    /* A hairline around the whole frame, so a white window still has an
     * edge against the wallpaper. The focused window's is darker, which is
     * the other half of how Windows shows which window is active. */
    gfx_set_target(win->surface);
    gfx_clip_reset();
    gfx_round_frame(0, 0, win->w, win->h, WM_CORNER_RADIUS, 1,
                    active ? BORDER_ACTIVE : BORDER_IDLE);

    win->needs_paint = 0;
}

void wm_composite(const gfx_rect_t* clip) {
    gfx_surface_t* screen = gfx_get_target();

    /* Repaint stale windows first: painting switches the gfx target, and
     * doing it mid-composite would trample the compositor's clip. */
    for (int i = 0; i < zcount; i++) {
        window_t* win = zorder[i];
        if (win->open && !win->minimized && win->needs_paint) paint_window(win);
    }

    gfx_set_target(screen);
    for (int i = 0; i < zcount; i++) {
        window_t* win = zorder[i];
        if (!win->open || win->minimized) continue;

        gfx_rect_t frame = { win->x, win->y, win->w, win->h };
        int pad = SHADOW_BLUR_ACTIVE + SHADOW_OFFSET_Y;
        gfx_rect_t with_shadow = { frame.x - pad, frame.y - pad,
                                   frame.w + pad * 2, frame.h + pad * 2 };
        if (!gfx_rect_intersects(&with_shadow, clip)) continue;

        int active = (win == focused);
        gfx_save();
        gfx_clip(clip->x, clip->y, clip->w, clip->h);
        gfx_shadow(win->x, win->y + SHADOW_OFFSET_Y, win->w, win->h,
                   WM_CORNER_RADIUS,
                   active ? SHADOW_BLUR_ACTIVE : SHADOW_BLUR_IDLE,
                   active ? SHADOW_ALPHA_ACTIVE : SHADOW_ALPHA_IDLE);
        gfx_blit_rounded(win->surface, win->x, win->y, WM_CORNER_RADIUS);
        gfx_restore();
    }

    /* The snap preview goes over every window: it is showing you where a
     * window is about to land, which is in front of whatever is there. */
    if (pending_snap != WM_SNAP_NONE) {
        gfx_rect_t r = pending_snap_rect;
        gfx_save();
        gfx_clip(clip->x, clip->y, clip->w, clip->h);
        gfx_round_rect(r.x + 4, r.y + 4, r.w - 8, r.h - 8, WM_CORNER_RADIUS,
                       SNAP_PREVIEW_FILL);
        gfx_round_frame(r.x + 4, r.y + 4, r.w - 8, r.h - 8, WM_CORNER_RADIUS, 2,
                        GFX_ARGB(0xCC, 0xFF, 0xFF, 0xFF));
        gfx_restore();
    }
}

/* --- hit testing ------------------------------------------------------- */

static window_t* window_at(int x, int y) {
    for (int i = zcount - 1; i >= 0; i--) {
        window_t* win = zorder[i];
        if (!win->open || win->minimized) continue;
        if (x >= win->x && x < win->x + win->w &&
            y >= win->y && y < win->y + win->h) {
            return win;
        }
    }
    return 0;
}

/* Resolves a window-relative point to one of the nine frame regions plus
 * the caption furniture. Order matters: the resize border wins over the
 * caption, which is why you can grab the very top of a window to resize
 * it even though the title bar is drawn there. */
static int hit_test(const window_t* win, int wx, int wy) {
    int resizable = win->resizable && !win->maximized;
    if (resizable) {
        int left = wx < RESIZE_BORDER;
        int right = wx >= win->w - RESIZE_BORDER;
        int top = wy < RESIZE_BORDER;
        int bottom = wy >= win->h - RESIZE_BORDER;
        int cleft = wx < RESIZE_CORNER;
        int cright = wx >= win->w - RESIZE_CORNER;
        int ctop = wy < RESIZE_CORNER;
        int cbottom = wy >= win->h - RESIZE_CORNER;

        if ((left || top) && cleft && ctop) return HIT_TL;
        if ((right || top) && cright && ctop) return HIT_TR;
        if ((left || bottom) && cleft && cbottom) return HIT_BL;
        if ((right || bottom) && cright && cbottom) return HIT_BR;
        if (left) return HIT_L;
        if (right) return HIT_R;
        if (top) return HIT_T;
        if (bottom) return HIT_B;
    }

    if (wy < WM_TITLEBAR_H) {
        for (int i = 0; i < 3; i++) {
            gfx_rect_t r;
            caption_button_rect(win, i, &r);
            if (wx >= r.x && wx < r.x + r.w && wy >= r.y && wy < r.y + r.h) {
                return HIT_BTN_MIN + i;
            }
        }
        if (wx < ICON_X + ICON_SIZE + 2) return HIT_ICON;
        return HIT_CAPTION;
    }
    return HIT_CLIENT;
}

static int cursor_for_hit(int hit) {
    switch (hit) {
        case HIT_L: case HIT_R: return WM_CURSOR_SIZE_WE;
        case HIT_T: case HIT_B: return WM_CURSOR_SIZE_NS;
        case HIT_TL: case HIT_BR: return WM_CURSOR_SIZE_NWSE;
        case HIT_TR: case HIT_BL: return WM_CURSOR_SIZE_NESW;
        default: return WM_CURSOR_ARROW;
    }
}

static void set_cursor(int shape) {
    if (shape == cursor_shape) return;
    cursor_shape = shape;
    /* The desktop repaints the pointer whenever it moves, and the pointer
     * has just moved if the shape changed - so there's nothing extra to
     * damage here. */
}

static void set_hover(window_t* win, int button) {
    if (hover_win == win && hover_button == button) return;
    if (hover_win && hover_win->open) {
        hover_win->needs_paint = 1;
        damage_caption(hover_win);
    }
    hover_win = win;
    hover_button = button;
    if (win && win->open) {
        win->needs_paint = 1;
        damage_caption(win);
    }
}

/* --- snapping ---------------------------------------------------------- */

static void set_pending_snap(int target) {
    if (target == pending_snap) return;
    if (pending_snap != WM_SNAP_NONE) {
        wm_damage(pending_snap_rect.x, pending_snap_rect.y,
                  pending_snap_rect.w, pending_snap_rect.h);
    }
    pending_snap = target;
    if (pending_snap != WM_SNAP_NONE) {
        snap_rect(pending_snap, &pending_snap_rect);
        wm_damage(pending_snap_rect.x, pending_snap_rect.y,
                  pending_snap_rect.w, pending_snap_rect.h);
    }
}

/* Where the pointer is, translated into the snap it is asking for. The
 * corner quarters live in the top and bottom quarter of each side band,
 * which is roughly where Windows puts them. */
static int snap_target_for(int x, int y) {
    int left = x <= work_x + SNAP_EDGE;
    int right = x >= work_x + work_w - 1 - SNAP_EDGE;
    int top = y <= work_y + SNAP_EDGE;

    if (left || right) {
        if (y < work_y + work_h / 4) return left ? WM_SNAP_TL : WM_SNAP_TR;
        if (y > work_y + work_h * 3 / 4) return left ? WM_SNAP_BL : WM_SNAP_BR;
        return left ? WM_SNAP_LEFT : WM_SNAP_RIGHT;
    }
    if (top) return WM_SNAP_MAXIMIZE;
    return WM_SNAP_NONE;
}

/* --- input ------------------------------------------------------------- */

static void deliver(window_t* win, int kind, int x, int y, int buttons, int wheel) {
    if (win && win->app && win->app->mouse) {
        win->app->mouse(win, x - win->x, y - win->y - WM_TITLEBAR_H, kind,
                        buttons, wheel);
    }
}

/* Applies a resize drag: whichever edges the press grabbed move with the
 * pointer, the others stay exactly where they were. */
static void drag_resize_to(int x, int y) {
    window_t* win = drag_win;
    int nx = win->x, ny = win->y, nw = win->w, nh = win->h;

    int edge = drag_edge;
    int left = (edge == HIT_L || edge == HIT_TL || edge == HIT_BL);
    int right = (edge == HIT_R || edge == HIT_TR || edge == HIT_BR);
    int top = (edge == HIT_T || edge == HIT_TL || edge == HIT_TR);
    int bottom = (edge == HIT_B || edge == HIT_BL || edge == HIT_BR);

    if (left) {
        nx = x - drag_grab_x;
        nw = drag_anchor_r - nx;
        if (nw < win->min_w) { nw = win->min_w; nx = drag_anchor_r - nw; }
    } else if (right) {
        nw = x - win->x + drag_grab_x;
    }
    if (top) {
        ny = y - drag_grab_y;
        nh = drag_anchor_b - ny;
        if (nh < win->min_h) { nh = win->min_h; ny = drag_anchor_b - nh; }
        if (ny < work_y) { ny = work_y; nh = drag_anchor_b - ny; }
    } else if (bottom) {
        nh = y - win->y + drag_grab_y;
    }
    set_frame(win, nx, ny, nw, nh);
}

/* A window being dragged out of a maximize or a snap gets its floating
 * size back immediately, positioned so the pointer stays at the same
 * relative spot along the title bar - which is what stops the window from
 * jumping out from under the cursor. */
static void unsnap_for_drag(int x) {
    window_t* win = drag_win;
    int old_w = win->w;
    win->maximized = 0;
    win->snapped = WM_SNAP_NONE;
    set_frame(win, win->x, win->y, win->restore_w, win->restore_h);
    generation++;

    int along = old_w > 1 ? drag_grab_x * win->w / old_w : drag_grab_x;
    if (along > win->w - 40) along = win->w - 40;
    if (along < 20) along = 20;
    drag_grab_x = along;
    wm_move(win, x - drag_grab_x, win->y);
}

int wm_handle_mouse(const mouse_event_t* ev) {
    int x = ev->x, y = ev->y;

    if (drag_mode != DRAG_NONE && drag_win) {
        if (ev->released & MOUSE_BUTTON_LEFT) {
            int target = pending_snap;
            window_t* win = drag_win;
            drag_mode = DRAG_NONE;
            drag_win = 0;
            set_pending_snap(WM_SNAP_NONE);
            if (target != WM_SNAP_NONE) wm_snap(win, target);
            set_cursor(WM_CURSOR_ARROW);
            return 1;
        }
        if (drag_mode == DRAG_MOVE) {
            int dx = x - drag_origin_x, dy = y - drag_origin_y;
            if (!drag_moved && dx * dx + dy * dy >= DRAG_UNSNAP * DRAG_UNSNAP) {
                drag_moved = 1;
                if (drag_win->maximized || drag_win->snapped != WM_SNAP_NONE) {
                    unsnap_for_drag(x);
                }
            }
            if (drag_moved) {
                wm_move(drag_win, x - drag_grab_x, y - drag_grab_y);
                set_pending_snap(drag_win->resizable ? snap_target_for(x, y)
                                                     : WM_SNAP_NONE);
            }
        } else {
            drag_resize_to(x, y);
        }
        return 1;
    }

    window_t* win = window_at(x, y);
    int hit = win ? hit_test(win, x - win->x, y - win->y) : HIT_NONE;

    if (ev->pressed & MOUSE_BUTTON_RIGHT) {
        if (!win) return 0;
        wm_focus(win);
        if (hit == HIT_CAPTION || hit == HIT_ICON) {
            if (sysmenu_handler) sysmenu_handler(win, x, y);
            return 1;
        }
        deliver(win, WM_MOUSE_RIGHT_DOWN, x, y, ev->buttons, 0);
        return 1;
    }

    if (ev->pressed & MOUSE_BUTTON_LEFT) {
        if (!win) {
            set_hover(0, -1);
            return 0;   /* the desktop gets first refusal on empty space */
        }
        wm_focus(win);
        /* Resolve the hover against what was actually pressed. Without
         * this, grabbing the title bar right after passing over the close
         * button leaves that button lit red for the whole drag - the drag
         * path returns early and never looks at hovering again. */
        set_hover(win, (hit >= HIT_BTN_MIN && hit <= HIT_BTN_CLOSE)
                           ? hit - HIT_BTN_MIN : -1);

        if (hit >= HIT_BTN_MIN && hit <= HIT_BTN_CLOSE) {
            pressed_button = hit - HIT_BTN_MIN;
            win->needs_paint = 1;
            damage_caption(win);
            return 1;
        }
        if (hit >= HIT_L && hit <= HIT_BR) {
            drag_mode = DRAG_RESIZE;
            drag_win = win;
            drag_edge = hit;
            drag_anchor_r = win->x + win->w;
            drag_anchor_b = win->y + win->h;
            /* The grab offset is measured from whichever edge is moving,
             * so the pointer keeps its grip on that edge exactly. */
            drag_grab_x = (hit == HIT_L || hit == HIT_TL || hit == HIT_BL)
                              ? x - win->x : win->x + win->w - x;
            drag_grab_y = (hit == HIT_T || hit == HIT_TL || hit == HIT_TR)
                              ? y - win->y : win->y + win->h - y;
            return 1;
        }
        if (hit == HIT_ICON) {
            uint32_t now = pit_get_ticks();
            if (last_click_win == win && now - last_click_tick < 50) {
                last_click_win = 0;
                wm_close(win);      /* double-clicking the icon closes, as it does on Windows */
                return 1;
            }
            last_click_win = win;
            last_click_tick = now;
            last_click_x = x;
            last_click_y = y;
            if (sysmenu_handler) sysmenu_handler(win, win->x + ICON_X, win->y + WM_TITLEBAR_H);
            return 1;
        }
        if (hit == HIT_CAPTION) {
            uint32_t now = pit_get_ticks();
            int dx = x - last_click_x, dy = y - last_click_y;
            if (last_click_win == win && now - last_click_tick < 50 &&
                dx * dx + dy * dy < 64) {
                last_click_win = 0;
                wm_toggle_maximize(win);
                return 1;
            }
            last_click_win = win;
            last_click_tick = now;
            last_click_x = x;
            last_click_y = y;

            drag_mode = DRAG_MOVE;
            drag_win = win;
            drag_moved = 0;
            drag_origin_x = x;
            drag_origin_y = y;
            drag_grab_x = x - win->x;
            drag_grab_y = y - win->y;
            return 1;
        }

        /* A click in the content area: report it, with a double-click
         * flagged separately so lists can act on it. */
        uint32_t now = pit_get_ticks();
        int dx = x - last_click_x, dy = y - last_click_y;
        int is_double = (last_click_win == win && now - last_click_tick < 50 &&
                         dx * dx + dy * dy < 64);
        last_click_win = is_double ? 0 : win;
        last_click_tick = now;
        last_click_x = x;
        last_click_y = y;
        deliver(win, is_double ? WM_MOUSE_DOUBLE : WM_MOUSE_DOWN, x, y,
                ev->buttons, 0);
        return 1;
    }

    if (ev->released & MOUSE_BUTTON_LEFT) {
        if (pressed_button >= 0) {
            int button = pressed_button;
            pressed_button = -1;
            if (win && hit == HIT_BTN_MIN + button) {
                if (button == 0) wm_minimize(win);
                else if (button == 1) wm_toggle_maximize(win);
                else wm_close(win);
            } else if (win && win->open) {
                win->needs_paint = 1;
                damage_caption(win);
            }
            return 1;
        }
        if (win) {
            deliver(win, WM_MOUSE_UP, x, y, ev->buttons, 0);
            return 1;
        }
        return 0;
    }

    if (ev->wheel) {
        if (!win) return 0;
        deliver(win, WM_MOUSE_WHEEL, x, y, ev->buttons, ev->wheel);
        return 1;
    }

    /* Plain movement: update the caption-button hover state, the pointer
     * shape, and let the window track the pointer. */
    if (win) {
        set_hover(win, (hit >= HIT_BTN_MIN && hit <= HIT_BTN_CLOSE)
                           ? hit - HIT_BTN_MIN : -1);
        set_cursor(cursor_for_hit(hit));
        deliver(win, (ev->buttons & MOUSE_BUTTON_LEFT) ? WM_MOUSE_DRAG : WM_MOUSE_MOVE,
                x, y, ev->buttons, 0);
        return 1;
    }
    set_hover(0, -1);
    set_cursor(WM_CURSOR_ARROW);
    return 0;
}

int wm_handle_key(const key_event_t* ev) {
    if (!ev->pressed) return 0;
    window_t* win = focused;

    /* Alt shortcuts: the window-management ones Windows binds. */
    if (ev->mods & KEY_MOD_ALT) {
        if (ev->code == KEY_F4) {
            if (win) { wm_close(win); return 1; }
            return 0;
        }
        if (ev->ascii == ' ') {
            if (win && sysmenu_handler) {
                sysmenu_handler(win, win->x + ICON_X, win->y + WM_TITLEBAR_H);
                return 1;
            }
            return 0;
        }
    }

    /* Win+arrow: snap, maximize, restore, minimize - in that order going
     * down, which is the sequence Windows walks a window through. */
    if ((ev->mods & KEY_MOD_SUPER) && win) {
        switch (ev->code) {
            case KEY_LEFT:  wm_snap(win, WM_SNAP_LEFT); return 1;
            case KEY_RIGHT: wm_snap(win, WM_SNAP_RIGHT); return 1;
            case KEY_UP:    wm_maximize(win); return 1;
            case KEY_DOWN:
                if (win->maximized || win->snapped != WM_SNAP_NONE) wm_restore(win);
                else wm_minimize(win);
                return 1;
            default: break;
        }
    }

    if (win && win->open && win->app && win->app->key) {
        win->app->key(win, ev);
        return 1;
    }
    return 0;
}

void wm_tick(void) {
    for (int i = 0; i < zcount; i++) {
        window_t* win = zorder[i];
        if (win->open && win->app && win->app->tick) win->app->tick(win);
    }
}
