/*
 * winenovaris.drv - windows, surfaces and input.
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
 *
 *
 * Three things happen in this file, and they are the three things a
 * display driver is:
 *
 *   - **A surface to draw into.** pCreateWindowSurface hands win32u a
 *     plain top-down 32-bit DIB; GDI renders the whole window into it,
 *     frame and caption included, and calls flush() with the dirty
 *     rectangle. flush copies those rows into the window's /dev/wm
 *     mapping and damages it.
 *
 *   - **Somewhere to put it.** pWindowPosChanged says where a window is,
 *     how big, and where it is in the stacking order. Every shown
 *     top-level window becomes a /dev/wm window at exactly that place -
 *     Novaris's compositor (Milestone 109) puts windows where Wine says,
 *     the way a virtual desktop does, and stacks them in Wine's order.
 *
 *   - **Input coming back.** Each thread that pumps messages holds a
 *     /dev/wm listener and has given it to the wineserver as its queue
 *     descriptor, so a click wakes it. pProcessEvents drains the
 *     listener and turns each event into an INPUT for
 *     NtUserSendHardwareInput - the same call X11 and Wayland make. The
 *     events carry screen coordinates and the wineserver finds the
 *     window, so from user32 upwards a click on Novaris is a click.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "novarisdrv.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(novarisdrv);

#define NOVARIS_MAX_WINDOWS 64

static int thread_listener(void);

static struct novaris_win_data win_data[NOVARIS_MAX_WINDOWS];

/* --- the window table --------------------------------------------------- */

struct novaris_win_data *novaris_win_data_get(HWND hwnd)
{
    int i;

    if (!hwnd) return NULL;
    pthread_mutex_lock(&novaris_mutex);
    for (i = 0; i < NOVARIS_MAX_WINDOWS; i++)
        if (win_data[i].hwnd == hwnd) return &win_data[i];
    pthread_mutex_unlock(&novaris_mutex);
    return NULL;
}

struct novaris_win_data *novaris_win_data_get_or_create(HWND hwnd)
{
    struct novaris_win_data *data;
    int i;

    if ((data = novaris_win_data_get(hwnd))) return data;

    pthread_mutex_lock(&novaris_mutex);
    for (i = 0; i < NOVARIS_MAX_WINDOWS; i++)
    {
        if (win_data[i].hwnd) continue;
        memset(&win_data[i], 0, sizeof(win_data[i]));
        win_data[i].hwnd = hwnd;
        win_data[i].fd = -1;
        return &win_data[i];
    }
    pthread_mutex_unlock(&novaris_mutex);

    WARN("out of window slots (%u); %p gets no window\n", NOVARIS_MAX_WINDOWS, hwnd);
    return NULL;
}

void novaris_win_data_release(struct novaris_win_data *data)
{
    if (data) pthread_mutex_unlock(&novaris_mutex);
}

/* For legacy.c, which walks the table the way the driver used to. */
struct novaris_win_data *novaris_win_data_slot(int i)
{
    return &win_data[i];
}

/* Closing the descriptor is the whole of it: /dev/wm frees the window
 * when the last descriptor naming it goes. */
void novaris_win_data_close(struct novaris_win_data *data)
{
    if (data->pixels)
    {
        munmap(data->pixels, data->bytes);
        data->pixels = NULL;
    }
    if (data->fd >= 0)
    {
        close(data->fd);
        data->fd = -1;
    }
    data->mapped = FALSE;
    data->shown = FALSE;
    data->width = data->height = 0;
    data->stride = data->cap_h = 0;
    data->bytes = 0;
}

void novaris_win_data_destroy(HWND hwnd)
{
    struct novaris_win_data *data;

    if (!(data = novaris_win_data_get(hwnd))) return;
    novaris_win_data_close(data);
    data->hwnd = 0;
    novaris_win_data_release(data);
}

/* --- which windows get one ------------------------------------------------
 *
 * Every top-level window. Until Milestone 109 only windows with a caption
 * or a frame got one, because the kernel drew a frame around each and a
 * frame around a menu is wrong - which meant menus, tooltips, combo-box
 * drop-downs and every borderless main window (a browser's, a game
 * launcher's) were drawn by win32u into a surface nobody showed. Now the
 * kernel draws no frames and Wine draws its own, so the test is only
 * whether the window is a top-level one: a child is drawn into its
 * parent's surface already, and a message-only window is never seen.
 */
static BOOL should_manage(HWND hwnd)
{
    HWND parent;

    if (!hwnd || hwnd == NtUserGetDesktopWindow()) return FALSE;
    parent = NtUserGetAncestor(hwnd, GA_PARENT);
    if (!parent) return FALSE;                              /* a desktop */
    if (parent == UlongToHandle(NtUserGetThreadInfo()->msg_window))
        return FALSE;                                       /* HWND_MESSAGE */
    if (parent != NtUserGetDesktopWindow()) return FALSE;   /* a child */
    return TRUE;
}

/* --- getting a window onto the desktop ----------------------------------- */

static void window_title(HWND hwnd, char *out, unsigned int size)
{
    WCHAR buffer[WM_DEV_TITLE_MAX];
    unsigned int i, len;

    len = NtUserInternalGetWindowText(hwnd, buffer, ARRAY_SIZE(buffer));
    for (i = 0; i < len && i < size - 1; i++)
        out[i] = (buffer[i] >= 32 && buffer[i] < 127) ? (char)buffer[i] : '?';
    out[i] = 0;
    if (!out[0]) strcpy(out, "Wine");
}

BOOL novaris_window_create(struct novaris_win_data *data, int width, int height)
{
    struct wm_create create;
    struct wm_info info;
    UINT64 id = (UINT64)(ULONG_PTR)data->hwnd;

    if (width <= 0 || height <= 0) return FALSE;

    if ((data->fd = open(WMDEV_PATH, O_RDWR)) < 0)
    {
        ERR("cannot open %s\n", WMDEV_PATH);
        return FALSE;
    }

    memset(&create, 0, sizeof(create));
    create.w = width;
    create.h = height;
    window_title(data->hwnd, create.title, sizeof(create.title));

    if (ioctl(data->fd, WMIO_CREATE, &create) != 0 ||
        ioctl(data->fd, WMIO_GETINFO, &info) != 0 || !info.stride || !info.bytes)
    {
        ERR("%s refused a %dx%d window\n", WMDEV_PATH, width, height);
        close(data->fd);
        data->fd = -1;
        return FALSE;
    }

    data->pixels = mmap(NULL, info.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, data->fd, 0);
    if (data->pixels == MAP_FAILED)
    {
        ERR("cannot map the window's pixels\n");
        data->pixels = NULL;
        close(data->fd);
        data->fd = -1;
        return FALSE;
    }

    /* The HWND is the window's name in the kernel too: it is the same
     * number in every Wine process, so a window in one process can be
     * stacked directly below another process's. */
    ioctl(data->fd, WMIO_SETID, &id);

    data->width = width;
    data->height = height;
    data->stride = info.stride;
    data->cap_h = info.cap_h;
    data->bytes = info.bytes;
    data->mapped = TRUE;
    data->shown = TRUE;

    TRACE("hwnd %p -> %dx%d window \"%s\"\n", data->hwnd, width, height, create.title);
    return TRUE;
}

/* Where a window goes in the stack, found the way the X11 driver finds
 * it: the nearest window above it in Wine's own order that is on the
 * Novaris desktop too. With none, it is on top. Asked before the table
 * is locked, because it calls back into win32u. */
static void stacking_for(HWND hwnd, struct wm_stack *stack)
{
    HWND prev = NtUserGetWindowRelative(hwnd, GW_HWNDPREV);

    while (prev && (!(NtUserGetWindowLongW(prev, GWL_STYLE) & WS_VISIBLE) ||
                    !should_manage(prev)))
        prev = NtUserGetWindowRelative(prev, GW_HWNDPREV);

    memset(stack, 0, sizeof(*stack));
    if (prev)
    {
        stack->mode = WM_STACK_BELOW;
        stack->other = (UINT64)(ULONG_PTR)prev;
    }
    else stack->mode = WM_STACK_TOP;
}

/**********************************************************************
 *      WindowPosChanged
 *
 * Shown, hidden, moved, resized or restacked: all of it arrives here,
 * and all of it is passed on. The kernel's window is the window's
 * visible rectangle, at its screen position; it grows by being made
 * again at the new size, since its buffer cannot.
 */
void NOVARIS_WindowPosChanged(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                              const struct window_rects *new_rects,
                              struct window_surface *surface)
{
    struct novaris_win_data *data;
    UINT style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    BOOL visible = (style & WS_VISIBLE) && !(style & WS_MINIMIZE);
    BOOL restack = !(swp_flags & SWP_NOZORDER) || (swp_flags & SWP_SHOWWINDOW);
    struct wm_stack stack;
    struct wm_rect place;
    int width, height;

    if (!novaris_compositor)
    {
        legacy_window_pos_changed(hwnd, insert_after, owner_hint, swp_flags, new_rects, surface);
        return;
    }
    if (!should_manage(hwnd))
    {
        novaris_win_data_destroy(hwnd);   /* a no-op if it never had one */
        return;
    }

    width = new_rects->visible.right - new_rects->visible.left;
    height = new_rects->visible.bottom - new_rects->visible.top;
    if (width > 2048) width = 2048;
    if (height > 2048) height = 2048;

    if (visible)
    {
        stacking_for(hwnd, &stack);
        /* The thread that shows a window is the one input will be for,
         * and it must be watching before any arrives: win32u only calls
         * ProcessEvents once the wineserver has seen the listener
         * readable, so a listener made inside ProcessEvents is never
         * made at all. */
        if (novaris_compositor) thread_listener();
    }

    if (!(data = novaris_win_data_get_or_create(hwnd))) return;
    data->rects = *new_rects;

    TRACE("hwnd %p %s style %08x visible %u swp %04x\n", hwnd,
          debugstr_window_rects(new_rects), style, visible, swp_flags);

    if (!visible || width <= 0 || height <= 0)
    {
        if (data->mapped && data->shown)
        {
            int off = 0;
            ioctl(data->fd, WMIO_SHOW, &off);
            data->shown = FALSE;
        }
        novaris_win_data_release(data);
        return;
    }

    if (data->mapped && (width > data->stride || height > data->cap_h))
    {
        TRACE("hwnd %p grew to %dx%d past its %dx%d buffer\n", hwnd, width, height,
              data->stride, data->cap_h);
        novaris_win_data_close(data);
        restack = TRUE;
    }
    if (!data->mapped)
    {
        if (!novaris_window_create(data, width, height))
        {
            novaris_win_data_release(data);
            return;
        }
        restack = TRUE;
    }

    place.x = new_rects->visible.left;
    place.y = new_rects->visible.top;
    place.w = width;
    place.h = height;
    ioctl(data->fd, WMIO_MOVE, &place);
    data->width = width;
    data->height = height;

    if (restack) ioctl(data->fd, WMIO_STACK, &stack);
    if (!data->shown)
    {
        int on = 1;
        ioctl(data->fd, WMIO_SHOW, &on);
        data->shown = TRUE;
    }

    novaris_win_data_release(data);
}

/**********************************************************************
 *      ActivateWindow
 *
 * Wine does not raise a window it activates: on X11 and Wayland the
 * window manager does, and Wine reads the new order back. Here the
 * window manager is this driver, so a window that becomes the
 * foreground is put on top of Wine's own order - which comes back
 * through WindowPosChanged to the compositor like any other restack,
 * and takes the window's owned popups with it, as on Windows.
 */
void NOVARIS_ActivateWindow(HWND hwnd, HWND previous)
{
    if (!novaris_compositor || !should_manage(hwnd)) return;
    NtUserSetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/**********************************************************************
 *      GetWindowStyleMasks
 *
 * "Which of these styles do you draw yourself?" None, on the compositor:
 * win32u draws every frame and caption, and hit-tests them, as it does
 * on Windows. The 32-bit kernel draws its own (legacy.c).
 */
BOOL NOVARIS_GetWindowStyleMasks(HWND hwnd, UINT style, UINT ex_style,
                                 UINT *style_mask, UINT *ex_style_mask)
{
    if (!novaris_compositor)
        return legacy_get_window_style_masks(hwnd, style, ex_style, style_mask, ex_style_mask);
    return FALSE;
}

/**********************************************************************
 *      SetWindowText
 */
void NOVARIS_SetWindowText(HWND hwnd, LPCWSTR text)
{
    struct novaris_win_data *data;
    char title[WM_DEV_TITLE_MAX];

    if (!(data = novaris_win_data_get(hwnd))) return;
    if (data->mapped)
    {
        window_title(hwnd, title, sizeof(title));
        ioctl(data->fd, WMIO_TITLE, title);
    }
    novaris_win_data_release(data);
}

/**********************************************************************
 *      DestroyWindow
 */
void NOVARIS_DestroyWindow(HWND hwnd)
{
    TRACE("hwnd %p\n", hwnd);
    novaris_win_data_destroy(hwnd);
}

/* --- OpenGL's frames ------------------------------------------------------
 *
 * A swapped frame goes straight into the window's buffer, over the client
 * area, the way a compositor puts a client's buffer over its window: the
 * window surface below is GDI's, and its flushes leave that rectangle
 * alone, so a repainted caption cannot put an old client area back over
 * the newest frame. `bottom_up` is GL's row order.
 */
void novaris_present_client(HWND hwnd, const void *bottom_up, int width, int height)
{
    struct novaris_win_data *data;
    struct wm_rect damage;
    int cx, cy, w, h, y;

    if (!(data = novaris_win_data_get(hwnd))) return;
    if (!data->mapped || !data->pixels)
    {
        novaris_win_data_release(data);
        return;
    }
    cx = data->rects.client.left - data->rects.visible.left;
    cy = data->rects.client.top - data->rects.visible.top;
    w = min(width, data->width - cx);
    h = min(height, data->height - cy);
    if (cx >= 0 && cy >= 0 && w > 0 && h > 0)
    {
        SetRect(&data->gl_rect, cx, cy, cx + w, cy + h);
        data->gl_active = TRUE;
        for (y = 0; y < h; y++)
            memcpy((char *)data->pixels + ((size_t)(cy + y) * data->stride + cx) * 4,
                   (const char *)bottom_up + (size_t)(height - 1 - y) * width * 4, (size_t)w * 4);
        damage.x = cx;
        damage.y = cy;
        damage.w = w;
        damage.h = h;
        ioctl(data->fd, WMIO_DAMAGE, &damage);
    }
    novaris_win_data_release(data);
}

void novaris_detach_client(HWND hwnd)
{
    struct novaris_win_data *data;

    if (!(data = novaris_win_data_get(hwnd))) return;
    data->gl_active = FALSE;
    novaris_win_data_release(data);
}

/* One row of a surface flush, leaving out the part OpenGL owns. */
static void copy_row(struct novaris_win_data *data, char *dst, const char *src, int wx, int wy, int w)
{
    int l, r;

    if (!data->gl_active || wy < data->gl_rect.top || wy >= data->gl_rect.bottom)
    {
        memcpy(dst, src, (size_t)w * 4);
        return;
    }
    l = max(wx, data->gl_rect.left);
    r = min(wx + w, data->gl_rect.right);
    if (l >= r)
    {
        memcpy(dst, src, (size_t)w * 4);
        return;
    }
    if (l > wx) memcpy(dst, src, (size_t)(l - wx) * 4);
    if (wx + w > r) memcpy(dst + (size_t)(r - wx) * 4, src + (size_t)(r - wx) * 4,
                           (size_t)(wx + w - r) * 4);
}

/* --- the window surface ---------------------------------------------------- */

struct novaris_window_surface
{
    struct window_surface base;
    HWND hwnd;
};

static struct novaris_window_surface *surface_cast(struct window_surface *base)
{
    return (struct novaris_window_surface *)base;
}

static void novaris_surface_set_clip(struct window_surface *base, const RECT *rects, UINT count)
{
}

/* GDI has finished drawing. `rect` is where the surface is in the
 * window's visible rectangle - not always its top-left: win32u crops a
 * surface to the screen - and `dirty` is what changed, in the surface's
 * own coordinates. */
static BOOL novaris_surface_flush(struct window_surface *base, const RECT *rect, const RECT *dirty,
                                  const BITMAPINFO *color_info, const void *color_bits,
                                  BOOL shape_changed, const BITMAPINFO *shape_info,
                                  const void *shape_bits)
{
    struct novaris_window_surface *surface = surface_cast(base);
    struct novaris_win_data *data;
    struct wm_rect damage;
    const char *src;
    char *dst;
    int src_stride, y, x0, y0, w, h, wx, wy;

    if (!(data = novaris_win_data_get(surface->hwnd))) return FALSE;
    if (!data->mapped || !data->pixels)
    {
        novaris_win_data_release(data);
        return FALSE;
    }

    x0 = max(dirty->left, 0);
    y0 = max(dirty->top, 0);
    wx = rect->left + x0;
    wy = rect->top + y0;
    w = min(dirty->right - x0, data->width - wx);
    h = min(dirty->bottom - y0, data->height - wy);
    if (wx < 0 || wy < 0 || w <= 0 || h <= 0)
    {
        novaris_win_data_release(data);
        return TRUE;
    }

    src_stride = color_info->bmiHeader.biWidth * 4;
    src = (const char *)color_bits + (size_t)y0 * src_stride + (size_t)x0 * 4;
    dst = (char *)data->pixels + ((size_t)wy * data->stride + wx) * 4;

    for (y = 0; y < h; y++)
    {
        copy_row(data, dst, src, wx, wy + y, w);
        src += src_stride;
        dst += (size_t)data->stride * 4;
    }

    damage.x = wx;
    damage.y = wy;
    damage.w = w;
    damage.h = h;
    ioctl(data->fd, WMIO_DAMAGE, &damage);

    novaris_win_data_release(data);
    return TRUE;
}

static void novaris_surface_destroy(struct window_surface *base)
{
    TRACE("surface %p\n", base);
}

static const struct window_surface_funcs novaris_surface_funcs =
{
    novaris_surface_set_clip,
    novaris_surface_flush,
    novaris_surface_destroy,
};

/**********************************************************************
 *      CreateWindowSurface
 */
BOOL NOVARIS_CreateWindowSurface(HWND hwnd, BOOL layered, const RECT *surface_rect,
                                 struct window_surface **surface)
{
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    struct window_surface *previous;
    int width = surface_rect->right - surface_rect->left;
    int height = surface_rect->bottom - surface_rect->top;

    TRACE("hwnd %p layered %u rect %s\n", hwnd, layered, wine_dbgstr_rect(surface_rect));

    if ((previous = *surface) && previous->funcs == &novaris_surface_funcs) return TRUE;
    if (previous) window_surface_release(previous);
    *surface = NULL;

    memset(info, 0, sizeof(*info));
    info->bmiHeader.biSize        = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -height;   /* top-down, like the framebuffer */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = 32;
    info->bmiHeader.biSizeImage   = width * height * 4;
    info->bmiHeader.biCompression = BI_RGB;

    if ((*surface = window_surface_create(sizeof(struct novaris_window_surface),
                                          &novaris_surface_funcs, hwnd, surface_rect, info, 0)))
    {
        surface_cast(*surface)->hwnd = hwnd;
    }

    return TRUE;
}

/* --- input ------------------------------------------------------------------ */

/* Each thread that waits for messages has a listener: a /dev/wm
 * descriptor that is readable while this process has input. Handed to
 * the wineserver as the thread's queue descriptor - exactly what X11
 * does with its display connection - so a thread asleep in GetMessage
 * is woken by a click rather than by its next timer. Kept in the
 * thread's driver_data, biased by one. */
static int thread_listener(void)
{
    struct ntuser_thread_info *info = NtUserGetThreadInfo();
    HANDLE handle;
    int fd, dummy = 0;
    NTSTATUS status;

    if (info->driver_data) return (int)info->driver_data - 1;

    if ((fd = open(WMDEV_PATH, O_RDWR)) < 0) return -1;
    if (ioctl(fd, WMIO_LISTEN, &dummy) != 0)
    {
        close(fd);
        return -1;
    }
    info->driver_data = (UINT_PTR)fd + 1;

    if (!(status = wine_server_fd_to_handle(fd, GENERIC_READ | SYNCHRONIZE, 0, &handle)))
    {
        SERVER_START_REQ( set_queue_fd )
        {
            req->handle = wine_server_obj_handle( handle );
            status = wine_server_call( req );
        }
        SERVER_END_REQ;
        if (status) ERR("the wineserver would not take the input descriptor: %08x\n", status);
        NtClose(handle);
    }
    else ERR("no handle for the input descriptor: %08x\n", status);
    return fd;
}

void NOVARIS_ThreadDetach(void)
{
    struct ntuser_thread_info *info = NtUserGetThreadInfo();

    if (info->driver_data) close((int)info->driver_data - 1);
    info->driver_data = 0;
}

/* The buttons the last pointer event this process sent held, so a new
 * one can say which went down and which came up. Read and written only
 * by the thread holding input_mutex. */
static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
static int last_buttons;

static void send_pointer(const struct wm_event *ev)
{
    static const struct { int bit; DWORD down, up; } buttons[] =
    {
        { WM_BUTTON_LEFT,   MOUSEEVENTF_LEFTDOWN,   MOUSEEVENTF_LEFTUP },
        { WM_BUTTON_RIGHT,  MOUSEEVENTF_RIGHTDOWN,  MOUSEEVENTF_RIGHTUP },
        { WM_BUTTON_MIDDLE, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP },
    };
    INPUT input = {.type = INPUT_MOUSE};
    unsigned int i;

    /* Pixels, not the 0-65535 of SendInput: a driver's hardware input is
     * in screen coordinates, as X11's is. */
    input.mi.dx = ev->a;
    input.mi.dy = ev->b;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    NtUserSendHardwareInput(0, 0, &input, 0);

    for (i = 0; i < ARRAY_SIZE(buttons); i++)
    {
        if (!((ev->c ^ last_buttons) & buttons[i].bit)) continue;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                           ((ev->c & buttons[i].bit) ? buttons[i].down : buttons[i].up);
        NtUserSendHardwareInput(0, 0, &input, 0);
    }
    last_buttons = ev->c;

    if (ev->d)
    {
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = ev->d * WHEEL_DELTA;
        NtUserSendHardwareInput(0, 0, &input, 0);
    }
}

/* A Linux keycode is a set-1 scancode for the main block; the extended
 * keys are the same codes behind an 0xE0. The scancode is what Wine
 * wants: win32u turns it into a virtual key through the keyboard layout,
 * which is what makes shift, caps lock and WM_CHAR come out right. */
static int scancode_for(int key)
{
    static const struct { unsigned short key, scan; } extended[] =
    {
        {  96, 0x1c }, {  97, 0x1d }, {  98, 0x35 }, { 100, 0x38 }, { 102, 0x47 },
        { 103, 0x48 }, { 104, 0x49 }, { 105, 0x4b }, { 106, 0x4d }, { 107, 0x4f },
        { 108, 0x50 }, { 109, 0x51 }, { 110, 0x52 }, { 111, 0x53 }, { 125, 0x5b },
        { 126, 0x5c }, { 127, 0x5d },
    };
    unsigned int i;

    if (key > 0 && key < 0x59) return key;
    for (i = 0; i < ARRAY_SIZE(extended); i++)
        if (extended[i].key == key) return 0x100 | extended[i].scan;
    return -1;
}

static void send_key(const struct wm_event *ev)
{
    INPUT input = {.type = INPUT_KEYBOARD};
    int scan = scancode_for(ev->a);

    if (scan < 0) return;
    input.ki.wScan = (scan & 0x100) ? 0xe000 | (scan & 0xff) : scan;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (scan & 0x100) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!ev->b) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    NtUserSendHardwareInput(0, 0, &input, 0);
}

/**********************************************************************
 *      ProcessEvents
 *
 * Called by win32u only after the wineserver has seen this thread's
 * listener readable and flagged its queue (QS_DRIVER); the server stops
 * watching the listener until this says it drained. So it always says
 * so: the input is the process's, not the thread's, and another thread
 * may already have taken it - a FALSE here would leave the flag set and
 * every wait on this thread returning at once. It must not block, and
 * it may be re-entered - NtUserSendHardwareInput can run a message loop
 * - so one thread drains at a time and a second leaves it to the first.
 */
BOOL NOVARIS_ProcessEvents(DWORD mask)
{
    struct wm_event ev;
    int fd;

    if (!novaris_compositor) return legacy_process_events(mask);
    if ((fd = thread_listener()) < 0) return FALSE;
    if (pthread_mutex_trylock(&input_mutex)) return TRUE;

    while (ioctl(fd, WMIO_POLL, &ev) == 1)
    {
        switch (ev.type)
        {
        case WM_EV_POINTER:
            send_pointer(&ev);
            break;
        case WM_EV_KEYCODE:
            send_key(&ev);
            break;
        default:
            break;
        }
    }

    pthread_mutex_unlock(&input_mutex);
    return TRUE;
}
