/*
 * winenovaris.drv - the PE/Unix boundary.
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

#ifndef __WINE_NOVARISDRV_UNIXLIB_H
#define __WINE_NOVARISDRV_UNIXLIB_H

#include <stdarg.h>
#include "winternl.h"
#include "wine/unixlib.h"

/* One call, and there does not need to be a second. Everything this
 * driver does after initialisation is driven by win32u calling into the
 * Unix half through the user driver table, so the PE half exists only to
 * be the thing the loader can load. */
enum novarisdrv_unix_func
{
    novarisdrv_unix_func_init,
    novarisdrv_unix_func_count,
};

#endif /* __WINE_NOVARISDRV_UNIXLIB_H */
