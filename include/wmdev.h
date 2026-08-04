#ifndef WMDEV_H
#define WMDEV_H

#include <stdint.h>

/* wmdev - /dev/wm, a window on the Novaris desktop for a ring-3 process.
 *
 * Milestone 36. Everything the desktop has drawn until now has been drawn
 * by kernel code: an app is a paint callback compiled into the kernel
 * (kernel/app_*.c), and `ROADMAP.md` has said since Milestone 11 that the
 * window manager "is not reachable from CreateWindowEx". This is the
 * thing that makes it reachable - not by teaching the kernel about
 * Windows, but by letting *any* process ask for a window and hand over
 * pixels.
 *
 * Which is exactly the shape Wine's display driver needs. win32u loads
 * one Unix .so and asks it for a window surface to draw into
 * (pCreateWindowSurface), tells it where the window is (pWindowPosChanged)
 * and pumps input through it (pProcessEvents). Those three map onto the
 * three things below, and that is not a coincidence - it is why the
 * interface is shaped this way rather than around what a kernel app
 * happens to need.
 *
 * The protocol, in the order a program uses it:
 *
 *     fd = open("/dev/wm", O_RDWR);     a window slot of this process's own
 *     ioctl(fd, WMIO_CREATE, &create);  size and title; the window appears
 *     px = mmap(NULL, w*h*4, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
 *     ... draw into px, which is 32-bit 0x00RRGGBB, w pixels per row ...
 *     ioctl(fd, WMIO_DAMAGE, &rect);    "this part changed, show it"
 *     ioctl(fd, WMIO_POLL, &event);     input, or WM_EV_NONE
 *     close(fd);                        the window goes away
 *
 * The mmap is the interesting one and it needed no new code: the device's
 * node points `data` at the pixel buffer, so the ordinary MAP_SHARED path
 * (which hands over the frames a file's bytes live in - see Milestone 30)
 * gives the process the very memory the compositor reads. There is no
 * copy anywhere in a frame.
 */

#define WMDEV_PATH "/dev/wm"

/* ioctl requests. Deliberately small integers rather than Linux's _IOW
 * encoding: nothing here multiplexes over a real driver stack, and a
 * number you can read in a trace is worth more than one you can decode. */
#define WMIO_CREATE  0x5701u   /* struct wm_create*  */
#define WMIO_DAMAGE  0x5702u   /* struct wm_rect*    */
#define WMIO_POLL    0x5703u   /* struct wm_event*   */
#define WMIO_GETSIZE 0x5704u   /* struct wm_rect*, x/y zero */
#define WMIO_TITLE   0x5705u   /* char[WM_DEV_TITLE_MAX] */

#define WM_DEV_TITLE_MAX 64

/* The largest window a process may ask for. A window is w*h*4 bytes of
 * kernel heap that the process then maps, and the heap is 192MB with a
 * compositor already in it - so this is a limit that exists to be a
 * limit, not a guess at what anybody wants. */
#define WM_DEV_MAX_W 2048
#define WM_DEV_MAX_H 2048

struct wm_create {
    uint32_t w, h;
    char title[WM_DEV_TITLE_MAX];
};

struct wm_rect {
    int32_t x, y, w, h;
};

/* Input, as a queue the process drains. Coordinates are client-relative,
 * which is what the window's own pixels are indexed by. */
enum {
    WM_EV_NONE = 0,
    WM_EV_MOUSE,        /* a: x, b: y, c: kind (WM_MOUSE_*), d: buttons */
    WM_EV_KEY,          /* a: character or 0, b: keycode, c: modifiers */
    WM_EV_CLOSE,        /* the user closed the window */
    WM_EV_RESIZE,       /* a: w, b: h - the pixel buffer is unchanged, see
                         * the note in wmdev.c */
};

struct wm_event {
    uint32_t type;
    int32_t  a, b, c, d;
};

/* Registers /dev/wm. Kernel side; called once, from desktop_start(), for
 * the reason given there. Declared unconditionally because a declaration
 * costs a user program nothing and a second header would cost a reader
 * something. */
void wmdev_init(void);

#endif /* WMDEV_H */
