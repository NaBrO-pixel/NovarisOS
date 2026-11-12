/*
 * winenovaris.drv - the driver as it was for the 32-bit kernel's compositor.
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
 * The 32-bit kernel (kernel/wm.c, kernel/wmdev.c) has a window manager of
 * its own: it places windows itself, draws a title bar and frame around
 * each, and reports input relative to a window, one /dev/wm descriptor at
 * a time. The 64-bit kernel's compositor (Milestone 109) does none of
 * that - Wine places windows and draws frames, and input comes through a
 * listener in screen coordinates - and window.c is written for it.
 *
 * This is the earlier driver, unchanged but for the names, used when
 * /dev/wm does not accept WMIO_LISTEN: a kernel that cannot listen is
 * the one that frames windows itself.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "novarisdrv.h"

WINE_DEFAULT_DEBUG_CHANNEL(novarisdrv);

#define NOVARIS_LEGACY_WINDOWS 64  /* the whole of window.c's table */

static BOOL legacy_should_manage(HWND hwnd)
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

void legacy_window_pos_changed(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
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
    if (!legacy_should_manage(hwnd))
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
        novaris_window_create(data, width, height);
    else if (!visible && data->mapped) novaris_win_data_close(data);
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

BOOL legacy_get_window_style_masks(HWND hwnd, UINT style, UINT ex_style,
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

BOOL legacy_process_events(DWORD mask)
{
    BOOL got_event = FALSE;
    int i;

    for (i = 0; i < NOVARIS_LEGACY_WINDOWS; i++)
    {
        struct novaris_win_data *data;
        struct wm_event ev;
        HWND hwnd;
        int fd;

        pthread_mutex_lock(&novaris_mutex);
        data = novaris_win_data_slot(i);
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
