/*
 * novawl - the compositor's server half (Milestone 111). See novawl.h.
 *
 * Copyright 2026 the Novaris project.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * What a Vulkan swapchain needs from Wayland and nothing more: a
 * wl_compositor that makes surfaces and regions, surfaces that take a
 * buffer, a frame callback and a commit, and wl_shm, which libwayland
 * implements itself. A commit hands the buffer's pixels to the driver,
 * releases the buffer at once - it has been copied - and answers every
 * frame callback, so a FIFO swapchain paces itself on the copy.
 *
 * libwayland-server is loaded at run time. The wrappers below have the
 * library's own names, because the protocol header's inline helpers call
 * those names; they are hidden, so nothing outside this driver can bind
 * to them, and the variadic entry points are not used at all - the array
 * forms are, which can be forwarded.
 */

#if 0
#pragma makedep unix
#endif

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "novawl.h"

#pragma GCC visibility push(hidden)

#define WRAP(ret, name, params, args) \
    static ret (*p_##name) params; \
    ret name params { return p_##name args; }
#define VWRAP(name, params, args) \
    static void (*p_##name) params; \
    void name params { p_##name args; }

WRAP(struct wl_display *, wl_display_create, (void), ())
WRAP(struct wl_event_loop *, wl_display_get_event_loop, (struct wl_display *d), (d))
WRAP(int, wl_display_init_shm, (struct wl_display *d), (d))
VWRAP(wl_display_flush_clients, (struct wl_display *d), (d))
WRAP(struct wl_client *, wl_client_create, (struct wl_display *d, int fd), (d, fd))
WRAP(struct wl_global *, wl_global_create,
     (struct wl_display *d, const struct wl_interface *i, int v, void *data, wl_global_bind_func_t bind),
     (d, i, v, data, bind))
WRAP(struct wl_resource *, wl_resource_create,
     (struct wl_client *c, const struct wl_interface *i, int v, uint32_t id), (c, i, v, id))
VWRAP(wl_resource_set_implementation,
      (struct wl_resource *r, const void *impl, void *data, wl_resource_destroy_func_t destroy),
      (r, impl, data, destroy))
WRAP(void *, wl_resource_get_user_data, (struct wl_resource *r), (r))
VWRAP(wl_resource_destroy, (struct wl_resource *r), (r))
VWRAP(wl_resource_post_event_array, (struct wl_resource *r, uint32_t op, union wl_argument *a), (r, op, a))
WRAP(uint32_t, wl_resource_get_id, (struct wl_resource *r), (r))
WRAP(int, wl_resource_get_version, (struct wl_resource *r), (r))
WRAP(struct wl_shm_buffer *, wl_shm_buffer_get, (struct wl_resource *r), (r))
WRAP(void *, wl_shm_buffer_get_data, (struct wl_shm_buffer *b), (b))
WRAP(int32_t, wl_shm_buffer_get_stride, (struct wl_shm_buffer *b), (b))
WRAP(int32_t, wl_shm_buffer_get_width, (struct wl_shm_buffer *b), (b))
WRAP(int32_t, wl_shm_buffer_get_height, (struct wl_shm_buffer *b), (b))
VWRAP(wl_shm_buffer_begin_access, (struct wl_shm_buffer *b), (b))
VWRAP(wl_shm_buffer_end_access, (struct wl_shm_buffer *b), (b))
WRAP(int, wl_event_loop_dispatch, (struct wl_event_loop *l, int timeout), (l, timeout))

static const struct wl_interface *i_compositor, *i_surface, *i_region, *i_callback;

static struct wl_display *display;
static novawl_commit_fn commit_fn;
static void *commit_ctx;

static int load(void)
{
    void *lib = dlopen("libwayland-server.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return -1;
#define LOAD(name) if (!(p_##name = dlsym(lib, #name))) return -1;
    LOAD(wl_display_create) LOAD(wl_display_get_event_loop) LOAD(wl_display_init_shm)
    LOAD(wl_display_flush_clients) LOAD(wl_client_create) LOAD(wl_global_create)
    LOAD(wl_resource_create) LOAD(wl_resource_set_implementation) LOAD(wl_resource_get_user_data)
    LOAD(wl_resource_destroy) LOAD(wl_resource_post_event_array) LOAD(wl_resource_get_id)
    LOAD(wl_resource_get_version) LOAD(wl_shm_buffer_get) LOAD(wl_shm_buffer_get_data)
    LOAD(wl_shm_buffer_get_stride) LOAD(wl_shm_buffer_get_width) LOAD(wl_shm_buffer_get_height)
    LOAD(wl_shm_buffer_begin_access) LOAD(wl_shm_buffer_end_access) LOAD(wl_event_loop_dispatch)
#undef LOAD
    if (!(i_compositor = dlsym(lib, "wl_compositor_interface")) ||
        !(i_surface = dlsym(lib, "wl_surface_interface")) ||
        !(i_region = dlsym(lib, "wl_region_interface")) ||
        !(i_callback = dlsym(lib, "wl_callback_interface")))
        return -1;
    return 0;
}

/* --- surfaces -------------------------------------------------------------- */

#define MAX_CALLBACKS 16

struct surface
{
    struct wl_resource *resource;
    struct wl_resource *buffer;     /* attached, not yet committed */
    int attached;
    struct wl_resource *callbacks[MAX_CALLBACKS];
    int ncallbacks;
};

static void callback_gone(struct wl_resource *resource)
{
    struct surface *s = wl_resource_get_user_data(resource);
    if (!s) return;
    for (int i = 0; i < s->ncallbacks; i++)
        if (s->callbacks[i] == resource)
        {
            s->callbacks[i] = s->callbacks[--s->ncallbacks];
            break;
        }
}

static void surface_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static void surface_gone(struct wl_resource *resource)
{
    struct surface *s = wl_resource_get_user_data(resource);
    /* Callbacks outlive nothing they belong to: let them forget it. */
    for (int i = 0; i < s->ncallbacks; i++)
        wl_resource_set_implementation(s->callbacks[i], NULL, NULL, NULL);
    free(s);
}

static void surface_attach(struct wl_client *client, struct wl_resource *resource,
                           struct wl_resource *buffer, int32_t x, int32_t y)
{
    struct surface *s = wl_resource_get_user_data(resource);
    s->buffer = buffer;
    s->attached = 1;
}

static void surface_damage(struct wl_client *client, struct wl_resource *resource,
                           int32_t x, int32_t y, int32_t w, int32_t h)
{
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    struct surface *s = wl_resource_get_user_data(resource);
    struct wl_resource *cb = wl_resource_create(client, i_callback, 1, id);

    if (!cb) return;
    if (s->ncallbacks == MAX_CALLBACKS)
    {
        union wl_argument done = { .u = 0 };
        wl_resource_post_event_array(cb, 0, &done);
        wl_resource_destroy(cb);
        return;
    }
    wl_resource_set_implementation(cb, NULL, s, callback_gone);
    s->callbacks[s->ncallbacks++] = cb;
}

static void surface_region(struct wl_client *client, struct wl_resource *resource,
                           struct wl_resource *region)
{
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* The whole point: the frame goes to the driver, the buffer goes back to
 * the client, and the client is told it may draw the next one. */
static void surface_commit(struct wl_client *client, struct wl_resource *resource)
{
    struct surface *s = wl_resource_get_user_data(resource);
    union wl_argument time = { .u = now_ms() };

    if (s->attached && s->buffer)
    {
        struct wl_shm_buffer *shm = wl_shm_buffer_get(s->buffer);
        if (shm)
        {
            wl_shm_buffer_begin_access(shm);
            commit_fn(commit_ctx, wl_resource_get_id(resource), wl_shm_buffer_get_data(shm),
                      wl_shm_buffer_get_width(shm), wl_shm_buffer_get_height(shm),
                      wl_shm_buffer_get_stride(shm));
            wl_shm_buffer_end_access(shm);
        }
        wl_resource_post_event_array(s->buffer, 0, NULL);       /* wl_buffer.release */
    }
    s->buffer = NULL;
    s->attached = 0;

    while (s->ncallbacks)
    {
        struct wl_resource *cb = s->callbacks[--s->ncallbacks];
        wl_resource_post_event_array(cb, 0, &time);              /* wl_callback.done */
        wl_resource_set_implementation(cb, NULL, NULL, NULL);
        wl_resource_destroy(cb);
    }
}

static void surface_transform(struct wl_client *client, struct wl_resource *resource, int32_t v)
{
}

static void surface_offset(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y)
{
}

static const struct wl_surface_interface surface_impl =
{
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_region,
    .set_input_region = surface_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_transform,
    .set_buffer_scale = surface_transform,
    .damage_buffer = surface_damage,
    .offset = surface_offset,
};

/* --- regions: accepted and ignored -------------------------------------------- */

static void region_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static void region_rect(struct wl_client *client, struct wl_resource *resource,
                        int32_t x, int32_t y, int32_t w, int32_t h)
{
}

static const struct wl_region_interface region_impl =
{
    .destroy = region_destroy,
    .add = region_rect,
    .subtract = region_rect,
};

/* --- the compositor ---------------------------------------------------------- */

static void compositor_create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    struct surface *s = calloc(1, sizeof(*s));

    if (!s) return;
    if (!(s->resource = wl_resource_create(client, i_surface, wl_resource_get_version(resource), id)))
    {
        free(s);
        return;
    }
    wl_resource_set_implementation(s->resource, &surface_impl, s, surface_gone);
}

static void compositor_create_region(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(client, i_region, 1, id);
    if (r) wl_resource_set_implementation(r, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl =
{
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(client, i_compositor, version < 5 ? version : 5, id);
    if (r) wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}

static void *server_thread(void *arg)
{
    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    for (;;)
    {
        wl_event_loop_dispatch(loop, -1);
        wl_display_flush_clients(display);
    }
    return NULL;
}

int novawl_server_start(novawl_commit_fn commit, void *ctx)
{
    pthread_t thread;
    int sv[2];

    if (load()) return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) return -1;
    commit_fn = commit;
    commit_ctx = ctx;
    if (!(display = wl_display_create()) || wl_display_init_shm(display) ||
        !wl_global_create(display, i_compositor, 5, NULL, compositor_bind) ||
        !wl_client_create(display, sv[0]) ||
        pthread_create(&thread, NULL, server_thread, NULL))
    {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    pthread_detach(thread);
    return sv[1];
}

#pragma GCC visibility pop
