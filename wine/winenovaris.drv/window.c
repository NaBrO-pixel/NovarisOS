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
 *     plain top-down 32-bit DIB; GDI renders the whole window into it and
 *     calls flush() with the dirty rectangle. flush copies those rows
 *     into the window's /dev/wm mapping and damages it.
 *
 *   - **Somewhere to put it.** pWindowPosChanged is where win32u says
 *     where a window is and how big. The first time a window is visible
 *     and has a size, that becomes an ioctl(WMIO_CREATE) and the window
 *     appears on the Novaris desktop.
 *
 *   - **Input coming back.** pProcessEvents drains ioctl(WMIO_POLL) and
 *     turns each event into an INPUT for NtUserSendHardwareInput. That
 *     call is the same one X11 and Wayland make; from user32 upwards
 *     there is no difference between a click on a Novaris window and a
 *     click on an X11 one.
 *
 * The bookkeeping in between is a fixed table of sixteen windows, because
 * sixteen is what kernel/wmdev.c has. A driver that quietly failed on the
 * seventeenth would be worse than one that says so.
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

WINE_DEFAULT_DEBUG_CHANNEL(novarisdrv);

#define NOVARIS_MAX_WINDOWS 16

static struct novaris_win_data win_data[NOVARIS_MAX_WINDOWS];

/* --- the window table ---------------------------------------------------
 *
 * Guarded by one mutex rather than one per window: everything here is a
 * handful of integers and an ioctl, and a driver whose locking is more
 * complicated than the thing it protects is a driver with a deadlock in
 * it somewhere.
 */

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

/* Gives the kernel's slot back. Closing the descriptor is the whole of
 * it: /dev/wm frees the window when the last reference to the descriptor
 * goes, so there is nothing to tear down in the right order. */
static void win_data_close(struct novaris_win_data *data)
{
    if (data->pixels)
    {
        /* The mapping's own length, not the window's: since the window
         * resizes, `width`*`height` is no longer what was mapped. */
        munmap(data->pixels, data->bytes);
        data->pixels = NULL;
    }
    if (data->fd >= 0)
    {
        close(data->fd);
        data->fd = -1;
    }
    data->mapped = FALSE;
    data->width = data->height = 0;
    data->stride = 0;
    data->bytes = 0;
}

void novaris_win_data_destroy(HWND hwnd)
{
    struct novaris_win_data *data;

    if (!(data = novaris_win_data_get(hwnd))) return;
    win_data_close(data);
    data->hwnd = 0;
    novaris_win_data_release(data);
}

/* --- which windows get one ----------------------------------------------
 *
 * Not every HWND is a window somebody should see. Wine creates a desktop
 * window, a message-only window per thread, child controls, and tool
 * windows that are never meant to be managed - and the first version of
 * this driver gave every one of them a /dev/wm slot, which is why a
 * screenshot of Notepad had four empty black windows behind it.
 *
 * The test is Wine's own, from nodrv_CreateWindow and X11's
 * is_window_managed: a window is ours if the desktop is its parent and it
 * looks like something a user would drag - a caption, a resizable frame,
 * or the explicit "this is an application window" flag. Everything else
 * is drawn by whoever owns it, into a surface nobody shows.
 */
static BOOL should_manage(HWND hwnd)
{
    UINT style, ex_style;
    HWND parent;

    if (!hwnd || hwnd == NtUserGetDesktopWindow()) return FALSE;

    parent = NtUserGetAncestor(hwnd, GA_PARENT);
    if (!parent) return FALSE;                              /* a desktop */
    if (parent == UlongToHandle(NtUserGetThreadInfo()->msg_window))
        return FALSE;                                       /* HWND_MESSAGE */
    if (parent != NtUserGetDesktopWindow()) return FALSE;   /* a child */

    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    if ((style & (WS_CHILD | WS_POPUP)) == WS_CHILD) return FALSE;
    if ((style & WS_CAPTION) == WS_CAPTION) return TRUE;
    if (style & WS_THICKFRAME) return TRUE;

    ex_style = NtUserGetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex_style & WS_EX_APPWINDOW) return TRUE;
    /* A popup with a system menu is a dialog in everything but name. */
    if ((style & WS_POPUP) && (style & WS_SYSMENU)) return TRUE;

    return FALSE;
}

/* --- getting a window onto the desktop ---------------------------------- */

/* The title, as a /dev/wm expects it: 7-bit, NUL-terminated, short.
 * Novaris draws its window titles in an 8x16 bitmap font that has no
 * glyphs above 126, so anything else would be drawn as nothing at all -
 * better to see "Notepad" with a character missing than a blank title
 * bar. */
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

/* Opens the window, at the size win32u says the *visible* rectangle is.
 * Called from pWindowPosChanged, which is the first moment both facts are
 * known: that the window is visible, and how big. */
static BOOL window_create(struct novaris_win_data *data, int width, int height)
{
    struct wm_create create;
    struct wm_info info;

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

    if (ioctl(data->fd, WMIO_CREATE, &create) != 0)
    {
        ERR("%s refused a %dx%d window\n", WMDEV_PATH, width, height);
        close(data->fd);
        data->fd = -1;
        return FALSE;
    }

    /* How big the buffer actually is, which is not how big the window is:
     * the kernel sizes it for the largest this window could be shown at so
     * that a resize never moves it. Map all of it, once - every later
     * resize is then a change of `width`/`height` and nothing else. */
    if (ioctl(data->fd, WMIO_GETINFO, &info) != 0 || !info.stride || !info.bytes)
    {
        ERR("%s did not answer WMIO_GETINFO\n", WMDEV_PATH);
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

    data->width = width;
    data->height = height;
    data->stride = info.stride;
    data->bytes = info.bytes;
    data->mapped = TRUE;

    TRACE("hwnd %p -> %dx%d window \"%s\" (buffer %ux%u)\n", data->hwnd, width, height,
          create.title, info.stride, info.cap_h);
    return TRUE;
}

/**********************************************************************
 *      WindowPosChanged
 *
 * Everything about where a window is arrives here: shown, hidden, moved,
 * resized, minimized. Novaris's window manager places and moves windows
 * itself - a program does not get to say where on the desktop it lands,
 * the same way it does not under a tiling window manager - so a move is
 * ignored and three things are acted on: a window becoming visible for
 * the first time, which opens it; a window becoming hidden, which closes
 * it; and a size change, which since Milestone 42 the window follows.
 *
 * A size change used to be deliberately ignored, because /dev/wm could
 * not resize a mapping under a process drawing into it. It no longer has
 * to: the kernel allocates the buffer for the largest the window can be
 * shown at and the client size is a sub-rectangle of it, so following a
 * resize is two stores here and nothing at all underneath.
 */
void NOVARIS_WindowPosChanged(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                              const struct window_rects *new_rects,
                              struct window_surface *surface)
{
    struct novaris_win_data *data;
    UINT style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    BOOL visible = (style & WS_VISIBLE) && !(style & WS_MINIMIZE);
    int width, height;

    /* A window that is not ours never gets a slot in the first place, but
     * one that *was* ours and has stopped being (a window can lose its
     * caption) has to give its slot back. */
    if (!should_manage(hwnd))
    {
        novaris_win_data_destroy(hwnd);   /* a no-op if it never had one */
        return;
    }

    if (!(data = novaris_win_data_get_or_create(hwnd))) return;

    data->rects = *new_rects;
    width = new_rects->visible.right - new_rects->visible.left;
    height = new_rects->visible.bottom - new_rects->visible.top;

    TRACE("hwnd %p %s style %08x visible %u %dx%d\n", hwnd,
          debugstr_window_rects(new_rects), style, visible, width, height);

    /* Clamped to the desktop, because a window may legitimately be bigger
     * than the screen and /dev/wm allocates what it is asked for. Wine
     * also resolves CW_USEDEFAULT against the screen it knows about, so a
     * request far outside it is a sign the display devices are not
     * reported yet rather than something to honour. */
    if (width > novaris_screen.w) width = novaris_screen.w;
    if (height > novaris_screen.h) height = novaris_screen.h;

    /* And big enough to be worth a frame. Explorer keeps a couple of
     * captioned windows whose client area is a pixel or two tall - real
     * windows, but not ones anybody is meant to look at, and a Novaris
     * frame around one is 32 pixels of title bar over nothing. Sixteen
     * square is well under any window a program shows on purpose. */
    if (visible && !data->mapped && !data->closing && width >= 16 && height >= 16)
        window_create(data, width, height);
    else if (!visible && data->mapped) win_data_close(data);
    else if (data->mapped && (width != data->width || height != data->height) &&
             width > 0 && height > 0)
    {
        /* A program resized its own window. Since Milestone 42 that costs
         * nothing on this side: the buffer was allocated for the largest
         * this window can be, so the mapping and its stride are unchanged
         * and only the part of it that is shown moves. Clamped to the
         * buffer, because the kernel will show at most that much.
         *
         * win32u recreates the window surface on a size change and will
         * flush the whole of it, so there is no repaint to ask for here. */
        if (width > data->stride) width = data->stride;
        data->width = width;
        data->height = height;
        TRACE("hwnd %p resized to %dx%d\n", hwnd, width, height);
    }

    novaris_win_data_release(data);
}

/**********************************************************************
 *      GetWindowStyleMasks
 *
 * "Which of these styles do you draw yourself?" Novaris's window manager
 * draws a title bar with the usual three buttons and a resizable frame
 * around every window it owns, so win32u must not draw them too - and
 * without this it did, leaving Notepad with its own blue caption sitting
 * inside the one Novaris had already drawn for it.
 *
 * Two things about the implementation, both learnt the hard way.
 *
 * It answers from the styles it is passed and calls nothing. This runs
 * inside win32u's window-position calculation - get_visible_rect(), with
 * the window locked - so a driver that calls back into win32u here
 * answers at the wrong time. X11's version calls nothing either, and that
 * is not an accident.
 *
 * And it depends on there being a font. win32u turns the mask into a
 * rectangle with NtUserAdjustWindowRect, whose caption height is
 * SM_CYCAPTION, which normalize_nonclientmetrics() computes as
 * `max(iCaptionHeight, 2 + tm.tmHeight)` for the caption font. With no
 * font engine that TEXTMETRICW is uninitialised, tm.tmHeight came out at
 * about six and three quarter million, and implementing this produced a
 * Notepad window 952 pixels wide and one pixel tall - invisible, with a
 * transcript that passed every assertion. That is why tools/install_wine.sh
 * ships Wine's fonts and FreeType: not for the text, though the text is
 * the visible half, but because the window frame is measured in glyphs.
 */
BOOL NOVARIS_GetWindowStyleMasks(HWND hwnd, UINT style, UINT ex_style,
                                 UINT *style_mask, UINT *ex_style_mask)
{
    (void)hwnd;
    *style_mask = *ex_style_mask = 0;

    if ((style & (WS_CHILD | WS_POPUP)) == WS_CHILD) return TRUE;

    if ((style & WS_CAPTION) == WS_CAPTION || (style & WS_THICKFRAME) ||
        (ex_style & WS_EX_APPWINDOW))
    {
        *style_mask = WS_CAPTION | WS_DLGFRAME | WS_THICKFRAME;
        *ex_style_mask = WS_EX_DLGMODALFRAME;
    }
    return TRUE;
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

/* --- the window surface -------------------------------------------------- */

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
    /* Novaris composites the window itself and clips to the frame, so
     * there is nothing here that the compositor is not already doing. */
}

/* GDI has finished drawing; `color_bits` is the surface's own DIB and
 * `dirty` is what changed, in surface coordinates. One row-by-row copy
 * into the window's mapping and one damage call.
 *
 * The copy is the price of not owning the surface's memory. It could be
 * avoided by passing the /dev/wm mapping to window_surface_create() as
 * the bitmap's bits, and that is worth doing - but only once resizing
 * works, because today the mapping's size is fixed at creation and a
 * surface is recreated on every size change.
 */
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
    int src_stride, y, x0, y0, w, h;

    if (!(data = novaris_win_data_get(surface->hwnd))) return FALSE;
    if (!data->mapped || !data->pixels)
    {
        novaris_win_data_release(data);
        return FALSE;
    }

    /* Surface coordinates are relative to `rect`, which is the visible
     * rectangle in screen coordinates; the window's pixels start at its
     * own top-left, so the dirty rectangle is already what we want. */
    x0 = max(dirty->left, 0);
    y0 = max(dirty->top, 0);
    w = min(dirty->right, data->width) - x0;
    h = min(dirty->bottom, data->height) - y0;
    if (w <= 0 || h <= 0)
    {
        novaris_win_data_release(data);
        return TRUE;
    }

    /* The destination steps by the *buffer's* stride, which is wider than
     * the window whenever the window is not at its largest. Stepping by
     * `data->width` here would shear the picture the first time anything
     * was resized. */
    src_stride = color_info->bmiHeader.biWidth * 4;
    src = (const char *)color_bits + (size_t)y0 * src_stride + (size_t)x0 * 4;
    dst = (char *)data->pixels + ((size_t)y0 * data->stride + x0) * 4;

    for (y = 0; y < h; y++)
    {
        memcpy(dst, src, (size_t)w * 4);
        src += src_stride;
        dst += (size_t)data->stride * 4;
    }

    damage.x = x0;
    damage.y = y0;
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

/* --- input --------------------------------------------------------------
 *
 * Every event becomes an INPUT and goes through NtUserSendHardwareInput,
 * which is the same call X11 and Wayland make. Above this line there is
 * no difference between a click on a Novaris window and a click on an X
 * one - that is the point of doing it this way rather than posting
 * messages directly.
 */

/* `origin` is the window's visible top-left in screen coordinates, taken
 * under the lock and passed in rather than read here: NtUserSendHardwareInput
 * can run a message loop, a message loop can call back into
 * pProcessEvents, and this driver's mutex is not recursive. Nothing below
 * this line touches shared state. */
static void send_mouse_input(HWND hwnd, POINT origin, const struct wm_event *ev)
{
    INPUT input = {.type = INPUT_MOUSE};

    input.mi.dx = origin.x + ev->a;
    input.mi.dy = origin.y + ev->b;

    switch (ev->c)
    {
    case WM_MOUSE_MOVE:
    case WM_MOUSE_DRAG:
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        break;
    case WM_MOUSE_DOWN:
    case WM_MOUSE_DOUBLE:
        input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        break;
    case WM_MOUSE_UP:
        input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        break;
    case WM_MOUSE_RIGHT_DOWN:
        input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        break;
    case WM_MOUSE_WHEEL:
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = ev->d * WHEEL_DELTA;
        break;
    default:
        return;
    }

    NtUserSendHardwareInput(hwnd, 0, &input, 0);
}

/* Novaris reports a key as a character plus a code; Windows wants a
 * virtual key and a scan code. The mapping below covers what a keyboard
 * produces that a program acts on - letters, digits, and the keys with
 * names. It is not a keyboard layout: a real one would be a KBDTABLES
 * through pKbdLayerDescriptor, which is the next thing this driver needs
 * and does not have.
 *
 * The Novaris key codes above 255 are kernel/keyboard.h's KEY_*, in the
 * order they are declared there. */
enum
{
    NOVARIS_KEY_ESCAPE = 256, NOVARIS_KEY_BACKSPACE, NOVARIS_KEY_TAB,
    NOVARIS_KEY_ENTER, NOVARIS_KEY_LEFT, NOVARIS_KEY_RIGHT,
    NOVARIS_KEY_UP, NOVARIS_KEY_DOWN, NOVARIS_KEY_HOME, NOVARIS_KEY_END,
    NOVARIS_KEY_PAGE_UP, NOVARIS_KEY_PAGE_DOWN, NOVARIS_KEY_DELETE,
    NOVARIS_KEY_INSERT,
};

static WORD vkey_for(const struct wm_event *ev)
{
    int ch = ev->a;

    switch (ev->b)
    {
    case NOVARIS_KEY_ESCAPE:    return VK_ESCAPE;
    case NOVARIS_KEY_BACKSPACE: return VK_BACK;
    case NOVARIS_KEY_TAB:       return VK_TAB;
    case NOVARIS_KEY_ENTER:     return VK_RETURN;
    case NOVARIS_KEY_LEFT:      return VK_LEFT;
    case NOVARIS_KEY_RIGHT:     return VK_RIGHT;
    case NOVARIS_KEY_UP:        return VK_UP;
    case NOVARIS_KEY_DOWN:      return VK_DOWN;
    case NOVARIS_KEY_HOME:      return VK_HOME;
    case NOVARIS_KEY_END:       return VK_END;
    case NOVARIS_KEY_PAGE_UP:   return VK_PRIOR;
    case NOVARIS_KEY_PAGE_DOWN: return VK_NEXT;
    case NOVARIS_KEY_DELETE:    return VK_DELETE;
    case NOVARIS_KEY_INSERT:    return VK_INSERT;
    default: break;
    }

    if (ch >= 'a' && ch <= 'z') return 'A' + (ch - 'a');
    if (ch >= 'A' && ch <= 'Z') return ch;
    if (ch >= '0' && ch <= '9') return ch;
    if (ch == ' ') return VK_SPACE;
    if (ch == '\n' || ch == '\r') return VK_RETURN;
    if (ch == '\b') return VK_BACK;
    if (ch == '\t') return VK_TAB;
    return 0;
}

static void send_key_input(HWND hwnd, const struct wm_event *ev)
{
    INPUT input = {.type = INPUT_KEYBOARD};
    WORD vkey = vkey_for(ev);

    if (!vkey) return;

    input.ki.wVk = vkey;
    input.ki.wScan = 0;
    input.ki.dwFlags = ev->d ? 0 : KEYEVENTF_KEYUP;

    NtUserSendHardwareInput(hwnd, 0, &input, 0);
}

/**********************************************************************
 *      ProcessEvents
 *
 * Called from win32u's message loop whenever a thread waits for a
 * message. Everything the driver has to say arrives here, and it must
 * not block: /dev/wm has no blocking read and the desktop is composited
 * by whichever process is asking, so a driver that waited would be
 * waiting for a frame it was itself supposed to draw.
 *
 * Returns whether an event was seen, which is what tells the caller to
 * check its message queue again before sleeping.
 */
BOOL NOVARIS_ProcessEvents(DWORD mask)
{
    BOOL got_event = FALSE;
    int i;

    for (i = 0; i < NOVARIS_MAX_WINDOWS; i++)
    {
        struct novaris_win_data *data;
        struct wm_event ev;
        HWND hwnd;
        int fd;

        pthread_mutex_lock(&novaris_mutex);
        data = &win_data[i];
        hwnd = data->hwnd;
        fd = data->mapped ? data->fd : -1;
        pthread_mutex_unlock(&novaris_mutex);

        if (!hwnd || fd < 0) continue;

        /* The lock is dropped across the ioctl and taken again to read
         * `data`, rather than held across it: an ioctl on /dev/wm
         * composites a frame, which can run arbitrary kernel drawing
         * code, and holding a driver-wide lock through that would
         * serialise every window in the process behind one. */
        while (ioctl(fd, WMIO_POLL, &ev) == 1)
        {
            POINT origin;

            got_event = TRUE;

            if (!(data = novaris_win_data_get(hwnd))) break;
            origin.x = data->rects.visible.left;
            origin.y = data->rects.visible.top;
            if (ev.type == WM_EV_CLOSE) data->closing = TRUE;
            novaris_win_data_release(data);

            /* Everything below runs with no lock held, because all of it
             * can re-enter this driver through win32u. */
            switch (ev.type)
            {
            case WM_EV_MOUSE:
                send_mouse_input(hwnd, origin, &ev);
                break;
            case WM_EV_KEY:
                send_key_input(hwnd, &ev);
                break;
            case WM_EV_CLOSE:
                TRACE("hwnd %p closed by the user\n", hwnd);
                NtUserPostMessage(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
                break;
            case WM_EV_RESIZE:
                /* The user dragged an edge on the Novaris desktop. Hand
                 * the new size up to win32u, which recreates the window
                 * surface at it, sends the program WM_SIZE, and comes
                 * back down through WindowPosChanged - where the only
                 * thing left to do is note the new width and height,
                 * because since Milestone 42 the buffer and the mapping
                 * are already the right ones.
                 *
                 * `ev.a`/`ev.b` are the kernel's client area, which is
                 * the rectangle this driver asked for at WMIO_CREATE and
                 * so is Wine's *visible* rectangle - the same quantity
                 * window_create() was given. The frame Novaris draws
                 * around it is the window manager's own and Wine does not
                 * count it (see GetWindowStyleMasks).
                 *
                 * The re-entry terminates: SetWindowPos reaches
                 * WindowPosChanged, which changes bookkeeping and sends
                 * nothing back to /dev/wm. */
                TRACE("hwnd %p resized to %dx%d by the user\n", hwnd, ev.a, ev.b);
                if (ev.a > 0 && ev.b > 0)
                    NtUserSetWindowPos(hwnd, 0, 0, 0, ev.a, ev.b,
                                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                break;
            default:
                break;
            }
        }
    }

    return got_event;
}
