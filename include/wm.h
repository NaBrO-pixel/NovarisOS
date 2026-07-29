#ifndef WM_H
#define WM_H

#include <stdint.h>
#include "gfx.h"
#include "keyboard.h"
#include "mouse.h"

/* wm - the window manager: window objects, z-order, focus, input routing,
 * damage tracking, and the chrome (title bar, caption buttons, border)
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
 *
 * The chrome and the interaction model are Windows': the title reads from
 * the left next to the window's icon, the caption buttons sit at the
 * top-right in minimize/maximize/close order with the close button
 * turning red under the pointer, every edge and corner resizes, and
 * dragging a window against a screen edge snaps it there.
 */

#define WM_MAX_WINDOWS   12
#define WM_TITLEBAR_H    32
#define WM_CORNER_RADIUS 8
#define WM_TITLE_MAX     64

/* Caption buttons, in the order they appear from the right edge inward.
 * 46x32 is what Windows 11 uses, and the ratio matters more than the
 * numbers: they are wide, short, and flush with the top-right corner. */
#define WM_CAPTION_BTN_W 46

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
    WM_MOUSE_RIGHT_DOWN,
};

/* What the pointer should look like. The desktop draws the cursor, so it
 * asks the window manager which shape the pointer is currently over. */
enum {
    WM_CURSOR_ARROW,
    WM_CURSOR_SIZE_NS,      /* top / bottom edge */
    WM_CURSOR_SIZE_WE,      /* left / right edge */
    WM_CURSOR_SIZE_NWSE,    /* top-left / bottom-right corner */
    WM_CURSOR_SIZE_NESW,    /* top-right / bottom-left corner */
};

/* Snap targets, as produced by dragging a window against a screen edge
 * (Aero Snap) or by Win+arrow. */
enum {
    WM_SNAP_NONE,
    WM_SNAP_MAXIMIZE,
    WM_SNAP_LEFT,   WM_SNAP_RIGHT,
    WM_SNAP_TL,     WM_SNAP_TR,
    WM_SNAP_BL,     WM_SNAP_BR,
};

/* An "app" is a vtable plus a name. Windows point at one; several windows
 * can share the same app (two Terminal windows, say). */
typedef struct {
    const char* name;   /* shown on the taskbar button and in the Start menu */
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
    uint8_t maximized;
    uint8_t snapped;         /* WM_SNAP_* if the frame came from a snap */
    uint8_t resizable;
    uint8_t needs_paint;
    uint32_t bg;             /* client-area background */

    /* The frame to go back to when a maximized or snapped window is
     * restored. Windows keeps exactly one of these, which is why
     * restoring a snapped-then-maximized window lands on the original
     * floating size rather than the snap. */
    int restore_x, restore_y, restore_w, restore_h;
};

/* `top` and `bottom` are the screen edges reserved by the shell - with a
 * taskbar at the bottom that's (0, taskbar height). Everything between
 * them is the work area: what a maximized window fills and what a snap
 * divides up. */
void wm_init(int screen_w, int screen_h, int top, int bottom);
void wm_work_area(gfx_rect_t* out);

/* Opens a window centered-ish on screen, with a cascade offset so a
 * second window of the same size doesn't land exactly on the first.
 * Returns 0 if the heap can't back it. */
window_t* wm_open(const app_t* app, const char* title, int w, int h, void* data);
void wm_close(window_t* win);
void wm_close_all_of(const app_t* app);

void wm_focus(window_t* win);
window_t* wm_focused(void);
/* The frontmost non-minimized window's app, or 0. */
const app_t* wm_active_app(void);

void wm_set_title(window_t* win, const char* title);
void wm_move(window_t* win, int x, int y);
void wm_resize(window_t* win, int w, int h);
void wm_minimize(window_t* win);
void wm_unminimize(window_t* win);
void wm_maximize(window_t* win);
void wm_restore(window_t* win);
void wm_toggle_maximize(window_t* win);
/* Puts a window in one of the WM_SNAP_* positions. WM_SNAP_NONE restores. */
void wm_snap(window_t* win, int target);

/* Show desktop (Win+D): minimizes everything, or puts back exactly what
 * the last call minimized. */
void wm_toggle_show_desktop(void);

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
 * to the desktop (taskbar, Start menu, desktop icons). */
int wm_handle_mouse(const mouse_event_t* ev);
int wm_handle_key(const key_event_t* ev);

void wm_tick(void);

/* Which pointer shape the last mouse position calls for (WM_CURSOR_*). */
int wm_cursor_shape(void);

/* While a drag is about to snap, this is the rectangle the window would
 * land in - the desktop paints it as a translucent preview, the way
 * Windows shows you where a snap will put a window before you let go.
 * Returns 0 when there's nothing pending. */
int wm_snap_preview(gfx_rect_t* out);

/* The shell supplies the window's system menu (right-click the title bar,
 * click the window icon, or Alt+Space): the window manager knows where
 * those gestures happened, the desktop knows how to draw a menu. */
void wm_set_sysmenu_handler(void (*fn)(window_t* win, int x, int y));

/* Bumped whenever the set of windows, their titles, or which one is
 * frontmost changes. The desktop watches it to know when the taskbar
 * needs redrawing - it never overlaps a window, so window damage alone
 * would never reach it. */
uint32_t wm_generation(void);

/* Repaints any stale window surfaces, then composites every visible
 * window (shadow, rounded corners and all) into the current gfx target,
 * clipped to `damage`. */
void wm_composite(const gfx_rect_t* damage);

/* --- damage ------------------------------------------------------------ */
/* One accumulator for the whole desktop: the window manager, the taskbar
 * and the Start menu all report through it, and the compositor drains it
 * once per frame. Kept here rather than in desktop.c so wm.c doesn't have
 * to depend on its own client. */
void wm_damage(int x, int y, int w, int h);
void wm_damage_all(void);
int  wm_take_damage(gfx_rect_t* out);   /* 1 if anything is dirty; clears it */
int  wm_has_damage(void);

#endif
