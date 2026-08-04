/*
 * winenovaris.drv - the PE half.
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
 * A Wine display driver is two halves. explorer.exe loads this one - a PE
 * module named `wine<name>.drv`, from the `Graphics` value under
 * HKCU\Software\Wine\Drivers - and all it does is bring in the Unix half,
 * which is where the driver actually lives. The split exists because the
 * Unix half may call libc and the host kernel; a PE module may not.
 *
 * On Novaris the host kernel is Novaris, and what the Unix half talks to
 * is /dev/wm (kernel/wmdev.c) - a device that hands a process a window on
 * the desktop and the pixels behind it.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "unixlib.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(novarisdrv);

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls(instance);
    if (__wine_init_unix_call()) return FALSE;

    /* The Unix half sets the user driver table from here. Failing this
     * call is how a driver declines - explorer then tries the next name
     * in the list, and the process ends up with the null driver and
     * "no driver could be loaded", which is the honest outcome when
     * /dev/wm is not there. */
    if (WINE_UNIX_CALL(novarisdrv_unix_func_init, NULL)) return FALSE;

    TRACE("winenovaris.drv loaded\n");
    return TRUE;
}
