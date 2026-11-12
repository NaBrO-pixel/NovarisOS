#ifndef WMDEV64_H
#define WMDEV64_H

#include <stdint.h>

/* /dev/wm for the 64-bit kernel: a compositor (Milestone 109).
 *
 * Until Milestone 109 this was a surface and nothing more: every window
 * was copied to the framebuffer at the screen's top-left corner, one
 * pixel at a time, with no stacking, no cursor and no input - WMIO_POLL
 * always said nothing had happened. That was enough for one window that
 * filled the screen. It is not enough for a program with a menu, a
 * tooltip, a dialog in front of its main window, or a pointer.
 *
 * What it is now:
 *
 *   - **Windows where Wine puts them.** WMIO_MOVE says where a window's
 *     top-left pixel is on the screen and how much of its buffer is
 *     shown. Wine decides placement, exactly as it does on a virtual
 *     desktop; the kernel does not second-guess it.
 *   - **Stacked.** WMIO_STACK puts a window on top, at the bottom, or
 *     directly below another window named by the id it was given with
 *     WMIO_SETID - Wine's HWND, which is the same number in every Wine
 *     process, so one process can stack its window under another's.
 *   - **Composited.** A changed rectangle is redrawn from the stack,
 *     row by row, topmost window first: each pixel on the screen is
 *     written once, from the window that is in front of it, or from the
 *     desktop where no window is. The pointer is drawn over the top.
 *   - **Input routed.** The PS/2 mouse moves a pointer the kernel owns;
 *     each packet goes to the process whose window is under it, or to
 *     the one that has it grabbed while a button is held. Keys go to the
 *     process whose window is on top. Both arrive as events in absolute
 *     screen coordinates - the wineserver decides which window they are
 *     for, as it does for X11.
 *   - **Something to wait on.** WMIO_LISTEN turns a descriptor into one
 *     that poll(2) reports readable when its process has input. Wine
 *     hands it to the wineserver with set_queue_fd, so a thread asleep
 *     in GetMessage wakes for a click - the same arrangement as the X11
 *     driver's display connection.
 *
 * The contract is include/wmdev.h's numbers, extended; the driver has
 * its own copy in novarisdrv.h and the two are written down twice. */

#define WM64_DEV_TITLE_MAX 64
#define WM64_MAX_W 2048
#define WM64_MAX_H 2048

#define WMIO64_CREATE  0x5701u   /* struct wm64_create*: a window, shown at (0,0) */
#define WMIO64_DAMAGE  0x5702u   /* struct wm64_rect*, window coordinates */
#define WMIO64_POLL    0x5703u   /* struct wm64_event*; returns 1 for an event */
#define WMIO64_GETSIZE 0x5704u
#define WMIO64_TITLE   0x5705u
#define WMIO64_SCREEN  0x5706u
#define WMIO64_GETINFO 0x5707u
#define WMIO64_MOVE    0x5708u   /* struct wm64_rect*: screen position, shown size */
#define WMIO64_SHOW    0x5709u   /* int*: 0 hides, 1 shows */
#define WMIO64_STACK   0x570Au   /* struct wm64_stack* */
#define WMIO64_SETID   0x570Bu   /* uint64_t*: the caller's name for it */
#define WMIO64_LISTEN  0x570Cu   /* int* (ignored): become an input listener */
#define WMIO64_INJECT  0x570Eu   /* struct wm64_inject*: as if from the hardware */
#define WMIO64_PEEK    0x570Fu   /* struct wm64_rect*: x,y in; w = pixel out */

#define WM64_EV_NONE    0
#define WM64_EV_MOUSE   1        /* the 32-bit kernel's; not produced here */
#define WM64_EV_KEY     2        /* likewise */
#define WM64_EV_CLOSE   3
#define WM64_EV_RESIZE  4
#define WM64_EV_POINTER 5        /* a: x, b: y (screen), c: buttons held, d: wheel */
#define WM64_EV_KEYCODE 6        /* a: Linux keycode, b: 1 pressed / 0 released */

#define WM64_BUTTON_LEFT   0x1
#define WM64_BUTTON_RIGHT  0x2
#define WM64_BUTTON_MIDDLE 0x4

#define WM64_STACK_TOP    0
#define WM64_STACK_BOTTOM 1
#define WM64_STACK_BELOW  2      /* directly below the window whose id is `other` */

#define WM64_INJECT_POINTER 1    /* a, b: where to; c: buttons held */
#define WM64_INJECT_KEY     2    /* a: Linux keycode; b: pressed */

struct wm64_create { uint32_t w, h; char title[WM64_DEV_TITLE_MAX]; };
struct wm64_rect   { int32_t x, y, w, h; };
struct wm64_info   { uint32_t w, h, stride, cap_h, bytes; };
struct wm64_event  { uint32_t type; int32_t a, b, c, d; };
struct wm64_stack  { uint32_t mode, pad; uint64_t other; };
struct wm64_inject { uint32_t type; int32_t a, b, c; };

/* Creates the node and marks it a device. Returns 0 if there is no
 * framebuffer, in which case there is no /dev/wm. */
int  wmdev64_register(void);

/* Handles: what a /dev/wm descriptor names, kept in its `pos` biased by
 * one. Below WM64_MAX_WINDOWS a window; from there up, a listener. */
#define WM64_MAX_WINDOWS   64
#define WM64_LISTENER_BASE WM64_MAX_WINDOWS

int  wmdev64_create(const struct wm64_create* req);
int  wmdev64_listen(void);               /* a listener for the caller's process */
void wmdev64_close(int h);
void wmdev64_ref(int h);                 /* one more descriptor names it */

int  wmdev64_screen(struct wm64_rect* out);
int  wmdev64_getinfo(int win, struct wm64_info* out);
int  wmdev64_getsize(int win, struct wm64_rect* out);
int  wmdev64_title(int win, const char* title);
int  wmdev64_damage(int win, const struct wm64_rect* r);
int  wmdev64_move(int win, const struct wm64_rect* r);
int  wmdev64_show(int win, int shown);
int  wmdev64_stack(int win, const struct wm64_stack* s);
int  wmdev64_setid(int win, uint64_t id);

/* The next event for the process `h` belongs to: 1 and *out filled, or
 * 0 when there is none. */
int  wmdev64_poll(int h, struct wm64_event* out);
/* Whether a poll(2) on `h` would see input. */
int  wmdev64_readable(int h);

int  wmdev64_inject(const struct wm64_inject* in);
int  wmdev64_peek(struct wm64_rect* io);

/* From the timer: routes whatever the mouse and keyboard have queued, if
 * there is a window to route it to. */
void wmdev64_timer(void);

/* The frames behind a window's pixels, for mmap. */
const uint64_t* wmdev64_frames(int win, uint64_t* nframes, uint64_t* bytes);

/* For the test layer: where the pointer is. */
void wmdev64_pointer(int32_t* x, int32_t* y);

void wmdev64_report(void);       /* every open window, on the serial port */

#endif
