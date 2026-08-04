/*
 * winenovaris.drv - the Unix half: initialisation and the driver table.
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
 * What this driver is, and what it is not.
 *
 * win32u draws a window into a *window surface*: a plain 32-bit top-down
 * DIB that GDI renders into. A display driver's job is to get that DIB
 * onto a screen and to send input back. On X11 that means an X window and
 * XPutImage; on Wayland, a wl_surface and a shm pool. On Novaris it means
 * /dev/wm - the kernel hands out a window on its own desktop and the
 * pixels behind it, and the driver copies the surface into them.
 *
 * That is the whole driver. There is no OpenGL, no Vulkan, no clipboard,
 * no IME, no display-mode changing: those are all optional entry points
 * and win32u has sensible behaviour for a driver that does not fill them
 * in. What is *not* optional is the set below, and the reason each is
 * here is written where it is implemented.
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

pthread_mutex_t novaris_mutex = PTHREAD_MUTEX_INITIALIZER;

struct wm_rect novaris_screen = { 0, 0, 1024, 768 };

/**********************************************************************
 *      UpdateDisplayDevices
 *
 * Wine has to be told a screen exists before any window can be placed on
 * one - CreateWindowEx with CW_USEDEFAULT, every call to GetSystemMetrics
 * for SM_CXSCREEN, and the desktop window's own size all come from here.
 * A driver that implements nothing else and this badly gets a zero-sized
 * desktop and no visible windows.
 *
 * One GPU, one source, one monitor, one mode: Novaris has one
 * framebuffer, set by GRUB at boot, and no way to change it. Reporting a
 * list of modes we cannot switch to would be a lie a program could act
 * on.
 */
UINT NOVARIS_UpdateDisplayDevices(const struct gdi_device_manager *manager, void *param)
{
    DEVMODEW mode = {.dmSize = sizeof(mode)};
    struct gdi_monitor monitor = {0};
    struct pci_id pci_id = {0};
    UINT dpi = NtUserGetSystemDpiForProcess(NULL);

    TRACE("reporting one %dx%d monitor\n", novaris_screen.w, novaris_screen.h);

    manager->add_gpu("Novaris framebuffer", &pci_id, NULL, param);
    manager->add_source("Novaris", DISPLAY_DEVICE_ATTACHED_TO_DESKTOP |
                        DISPLAY_DEVICE_PRIMARY_DEVICE, dpi, param);

    SetRect(&monitor.rc_monitor, 0, 0, novaris_screen.w, novaris_screen.h);
    /* The work area is the monitor: the desktop's taskbar is Novaris's
     * own, outside anything Wine puts on the screen, and the kernel has
     * already excluded it from what WMIO_SCREEN reports. */
    monitor.rc_work = monitor.rc_monitor;
    manager->add_monitor(&monitor, param);

    mode.dmFields = DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH |
                    DM_PELSHEIGHT | DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY | DM_POSITION;
    mode.dmDisplayOrientation = DMDO_DEFAULT;
    mode.dmBitsPerPel = 32;
    mode.dmPelsWidth = novaris_screen.w;
    mode.dmPelsHeight = novaris_screen.h;
    mode.dmDisplayFrequency = 60;
    manager->add_modes(&mode, 1, &mode, param);

    return STATUS_SUCCESS;
}

static const struct user_driver_funcs novarisdrv_funcs =
{
    .pCreateWindowSurface = NOVARIS_CreateWindowSurface,
    .pDestroyWindow = NOVARIS_DestroyWindow,
    .pGetWindowStyleMasks = NOVARIS_GetWindowStyleMasks,
    .pProcessEvents = NOVARIS_ProcessEvents,
    .pSetWindowText = NOVARIS_SetWindowText,
    .pUpdateDisplayDevices = NOVARIS_UpdateDisplayDevices,
    .pWindowPosChanged = NOVARIS_WindowPosChanged,
};

/* Asks the kernel how big the desktop's work area is. Doubles as the
 * test for whether this driver can run at all: no /dev/wm, no windows,
 * and saying so here is much better than saying it once per window. */
static BOOL query_screen(void)
{
    struct wm_rect r;
    int fd = open(WMDEV_PATH, O_RDWR);

    if (fd < 0)
    {
        WARN("no %s: this kernel has no window manager to draw on\n", WMDEV_PATH);
        return FALSE;
    }
    if (ioctl(fd, WMIO_SCREEN, &r) == 0 && r.w > 0 && r.h > 0) novaris_screen = r;
    else WARN("%s did not answer WMIO_SCREEN; assuming %dx%d\n", WMDEV_PATH,
              novaris_screen.w, novaris_screen.h);
    close(fd);

    TRACE("desktop work area %dx%d at (%d,%d)\n",
          novaris_screen.w, novaris_screen.h, novaris_screen.x, novaris_screen.y);
    return TRUE;
}

static NTSTATUS novarisdrv_unix_init(void *arg)
{
    if (!query_screen()) return STATUS_NOT_SUPPORTED;

    __wine_set_user_driver(&novarisdrv_funcs, WINE_GDI_DRIVER_VERSION);
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    novarisdrv_unix_init,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == novarisdrv_unix_func_count);

#ifdef _WIN64

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    novarisdrv_unix_init,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == novarisdrv_unix_func_count);

#endif /* _WIN64 */
