/* wmdev.c - /dev/wm: a window on the desktop, for a ring-3 process.
 *
 * Milestone 36. See include/wmdev.h for the protocol and for why it is
 * shaped the way it is.
 *
 * The design in one paragraph. A slot holds three things that belong
 * together: a `window_t` the window manager owns, a page-aligned pixel
 * buffer the *process* owns through mmap, and an input queue. The window's
 * paint callback blits the buffer into the client area, so a frame costs
 * one blit and no copies on the way in - the process draws straight into
 * memory the compositor reads. Everything else here is bookkeeping around
 * that.
 *
 * **The window resizes** (Milestone 42), and the way it does is the only
 * interesting decision in this file. Until now it could not: the buffer
 * was allocated at the client size, so following a resize meant handing
 * the process a different mapping - unmapping frames out from under code
 * that is drawing into them, which there is no safe moment to do.
 *
 * So the buffer is not the window. It is allocated once, at the largest
 * client area this window could ever be given (the desktop work area,
 * since the window manager never makes a window bigger than that), and
 * the client size is a *sub-rectangle* of it: `w`/`h` rows and columns of
 * a buffer that is `alloc_w` wide. `gfx_surface_t` has carried `pitch`
 * and `cap_h` since the compositor was written, so the paint below needed
 * one line to say the allocation is wider than the picture.
 *
 * A resize is then bookkeeping. Nothing is reallocated, no address
 * changes, the mapping the process made at startup stays exactly as valid
 * as it was, and the process is told the new size (WM_EV_RESIZE) and
 * draws into a different part of memory it already has.
 *
 * The price is memory: a 1280x800 desktop makes every window a 4MB
 * allocation whatever size it is on screen. That is a real cost and it is
 * bounded - WMDEV_MAX_SLOTS windows of it - and it buys the removal of
 * the one thing that made resizing unsafe, which is a trade worth
 * writing down rather than hiding.
 *
 * One thing is still deliberately not done:
 *
 *   - **One window per open.** A process that wants two opens /dev/wm
 *     twice. Wine's driver wants one window per HWND, so this will have to
 *     change; the interface does not have to.
 */

#include "wmdev.h"
#include "posix.h"      /* the errno numbers an ioctl answers with */
#include "wm.h"
#include "gfx.h"
#include "vfs.h"
#include "kheap.h"
#include "kstring.h"
#include "keyboard.h"
#include "console.h"
#include "desktop.h"

#define WMDEV_MAX_SLOTS 16
#define WMDEV_QUEUE     64

typedef struct {
    int          in_use;
    window_t*    win;
    vfs_node_t*  node;      /* the per-open node; its `data` is `pixels` */

    uint8_t*     alloc;     /* the unaligned allocation */
    uint32_t*    pixels;    /* page-aligned, alloc_w*alloc_h*4 */
    int          w, h;              /* the client area now: what is shown */
    int          alloc_w, alloc_h;  /* the buffer: fixed for the window's life */
    int          last_w, last_h;    /* client area at the last WM_EV_RESIZE */

    /* A ring of input events. Dropped rather than blocking when it
     * overflows: a process that is not draining its input is not one the
     * kernel should be made to wait for. */
    struct wm_event q[WMDEV_QUEUE];
    uint32_t     head, tail;
} wmdev_slot_t;

static wmdev_slot_t slots[WMDEV_MAX_SLOTS];

static wmdev_slot_t* slot_of(window_t* win) {
    for (int i = 0; i < WMDEV_MAX_SLOTS; i++) {
        if (slots[i].in_use && slots[i].win == win) return &slots[i];
    }
    return 0;
}

static void queue_push(wmdev_slot_t* s, uint32_t type,
                       int32_t a, int32_t b, int32_t c, int32_t d) {
    uint32_t next = (s->head + 1) % WMDEV_QUEUE;
    if (next == s->tail) return;         /* full: drop the newest */
    s->q[s->head].type = type;
    s->q[s->head].a = a;
    s->q[s->head].b = b;
    s->q[s->head].c = c;
    s->q[s->head].d = d;
    s->head = next;
}

/* --- the app the window manager sees ------------------------------------ */

/* One blit per frame, from the process's buffer into the client area.
 * `gfx_origin` has already been moved to the client origin and the clip
 * set by the window manager, so this draws at (0,0). */
static void wmdev_paint(window_t* win) {
    wmdev_slot_t* s = slot_of(win);
    if (!s || !s->pixels) return;

    /* The client area changed size since the last frame - adopt it and
     * tell the process. There is no resize hook on app_t to hang this off,
     * and there does not need to be: a resize is followed by a repaint, so
     * noticing it here notices every one of them exactly once.
     *
     * Nothing is reallocated. `w`/`h` name a sub-rectangle of a buffer
     * that was made big enough at WMIO_CREATE, so this is two stores and
     * an event - and the process's mapping is as valid afterwards as it
     * was before. The clamp is belt and braces: the window manager does
     * not exceed the work area the buffer was sized from, and if that ever
     * stops being true a clipped window beats a buffer overrun. */
    int cw = wm_client_w(win), ch = wm_client_h(win);
    if (cw > s->alloc_w) cw = s->alloc_w;
    if (ch > s->alloc_h) ch = s->alloc_h;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    if (cw != s->w || ch != s->h) {
        s->w = cw;
        s->h = ch;
        if (s->last_w != cw || s->last_h != ch) {
            s->last_w = cw;
            s->last_h = ch;
            queue_push(s, WM_EV_RESIZE, cw, ch, 0, 0);
        }
    }

    /* A surface header around memory this file already owns - no
     * allocation, and gfx_copy_region does the clipping. `pitch` is the
     * buffer's width rather than the window's, which is the whole of what
     * makes the picture come out unsheared after a resize. */
    gfx_surface_t src;
    src.pixels = s->pixels;
    src.w = s->w;
    src.h = s->h;
    src.pitch = s->alloc_w;
    src.cap_h = s->alloc_h;

    gfx_blit(&src, 0, 0);
}

static void wmdev_mouse(window_t* win, int x, int y, int kind, int buttons,
                        int wheel) {
    wmdev_slot_t* s = slot_of(win);
    if (!s) return;
    queue_push(s, WM_EV_MOUSE, x, y, kind, kind == WM_MOUSE_WHEEL ? wheel
                                                                  : buttons);
}

static void wmdev_key(window_t* win, const key_event_t* ev) {
    wmdev_slot_t* s = slot_of(win);
    if (!s || !ev) return;
    queue_push(s, WM_EV_KEY, ev->ascii, ev->code, ev->mods, ev->pressed);
}

/* The window manager closed the window - the user clicked the X, or the
 * desktop is tearing down. The process still holds a descriptor and a
 * mapping, so nothing is freed here: the slot is marked windowless and
 * the process is told, and the memory goes when the descriptor does. */
static void wmdev_closed(window_t* win) {
    wmdev_slot_t* s = slot_of(win);
    if (!s) return;
    queue_push(s, WM_EV_CLOSE, 0, 0, 0, 0);
    s->win = 0;
}

static const app_t wmdev_app = {
    "Wine", wmdev_paint, wmdev_key, wmdev_mouse, 0, wmdev_closed
};

/* --- the device ---------------------------------------------------------- */

static void slot_release(wmdev_slot_t* s) {
    if (!s->in_use) return;
    if (s->win) {
        window_t* w = s->win;
        s->win = 0;              /* so wmdev_closed() finds nothing to do */
        wm_close(w);
    }
    if (s->node) {
        /* The node must not free the pixels: they are this file's, and a
         * mapping may still be holding the frames. */
        s->node->data = 0;
        s->node->length = 0;
        s->node->capacity = 0;
        s->node->mappable = 0;
        s->node = 0;
    }
    if (s->alloc) kfree(s->alloc);
    s->alloc = 0;
    s->pixels = 0;
    s->w = s->h = 0;
    s->alloc_w = s->alloc_h = 0;
    s->in_use = 0;
    s->head = s->tail = 0;
}

static void wmdev_forget(vfs_node_t* node) {
    for (int i = 0; i < WMDEV_MAX_SLOTS; i++) {
        if (slots[i].in_use && slots[i].node == node) {
            slot_release(&slots[i]);
            return;
        }
    }
}

static int32_t wmdev_ioctl(vfs_node_t* node, uint32_t req, void* arg);

static const vfs_ops_t wmdev_ops = {
    0,              /* write     */
    0,              /* truncate  */
    0,              /* create    */
    0,              /* unlink    */
    0,              /* rename    */
    0,              /* materialize */
    wmdev_forget,
    0,              /* open: filled in below by the control node only */
    wmdev_ioctl,
};

static int32_t wmdev_create(wmdev_slot_t* s, const struct wm_create* c) {
    if (s->win || s->pixels) return -EEXIST;      /* one window per open */
    if (!c) return -EFAULT;

    uint32_t w = c->w, h = c->h;
    if (w == 0 || h == 0 || w > WM_DEV_MAX_W || h > WM_DEV_MAX_H) {
        return -EINVAL;
    }

    /* The buffer is sized for the largest this window could ever be shown
     * at, not for the size asked for - which is what lets a resize be
     * bookkeeping instead of a new mapping. The work area is that size:
     * the window manager maximizes to it and never past it. A window that
     * asks for more than the desktop still gets what it asked for, since
     * the buffer only has to be big enough, and clamping here would make
     * WMIO_CREATE lie about the size it accepted. */
    gfx_rect_t work;
    wm_work_area(&work);
    uint32_t aw = (uint32_t)(work.w > 0 ? work.w : (int)w);
    uint32_t ah = (uint32_t)(work.h > 0 ? work.h : (int)h);
    if (aw < w) aw = w;
    if (ah < h) ah = h;
    if (aw > WM_DEV_MAX_W) aw = WM_DEV_MAX_W;
    if (ah > WM_DEV_MAX_H) ah = WM_DEV_MAX_H;

    /* Page-aligned, because the process is going to map it and a mapping
     * hands over whole frames. Over-allocate and round up, the same blunt
     * way vfs_make_mappable() does it and for the same reason. */
    uint32_t bytes = (aw * ah * 4u + 4095u) & ~4095u;
    uint8_t* raw = (uint8_t*)kmalloc(bytes + 4096u);
    if (!raw) return -ENOMEM;
    uint32_t* px = (uint32_t*)(((uint32_t)raw + 4095u) & ~4095u);
    kmemset(px, 0, bytes);

    char title[WM_DEV_TITLE_MAX];
    uint32_t i = 0;
    while (i < WM_DEV_TITLE_MAX - 1 && c->title[i]) { title[i] = c->title[i]; i++; }
    title[i] = '\0';
    if (!title[0]) kstrcpy(title, "Program");

    /* The frame is the client area plus the title bar, so that a program
     * asking for 640x480 gets 640x480 to draw in. */
    window_t* win = wm_open(&wmdev_app, title, (int)w, (int)h + WM_TITLEBAR_H, 0);
    if (!win) {
        kfree(raw);
        return -ENOSPC;
    }

    s->alloc = raw;
    s->pixels = px;
    s->w = s->last_w = (int)w;
    s->h = s->last_h = (int)h;
    s->alloc_w = (int)aw;
    s->alloc_h = (int)ah;
    s->win = win;

    /* This is what makes mmap work with no new code: the node's bytes
     * *are* the window's pixels, already page-aligned, already mappable.
     * sys_mmap2's MAP_SHARED path hands the frames straight over. */
    s->node->data = (uint8_t*)px;
    /* The whole buffer, not the client area: a process maps this once and
     * the mapping has to still cover the window after it is made bigger. */
    s->node->length = aw * ah * 4u;
    s->node->capacity = bytes;
    s->node->mappable = 1;
    s->node->from_initrd = 0;

    wm_focus(win);
    wm_invalidate(win);
    return 0;
}

/* The work area, which is a property of the desktop rather than of any
 * window - so both the control node and an open window answer it. */
static int32_t wmdev_screen(void* arg) {
    struct wm_rect* r = (struct wm_rect*)arg;
    if (!r) return -EFAULT;
    gfx_rect_t work;
    wm_work_area(&work);
    r->x = work.x;
    r->y = work.y;
    r->w = work.w;
    r->h = work.h;
    return 0;
}

static int32_t wmdev_ioctl(vfs_node_t* node, uint32_t req, void* arg) {
    if (req == WMIO_SCREEN) return wmdev_screen(arg);

    wmdev_slot_t* s = 0;
    for (int i = 0; i < WMDEV_MAX_SLOTS; i++) {
        if (slots[i].in_use && slots[i].node == node) { s = &slots[i]; break; }
    }
    if (!s) return -ENOTTY;

    switch (req) {
        case WMIO_CREATE:
            return wmdev_create(s, (const struct wm_create*)arg);

        case WMIO_DAMAGE: {
            if (!s->win) return -EPIPE;      /* the window has been closed */
            const struct wm_rect* r = (const struct wm_rect*)arg;
            if (!r) wm_invalidate(s->win);
            else {
                /* Client coordinates, which is what the process's pixels
                 * are indexed by; the window manager shifts them by the
                 * title bar itself. */
                wm_invalidate_rect(s->win, r->x, r->y, r->w, r->h);
            }
            /* And show it. The desktop's event loop is blocked on the
             * stack below this syscall - the calling process is running
             * inside a shell command, and the shell runs inside that loop
             * - so a frame only reaches the screen if the process asking
             * for it turns the crank. See desktop_pump(). */
            desktop_pump();
            return 0;
        }

        case WMIO_POLL: {
            struct wm_event* out = (struct wm_event*)arg;
            if (!out) return -EFAULT;
            /* Input arrives the same way a frame leaves: nothing is
             * draining the keyboard and mouse queues while this process
             * runs except this call. Pump before looking, so a poll
             * reports what has happened rather than what had happened
             * last time round. */
            if (s->tail == s->head) desktop_pump();
            if (s->tail == s->head) { out->type = WM_EV_NONE; return 0; }
            *out = s->q[s->tail];
            s->tail = (s->tail + 1) % WMDEV_QUEUE;
            return 1;
        }

        case WMIO_GETSIZE: {
            struct wm_rect* r = (struct wm_rect*)arg;
            if (!r) return -EFAULT;
            r->x = r->y = 0;
            r->w = s->w;
            r->h = s->h;
            return 0;
        }

        /* The client area and the buffer behind it, which stopped being
         * the same thing in Milestone 42. A process that draws with
         * `stride` survives a resize; one that assumes `w` shears. */
        case WMIO_GETINFO: {
            struct wm_info* i = (struct wm_info*)arg;
            if (!i) return -EFAULT;
            if (!s->pixels) return -EPIPE;   /* no window created yet */
            i->w      = (uint32_t)s->w;
            i->h      = (uint32_t)s->h;
            i->stride = (uint32_t)s->alloc_w;
            i->cap_h  = (uint32_t)s->alloc_h;
            i->bytes  = (uint32_t)s->alloc_w * (uint32_t)s->alloc_h * 4u;
            return 0;
        }

        case WMIO_TITLE: {
            if (!s->win || !arg) return -EPIPE;
            kstrlcpy(s->win->title, (const char*)arg, WM_TITLE_MAX);
            wm_invalidate(s->win);
            return 0;
        }

        default:
            return -ENOTTY;
    }
}

/* --- the control node ---------------------------------------------------
 *
 * /dev/wm itself is not a window; opening it makes one. Its `open` hook
 * hands back a *different* node, private to that descriptor, which is how
 * two processes get two windows out of one path. The private node is
 * created unlinked, so the last descriptor to close it is what frees it -
 * and freeing it is what closes the window (wmdev_forget). */

static vfs_node_t* wmdev_open(vfs_node_t* control) {
    (void)control;
    for (int i = 0; i < WMDEV_MAX_SLOTS; i++) {
        if (slots[i].in_use) continue;

        vfs_node_t* n = vfs_node_alloc();
        if (!n) return 0;
        kstrcpy(n->name, "wm");
        n->flags = VFS_FILE;
        n->mode = 0600u;
        n->ops = &wmdev_ops;
        /* Never in a directory, so the last unref releases it. */
        n->unlinked = 1;

        slots[i].in_use = 1;
        slots[i].node = n;
        slots[i].win = 0;
        slots[i].alloc = 0;
        slots[i].pixels = 0;
        slots[i].w = slots[i].h = 0;
        slots[i].alloc_w = slots[i].alloc_h = 0;
        slots[i].last_w = slots[i].last_h = 0;
        slots[i].head = slots[i].tail = 0;
        return n;
    }
    return 0;
}

/* /dev/wm itself answers exactly one question - how big the desktop is -
 * because a program needs that before it has a window to ask. */
static int32_t wmdev_control_ioctl(vfs_node_t* node, uint32_t req, void* arg) {
    (void)node;
    if (req == WMIO_SCREEN) return wmdev_screen(arg);
    return -ENOTTY;
}

static const vfs_ops_t wmdev_control_ops = {
    0, 0, 0, 0, 0, 0, 0,
    wmdev_open,
    wmdev_control_ioctl,
};

void wmdev_init(void) {
    vfs_node_t* root = vfs_lookup("/");
    if (!root) return;

    vfs_node_t* dev = vfs_lookup("/dev");
    if (!dev) dev = vfs_create(root, "dev", VFS_DIRECTORY);
    if (!dev) return;

    vfs_node_t* n = vfs_create(dev, "wm", VFS_FILE);
    if (!n) return;
    n->ops = &wmdev_control_ops;
    n->mode = 0666u;
}
