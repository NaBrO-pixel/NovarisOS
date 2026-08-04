/*
 * winenovaris.drv - the Unix half's own header.
 *
 * Copyright 2026 the Novaris project.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_NOVARISDRV_H
#define __WINE_NOVARISDRV_H

#ifndef __WINE_CONFIG_H
#error You must include config.h to use this header
#endif

#include <pthread.h>
#include <stdint.h>

/* ntstatus.h before windef.h, and WIN32_NO_STATUS between them: the two
 * headers define the same STATUS_* names, and this is the order Wine's
 * own Unix-side sources use to get the NTSTATUS ones. */
#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "windef.h"
#include "winbase.h"
#include "ntgdi.h"
#include "ntuser.h"
#include "wine/gdi_driver.h"
#include "wine/debug.h"

#include "unixlib.h"

/* --- /dev/wm ------------------------------------------------------------
 *
 * The Novaris side of this driver, copied from the kernel's
 * include/wmdev.h rather than included from it: the two trees are built
 * by different compilers for different targets and share no include path.
 * The numbers are the interface, and they are small on purpose.
 */

#define WMDEV_PATH "/dev/wm"

#define WMIO_CREATE  0x5701u   /* struct wm_create*  */
#define WMIO_DAMAGE  0x5702u   /* struct wm_rect*    */
#define WMIO_POLL    0x5703u   /* struct wm_event*   */
#define WMIO_GETSIZE 0x5704u   /* struct wm_rect*    */
#define WMIO_TITLE   0x5705u   /* char[64]           */
#define WMIO_SCREEN  0x5706u   /* struct wm_rect*    */

#define WM_DEV_TITLE_MAX 64

struct wm_create
{
    unsigned int w, h;
    char title[WM_DEV_TITLE_MAX];
};

struct wm_rect
{
    int x, y, w, h;
};

/* Mouse event kinds, as the window manager routes them (kernel/wm.h). */
enum
{
    WM_MOUSE_MOVE,
    WM_MOUSE_DOWN,
    WM_MOUSE_UP,
    WM_MOUSE_DRAG,
    WM_MOUSE_WHEEL,
    WM_MOUSE_DOUBLE,
    WM_MOUSE_RIGHT_DOWN,
};

enum
{
    WM_EV_NONE = 0,
    WM_EV_MOUSE,        /* a: x, b: y, c: kind, d: buttons or wheel */
    WM_EV_KEY,          /* a: character, b: keycode, c: mods, d: pressed */
    WM_EV_CLOSE,
    WM_EV_RESIZE,       /* a: w, b: h */
};

struct wm_event
{
    unsigned int type;
    int a, b, c, d;
};

/* --- per-window state ---------------------------------------------------
 *
 * One of these per HWND that has a window on the Novaris desktop. Not
 * every HWND does: Wine creates message-only windows, tool windows and
 * windows that are never shown, and giving each of those a slot would
 * exhaust the sixteen the kernel has before the program's real window
 * appeared. See novaris_win_data_create() for what qualifies.
 */

struct novaris_win_data
{
    HWND    hwnd;
    int     fd;                 /* /dev/wm, one per window */
    void   *pixels;             /* the mapping; NULL until created */
    int     width, height;      /* the size the window was created at */
    struct window_rects rects;  /* what win32u last told us */
    BOOL    mapped;             /* the kernel has a window for this */
    BOOL    closing;            /* the user clicked the X */
};

extern pthread_mutex_t novaris_mutex;

struct novaris_win_data *novaris_win_data_get(HWND hwnd);
struct novaris_win_data *novaris_win_data_get_or_create(HWND hwnd);
void novaris_win_data_release(struct novaris_win_data *data);
void novaris_win_data_destroy(HWND hwnd);

/* The desktop's work area, cached at init: the screen as far as Wine is
 * concerned. */
extern struct wm_rect novaris_screen;

/* the driver entry points, split the way the sources are */
BOOL     NOVARIS_CreateWindowSurface(HWND hwnd, BOOL layered, const RECT *surface_rect,
                                     struct window_surface **surface);
void     NOVARIS_DestroyWindow(HWND hwnd);
BOOL     NOVARIS_ProcessEvents(DWORD mask);
UINT     NOVARIS_UpdateDisplayDevices(const struct gdi_device_manager *manager, void *param);
void     NOVARIS_WindowPosChanged(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                                  const struct window_rects *new_rects,
                                  struct window_surface *surface);
void     NOVARIS_SetWindowText(HWND hwnd, LPCWSTR text);

#endif /* __WINE_NOVARISDRV_H */
