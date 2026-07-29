#ifndef WM_H
#define WM_H

#include <stdint.h>
#include "gfx.h"
#include "keyboard.h"
#include "mouse.h"

/* wm - the window manager: window objects, z-order, focus, input routing,
 * damage tracking, and the chrome (title bar, traffic lights, shadow)
 * drawn around every window.
 *
 * Milestone 7's "window" was a rectangle painted once at boot. A real one
 * needs four things that rectangle didn't have:
 *
 *   - its own backing surface, so moving a window is a blit instead of a
 *     request that its app redraw everything at 60 frames a second;
 *   - a z-order, so overlapping windows have a defined winner and clicks
 *     land in the right one;
 *   - damage tracking, so a blinking cursor repaints ~200 pixels instead
 *     of a megapixel;
 *   - input routing, so the keyboard follows focus and the mouse follows
 *     the pointer.
 */

#define WM_MAX_WINDOWS   12
#define WM_TITLEBAR_H    28
#define WM_CORNER_RADIUS 10
#define WM_TITLE_MAX     64

typedef struct window window_t;

/* How a mouse event reached an app. Coordinates are always client-area
 * relative, and can be outside the window during a drag. */
enum {
    WM_MOUSE_MOVE,
    WM_MOUSE_DOWN,
    WM_MOUSE_UP,
    WM_MOUSE_DRAG,
    WM_MOUSE_WHEEL,
    WM_MOUSE_DOUBLE,
};

/* An "app" is a vtable plus a name. Windows point at one; several windows
 * can share the same app (two Terminal windows, say). */
typedef struct {
    const char* name;   /* shown in the menu bar when this app is frontmost */
    void (*paint)(window_t* win);
    void (*key)(window_t* win, const key_event_t* ev);
    void (*mouse)(window_t* win, int x, int y, int kind, int buttons, int wheel);
    void (*tick)(window_t* win);    /* ~10 Hz while the window is open */
    void (*closed)(window_t* win);  /* free whatever `data` points at */
} app_t;

struct window {
    int x, y, w, h;          /* frame, including the title bar */
    int min_w, min_h;
    char title[WM_TITLE_MAX];
    const app_t* app;
    void* data;              /* app-owned state */
    gfx_surface_t* surface;  /* backing store: the whole frame */

    uint8_t open;
    uint8_t minimized;
    uint8_t zoomed;
    uint8_t resizable;
    uint8_t needs_paint;
    uint32_t bg;             /* client-area background */

    int restore_x, restore_y, restore_w, restore_h; /* frame before zooming */
};

/* `top` is the height reserved for the menu bar - windows can't be moved
 * above it - and `bottom` the height reserved for the Dock, used when
 * placing and zooming windows. */
void wm_init(int screen_w, int screen_h, int top, int bottom);

/* Opens a window centered-ish on screen, with a cascade offset so a
 * second window of the same size doesn't land exactly on the first.
 * Returns 0 if the heap can't back it. */
window_t* wm_open(const app_t* app, const char* title, int w, int h, void* data);
void wm_close(window_t* win);
void wm_close_all_of(const app_t* app);

void wm_focus(window_t* win);
window_t* wm_focused(void);
/* The frontmost non-minimized window's app, or 0 - this is what the menu
 * bar names on the left. */
const app_t* wm_active_app(void);

void wm_set_title(window_t* win, const char* title);
void wm_move(window_t* win, int x, int y);
void wm_resize(window_t* win, int w, int h);
void wm_minimize(window_t* win);
void wm_unminimize(window_t* win);
void wm_zoom(window_t* win);

/* Marks a window's contents stale: its app repaints before the next
 * frame, and the region it occupies is redrawn. */
void wm_invalidate(window_t* win);
/* Same, for one rectangle of the client area (still repaints the app -
 * apps here are cheap to draw - but damages far less of the screen). */
void wm_invalidate_rect(window_t* win, int x, int y, int w, int h);

/* Window list in z-order, back to front. Includes minimized windows. */
int wm_count(void);
window_t* wm_at(int index);
window_t* wm_find_by_app(const app_t* app);
int wm_app_window_count(const app_t* app);

/* Client area (the part below the title bar), in screen coordinates. */
void wm_client_rect(const window_t* win, gfx_rect_t* out);
static inline int wm_client_w(const window_t* win) { return win->w; }
static inline int wm_client_h(const window_t* win) { return win->h - WM_TITLEBAR_H; }

/* Input. Both return 1 if the event was consumed and shouldn't be offered
 * to the desktop (menus, Dock, Spotlight). */
int wm_handle_mouse(const mouse_event_t* ev);
int wm_handle_key(const key_event_t* ev);

void wm_tick(void);

/* Bumped whenever the set of windows, their titles, or which one is
 * frontmost changes. The desktop watches it to know when the Dock and the
 * menu bar need redrawing - neither of them overlaps a window, so window
 * damage alone would never reach them. */
uint32_t wm_generation(void);

/* Repaints any stale window surfaces, then composites every visible
 * window (shadow, rounded corners and all) into the current gfx target,
 * clipped to `damage`. */
void wm_composite(const gfx_rect_t* damage);

/* --- damage ------------------------------------------------------------ */
/* One accumulator for the whole desktop: the window manager, the Dock and
 * the menu bar all report through it, and the compositor drains it once
 * per frame. Kept here rather than in desktop.c so wm.c doesn't have to
 * depend on its own client. */
void wm_damage(int x, int y, int w, int h);
void wm_damage_all(void);
int  wm_take_damage(gfx_rect_t* out);   /* 1 if anything is dirty; clears it */
int  wm_has_damage(void);

#endif
