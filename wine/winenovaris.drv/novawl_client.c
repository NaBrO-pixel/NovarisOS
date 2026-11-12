/*
 * novawl - the compositor's client half (Milestone 111). See novawl.h.
 *
 * Copyright 2026 the Novaris project.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * The connection Vulkan is handed. It is the driver's own: the driver
 * binds the compositor on it and makes each window's wl_surface, and
 * Mesa's swapchain code then uses the same connection on queues of its
 * own. libwayland-client is the one Mesa already links, loaded by soname
 * so it is the same copy.
 */

#if 0
#pragma makedep unix
#endif

#include <dlfcn.h>
#include <pthread.h>
#include <string.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include "novawl.h"

#pragma GCC visibility push(hidden)

static struct wl_display *(*p_connect_to_fd)(int fd);
static int (*p_roundtrip)(struct wl_display *d);
static int (*p_flush)(struct wl_display *d);
static struct wl_proxy *(*p_marshal)(struct wl_proxy *p, uint32_t op, const struct wl_interface *i,
                                     uint32_t version, uint32_t flags, union wl_argument *args);
static int (*p_add_listener)(struct wl_proxy *p, void (**impl)(void), void *data);
static uint32_t (*p_get_id)(struct wl_proxy *p);
static uint32_t (*p_get_version)(struct wl_proxy *p);
static const struct wl_interface *i_registry, *i_compositor, *i_surface;

static struct wl_display *display;
static struct wl_proxy *compositor;
static uint32_t compositor_version;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    union wl_argument args[4];

    if (strcmp(interface, "wl_compositor") || compositor) return;
    compositor_version = version < 4 ? version : 4;
    args[0].u = name;
    args[1].s = i_compositor->name;
    args[2].u = compositor_version;
    args[3].n = 0;
    compositor = p_marshal((struct wl_proxy *)registry, 0, i_compositor, compositor_version, 0, args);
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static void (*registry_listener[])(void) =
{
    (void (*)(void))registry_global,
    (void (*)(void))registry_global_remove,
};

int novawl_start(novawl_commit_fn commit, void *ctx)
{
    void *lib;
    struct wl_proxy *registry;
    union wl_argument arg = { .n = 0 };
    int fd, ret = -1;

    pthread_mutex_lock(&lock);
    if (display) { ret = 0; goto done; }
    if (!(lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_GLOBAL))) goto done;
    p_connect_to_fd = dlsym(lib, "wl_display_connect_to_fd");
    p_roundtrip = dlsym(lib, "wl_display_roundtrip");
    p_flush = dlsym(lib, "wl_display_flush");
    p_marshal = dlsym(lib, "wl_proxy_marshal_array_flags");
    p_add_listener = dlsym(lib, "wl_proxy_add_listener");
    p_get_id = dlsym(lib, "wl_proxy_get_id");
    p_get_version = dlsym(lib, "wl_proxy_get_version");
    i_registry = dlsym(lib, "wl_registry_interface");
    i_compositor = dlsym(lib, "wl_compositor_interface");
    i_surface = dlsym(lib, "wl_surface_interface");
    if (!p_connect_to_fd || !p_roundtrip || !p_flush || !p_marshal || !p_add_listener ||
        !p_get_id || !p_get_version || !i_registry || !i_compositor || !i_surface)
        goto done;

    if ((fd = novawl_server_start(commit, ctx)) < 0) goto done;
    if (!(display = p_connect_to_fd(fd))) goto done;
    registry = p_marshal((struct wl_proxy *)display, 1, i_registry, 1, 0, &arg);  /* get_registry */
    if (!registry) goto done;
    p_add_listener(registry, registry_listener, NULL);
    p_roundtrip(display);
    if (compositor) ret = 0;

done:
    pthread_mutex_unlock(&lock);
    return ret;
}

void *novawl_display(void)
{
    return display;
}

void *novawl_surface_create(uint32_t *id)
{
    union wl_argument arg = { .n = 0 };
    struct wl_proxy *surface;

    if (!compositor) return NULL;
    pthread_mutex_lock(&lock);
    surface = p_marshal(compositor, 0, i_surface, compositor_version, 0, &arg);  /* create_surface */
    if (surface) *id = p_get_id(surface);
    p_flush(display);
    pthread_mutex_unlock(&lock);
    return surface;
}

void novawl_surface_destroy(void *surface)
{
    if (!surface) return;
    pthread_mutex_lock(&lock);
    p_marshal(surface, 0, NULL, p_get_version(surface), WL_MARSHAL_FLAG_DESTROY, NULL);
    p_flush(display);
    pthread_mutex_unlock(&lock);
}

#pragma GCC visibility pop
