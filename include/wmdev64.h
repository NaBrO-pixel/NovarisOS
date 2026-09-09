#ifndef WMDEV64_H
#define WMDEV64_H

#include <stdint.h>

/* /dev/wm for the 64-bit kernel.
 *
 * The 32-bit kernel has a compositor - wm.c, gfx.c, desktop.c, wmdev.c,
 * about 4,250 lines - and this kernel has none of it. What it has is the
 * two ends: fb64.c owns a linear framebuffer and input64.c owns the
 * keyboard and mouse. Everything between them is missing.
 *
 * That was invisible until Wine got far enough to want a window.
 * winenovaris.drv says what it needs in its own words - "no /dev/wm, no
 * windows" - and its unix half refuses to register as the display driver
 * unless one ioctl answers:
 *
 *     open("/dev/wm", O_RDWR);
 *     ioctl(fd, WMIO_SCREEN, &rect);      <- query_screen(), the gate
 *
 * Failing that gate is what `err:win:get_desktop_window failed to create
 * desktop window` means, and every symptom above it - explorer.exe
 * restarted forty-six times, a hundred and five live processes, 2GB of
 * RAM exhausted - is downstream of this file not existing.
 *
 * So this is the device, not the compositor. One surface per descriptor,
 * drawn straight to the framebuffer at the position it asked for, no
 * stacking, no decorations, no focus, no input. A window manager decides
 * which of several windows the user is looking at; there is exactly one
 * thing here that wants a window, it wants the whole screen, and it is
 * the desktop. The parts that need a compositor to be meaningful say so
 * where they are implemented rather than pretending.
 *
 * The contract is include/wmdev.h's, unchanged - the driver has its own
 * copy of these numbers in novarisdrv.h and neither side includes the
 * other, so they have to agree by being written down twice and not
 * drifting. */

#define WM64_DEV_TITLE_MAX 64
#define WM64_MAX_W 2048
#define WM64_MAX_H 2048

#define WMIO64_CREATE  0x5701u
#define WMIO64_DAMAGE  0x5702u
#define WMIO64_POLL    0x5703u
#define WMIO64_GETSIZE 0x5704u
#define WMIO64_TITLE   0x5705u
#define WMIO64_SCREEN  0x5706u
#define WMIO64_GETINFO 0x5707u

#define WM64_EV_NONE   0
#define WM64_EV_MOUSE  1
#define WM64_EV_KEY    2
#define WM64_EV_CLOSE  3
#define WM64_EV_RESIZE 4

struct wm64_create { uint32_t w, h; char title[WM64_DEV_TITLE_MAX]; };
struct wm64_rect   { int32_t x, y, w, h; };
struct wm64_info   { uint32_t w, h, stride, cap_h, bytes; };
struct wm64_event  { uint32_t type; int32_t a, b, c, d; };

/* Creates the node and marks it a device. Called once at boot; returns 0
 * if there is no framebuffer to draw on, in which case /dev/wm is not
 * created at all and the driver's open fails as it did before - which is
 * the honest answer on a machine with no screen. */
int  wmdev64_register(void);

/* A window, identified by the descriptor that created it: each open of
 * /dev/wm is one window, which is the driver's own model (win_data holds
 * one fd per window). -1 means "this descriptor has no window yet". */
int  wmdev64_create(const struct wm64_create* req);
void wmdev64_close(int win);

int  wmdev64_screen(struct wm64_rect* out);
int  wmdev64_getinfo(int win, struct wm64_info* out);
int  wmdev64_getsize(int win, struct wm64_rect* out);
int  wmdev64_title(int win, const char* title);
int  wmdev64_damage(int win, const struct wm64_rect* r);
int  wmdev64_poll(int win, struct wm64_event* out);

/* The frames behind a window's pixels, for mmap. */
const uint64_t* wmdev64_frames(int win, uint64_t* nframes, uint64_t* bytes);

#endif
