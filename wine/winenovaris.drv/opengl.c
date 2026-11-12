/*
 * winenovaris.drv - OpenGL in a window.
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
 * Milestone 110. win32u renders OpenGL through EGL whatever the driver,
 * but for a driver with no OpenGL of its own it draws a window's frames
 * into framebuffer objects nobody ever sees: SwapBuffers is a no-op
 * there by design. A driver that wants the frames on the screen supplies
 * the window surfaces itself, which is this file.
 *
 * There is no window system for EGL to draw into here, so the platform
 * is Mesa's surfaceless one, each GL window renders into an EGL pbuffer
 * the size of its client area, and SwapBuffers reads the frame back and
 * puts it straight into the window's /dev/wm buffer over the client area
 * (window.c), which the window surface's own flushes then leave alone.
 * That is one copy a frame, which next to llvmpipe drawing the frame is
 * nothing; a GPU would want the buffer shared instead.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "novarisdrv.h"
#include "wine/opengl_driver.h"

#ifdef SONAME_LIBEGL

WINE_DEFAULT_DEBUG_CHANNEL(novarisdrv);

static const struct egl_platform *egl;
static const struct opengl_funcs *funcs;

/* --- the client surface: where a swapped frame goes ------------------- */

struct novaris_client
{
    struct client_surface base;
    void *pixels;               /* the last frame, bottom row first, BGRA */
    int width, height;
};

static struct novaris_client *client_from(struct client_surface *base)
{
    return CONTAINING_RECORD(base, struct novaris_client, base);
}

static void client_destroy(struct client_surface *base)
{
    free(client_from(base)->pixels);
}

static void client_detach(struct client_surface *base)
{
    if (base->hwnd) novaris_detach_client(base->hwnd);
}

static void client_update(struct client_surface *base)
{
}

static void client_present(struct client_surface *base, HDC hdc)
{
}

static const struct client_surface_funcs client_funcs =
{
    .destroy = client_destroy,
    .detach = client_detach,
    .update = client_update,
    .present = client_present,
};

/* --- the drawable: a pbuffer the size of the client area -------------- */

struct novaris_gl_drawable
{
    struct opengl_drawable base;
    int width, height;
};

static const struct opengl_drawable_funcs drawable_funcs;

static struct novaris_gl_drawable *drawable_from(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct novaris_gl_drawable, base);
}

static EGLConfig config_for_format(int format)
{
    return egl->configs[(format - 1) % egl->config_count];
}

static void client_size(HWND hwnd, int *width, int *height)
{
    RECT rect = {0};

    NtUserGetClientRect(hwnd, &rect, NtUserGetDpiForWindow(hwnd));
    *width = max(rect.right - rect.left, 1);
    *height = max(rect.bottom - rect.top, 1);
}

static EGLSurface pbuffer_for(int format, int width, int height)
{
    const EGLint attribs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    return funcs->p_eglCreatePbufferSurface(egl->display, config_for_format(format), attribs);
}

static BOOL novaris_surface_create(HWND hwnd, int format, struct opengl_drawable **drawable)
{
    struct opengl_drawable *previous;
    struct novaris_client *client;
    struct novaris_gl_drawable *gl;

    TRACE("hwnd %p format %d\n", hwnd, format);

    if ((previous = *drawable) && previous->format == format) return TRUE;

    if (!(client = client_surface_create(sizeof(*client), &client_funcs, hwnd))) return FALSE;
    gl = opengl_drawable_create(sizeof(*gl), &drawable_funcs, format, &client->base);
    client_surface_release(&client->base);
    if (!gl) return FALSE;

    /* A pbuffer has one colour buffer, the back one; whatever the
     * program names, it means that one. */
    gl->base.buffer_map[0] = GL_BACK_LEFT;
    gl->base.buffer_map[1] = GL_BACK_RIGHT;
    gl->base.buffer_map[GL_FRONT - GL_FRONT_LEFT] = GL_BACK;
    gl->base.buffer_map[GL_FRONT_AND_BACK - GL_FRONT_LEFT] = GL_BACK;

    client_size(hwnd, &gl->width, &gl->height);
    if (!(gl->base.surface = pbuffer_for(format, gl->width, gl->height)))
    {
        ERR("no %dx%d pbuffer for format %d: %#x\n", gl->width, gl->height, format,
            funcs->p_eglGetError());
        opengl_drawable_release(&gl->base);
        return FALSE;
    }

    TRACE("hwnd %p: %dx%d pbuffer %p\n", hwnd, gl->width, gl->height, gl->base.surface);
    if (previous) opengl_drawable_release(previous);
    *drawable = &gl->base;
    return TRUE;
}

static void novaris_drawable_destroy(struct opengl_drawable *base)
{
}

static void novaris_drawable_flush(struct opengl_drawable *base, UINT flags)
{
    struct novaris_gl_drawable *gl = drawable_from(base);
    int width, height;

    if (!(flags & GL_FLUSH_UPDATED)) return;
    client_size(base->client->hwnd, &width, &height);
    if (width != gl->width || height != gl->height)
        FIXME("hwnd %p resized to %dx%d; its pbuffer stays %dx%d\n", base->client->hwnd,
              width, height, gl->width, gl->height);
}

/* What swap calls beyond OpenGL 1.1. win32u's table has the 1.1 entry
 * points from the start and the rest only once a program asks for them,
 * so these are looked up here - calling glBindBuffer through the table
 * was a jump to address 0 on every SwapBuffers, which Wine caught and
 * survived, and nothing reached the screen. */
static PFN_glBindBuffer p_glBindBuffer;
static PFN_glBindFramebuffer p_glBindFramebuffer;

static BOOL load_swap_funcs(void)
{
    if (!p_glBindBuffer) p_glBindBuffer = (PFN_glBindBuffer)funcs->p_eglGetProcAddress("glBindBuffer");
    if (!p_glBindFramebuffer)
        p_glBindFramebuffer = (PFN_glBindFramebuffer)funcs->p_eglGetProcAddress("glBindFramebuffer");
    return p_glBindBuffer && p_glBindFramebuffer;
}

/* The frame, read back from the pbuffer with every piece of state that
 * would redirect or reshape the read set to the default and put back. */
static BOOL novaris_drawable_swap(struct opengl_drawable *base)
{
    struct novaris_gl_drawable *gl = drawable_from(base);
    struct novaris_client *client = client_from(base->client);
    GLint read_fbo = 0, pack_buffer = 0, read_buffer = GL_BACK;
    GLint row_length = 0, skip_rows = 0, skip_pixels = 0, alignment = 4;
    size_t bytes = (size_t)gl->width * gl->height * 4;

    if (!load_swap_funcs())
    {
        ERR("no glBindBuffer or glBindFramebuffer from EGL\n");
        return FALSE;
    }
    if (!client->pixels || client->width != gl->width || client->height != gl->height)
    {
        free(client->pixels);
        if (!(client->pixels = malloc(bytes))) return FALSE;
        client->width = gl->width;
        client->height = gl->height;
    }

    funcs->p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    funcs->p_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    funcs->p_glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    funcs->p_glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
    funcs->p_glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
    funcs->p_glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
    funcs->p_glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);

    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    funcs->p_glReadBuffer(GL_BACK);
    funcs->p_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    funcs->p_glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    funcs->p_glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    funcs->p_glPixelStorei(GL_PACK_ALIGNMENT, 4);

    funcs->p_glReadPixels(0, 0, gl->width, gl->height, GL_BGRA, GL_UNSIGNED_BYTE, client->pixels);

    funcs->p_glPixelStorei(GL_PACK_ALIGNMENT, alignment);
    funcs->p_glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
    funcs->p_glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
    funcs->p_glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
    funcs->p_glReadBuffer(read_buffer);
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);

    novaris_present_client(base->client->hwnd,
                           (char *)client->pixels + (size_t)(gl->height - 1) * gl->width * 4,
                           gl->width, gl->height, -gl->width * 4);
    return TRUE;
}

static const struct opengl_drawable_funcs drawable_funcs =
{
    .destroy = novaris_drawable_destroy,
    .flush = novaris_drawable_flush,
    .swap = novaris_drawable_swap,
};

/* --- the platform -------------------------------------------------------- */

static void novaris_init_egl_platform(struct egl_platform *platform)
{
    platform->type = EGL_PLATFORM_SURFACELESS_MESA;
    platform->native_display = EGL_DEFAULT_DISPLAY;
    platform->force_pbuffer_formats = TRUE;
    egl = platform;
}

static struct opengl_driver_funcs novaris_driver_funcs =
{
    .p_init_egl_platform = novaris_init_egl_platform,
    .p_surface_create = novaris_surface_create,
};

/**********************************************************************
 *      OpenGLInit
 */
UINT NOVARIS_OpenGLInit(UINT version, const struct opengl_funcs *opengl_funcs,
                        const struct opengl_driver_funcs **driver_funcs)
{
    if (version != WINE_OPENGL_DRIVER_VERSION)
    {
        ERR("version mismatch, opengl32 wants %u but the driver has %u\n",
            version, WINE_OPENGL_DRIVER_VERSION);
        return STATUS_INVALID_PARAMETER;
    }
    if (!novaris_compositor || !opengl_funcs->egl_handle) return STATUS_NOT_SUPPORTED;
    funcs = opengl_funcs;

    /* Everything but where the frames go is win32u's own EGL code. */
    novaris_driver_funcs.p_get_proc_address = (*driver_funcs)->p_get_proc_address;
    novaris_driver_funcs.p_init_pixel_formats = (*driver_funcs)->p_init_pixel_formats;
    novaris_driver_funcs.p_describe_pixel_format = (*driver_funcs)->p_describe_pixel_format;
    novaris_driver_funcs.p_init_wgl_extensions = (*driver_funcs)->p_init_wgl_extensions;
    novaris_driver_funcs.p_context_create = (*driver_funcs)->p_context_create;
    novaris_driver_funcs.p_context_destroy = (*driver_funcs)->p_context_destroy;
    novaris_driver_funcs.p_make_current = (*driver_funcs)->p_make_current;
    novaris_driver_funcs.p_pbuffer_create = (*driver_funcs)->p_pbuffer_create;
    novaris_driver_funcs.p_pbuffer_updated = (*driver_funcs)->p_pbuffer_updated;
    novaris_driver_funcs.p_pbuffer_bind = (*driver_funcs)->p_pbuffer_bind;

    *driver_funcs = &novaris_driver_funcs;
    return STATUS_SUCCESS;
}

#else /* no EGL */

UINT NOVARIS_OpenGLInit(UINT version, const struct opengl_funcs *opengl_funcs,
                        const struct opengl_driver_funcs **driver_funcs)
{
    return STATUS_NOT_IMPLEMENTED;
}

#endif
