/* /dev/wm - a compositor for a display driver (Milestone 109).
 *
 * See include/wmdev64.h for what this is. The short version: windows at
 * the positions Wine gives them, in the order Wine stacks them, drawn
 * onto the framebuffer one row at a time from whichever window is in
 * front, with a pointer on top; and the mouse and keyboard delivered to
 * the process whose window they are over.
 *
 * Everything here is under wm_lock. It is reached from syscalls, under
 * the kernel lock, and from the timer, which is not: the timer is what
 * moves the pointer when nobody is asking. In the lock order it comes
 * after the pipes and before the task table and the frame allocator,
 * both of which it calls.
 */

#include "wmdev64.h"
#include "ramfs64.h"
#include "fb64.h"
#include "pmm64.h"
#include "paging64.h"
#include "kstring.h"
#include "serial64.h"
#include "input64.h"
#include "proc64.h"
#include "sched64.h"
#include "spin64.h"

#define WM64_MAX_FRAMES    4096    /* 16MB: 2048x2048x4 */
#define WM64_MAX_LISTENERS 64
#define WM64_MAX_INBOXES   32
#define WM64_INBOX_EVENTS  128

/* Anything woken releases every poll and epoll_wait parked on the shared
 * condition key, so the key itself only has to be one nobody else uses. */
#define WM64_WAIT_KEY 0x5700000000000000ULL

typedef struct {
    int      used;
    int      refs;            /* descriptors naming it (fork, dup, SCM_RIGHTS) */
    int      pid;             /* the process that made it, whose input it gets */
    int      shown;
    int32_t  x, y;            /* where its top-left pixel is on the screen */
    uint32_t w, h;            /* how much of the buffer is shown */
    uint32_t stride, cap_h;   /* the buffer */
    uint64_t bytes;
    uint64_t frames[WM64_MAX_FRAMES];
    uint64_t nframes;
    uint64_t id;              /* the driver's name for it: Wine's HWND */
    char     title[WM64_DEV_TITLE_MAX];
    uint64_t damages, pixels;
} wm64_window_t;

typedef struct { int used, refs, pid; } wm64_listener_t;

/* Input waits per process, not per window: the wineserver routes it to a
 * window itself, so all that matters is that it reaches one Wine process
 * exactly once. */
typedef struct {
    int used, pid;
    struct wm64_event ev[WM64_INBOX_EVENTS];
    int head, n;
    uint64_t dropped;
} wm64_inbox_t;

static wm64_window_t   windows[WM64_MAX_WINDOWS];
static wm64_listener_t listeners[WM64_MAX_LISTENERS];
static wm64_inbox_t    inboxes[WM64_MAX_INBOXES];

/* The stack, bottom first. Holds every window, shown or not. */
static int stack[WM64_MAX_WINDOWS];
static int nstack;
static int nwindows;

static spin64_t wm_lock = SPIN64_INIT;

/* The pointer. */
static int32_t  ptr_x, ptr_y;
static uint32_t ptr_buttons;
static int      ptr_grab = -1;      /* window holding it while a button is down */
static int      ptr_last_pid = -1;  /* where the last pointer event went */
static int32_t  acc_dx, acc_dy;     /* a packet in progress */
static uint32_t acc_buttons;
static int      acc_changed;

static uint64_t routed_pointer, routed_keys, routed_nowhere;

#define DESKTOP_COLOR 0x00305878u

/* --- the serial log ------------------------------------------------------ */

static void say_window(const char* what, int slot, const wm64_window_t* w) {
    serial64_puts("NOVARIS64: [wm] ");
    serial64_puts(what);
    serial64_puts(" window ");
    serial64_putdec((uint64_t)slot);
    serial64_puts(" ");
    serial64_putdec(w->w);
    serial64_puts("x");
    serial64_putdec(w->h);
    serial64_puts(" \"");
    serial64_puts(w->title);
    serial64_puts("\" damages ");
    serial64_putdec(w->damages);
    serial64_puts(" pixels ");
    serial64_putdec(w->pixels);
    serial64_putc('\n');
}

/* --- registration -------------------------------------------------------- */

int wmdev64_register(void) {
    int node;
    if (!fb64_ready()) return 0;
    node = ramfs64_create("/dev/wm", 0);
    if (node < 0) return 0;
    if (!nwindows) {
        ptr_x = (int32_t)fb64_width() / 2;
        ptr_y = (int32_t)fb64_height() / 2;
    }
    return ramfs64_set_device(node, RAMFS64_DEV_WM);
}

int wmdev64_screen(struct wm64_rect* out) {
    if (!out) return -14;
    if (!fb64_ready()) return -19;
    out->x = 0;
    out->y = 0;
    out->w = (int32_t)fb64_width();
    out->h = (int32_t)fb64_height();
    return 0;
}

/* --- the pointer's picture ----------------------------------------------- */

#define CURSOR_W 12
#define CURSOR_H 19
static const char* const cursor_shape[CURSOR_H] = {
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
    "X..XX..X    ",
    "X.X  X..X   ",
    "XX   X..X   ",
    "X     X..X  ",
    "      X..X  ",
    "       XX   ",
};

/* --- composition ---------------------------------------------------------
 *
 * One row at a time into a row buffer, then one blit. Each pixel of the
 * row comes from the topmost shown window covering it, found span by
 * span: from x, the topmost window covering x decides the colour, and
 * the span ends where that window ends or where a window above it
 * begins, whichever is first. The pointer is painted into the row last,
 * so the screen never shows a frame without it.
 */

static uint32_t rowbuf[4096];

static int covers_row(const wm64_window_t* w, int32_t y) {
    return w->shown && y >= w->y && y < w->y + (int32_t)w->h;
}

/* `n` pixels of window `w`'s row `wy` from column `wx`, which may cross
 * frames: the buffer is a list of frames, not one run. */
static void copy_window_span(wm64_window_t* w, int32_t wx, int32_t wy, int n, uint32_t* out) {
    uint64_t off = ((uint64_t)(uint32_t)wy * w->stride + (uint32_t)wx) * 4;
    while (n > 0) {
        uint64_t in_frame = (PAGE64_SIZE - (off % PAGE64_SIZE)) / 4;
        int chunk = n < (int)in_frame ? n : (int)in_frame;
        const uint32_t* src = (const uint32_t*)((uint8_t*)phys64_to_virt(
            w->frames[off / PAGE64_SIZE]) + (off % PAGE64_SIZE));
        kmemcpy(out, src, (uint64_t)chunk * 4);
        out += chunk;
        n -= chunk;
        off += (uint64_t)chunk * 4;
        w->pixels += (uint64_t)chunk;
    }
}

static void compose_row(int32_t y, int32_t x0, int32_t x1) {
    int32_t x = x0;
    while (x < x1) {
        int top = -1;
        int32_t end = x1;
        for (int k = nstack - 1; k >= 0; k--) {
            wm64_window_t* w = &windows[stack[k]];
            if (covers_row(w, y) && x >= w->x && x < w->x + (int32_t)w->w) { top = k; break; }
        }
        if (top >= 0) {
            wm64_window_t* w = &windows[stack[top]];
            if (w->x + (int32_t)w->w < end) end = w->x + (int32_t)w->w;
        }
        for (int k = nstack - 1; k > top; k--) {
            wm64_window_t* w = &windows[stack[k]];
            if (covers_row(w, y) && w->x > x && w->x < end) end = w->x;
        }
        if (top >= 0) {
            wm64_window_t* w = &windows[stack[top]];
            copy_window_span(w, x - w->x, y - w->y, end - x, &rowbuf[x - x0]);
        } else {
            for (int32_t i = x; i < end; i++) rowbuf[i - x0] = DESKTOP_COLOR;
        }
        x = end;
    }

    if (y >= ptr_y && y < ptr_y + CURSOR_H) {
        const char* line = cursor_shape[y - ptr_y];
        for (int i = 0; i < CURSOR_W; i++) {
            int32_t px = ptr_x + i;
            if (px < x0 || px >= x1) continue;
            if (line[i] == 'X') rowbuf[px - x0] = 0x00000000u;
            else if (line[i] == '.') rowbuf[px - x0] = 0x00FFFFFFu;
        }
    }
}

/* Redraws a rectangle of the screen, clipped to it. */
static void compose(int32_t x, int32_t y, int32_t w, int32_t h) {
    int32_t sw = (int32_t)fb64_width(), sh = (int32_t)fb64_height();
    int32_t x1, y1;
    if (!fb64_ready() || w <= 0 || h <= 0) return;
    x1 = x + w; y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    if (x1 - x > (int32_t)(sizeof(rowbuf) / 4)) x1 = x + (int32_t)(sizeof(rowbuf) / 4);
    for (int32_t row = y; row < y1; row++) {
        compose_row(row, x, x1);
        fb64_blit((uint32_t)x, (uint32_t)row, (uint32_t)(x1 - x), 1, rowbuf, 0);
    }
}

static void compose_window(const wm64_window_t* w) {
    compose(w->x, w->y, (int32_t)w->w, (int32_t)w->h);
}

static void compose_all(void) {
    compose(0, 0, (int32_t)fb64_width(), (int32_t)fb64_height());
}

/* --- the stack ------------------------------------------------------------ */

static void stack_remove(int slot) {
    for (int k = 0; k < nstack; k++)
        if (stack[k] == slot) {
            for (int j = k; j < nstack - 1; j++) stack[j] = stack[j + 1];
            nstack--;
            return;
        }
}

static void stack_insert(int slot, int at) {
    if (at < 0) at = 0;
    if (at > nstack) at = nstack;
    for (int j = nstack; j > at; j--) stack[j] = stack[j - 1];
    stack[at] = slot;
    nstack++;
}

static int stack_index(int slot) {
    for (int k = 0; k < nstack; k++) if (stack[k] == slot) return k;
    return -1;
}

/* The topmost shown window under a point, or -1. */
static int window_at(int32_t x, int32_t y) {
    for (int k = nstack - 1; k >= 0; k--) {
        wm64_window_t* w = &windows[stack[k]];
        if (w->shown && x >= w->x && x < w->x + (int32_t)w->w &&
            y >= w->y && y < w->y + (int32_t)w->h)
            return stack[k];
    }
    return -1;
}

static int top_window(void) {
    for (int k = nstack - 1; k >= 0; k--)
        if (windows[stack[k]].shown) return stack[k];
    return -1;
}

/* --- inboxes --------------------------------------------------------------- */

static wm64_inbox_t* inbox_for(int pid, int create) {
    wm64_inbox_t* spare = 0;
    for (int i = 0; i < WM64_MAX_INBOXES; i++) {
        if (inboxes[i].used && inboxes[i].pid == pid) return &inboxes[i];
        if (!inboxes[i].used && !spare) spare = &inboxes[i];
    }
    if (!create || !spare) return 0;
    kmemset(spare, 0, sizeof(*spare));
    spare->used = 1;
    spare->pid = pid;
    return spare;
}

/* An inbox goes when nothing in its process can read it any more. */
static void inbox_release_if_unused(int pid) {
    wm64_inbox_t* in = inbox_for(pid, 0);
    if (!in) return;
    for (int i = 0; i < WM64_MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].pid == pid) return;
    for (int i = 0; i < WM64_MAX_LISTENERS; i++)
        if (listeners[i].used && listeners[i].pid == pid) return;
    in->used = 0;
}

/* Queues an event for a process. A pointer move with nothing else in it
 * replaces one still waiting, so a process that is slow to read sees
 * where the pointer is now rather than everywhere it has been; a
 * button change is never merged away. */
static void deliver(int pid, const struct wm64_event* ev) {
    wm64_inbox_t* in = inbox_for(pid, 1);
    if (!in) { routed_nowhere++; return; }
    if (ev->type == WM64_EV_POINTER && ev->d == 0 && in->n > 0) {
        struct wm64_event* last = &in->ev[(in->head + in->n - 1) % WM64_INBOX_EVENTS];
        if (last->type == WM64_EV_POINTER && last->c == ev->c && last->d == 0) {
            last->a = ev->a;
            last->b = ev->b;
            return;
        }
    }
    if (in->n == WM64_INBOX_EVENTS) { in->dropped++; return; }
    in->ev[(in->head + in->n) % WM64_INBOX_EVENTS] = *ev;
    in->n++;
    sched64_wake(WM64_WAIT_KEY, SCHED64_MAX_TASKS);
}

/* --- routing ---------------------------------------------------------------- */

static void pointer_moved(int32_t ox, int32_t oy) {
    if (ox == ptr_x && oy == ptr_y) return;
    compose(ox, oy, CURSOR_W, CURSOR_H);
    compose(ptr_x, ptr_y, CURSOR_W, CURSOR_H);
}

/* One whole packet, at its SYN_REPORT. */
static void pointer_packet(void) {
    int32_t ox = ptr_x, oy = ptr_y;
    int32_t sw = (int32_t)fb64_width(), sh = (int32_t)fb64_height();
    uint32_t was = ptr_buttons;
    struct wm64_event ev;
    int target;

    ptr_x += acc_dx;
    ptr_y += acc_dy;
    if (ptr_x < 0) ptr_x = 0;
    if (ptr_y < 0) ptr_y = 0;
    if (ptr_x >= sw) ptr_x = sw - 1;
    if (ptr_y >= sh) ptr_y = sh - 1;
    ptr_buttons = acc_buttons;
    acc_dx = acc_dy = 0;
    acc_changed = 0;
    pointer_moved(ox, oy);

    /* Pressed with nothing held: whatever is under the pointer has it
     * until every button is up again, so a drag that leaves the window
     * still ends in it - which is what makes a scrollbar or a window
     * edge draggable at all. */
    if (!was && ptr_buttons) ptr_grab = window_at(ptr_x, ptr_y);
    target = ptr_grab >= 0 && windows[ptr_grab].used ? ptr_grab : window_at(ptr_x, ptr_y);

    ev.type = WM64_EV_POINTER;
    ev.a = ptr_x;
    ev.b = ptr_y;
    ev.c = (int32_t)ptr_buttons;
    ev.d = 0;
    if (target >= 0) {
        ptr_last_pid = windows[target].pid;
        deliver(ptr_last_pid, &ev);
        routed_pointer++;
    } else if (ptr_last_pid >= 0 && inbox_for(ptr_last_pid, 0)) {
        /* Over the desktop: the process that had it last hears that it
         * left, rather than believing the pointer stopped at its edge. */
        deliver(ptr_last_pid, &ev);
        routed_pointer++;
    } else {
        routed_nowhere++;
    }
    if (!ptr_buttons) ptr_grab = -1;
}

static void route_mouse(const input64_event_t* e) {
    if (e->type == EV64_REL) {
        if (e->code == REL64_X) acc_dx += e->value;
        if (e->code == REL64_Y) acc_dy += e->value;
        acc_changed = 1;
    } else if (e->type == EV64_KEY) {
        uint32_t bit = e->code == BTN64_LEFT ? WM64_BUTTON_LEFT
                     : e->code == BTN64_RIGHT ? WM64_BUTTON_RIGHT
                     : e->code == BTN64_MIDDLE ? WM64_BUTTON_MIDDLE : 0;
        if (e->value) acc_buttons |= bit; else acc_buttons &= ~bit;
        acc_changed = 1;
    } else if (e->type == EV64_SYN && acc_changed) {
        pointer_packet();
    }
}

/* Keys go to the process whose window is in front. Wine raises the
 * window it activates, so that is its foreground window; and which of
 * that process's threads gets the key is the wineserver's business. */
static void route_key(const input64_event_t* e) {
    struct wm64_event ev;
    int top;
    if (e->type != EV64_KEY) return;
    top = top_window();
    if (top < 0) { routed_nowhere++; return; }
    ev.type = WM64_EV_KEYCODE;
    ev.a = e->code;
    ev.b = e->value ? 1 : 0;
    ev.c = (int32_t)input64_modifiers();
    ev.d = 0;
    deliver(windows[top].pid, &ev);
    routed_keys++;
}

/* The hardware's queues are drained only while there is a window: with
 * none, /dev/input is the only reader and keeps what it is sent. */
static void route_input(void) {
    input64_event_t e;
    if (!nwindows) return;
    while (input64_read(INPUT64_MOUSE, &e, sizeof(e)) == sizeof(e)) route_mouse(&e);
    while (input64_read(INPUT64_KBD, &e, sizeof(e)) == sizeof(e)) route_key(&e);
}

void wmdev64_timer(void) {
    uint64_t fl;
    if (!nwindows) return;
    if (!input64_queued(INPUT64_MOUSE) && !input64_queued(INPUT64_KBD)) return;
    fl = spin64_acquire(&wm_lock);
    route_input();
    spin64_release(&wm_lock, fl);
}

/* --- windows ------------------------------------------------------------------ */

static wm64_window_t* get(int win) {
    if (win < 0 || win >= WM64_MAX_WINDOWS || !windows[win].used) return 0;
    return &windows[win];
}

static void free_window(wm64_window_t* win) {
    for (uint64_t i = 0; i < win->nframes; i++) pmm64_free_frame(win->frames[i]);
    win->nframes = 0;
    win->used    = 0;
}

int wmdev64_create(const struct wm64_create* req) {
    uint32_t w, h;
    uint64_t bytes, npages, fl;
    int slot;
    wm64_window_t* win;

    if (!req) return -14;
    w = req->w;
    h = req->h;
    if (!w || !h || w > WM64_MAX_W || h > WM64_MAX_H) return -22;

    bytes  = (uint64_t)w * h * 4;
    bytes  = (bytes + PAGE64_SIZE - 1) & ~(uint64_t)(PAGE64_SIZE - 1);
    npages = bytes / PAGE64_SIZE;
    if (npages > WM64_MAX_FRAMES) return -12;

    fl = spin64_acquire(&wm_lock);
    for (slot = 0; slot < WM64_MAX_WINDOWS; slot++)
        if (!windows[slot].used) break;
    if (slot == WM64_MAX_WINDOWS) { spin64_release(&wm_lock, fl); return -24; }
    win = &windows[slot];
    win->used = 1;
    win->nframes = 0;
    for (uint64_t i = 0; i < npages; i++) {
        uint64_t f = pmm64_alloc_frame();
        if (!f) { free_window(win); spin64_release(&wm_lock, fl); return -12; }
        kmemset(phys64_to_virt(f), 0, PAGE64_SIZE);
        win->frames[win->nframes++] = f;
    }
    win->refs   = 1;
    win->pid    = proc64_current_pid();
    win->shown  = 1;
    win->x = win->y = 0;
    win->w      = w;
    win->h      = h;
    win->stride = w;
    win->cap_h  = h;
    win->bytes  = bytes;
    win->id     = 0;
    kstrlcpy(win->title, req->title, WM64_DEV_TITLE_MAX);
    win->damages = 0;
    win->pixels = 0;
    stack_insert(slot, nstack);
    if (nwindows++ == 0) compose_all();      /* the desktop appears */
    else compose_window(win);
    say_window("opened", slot, win);
    spin64_release(&wm_lock, fl);
    return slot;
}

int wmdev64_listen(void) {
    uint64_t fl = spin64_acquire(&wm_lock);
    for (int i = 0; i < WM64_MAX_LISTENERS; i++) {
        if (listeners[i].used) continue;
        listeners[i].used = 1;
        listeners[i].refs = 1;
        listeners[i].pid = proc64_current_pid();
        inbox_for(listeners[i].pid, 1);
        spin64_release(&wm_lock, fl);
        return WM64_LISTENER_BASE + i;
    }
    spin64_release(&wm_lock, fl);
    return -24;
}

void wmdev64_close(int h) {
    uint64_t fl = spin64_acquire(&wm_lock);
    if (h >= WM64_LISTENER_BASE && h < WM64_LISTENER_BASE + WM64_MAX_LISTENERS) {
        wm64_listener_t* l = &listeners[h - WM64_LISTENER_BASE];
        if (l->used && --l->refs <= 0) {
            l->used = 0;
            inbox_release_if_unused(l->pid);
        }
    } else if (h >= 0 && h < WM64_MAX_WINDOWS && windows[h].used) {
        wm64_window_t* w = &windows[h];
        if (--w->refs <= 0) {
            int pid = w->pid;
            say_window("closed", h, w);
            w->shown = 0;
            stack_remove(h);
            if (ptr_grab == h) ptr_grab = -1;
            compose_window(w);
            free_window(w);
            nwindows--;
            inbox_release_if_unused(pid);
        }
    }
    spin64_release(&wm_lock, fl);
}

void wmdev64_ref(int h) {
    uint64_t fl = spin64_acquire(&wm_lock);
    if (h >= WM64_LISTENER_BASE && h < WM64_LISTENER_BASE + WM64_MAX_LISTENERS) {
        if (listeners[h - WM64_LISTENER_BASE].used) listeners[h - WM64_LISTENER_BASE].refs++;
    } else if (h >= 0 && h < WM64_MAX_WINDOWS && windows[h].used) {
        windows[h].refs++;
    }
    spin64_release(&wm_lock, fl);
}

int wmdev64_getinfo(int win, struct wm64_info* out) {
    wm64_window_t* w = get(win);
    if (!out) return -14;
    if (!w) return -9;
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
    out->x = w->x; out->y = w->y;
    out->w = (int32_t)w->w;
    out->h = (int32_t)w->h;
    return 0;
}

int wmdev64_title(int win, const char* title) {
    wm64_window_t* w = get(win);
    if (!title) return -14;
    if (!w) return -9;
    kstrlcpy(w->title, title, WM64_DEV_TITLE_MAX);
    return 0;
}

int wmdev64_damage(int win, const struct wm64_rect* r) {
    wm64_window_t* w;
    struct wm64_rect box;
    uint64_t fl;

    if (!r) return -14;
    if (!fb64_ready()) return -19;
    fl = spin64_acquire(&wm_lock);
    if (!(w = get(win))) { spin64_release(&wm_lock, fl); return -9; }
    box = *r;
    w->damages++;
    if (box.x < 0) { box.w += box.x; box.x = 0; }
    if (box.y < 0) { box.h += box.y; box.y = 0; }
    if (box.x + box.w > (int32_t)w->w) box.w = (int32_t)w->w - box.x;
    if (box.y + box.h > (int32_t)w->h) box.h = (int32_t)w->h - box.y;
    if (w->shown && box.w > 0 && box.h > 0)
        compose(w->x + box.x, w->y + box.y, box.w, box.h);
    route_input();
    spin64_release(&wm_lock, fl);
    return 0;
}

/* Where it is and how much of it shows. The size is clamped to the
 * buffer: a driver that wants a bigger window makes a new one. */
int wmdev64_move(int win, const struct wm64_rect* r) {
    wm64_window_t* w;
    int32_t ox, oy, ow, oh;
    uint64_t fl;

    if (!r) return -14;
    fl = spin64_acquire(&wm_lock);
    if (!(w = get(win))) { spin64_release(&wm_lock, fl); return -9; }
    ox = w->x; oy = w->y; ow = (int32_t)w->w; oh = (int32_t)w->h;
    w->x = r->x;
    w->y = r->y;
    if (r->w > 0) w->w = (uint32_t)r->w > w->stride ? w->stride : (uint32_t)r->w;
    if (r->h > 0) w->h = (uint32_t)r->h > w->cap_h ? w->cap_h : (uint32_t)r->h;
    if (w->shown && (ox != w->x || oy != w->y || ow != (int32_t)w->w || oh != (int32_t)w->h)) {
        compose(ox, oy, ow, oh);
        compose_window(w);
    }
    spin64_release(&wm_lock, fl);
    return 0;
}

int wmdev64_show(int win, int shown) {
    wm64_window_t* w;
    uint64_t fl = spin64_acquire(&wm_lock);
    if (!(w = get(win))) { spin64_release(&wm_lock, fl); return -9; }
    shown = shown != 0;
    if (w->shown != shown) {
        w->shown = shown;
        if (!shown && ptr_grab == win) ptr_grab = -1;
        compose_window(w);
    }
    spin64_release(&wm_lock, fl);
    return 0;
}

int wmdev64_stack(int win, const struct wm64_stack* s) {
    wm64_window_t* w;
    int at, before;
    uint64_t fl;

    if (!s) return -14;
    fl = spin64_acquire(&wm_lock);
    if (!(w = get(win))) { spin64_release(&wm_lock, fl); return -9; }
    before = stack_index(win);
    stack_remove(win);
    at = nstack;                                      /* the top */
    if (s->mode == WM64_STACK_BOTTOM) at = 0;
    else if (s->mode == WM64_STACK_BELOW && s->other) {
        for (int k = 0; k < nstack; k++)
            if (windows[stack[k]].id == s->other) { at = k; break; }
    }
    stack_insert(win, at);
    if (w->shown && stack_index(win) != before) compose_window(w);
    spin64_release(&wm_lock, fl);
    return 0;
}

int wmdev64_setid(int win, uint64_t id) {
    wm64_window_t* w = get(win);
    if (!w) return -9;
    w->id = id;
    return 0;
}

/* --- input, as the driver sees it ------------------------------------------- */

static int handle_valid(int h) {
    if (h >= WM64_LISTENER_BASE && h < WM64_LISTENER_BASE + WM64_MAX_LISTENERS)
        return listeners[h - WM64_LISTENER_BASE].used;
    return h >= 0 && h < WM64_MAX_WINDOWS && windows[h].used;
}

static int handle_pid(int h) {
    if (h >= WM64_LISTENER_BASE && h < WM64_LISTENER_BASE + WM64_MAX_LISTENERS)
        return listeners[h - WM64_LISTENER_BASE].used ? listeners[h - WM64_LISTENER_BASE].pid : -1;
    if (h >= 0 && h < WM64_MAX_WINDOWS && windows[h].used) return windows[h].pid;
    return -1;
}

int wmdev64_poll(int h, struct wm64_event* out) {
    wm64_inbox_t* in;
    int pid, got = 0;
    uint64_t fl;

    if (!out) return -14;
    fl = spin64_acquire(&wm_lock);
    if (!handle_valid(h)) { spin64_release(&wm_lock, fl); return -9; }
    pid = handle_pid(h);
    route_input();
    in = inbox_for(pid, 0);
    if (in && in->n) {
        *out = in->ev[in->head];
        in->head = (in->head + 1) % WM64_INBOX_EVENTS;
        in->n--;
        got = 1;
    } else {
        out->type = WM64_EV_NONE;
        out->a = out->b = out->c = out->d = 0;
    }
    spin64_release(&wm_lock, fl);
    return got;
}

int wmdev64_readable(int h) {
    wm64_inbox_t* in;
    int r = 0;
    uint64_t fl = spin64_acquire(&wm_lock);
    route_input();
    if (handle_valid(h) && (in = inbox_for(handle_pid(h), 0)) && in->n) r = 1;
    spin64_release(&wm_lock, fl);
    return r;
}

/* As if from the hardware: through the PS/2 decoder, so a test of
 * routing is also a test of the path a real mouse takes. It is
 * Linux's uinput in miniature, and only as safe as uinput: whoever can
 * open /dev/wm can type. */
int wmdev64_inject(const struct wm64_inject* in) {
    uint64_t fl;
    if (!in) return -14;
    fl = spin64_acquire(&wm_lock);
    route_input();                       /* so the pointer is where it is */
    if (in->type == WM64_INJECT_POINTER) {
        int32_t dx = in->a - ptr_x, dy = in->b - ptr_y;
        uint8_t buttons = (uint8_t)(in->c & 7);
        int first = 1;
        while (first || dx || dy) {
            int32_t sx = dx > 255 ? 255 : dx < -255 ? -255 : dx;
            int32_t sy = dy > 255 ? 255 : dy < -255 ? -255 : dy;
            int32_t py = -sy;            /* the mouse counts upwards */
            uint8_t flags = 0x08 | buttons;
            if (sx < 0) flags |= 0x10;
            if (py < 0) flags |= 0x20;
            input64_feed_mouse_byte(flags);
            input64_feed_mouse_byte((uint8_t)(sx & 0xFF));
            input64_feed_mouse_byte((uint8_t)(py & 0xFF));
            dx -= sx; dy -= sy;
            first = 0;
        }
    } else if (in->type == WM64_INJECT_KEY) {
        int sc = input64_scancode_for(in->a);
        if (sc < 0) { spin64_release(&wm_lock, fl); return -22; }
        if (sc & 0x100) input64_feed_scancode(0xE0);
        input64_feed_scancode((uint8_t)((sc & 0x7F) | (in->b ? 0 : 0x80)));
    } else {
        spin64_release(&wm_lock, fl);
        return -22;
    }
    route_input();
    spin64_release(&wm_lock, fl);
    return 0;
}

int wmdev64_peek(struct wm64_rect* io) {
    if (!io) return -14;
    if (!fb64_ready()) return -19;
    if (io->x < 0 || io->y < 0 || (uint32_t)io->x >= fb64_width() ||
        (uint32_t)io->y >= fb64_height()) return -22;
    io->w = (int32_t)(fb64_get_pixel((uint32_t)io->x, (uint32_t)io->y) & 0x00FFFFFFu);
    return 0;
}

void wmdev64_pointer(int32_t* x, int32_t* y) {
    uint64_t fl = spin64_acquire(&wm_lock);
    route_input();
    *x = ptr_x;
    *y = ptr_y;
    spin64_release(&wm_lock, fl);
}

const uint64_t* wmdev64_frames(int win, uint64_t* nframes, uint64_t* bytes) {
    wm64_window_t* w = get(win);
    if (!w) return 0;
    if (nframes) *nframes = w->nframes;
    if (bytes)   *bytes   = w->bytes;
    return w->frames;
}

void wmdev64_report(void) {
    for (int i = 0; i < WM64_MAX_WINDOWS; i++)
        if (windows[i].used) say_window("still open:", i, &windows[i]);
    serial64_puts("NOVARIS64: [wm] input: pointer ");
    serial64_putdec(routed_pointer);
    serial64_puts(" keys ");
    serial64_putdec(routed_keys);
    serial64_puts(" nowhere ");
    serial64_putdec(routed_nowhere);
    serial64_putc('\n');
}
