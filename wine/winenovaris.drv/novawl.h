/*
 * novawl - a Wayland compositor small enough to live inside a display
 * driver (Milestone 111).
 *
 * Copyright 2026 the Novaris project.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Mesa's software Vulkan (lavapipe) can present a swapchain to Wayland
 * through shared-memory buffers, and to nothing else that a driver could
 * read back: its headless surface shows nothing by design. So the driver
 * runs a Wayland server of its own, on a socket pair, offering exactly a
 * compositor, surfaces, regions and wl_shm - and every buffer a surface
 * commits comes back through `novawl_commit_fn` to be put on the screen.
 *
 * No Wine headers in here, so the same two files build into a plain
 * Linux test. libwayland-server and libwayland-client are loaded at run
 * time, so a build without them still links and simply has no Vulkan
 * windows.
 */

#ifndef NOVAWL_H
#define NOVAWL_H

#include <stdint.h>

/* A committed frame: top row first, `stride` bytes apart, XRGB/ARGB8888
 * (BGRA bytes). Called on the compositor's thread. */
typedef void (*novawl_commit_fn)(void *ctx, uint32_t surface_id, const void *pixels,
                                 int width, int height, int stride);

/* Starts the server and connects to it. 0 on success. Once per process. */
int   novawl_start(novawl_commit_fn commit, void *ctx);

/* The client side, for VkWaylandSurfaceCreateInfoKHR. */
void *novawl_display(void);
/* A new wl_surface and its protocol id, which is the id the commit
 * callback names it by. */
void *novawl_surface_create(uint32_t *id);
void  novawl_surface_destroy(void *surface);

/* novawl_server.c's half, for novawl_client.c. */
int   novawl_server_start(novawl_commit_fn commit, void *ctx);

#endif
