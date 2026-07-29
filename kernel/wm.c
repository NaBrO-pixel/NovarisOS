/* wm.c - the window manager. See include/wm.h for the design notes.
 *
 * Layout of a window in memory: each one owns a gfx surface the size of
 * its whole frame (title bar included). Apps draw into that surface only
 * when something changes; compositing a frame is then shadow + rounded
 * blit per window, which is why dragging a window full of text stays
 * smooth - the text isn't re-rendered, it's copied.
 */

#include "wm.h"
#include "gfx.h"
#include "kstring.h"
#include "pit.h"

/* --- the look ---------------------------------------------------------- */

#define TITLE_TOP_ACTIVE     GFX_RGB(0xEC, 0xEC, 0xEC)
#define TITLE_BOTTOM_ACTIVE  GFX_RGB(0xDE, 0xDE, 0xDE)
#define TITLE_TOP_IDLE       GFX_RGB(0xF7, 0xF7, 0xF7)
#define TITLE_BOTTOM_IDLE    GFX_RGB(0xF0, 0xF0, 0xF0)
#define TITLE_SEPARATOR      GFX_RGB(0xC6, 0xC6, 0xC6)
#define TITLE_TEXT_ACTIVE    GFX_RGB(0x33, 0x33, 0x33)
#define TITLE_TEXT_IDLE      GFX_RGB(0xA8, 0xA8, 0xA8)
#define WINDOW_EDGE          GFX_ARGB(0x40, 0x00, 0x00, 0x00)

#define LIGHT_CLOSE          GFX_RGB(0xFF, 0x5F, 0x57)
#define LIGHT_MINIMIZE       GFX_RGB(0xFE, 0xBC, 0x2E)
#define LIGHT_ZOOM           GFX_RGB(0x28, 0xC8, 0x40)
#define LIGHT_IDLE           GFX_RGB(0xD6, 0xD3, 0xD1)
#define LIGHT_GLYPH_CLOSE    GFX_ARGB(0xCC, 0x66, 0x00, 0x00)
#define LIGHT_GLYPH_MIN      GFX_ARGB(0xCC, 0x66, 0x40, 0x00)
#define LIGHT_GLYPH_ZOOM     GFX_ARGB(0xCC, 0x00, 0x50, 0x00)

#define LIGHT_RADIUS   6
#define LIGHT_SPACING  20
#define LIGHT_FIRST_X  19    /* center of the close button */

#define SHADOW_BLUR_ACTIVE 26
#define SHADOW_BLUR_IDLE   14
#define SHADOW_ALPHA_ACTIVE 130
#define SHADOW_ALPHA_IDLE   70
#define SHADOW_OFFSET_Y     7

#define RESIZE_GRIP    14   /* bottom-right corner hot zone */
#define RESIZE_EDGE     4   /* right / bottom edge hot zone */

/* --- state ------------------------------------------------------------- */

static window_t windows[WM_MAX_WINDOWS];
static window_t* zorder[WM_MAX_WINDOWS];   /* back to front */
static int zcount;

static int screen_w = 640, screen_h = 480;
static int reserve_top = 0, reserve_bottom = 0;
static int cascade;

static window_t* focused;
static window_t* hover_win;
static int hover_light = -1;    /* traffic light under the pointer, or -1 */
static int pressed_light = -1;

static void set_hover(window_t* win, int light);

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };
static int drag_mode = DRAG_NONE;
static window_t* drag_win;
static int drag_grab_x, drag_grab_y;

static uint32_t last_click_tick;
static int last_click_x, last_click_y;
static window_t* last_click_win;

static gfx_rect_t damage;
static uint32_t generation;

uint32_t wm_generation(void) { return generation; }

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
    reserve_top = top;
    reserve_bottom = bottom;
    zcount = 0;
    focused = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) windows[i].open = 0;
}

window_t* wm_open(const app_t* app, const char* title, int w, int h, void* data) {
    window_t* win = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!windows[i].open) { win = &windows[i]; break; }
    }
    if (!win) return 0;

    if (w > screen_w - 20) w = screen_w - 20;
    if (h > screen_h - reserve_top - 20) h = screen_h - reserve_top - 20;

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
    win->zoomed = 0;
    win->needs_paint = 1;
    win->bg = GFX_RGB(0xFF, 0xFF, 0xFF);
    kstrlcpy(win->title, title ? title : "Untitled", WM_TITLE_MAX);

    /* Cascade, the way a desktop does when you open a second window:
     * offset from the last one, wrapping before it walks off screen. */
    int slot = cascade++ % 6;
    win->x = (screen_w - w) / 2 + slot * 26 - 65;
    win->y = reserve_top + 32 + slot * 24;
    if (win->x < 12) win->x = 12;
    if (win->x + w > screen_w - 12) win->x = screen_w - 12 - w;
    if (win->y + h > screen_h - reserve_bottom) {
        win->y = reserve_top + 20;
    }

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
    if (hover_win == win) hover_win = 0;
    if (drag_win == win) { drag_win = 0; drag_mode = DRAG_NONE; }
    if (last_click_win == win) last_click_win = 0;
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
    /* Windows live below the menu bar and can't be dragged off the top or
     * sides entirely - the title bar always stays grabbable. */
    if (y < reserve_top) y = reserve_top;
    if (y > screen_h - WM_TITLEBAR_H) y = screen_h - WM_TITLEBAR_H;
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
    if (h > screen_h - reserve_top) h = screen_h - reserve_top;
    if (w == win->w && h == win->h) return;

    damage_window(win);
    if (!gfx_surface_resize(win->surface, w, h)) return; /* keep the old size */
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

void wm_zoom(window_t* win) {
    if (!win || !win->open || !win->resizable) return;
    if (win->zoomed) {
        damage_window(win);
        win->x = win->restore_x;
        win->y = win->restore_y;
        wm_resize(win, win->restore_w, win->restore_h);
        win->zoomed = 0;
    } else {
        win->restore_x = win->x;
        win->restore_y = win->y;
        win->restore_w = win->w;
        win->restore_h = win->h;
        damage_window(win);
        win->x = 8;
        win->y = reserve_top + 6;
        wm_resize(win, screen_w - 16, screen_h - reserve_top - reserve_bottom - 12);
        win->zoomed = 1;
    }
    win->needs_paint = 1;
    damage_window(win);
    /* The window just changed shape under a stationary pointer, so the
     * traffic-light hover state is about to be a lie until the mouse
     * moves again. Drop it now. */
    set_hover(0, -1);
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

/* --- chrome ------------------------------------------------------------ */

static void draw_close_glyph(int cx, int cy) {
    for (int i = -2; i <= 2; i++) {
        gfx_pixel(cx + i, cy + i, LIGHT_GLYPH_CLOSE);
        gfx_pixel(cx + i, cy - i, LIGHT_GLYPH_CLOSE);
    }
}

static void draw_minimize_glyph(int cx, int cy) {
    gfx_fill(cx - 3, cy, 7, 1, LIGHT_GLYPH_MIN);
}

/* Two little triangles pointing out of opposite corners - the "fill the
 * screen" mark. */
static void draw_zoom_glyph(int cx, int cy) {
    for (int i = 0; i < 4; i++) {
        gfx_fill(cx - 3, cy - 3 + i, 4 - i, 1, LIGHT_GLYPH_ZOOM);
        gfx_fill(cx + i, cy + 2 - i, 4 - i, 1, LIGHT_GLYPH_ZOOM);
    }
}

static void draw_traffic_lights(window_t* win, int active) {
    int cy = WM_TITLEBAR_H / 2;
    static const uint32_t colors[3] = { LIGHT_CLOSE, LIGHT_MINIMIZE, LIGHT_ZOOM };

    for (int i = 0; i < 3; i++) {
        int cx = LIGHT_FIRST_X + i * LIGHT_SPACING;
        int enabled = (i != 2) || win->resizable;
        uint32_t color = (active && enabled) ? colors[i] : LIGHT_IDLE;
        gfx_circle(cx, cy, LIGHT_RADIUS, color);
        /* A darker rim, then a highlight across the top: what makes them
         * read as buttons rather than flat dots. */
        gfx_ring(cx, cy, LIGHT_RADIUS, 1, GFX_ARGB(0x30, 0, 0, 0));
        gfx_fill(cx - 2, cy - LIGHT_RADIUS + 1, 4, 1, GFX_ARGB(0x50, 0xFF, 0xFF, 0xFF));

        /* macOS only shows the glyphs while the pointer is over the
         * group, not over the individual button. */
        if (hover_win == win && hover_light >= 0 && enabled) {
            if (i == 0) draw_close_glyph(cx, cy);
            else if (i == 1) draw_minimize_glyph(cx, cy);
            else draw_zoom_glyph(cx, cy);
        }
    }
}

static void paint_window(window_t* win) {
    int active = (win == focused);
    gfx_set_target(win->surface);

    gfx_vgradient(0, 0, win->w, WM_TITLEBAR_H,
                  active ? TITLE_TOP_ACTIVE : TITLE_TOP_IDLE,
                  active ? TITLE_BOTTOM_ACTIVE : TITLE_BOTTOM_IDLE);
    gfx_fill(0, WM_TITLEBAR_H - 1, win->w, 1, TITLE_SEPARATOR);
    draw_traffic_lights(win, active);

    /* The title is centered on the window, and only gives ground to the
     * traffic lights when it would otherwise run into them. */
    int reserved = LIGHT_FIRST_X + 2 * LIGHT_SPACING + LIGHT_RADIUS + 12;
    int avail = win->w - reserved * 2;
    if (avail < 40) avail = 40;
    gfx_save();
    if (gfx_clip(reserved, 0, win->w - reserved * 2, WM_TITLEBAR_H)) {
        int tw = gfx_text_width(&uifont_bold, win->title);
        int tx = (win->w - tw) / 2;
        if (tx < reserved) tx = reserved;
        gfx_text_ellipsized(&uifont_bold, tx, (WM_TITLEBAR_H - uifont_bold.line_height) / 2 + 1,
                            avail, win->title,
                            active ? TITLE_TEXT_ACTIVE : TITLE_TEXT_IDLE);
    }
    gfx_restore();

    gfx_save();
    gfx_origin(0, WM_TITLEBAR_H);
    if (gfx_clip(0, 0, win->w, wm_client_h(win))) {
        gfx_fill(0, 0, win->w, wm_client_h(win), win->bg);
        if (win->app && win->app->paint) win->app->paint(win);
    }
    gfx_restore();

    /* A hairline around the whole frame, so a white window still has an
     * edge against a light wallpaper. */
    gfx_set_target(win->surface);
    gfx_clip_reset();
    gfx_round_frame(0, 0, win->w, win->h, WM_CORNER_RADIUS, 1, WINDOW_EDGE);

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

/* Which traffic light (0/1/2) is at this window-relative point, or -1. */
static int light_at(const window_t* win, int wx, int wy) {
    (void)win;  /* the lights sit at a fixed offset in every title bar */
    if (wy >= WM_TITLEBAR_H) return -1;
    int cy = WM_TITLEBAR_H / 2;
    for (int i = 0; i < 3; i++) {
        int cx = LIGHT_FIRST_X + i * LIGHT_SPACING;
        int dx = wx - cx, dy = wy - cy;
        if (dx * dx + dy * dy <= (LIGHT_RADIUS + 2) * (LIGHT_RADIUS + 2)) return i;
    }
    return -1;
}

/* Is this point in the region that starts a resize? */
static int resize_zone(const window_t* win, int wx, int wy) {
    if (!win->resizable) return 0;
    int right = wx >= win->w - RESIZE_EDGE;
    int bottom = wy >= win->h - RESIZE_EDGE;
    if (wx >= win->w - RESIZE_GRIP && wy >= win->h - RESIZE_GRIP) return 1;
    return right || bottom;
}

static void set_hover(window_t* win, int light) {
    if (hover_win == win && hover_light == light) return;
    /* Only the traffic-light corner changes appearance, so that's all
     * that needs repainting. */
    if (hover_win && hover_win->open) {
        hover_win->needs_paint = 1;
        wm_damage(hover_win->x, hover_win->y, 90, WM_TITLEBAR_H);
    }
    hover_win = win;
    hover_light = light;
    if (win && win->open) {
        win->needs_paint = 1;
        wm_damage(win->x, win->y, 90, WM_TITLEBAR_H);
    }
}

static void deliver(window_t* win, int kind, int x, int y, int buttons, int wheel) {
    if (win && win->app && win->app->mouse) {
        win->app->mouse(win, x - win->x, y - win->y - WM_TITLEBAR_H, kind,
                        buttons, wheel);
    }
}

int wm_handle_mouse(const mouse_event_t* ev) {
    int x = ev->x, y = ev->y;

    if (drag_mode != DRAG_NONE && drag_win) {
        if (ev->released & MOUSE_BUTTON_LEFT) {
            drag_mode = DRAG_NONE;
            drag_win = 0;
        } else if (drag_mode == DRAG_MOVE) {
            wm_move(drag_win, x - drag_grab_x, y - drag_grab_y);
        } else {
            wm_resize(drag_win, x - drag_win->x + drag_grab_x,
                      y - drag_win->y + drag_grab_y);
        }
        return 1;
    }

    window_t* win = window_at(x, y);

    if (ev->pressed & MOUSE_BUTTON_LEFT) {
        if (!win) {
            set_hover(0, -1);
            return 0;   /* the desktop gets first refusal on empty space */
        }
        wm_focus(win);
        int wx = x - win->x, wy = y - win->y;

        int light = light_at(win, wx, wy);
        if (light >= 0) {
            pressed_light = light;
            return 1;
        }
        if (resize_zone(win, wx, wy)) {
            drag_mode = DRAG_RESIZE;
            drag_win = win;
            drag_grab_x = win->x + win->w - x;
            drag_grab_y = win->y + win->h - y;
            return 1;
        }
        if (wy < WM_TITLEBAR_H) {
            /* Double-clicking the title bar zooms, same as the green
             * button - the click has to land on the same window, near the
             * same spot, within half a second. */
            uint32_t now = pit_get_ticks();
            int dx = x - last_click_x, dy = y - last_click_y;
            if (last_click_win == win && now - last_click_tick < 50 &&
                dx * dx + dy * dy < 64) {
                last_click_win = 0;
                wm_zoom(win);
                return 1;
            }
            last_click_win = win;
            last_click_tick = now;
            last_click_x = x;
            last_click_y = y;

            drag_mode = DRAG_MOVE;
            drag_win = win;
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
        if (pressed_light >= 0) {
            int light = pressed_light;
            pressed_light = -1;
            if (win && light_at(win, x - win->x, y - win->y) == light) {
                if (light == 0) wm_close(win);
                else if (light == 1) wm_minimize(win);
                else wm_zoom(win);
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

    /* Plain movement: update the traffic-light hover state and let the
     * window track the pointer. */
    if (win) {
        set_hover(win, light_at(win, x - win->x, y - win->y));
        deliver(win, (ev->buttons & MOUSE_BUTTON_LEFT) ? WM_MOUSE_DRAG : WM_MOUSE_MOVE,
                x, y, ev->buttons, 0);
        return 1;
    }
    set_hover(0, -1);
    return 0;
}

int wm_handle_key(const key_event_t* ev) {
    if (!ev->pressed) return 0;

    /* Window-level Command shortcuts, handled before the app sees them. */
    if (ev->mods & KEY_MOD_COMMAND) {
        window_t* win = focused;
        switch (ev->ascii) {
            case 'w': case 'W':
                if (win) { wm_close(win); return 1; }
                return 0;
            case 'm': case 'M':
                if (win) { wm_minimize(win); return 1; }
                return 0;
            case '`':
                /* Cycle windows front to back, like Command-` does. */
                if (zcount > 1) {
                    for (int i = 0; i < zcount; i++) {
                        if (zorder[i]->open && !zorder[i]->minimized) {
                            wm_focus(zorder[i]);
                            break;
                        }
                    }
                    return 1;
                }
                return 0;
        }
        if (ev->code == KEY_TAB && zcount > 1) {
            for (int i = 0; i < zcount; i++) {
                if (zorder[i]->open && !zorder[i]->minimized) {
                    wm_focus(zorder[i]);
                    return 1;
                }
            }
        }
    }

    if (focused && focused->open && focused->app && focused->app->key) {
        focused->app->key(focused, ev);
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
