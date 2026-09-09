/* /dev/wm - a surface for a display driver, without a compositor.
 *
 * See include/wmdev64.h for why this exists and what it deliberately
 * is not. In short: winenovaris.drv will not register as Wine's display
 * driver unless /dev/wm answers WMIO_SCREEN, and this kernel had no
 * /dev/wm, so Wine had no way to make a window and everything above that
 * failed in terms that named something else.
 */

#include "wmdev64.h"
#include "ramfs64.h"
#include "fb64.h"
#include "pmm64.h"
#include "paging64.h"
#include "kstring.h"
#include "serial64.h"

/* Sixteen, because there is one thing on this machine that wants a
 * window and it wants one window. A real compositor would want this to
 * be a list; a table that cannot run out in the case it is built for is
 * the smaller mistake to make first. */
#define WM64_MAX_WINDOWS 16
#define WM64_MAX_FRAMES  2048     /* 8MB: 2048x1024x4 */

typedef struct {
    int      used;
    uint32_t w, h;                /* the client area */
    uint32_t stride, cap_h;       /* the buffer, which never changes */
    uint64_t bytes;               /* stride * cap_h * 4, page-rounded */
    uint64_t frames[WM64_MAX_FRAMES];
    uint64_t nframes;
    char     title[WM64_DEV_TITLE_MAX];
} wm64_window_t;

static wm64_window_t windows[WM64_MAX_WINDOWS];

/* --- registration ----------------------------------------------------- */

int wmdev64_register(void) {
    int node;

    /* No framebuffer, no device. A driver that opens /dev/wm and is told
     * it is not there behaves correctly - it declines to draw - and a
     * driver handed a device that cannot show anything does not. */
    if (!fb64_ready()) return 0;

    node = ramfs64_create("/dev/wm", 0);
    if (node < 0) return 0;
    return ramfs64_set_device(node, RAMFS64_DEV_WM);
}

/* --- the screen ------------------------------------------------------- */

int wmdev64_screen(struct wm64_rect* out) {
    if (!out) return -14;                         /* -EFAULT */
    if (!fb64_ready()) return -19;                /* -ENODEV */
    /* The whole framebuffer is the work area. A compositor would subtract
     * a taskbar here; there is nothing to subtract. */
    out->x = 0;
    out->y = 0;
    out->w = (int32_t)fb64_width();
    out->h = (int32_t)fb64_height();
    return 0;
}

/* --- windows ---------------------------------------------------------- */

static void free_window(wm64_window_t* win) {
    for (uint64_t i = 0; i < win->nframes; i++) pmm64_free_frame(win->frames[i]);
    win->nframes = 0;
    win->used    = 0;
}

int wmdev64_create(const struct wm64_create* req) {
    uint32_t w, h;
    uint64_t bytes, npages;
    int slot;

    if (!req) return -14;                          /* -EFAULT */
    w = req->w;
    h = req->h;
    if (!w || !h || w > WM64_MAX_W || h > WM64_MAX_H) return -22;  /* -EINVAL */

    for (slot = 0; slot < WM64_MAX_WINDOWS; slot++)
        if (!windows[slot].used) break;
    if (slot == WM64_MAX_WINDOWS) return -24;      /* -EMFILE */

    /* stride == w and cap_h == h: this device never resizes a window, so
     * the buffer is exactly the client area. The two fields exist in the
     * contract because a compositor's buffer outlives a resize, and a
     * driver that reads them gets the right answer either way. */
    bytes  = (uint64_t)w * h * 4;
    bytes  = (bytes + PAGE64_SIZE - 1) & ~(uint64_t)(PAGE64_SIZE - 1);
    npages = bytes / PAGE64_SIZE;
    if (npages > WM64_MAX_FRAMES) return -12;      /* -ENOMEM */

    windows[slot].nframes = 0;
    for (uint64_t i = 0; i < npages; i++) {
        uint64_t f = pmm64_alloc_frame();
        if (!f) {
            windows[slot].used = 1;                /* so free_window walks it */
            free_window(&windows[slot]);
            return -12;                            /* -ENOMEM */
        }
        /* A window a process is about to map must not show it whatever
         * the last owner of these frames left behind. */
        kmemset(phys64_to_virt(f), 0, PAGE64_SIZE);
        windows[slot].frames[windows[slot].nframes++] = f;
    }

    windows[slot].used   = 1;
    windows[slot].w      = w;
    windows[slot].h      = h;
    windows[slot].stride = w;
    windows[slot].cap_h  = h;
    windows[slot].bytes  = bytes;
    kstrlcpy(windows[slot].title, req->title, WM64_DEV_TITLE_MAX);
    return slot;
}

void wmdev64_close(int win) {
    if (win < 0 || win >= WM64_MAX_WINDOWS || !windows[win].used) return;
    free_window(&windows[win]);
}

static wm64_window_t* get(int win) {
    if (win < 0 || win >= WM64_MAX_WINDOWS || !windows[win].used) return 0;
    return &windows[win];
}

int wmdev64_getinfo(int win, struct wm64_info* out) {
    wm64_window_t* w = get(win);
    if (!out) return -14;
    if (!w) return -9;                             /* -EBADF */
    out->w      = w->w;
    out->h      = w->h;
    out->stride = w->stride;
    out->cap_h  = w->cap_h;
    out->bytes  = (uint32_t)w->bytes;
    return 0;
}

int wmdev64_getsize(int win, struct wm64_rect* out) {
    wm64_window_t* w = get(win);
    if (!out) return -14;
    if (!w) return -9;
    out->x = 0; out->y = 0;
    out->w = (int32_t)w->w;
    out->h = (int32_t)w->h;
    return 0;
}

int wmdev64_title(int win, const char* title) {
    wm64_window_t* w = get(win);
    if (!title) return -14;
    if (!w) return -9;
    /* Kept and not shown. There is no decoration to put it in - see the
     * header. Storing it means a driver that sets a title and reads one
     * back is not lied to. */
    kstrlcpy(w->title, title, WM64_DEV_TITLE_MAX);
    return 0;
}

/* --- showing it ------------------------------------------------------- */

int wmdev64_damage(int win, const struct wm64_rect* r) {
    wm64_window_t* w = get(win);
    struct wm64_rect box;
    uint32_t fbw, fbh;

    if (!r) return -14;
    if (!w) return -9;
    if (!fb64_ready()) return -19;

    box = *r;
    if (box.w <= 0 || box.h <= 0) return 0;        /* nothing changed */

    /* Clipped to the window and then to the screen, in that order,
     * because a caller may name a rectangle larger than either and a
     * blit that trusts it walks off both. */
    if (box.x < 0) { box.w += box.x; box.x = 0; }
    if (box.y < 0) { box.h += box.y; box.y = 0; }
    if (box.x >= (int32_t)w->w || box.y >= (int32_t)w->h) return 0;
    if (box.x + box.w > (int32_t)w->w) box.w = (int32_t)w->w - box.x;
    if (box.y + box.h > (int32_t)w->h) box.h = (int32_t)w->h - box.y;

    fbw = fb64_width();
    fbh = fb64_height();

    /* Straight to the framebuffer at the window's own coordinates. With
     * one window that is the whole picture; with two, the later blit
     * wins, which is what "no compositor" means and is why the header
     * says this is a device rather than a window manager. */
    for (int32_t row = 0; row < box.h; row++) {
        uint32_t sy = (uint32_t)(box.y + row);
        uint32_t dy = sy;
        uint64_t off;
        const uint32_t* src;

        if (dy >= fbh) break;
        off = ((uint64_t)sy * w->stride + (uint32_t)box.x) * 4;
        src = (const uint32_t*)((uint8_t*)phys64_to_virt(
                  w->frames[off / PAGE64_SIZE]) + (off % PAGE64_SIZE));

        for (int32_t col = 0; col < box.w; col++) {
            uint32_t dx = (uint32_t)(box.x + col);
            uint64_t poff = off + (uint64_t)col * 4;
            const uint32_t* px;

            if (dx >= fbw) break;
            /* Re-derived per pixel rather than walked, because a row can
             * cross a frame boundary: the buffer is a list of frames,
             * not one contiguous run, and a pointer incremented past the
             * end of one frame does not arrive at the start of the next. */
            px = (const uint32_t*)((uint8_t*)phys64_to_virt(
                     w->frames[poff / PAGE64_SIZE]) + (poff % PAGE64_SIZE));
            fb64_put_pixel(dx, dy, *px);
        }
        (void)src;
    }
    return 0;
}

/* --- input ------------------------------------------------------------ */

int wmdev64_poll(int win, struct wm64_event* out) {
    if (!out) return -14;
    if (!get(win)) return -9;
    /* Always empty, and honestly so.
     *
     * Routing a keystroke to a window is the part of a window manager
     * that decides which window has focus, and there is no such
     * decision here. input64.c has the events; nothing owns the question
     * of whose they are. A driver that drains this queue gets WM_EV_NONE
     * and draws a desktop nobody can click on, which is exactly as far
     * as this milestone goes. */
    out->type = WM64_EV_NONE;
    out->a = out->b = out->c = out->d = 0;
    return 0;
}

const uint64_t* wmdev64_frames(int win, uint64_t* nframes, uint64_t* bytes) {
    wm64_window_t* w = get(win);
    if (!w) return 0;
    if (nframes) *nframes = w->nframes;
    if (bytes)   *bytes   = w->bytes;
    return w->frames;
}
