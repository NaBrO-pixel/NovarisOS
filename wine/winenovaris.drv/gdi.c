/*
 * winenovaris.drv - drawing into another process's window.
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
 * Milestone 113. A window's surface belongs to the process that made the
 * window; a DC on that window from any other process gets no surface
 * from win32u and draws through the display driver's own device - and
 * this driver had none, so it drew nowhere. Under X11 the X server is
 * the device and every process reaches it, which is why nobody notices
 * there. Chromium notices: its GPU process paints the browser's window
 * with GetDC and BitBlt, so every Chromium-based program - Steam's
 * client, Electron applications, CEF launchers - showed a window that
 * stayed black.
 *
 * So the driver has a device of its own, the X11 driver's arrangement:
 * GetDC tells it, by an escape, which top-level window the DC draws in
 * and where in it; PutImage - what every blit from a bitmap arrives as -
 * writes the pixels straight into that window's /dev/wm buffer, which
 * the kernel shares with any process that names the window's id
 * (WMIO_ATTACH), and damages it. A window's own process draws through
 * its window surface as before; this is only reached when there is none.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "novarisdrv.h"

WINE_DEFAULT_DEBUG_CHANNEL(novarisdrv);

#define NOVARIS_ESCAPE 0x4e4f5641   /* 'NOVA' */

struct novaris_escape
{
    int     code;
    HWND    top;
    RECT    dc_rect;     /* the DC's area within the top-level window */
};

struct novaris_physdev
{
    struct gdi_physdev dev;
    HWND     top;
    RECT     dc_rect;
    int      fd;          /* attached to the top-level's window, or -1 */
    unsigned int *pixels;
    size_t   bytes;
    int      width, height, stride;
};

static const struct gdi_dc_funcs novaris_dc_funcs;

static struct novaris_physdev *get_physdev(PHYSDEV dev)
{
    return CONTAINING_RECORD(dev, struct novaris_physdev, dev);
}

static void detach(struct novaris_physdev *p)
{
    if (p->pixels) munmap(p->pixels, p->bytes);
    if (p->fd >= 0) novaris_wm_close(p->fd);
    p->pixels = NULL;
    p->fd = -1;
}

/* The window's pixels, named by its HWND, which its owner gave the kernel
 * as the window's id. */
static BOOL attach(struct novaris_physdev *p)
{
    UINT64 id = (UINT64)(ULONG_PTR)p->top;
    struct wm_info info;

    if (p->pixels) return TRUE;
    if (!p->top || (p->fd = novaris_wm_open()) < 0) return FALSE;
    if (novaris_wm_ioctl(p->fd, WMIO_ATTACH, &id) || novaris_wm_ioctl(p->fd, WMIO_GETINFO, &info) ||
        (p->pixels = mmap(NULL, info.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, p->fd, 0)) == MAP_FAILED)
    {
        TRACE("no window %p to draw into\n", p->top);
        p->pixels = NULL;
        detach(p);
        return FALSE;
    }
    p->bytes = info.bytes;
    p->width = info.w;
    p->height = info.h;
    p->stride = info.stride;
    return TRUE;
}

static BOOL novaris_CreateDC(PHYSDEV *pdev, LPCWSTR device, LPCWSTR output, const DEVMODEW *devmode)
{
    struct novaris_physdev *p = calloc(1, sizeof(*p));

    if (!p) return FALSE;
    p->fd = -1;
    push_dc_driver(pdev, &p->dev, &novaris_dc_funcs);
    return TRUE;
}

static BOOL novaris_DeleteDC(PHYSDEV dev)
{
    struct novaris_physdev *p = get_physdev(dev);

    detach(p);
    free(p);
    return TRUE;
}

static INT novaris_ExtEscape(PHYSDEV dev, INT escape, INT in_count, const void *in_data,
                             INT out_count, void *out_data)
{
    struct novaris_physdev *p = get_physdev(dev);
    const struct novaris_escape *e = in_data;

    if (escape == NOVARIS_ESCAPE && in_count == sizeof(*e) && e->code == 1)
    {
        if (p->top != e->top) detach(p);
        p->top = e->top;
        p->dc_rect = e->dc_rect;
        return TRUE;
    }
    dev = GET_NEXT_PHYSDEV(dev, pExtEscape);
    return dev->funcs->pExtEscape(dev, escape, in_count, in_data, out_count, out_data);
}

static BOOL is_xrgb(const BITMAPINFO *info)
{
    if (info->bmiHeader.biPlanes != 1 || info->bmiHeader.biBitCount != 32) return FALSE;
    if (info->bmiHeader.biCompression == BI_RGB) return TRUE;
    if (info->bmiHeader.biCompression == BI_BITFIELDS)
    {
        const DWORD *masks = (const DWORD *)info->bmiColors;
        return masks[0] == 0xff0000 && masks[1] == 0xff00 && masks[2] == 0xff;
    }
    return FALSE;
}

static BOOL intersect(RECT *r, const RECT *a, const RECT *b)
{
    r->left = max(a->left, b->left);
    r->top = max(a->top, b->top);
    r->right = min(a->right, b->right);
    r->bottom = min(a->bottom, b->bottom);
    return r->left < r->right && r->top < r->bottom;
}

/* One rectangle of the image, in the DC's device coordinates, into the
 * window. */
static void put_rect(struct novaris_physdev *p, const BITMAPINFO *info, const BYTE *bits,
                     const struct bitblt_coords *src, const struct bitblt_coords *dst, RECT r, RECT *bounds)
{
    int stride = info->bmiHeader.biWidth * 4;
    int height = abs(info->bmiHeader.biHeight);

    if (!intersect(&r, &r, &dst->visrect)) return;
    for (int y = r.top; y < r.bottom; y++)
    {
        int sy = src->visrect.top + (y - dst->visrect.top);
        int wy = p->dc_rect.top + y;
        int sx = src->visrect.left + (r.left - dst->visrect.left);
        int wx = p->dc_rect.left + r.left;
        int w = r.right - r.left;
        const BYTE *row;

        if (wy < 0 || wy >= p->height || sy < 0 || sy >= height) continue;
        if (wx < 0) { sx -= wx; w += wx; wx = 0; }
        if (wx + w > p->width) w = p->width - wx;
        if (w <= 0) continue;
        row = bits + (size_t)(info->bmiHeader.biHeight < 0 ? sy : height - 1 - sy) * stride;
        memcpy(p->pixels + (size_t)wy * p->stride + wx, row + (size_t)sx * 4, (size_t)w * 4);
        bounds->left = min(bounds->left, wx);
        bounds->top = min(bounds->top, wy);
        bounds->right = max(bounds->right, wx + w);
        bounds->bottom = max(bounds->bottom, wy + 1);
    }
}

static DWORD novaris_PutImage(PHYSDEV dev, HRGN clip, BITMAPINFO *info, const struct gdi_image_bits *bits,
                              struct bitblt_coords *src, struct bitblt_coords *dst, DWORD rop)
{
    struct novaris_physdev *p = get_physdev(dev);
    RECT bounds = { INT_MAX, INT_MAX, INT_MIN, INT_MIN };
    struct wm_rect damage;

    if (!is_xrgb(info))
    {
        info->bmiHeader.biPlanes = 1;
        info->bmiHeader.biBitCount = 32;
        info->bmiHeader.biCompression = BI_RGB;
        info->bmiHeader.biClrUsed = 0;
        if (info->bmiHeader.biHeight > 0) info->bmiHeader.biHeight = -info->bmiHeader.biHeight;
        return ERROR_BAD_FORMAT;
    }
    if (!bits) return ERROR_SUCCESS;
    if (src->width != dst->width || src->height != dst->height) return ERROR_TRANSFORM_NOT_SUPPORTED;
    if (rop != SRCCOPY)
    {
        FIXME("rop %08x into another process's window\n", (UINT)rop);
        return ERROR_SUCCESS;
    }
    if (!attach(p)) return ERROR_SUCCESS;

    if (clip)
    {
        DWORD size = NtGdiGetRegionData(clip, 0, NULL);
        RGNDATA *data = size ? malloc(size) : NULL;
        if (data && NtGdiGetRegionData(clip, size, data))
        {
            const RECT *rects = (const RECT *)data->Buffer;
            for (DWORD i = 0; i < data->rdh.nCount; i++)
                put_rect(p, info, bits->ptr, src, dst, rects[i], &bounds);
        }
        free(data);
    }
    else put_rect(p, info, bits->ptr, src, dst, dst->visrect, &bounds);

    if (bounds.right > bounds.left)
    {
        damage.x = bounds.left;
        damage.y = bounds.top;
        damage.w = bounds.right - bounds.left;
        damage.h = bounds.bottom - bounds.top;
        novaris_wm_ioctl(p->fd, WMIO_DAMAGE, &damage);
    }
    return ERROR_SUCCESS;
}

static const struct gdi_dc_funcs novaris_dc_funcs =
{
    .pCreateDC = novaris_CreateDC,
    .pDeleteDC = novaris_DeleteDC,
    .pExtEscape = novaris_ExtEscape,
    .pPutImage = novaris_PutImage,
    .priority = GDI_PRIORITY_GRAPHICS_DRV,
};

const struct gdi_dc_funcs *novaris_get_dc_funcs(void)
{
    return &novaris_dc_funcs;
}

/**********************************************************************
 *      GetDC
 *
 * Which top-level window the DC draws in, and where in it - as the X11
 * driver tells its device which drawable. Every window DC passes here;
 * only one with no window surface ever draws through it.
 */
void NOVARIS_GetDC(HDC hdc, HWND hwnd, HWND top, const RECT *win_rect, const RECT *top_rect, DWORD flags)
{
    struct novaris_escape e;

    if (!novaris_compositor) return;
    e.code = 1;
    e.top = top;
    e.dc_rect.left = win_rect->left - top_rect->left;
    e.dc_rect.top = win_rect->top - top_rect->top;
    e.dc_rect.right = win_rect->right - top_rect->left;
    e.dc_rect.bottom = win_rect->bottom - top_rect->top;
    NtGdiExtEscape(hdc, NULL, 0, NOVARIS_ESCAPE, sizeof(e), (LPSTR)&e, 0, NULL);
}

void NOVARIS_ReleaseDC(HWND hwnd, HDC hdc)
{
    struct novaris_escape e;

    if (!novaris_compositor) return;
    memset(&e, 0, sizeof(e));
    e.code = 1;
    NtGdiExtEscape(hdc, NULL, 0, NOVARIS_ESCAPE, sizeof(e), (LPSTR)&e, 0, NULL);
}
