/*
 * winenovaris.drv - Vulkan in a window.
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
 * Milestone 111. For a driver without Vulkan, win32u makes every window
 * surface a headless one, and a headless swapchain presents to nothing.
 * Mesa's lavapipe can present to Wayland through shared memory, so each
 * Vulkan window here is a wl_surface on novawl, the driver's own Wayland
 * compositor (novawl_*.c), and every frame the swapchain commits is
 * copied over the window's client area exactly as an OpenGL frame is
 * (window.c). VK_KHR_win32_surface becomes VK_KHR_wayland_surface on the
 * host, as it does for the Wayland driver.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "novarisdrv.h"
#include "novawl.h"

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(novarisdrv);

/* Which window each wl_surface draws into, by protocol id: the commit
 * callback runs on the compositor's thread and has nothing else to go
 * by, and must not call into win32u to find out. */
#define MAX_VK_SURFACES 64

static pthread_mutex_t surfaces_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t id; HWND hwnd; } surfaces[MAX_VK_SURFACES];

static void set_surface(uint32_t id, HWND hwnd)
{
    int i, free_slot = -1;

    pthread_mutex_lock(&surfaces_mutex);
    for (i = 0; i < MAX_VK_SURFACES; i++)
    {
        if (surfaces[i].id == id) break;
        if (!surfaces[i].id && free_slot < 0) free_slot = i;
    }
    if (i == MAX_VK_SURFACES) i = free_slot;
    if (i >= 0)
    {
        surfaces[i].id = hwnd ? id : 0;
        surfaces[i].hwnd = hwnd;
    }
    pthread_mutex_unlock(&surfaces_mutex);
}

static HWND surface_window(uint32_t id)
{
    HWND hwnd = 0;

    pthread_mutex_lock(&surfaces_mutex);
    for (int i = 0; i < MAX_VK_SURFACES; i++)
        if (surfaces[i].id == id) hwnd = surfaces[i].hwnd;
    pthread_mutex_unlock(&surfaces_mutex);
    return hwnd;
}

static void on_commit(void *ctx, uint32_t id, const void *pixels, int width, int height, int stride)
{
    HWND hwnd = surface_window(id);
    if (hwnd) novaris_present_client(hwnd, pixels, width, height, stride);
}

/* --- the client surface ---------------------------------------------------- */

struct novaris_vk_client
{
    struct client_surface base;
    void *wl_surface;
    uint32_t id;
};

static struct novaris_vk_client *vk_client_from(struct client_surface *base)
{
    return CONTAINING_RECORD(base, struct novaris_vk_client, base);
}

static void vk_client_destroy(struct client_surface *base)
{
    struct novaris_vk_client *client = vk_client_from(base);

    set_surface(client->id, 0);
    novawl_surface_destroy(client->wl_surface);
}

static void vk_client_detach(struct client_surface *base)
{
    struct novaris_vk_client *client = vk_client_from(base);

    set_surface(client->id, 0);
    if (base->hwnd) novaris_detach_client(base->hwnd);
}

static void vk_client_update(struct client_surface *base)
{
}

static void vk_client_present(struct client_surface *base, HDC hdc)
{
}

static const struct client_surface_funcs vk_client_funcs =
{
    .destroy = vk_client_destroy,
    .detach = vk_client_detach,
    .update = vk_client_update,
    .present = vk_client_present,
};

/* --- the driver -------------------------------------------------------------- */

static VkResult novaris_vulkan_surface_create(HWND hwnd, const struct vulkan_instance *instance,
                                              VkSurfaceKHR *handle, struct client_surface **client)
{
    VkWaylandSurfaceCreateInfoKHR info = { .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
    struct novaris_vk_client *surface;
    VkResult res;

    TRACE("hwnd %p\n", hwnd);

    if (!(surface = client_surface_create(sizeof(*surface), &vk_client_funcs, hwnd)))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!(surface->wl_surface = novawl_surface_create(&surface->id)))
    {
        client_surface_release(&surface->base);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    set_surface(surface->id, hwnd);

    info.display = novawl_display();
    info.surface = surface->wl_surface;
    if ((res = instance->p_vkCreateWaylandSurfaceKHR(instance->host.instance, &info, NULL, handle)))
    {
        ERR("no Wayland surface for hwnd %p: %d\n", hwnd, res);
        client_surface_release(&surface->base);
        return res;
    }

    *client = &surface->base;
    return VK_SUCCESS;
}

static VkBool32 novaris_get_physical_device_presentation_support(struct vulkan_physical_device *physical_device,
                                                                 uint32_t queue)
{
    struct vulkan_instance *instance = physical_device->instance;

    return instance->p_vkGetPhysicalDeviceWaylandPresentationSupportKHR(physical_device->host.physical_device,
                                                                        queue, novawl_display());
}

static void novaris_map_instance_extensions(struct vulkan_instance_extensions *extensions)
{
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_KHR_wayland_surface = 1;
    if (extensions->has_VK_KHR_wayland_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void novaris_map_device_extensions(struct vulkan_device_extensions *extensions)
{
    if (extensions->has_VK_KHR_external_memory_win32) extensions->has_VK_KHR_external_memory_fd = 1;
    if (extensions->has_VK_KHR_external_memory_fd) extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_semaphore_win32) extensions->has_VK_KHR_external_semaphore_fd = 1;
    if (extensions->has_VK_KHR_external_semaphore_fd) extensions->has_VK_KHR_external_semaphore_win32 = 1;
    if (extensions->has_VK_KHR_external_fence_win32) extensions->has_VK_KHR_external_fence_fd = 1;
    if (extensions->has_VK_KHR_external_fence_fd) extensions->has_VK_KHR_external_fence_win32 = 1;
}

static const struct vulkan_driver_funcs novaris_vulkan_funcs =
{
    .p_vulkan_surface_create = novaris_vulkan_surface_create,
    .p_get_physical_device_presentation_support = novaris_get_physical_device_presentation_support,
    .p_map_instance_extensions = novaris_map_instance_extensions,
    .p_map_device_extensions = novaris_map_device_extensions,
};

/**********************************************************************
 *      VulkanInit
 *
 * Only on the compositor: the 32-bit kernel's window manager has no use
 * for it. Without libwayland, win32u's headless surfaces remain - a
 * program still renders, and nothing shows.
 */
UINT NOVARIS_VulkanInit(UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs)
{
    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR("version mismatch, win32u wants %u but the driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION);
        return STATUS_INVALID_PARAMETER;
    }
    if (!novaris_compositor) return STATUS_NOT_IMPLEMENTED;
    if (novawl_start(on_commit, NULL))
    {
        WARN("no Wayland libraries: Vulkan windows will show nothing\n");
        return STATUS_NOT_IMPLEMENTED;
    }
    *driver_funcs = &novaris_vulkan_funcs;
    return STATUS_SUCCESS;
}
