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
#include <stddef.h>     /* size_t, for the mapping length in win_data */
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
#define WMIO_GETINFO 0x5707u   /* struct wm_info*    */
#define WMIO_MOVE    0x5708u   /* struct wm_rect*: screen position, shown size */
#define WMIO_SHOW    0x5709u   /* int*               */
#define WMIO_STACK   0x570Au   /* struct wm_stack*   */
#define WMIO_SETID   0x570Bu   /* UINT64*: the HWND  */
#define WMIO_LISTEN  0x570Cu   /* int* (ignored)     */

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

/* The window's shown size and the buffer behind it: a row is `stride`
 * pixels. The kernel allocates the buffer at the size the window was
 * created; a window that grows past it is made again. */
struct wm_info
{
    unsigned int w, h;
    unsigned int stride;
    unsigned int cap_h;
    unsigned int bytes;
};

#define WM_STACK_TOP    0
#define WM_STACK_BOTTOM 1
#define WM_STACK_BELOW  2   /* directly below the window whose id is `other` */

struct wm_stack
{
    unsigned int mode, pad;
    UINT64 other;
};

/* Mouse event kinds, as the 32-bit kernel's window manager routes them
 * (kernel/wm.h). */
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
    WM_EV_MOUSE,        /* 32-bit kernel: a: x, b: y (window), c: kind, d: buttons or wheel */
    WM_EV_KEY,          /* 32-bit kernel: a: character, b: keycode, c: mods, d: pressed */
    WM_EV_CLOSE,
    WM_EV_RESIZE,       /* 32-bit kernel: a: w, b: h */
    WM_EV_POINTER,      /* a, b: screen position; c: buttons held; d: wheel */
    WM_EV_KEYCODE,      /* a: Linux keycode; b: pressed */
};

#define WM_BUTTON_LEFT   0x1
#define WM_BUTTON_RIGHT  0x2
#define WM_BUTTON_MIDDLE 0x4

struct wm_event
{
    unsigned int type;
    int a, b, c, d;
};

/* --- per-window state ---------------------------------------------------
 *
 * One of these per top-level HWND that is shown on the Novaris desktop -
 * every one, since Milestone 109, menus and tooltips included. Child
 * windows are drawn into their top-level window's surface by win32u and
 * need nothing here.
 */

struct novaris_win_data
{
    HWND    hwnd;
    int     fd;                 /* /dev/wm, one per window */
    void   *pixels;             /* the mapping; NULL until created */
    int     width, height;      /* shown now */
    int     stride, cap_h;      /* the buffer */
    size_t  bytes;              /* the mapping's length, for munmap */
    struct window_rects rects;  /* what win32u last told us */
    BOOL    mapped;             /* the kernel has a window for this */
    BOOL    shown;
    BOOL    closing;            /* 32-bit kernel: the user clicked its X */
    BOOL    gl_active;          /* OpenGL draws the client area (opengl.c) */
    RECT    gl_rect;            /* where, in the window's buffer */
};

extern pthread_mutex_t novaris_mutex;

struct novaris_win_data *novaris_win_data_get(HWND hwnd);
struct novaris_win_data *novaris_win_data_get_or_create(HWND hwnd);
void novaris_win_data_release(struct novaris_win_data *data);
void novaris_win_data_destroy(HWND hwnd);
struct novaris_win_data *novaris_win_data_slot(int i);
BOOL novaris_window_create(struct novaris_win_data *data, int width, int height);
void novaris_present_client(HWND hwnd, const void *bottom_up, int width, int height);
void novaris_detach_client(HWND hwnd);
void novaris_win_data_close(struct novaris_win_data *data);

/* Whether /dev/wm is the 64-bit kernel's compositor, which places windows
 * where Wine says and leaves their frames to Wine, or the 32-bit kernel's
 * window manager, which does both itself (legacy.c). */
extern BOOL novaris_compositor;

void legacy_window_pos_changed(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                               const struct window_rects *new_rects,
                               struct window_surface *surface);
BOOL legacy_get_window_style_masks(HWND hwnd, UINT style, UINT ex_style,
                                   UINT *style_mask, UINT *ex_style_mask);
BOOL legacy_process_events(DWORD mask);

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
void     NOVARIS_ThreadDetach(void);
void     NOVARIS_ActivateWindow(HWND hwnd, HWND previous);
struct opengl_funcs;
struct opengl_driver_funcs;
UINT     NOVARIS_OpenGLInit(UINT version, const struct opengl_funcs *opengl_funcs,
                            const struct opengl_driver_funcs **driver_funcs);
BOOL     NOVARIS_GetWindowStyleMasks(HWND hwnd, UINT style, UINT ex_style,
                                     UINT *style_mask, UINT *ex_style_mask);

#endif /* __WINE_NOVARISDRV_H */
