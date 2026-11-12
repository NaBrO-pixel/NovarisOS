/* vkwl64.c - a Vulkan swapchain that reaches a compositor (Milestone 111).
 *
 * The presentation path winenovaris.drv uses for Vulkan windows, without
 * Wine: lavapipe presents to a Wayland surface, the surface belongs to
 * novawl - the driver's own few-hundred-line compositor, linked in here
 * from wine/winenovaris.drv - and novawl hands each committed frame to a
 * callback. Two frames of two colours, so a compositor that kept showing
 * the first would fail. Run on the Linux host and on NovarisOS, and the
 * two must agree.
 *
 * Exits 112 when everything held.
 */

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <vulkan/vulkan.h>
#include "novawl.h"

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s vkwl: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) failures++;
}

#define W 64
#define H 48

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int frames;
static uint32_t last_pixel, last_w, last_h;

static void on_commit(void *ctx, uint32_t id, const void *pixels, int width, int height, int stride)
{
    pthread_mutex_lock(&lock);
    frames++;
    last_w = width;
    last_h = height;
    last_pixel = *(const uint32_t *)((const char *)pixels + (size_t)(height / 2) * stride + (width / 2) * 4);
    pthread_mutex_unlock(&lock);
}

static int wait_frames(int n)
{
    for (int i = 0; i < 500; i++) {
        struct timespec ts = { 0, 10000000 };
        int got;
        pthread_mutex_lock(&lock);
        got = frames;
        pthread_mutex_unlock(&lock);
        if (got >= n) return 1;
        nanosleep(&ts, NULL);
    }
    return 0;
}

int main(void)
{
    const char *inst_ext[] = { "VK_KHR_surface", "VK_KHR_wayland_surface" };
    const char *dev_ext[] = { "VK_KHR_swapchain" };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance inst;
    VkPhysicalDevice pd = VK_NULL_HANDLE, pds[8];
    uint32_t npd = 8, nimg = 8, sid = 0;
    VkPhysicalDeviceProperties props;
    VkSurfaceKHR surface;
    VkDevice dev;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkImage images[8];
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkBool32 supported = 0;
    void *wl_surface;

    setvbuf(stdout, NULL, _IONBF, 0);

    ok("the compositor starts and is connected to", novawl_start(on_commit, NULL) == 0);
    if (failures) return 1;
    wl_surface = novawl_surface_create(&sid);
    ok("a wl_surface", wl_surface != NULL && sid != 0);

    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_ext;
    ok("an instance with Wayland surfaces", vkCreateInstance(&ici, NULL, &inst) == VK_SUCCESS);
    if (failures) return 1;
    vkEnumeratePhysicalDevices(inst, &npd, pds);
    for (uint32_t i = 0; i < npd; i++) {
        vkGetPhysicalDeviceProperties(pds[i], &props);
        if (!strncmp(props.deviceName, "llvmpipe", 8)) pd = pds[i];
    }
    ok("lavapipe", pd != VK_NULL_HANDLE);
    if (failures) return 1;
    {
        VkWaylandSurfaceCreateInfoKHR wci = { VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
        wci.display = novawl_display();
        wci.surface = wl_surface;
        ok("a Vulkan surface on it", vkCreateWaylandSurfaceKHR(inst, &wci, NULL, &surface) == VK_SUCCESS);
    }
    vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surface, &supported);
    ok("queue 0 can present to it", supported);
    {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = dev_ext;
        ok("a device with swapchains", vkCreateDevice(pd, &dci, NULL, &dev) == VK_SUCCESS);
    }
    if (failures) return 1;
    vkGetDeviceQueue(dev, 0, 0, &queue);
    {
        VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        sci.surface = surface;
        sci.minImageCount = 2;
        sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        sci.imageExtent.width = W;
        sci.imageExtent.height = H;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        ok("a swapchain", vkCreateSwapchainKHR(dev, &sci, NULL, &swapchain) == VK_SUCCESS);
    }
    if (failures) return 1;
    vkGetSwapchainImagesKHR(dev, swapchain, &nimg, images);

    {
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(dev, &pci, NULL, &pool);
        cai.commandPool = pool;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &cai, &cmd);
        vkCreateFence(dev, &fci, NULL, &fence);
    }

    /* Two frames: (0.25, 0.5, 0.75), then red. */
    for (int frame = 0; frame < 2; frame++) {
        static const VkClearColorValue colours[2] = {
            { .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } },
            { .float32 = { 1.0f, 0.0f, 0.0f, 1.0f } },
        };
        static const uint32_t want[2] = { 0x4080bf, 0xff0000 };
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        uint32_t index = 0;
        char what[96];

        ok("an image is acquired",
           vkAcquireNextImageKHR(dev, swapchain, 5000000000ull, VK_NULL_HANDLE, fence, &index) == VK_SUCCESS);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
        vkResetFences(dev, 1, &fence);

        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &begin);
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[index];
        b.subresourceRange = range;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colours[frame], 1, &range);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        vkEndCommandBuffer(cmd);
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
        vkResetFences(dev, 1, &fence);

        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        ok("it is presented", vkQueuePresentKHR(queue, &pi) == VK_SUCCESS);
        ok("and the compositor receives the frame", wait_frames(frame + 1));
        pthread_mutex_lock(&lock);
        printf("     frame %d: %ux%u, centre #%06x\n", frame, last_w, last_h, last_pixel & 0xffffff);
        snprintf(what, sizeof(what), "frame %d is the colour it was cleared to", frame);
        ok(what, last_w == W && last_h == H && (last_pixel & 0xffffff) == want[frame]);
        pthread_mutex_unlock(&lock);
    }

    vkDeviceWaitIdle(dev);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroySwapchainKHR(dev, swapchain, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroySurfaceKHR(inst, surface, NULL);
    vkDestroyInstance(inst, NULL);
    novawl_surface_destroy(wl_surface);

    printf("vkwl: %s\n", failures ? "FAILED" : "all held");
    return failures ? 1 : 112;
}
