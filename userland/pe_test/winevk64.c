/* winevk64.c - a Windows Vulkan program, in a window (Milestone 111).
 *
 * What every DXVK game does underneath: VK_KHR_win32_surface on a window,
 * a swapchain the size of its client area, frames presented. Under Wine
 * on winenovaris.drv the surface is a Wayland one on the driver's own
 * compositor, and each presented frame is copied over the window's client
 * area. Checked on the screen itself, through WMIO_PEEK: two frames of two
 * colours, so a window that kept the first one would fail.
 *
 * vulkan-1.dll is loaded at run time - mingw has no import library for
 * it - and every entry point comes through vkGetInstanceProcAddr.
 *
 * Exit code 0 when everything held; otherwise the number of the first
 * check that did not.
 */

#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define LINUX_OPEN  2
#define LINUX_CLOSE 3
#define LINUX_IOCTL 16
#define WMIO_PEEK   0x570Fu

struct wm_rect { int x, y, w, h; };

static LONGLONG linux3(LONGLONG n, LONGLONG a, LONGLONG b, LONGLONG c)
{
    LONGLONG ret;
    __asm__ __volatile__("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c)
                         : "rcx", "r11", "memory");
    return ret;
}

static int wm_fd = -1;

static int peek(int x, int y)
{
    struct wm_rect r = { x, y, 0, 0 };
    if (linux3(LINUX_IOCTL, wm_fd, WMIO_PEEK, (LONGLONG)(INT_PTR)&r) != 0) return -1;
    return r.w;
}

static int failed, check_no;

static void check(const char *what, int ok)
{
    check_no++;
    printf("%s  %s\n", ok ? "ok " : "bad", what);
    fflush(stdout);
    if (!ok && !failed) failed = check_no;
}

static void pump(int ms)
{
    DWORD start = GetTickCount();
    MSG msg;
    while ((int)(GetTickCount() - start) < ms)
    {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    }
}

#define FUNCS \
    F(vkCreateInstance) F(vkEnumeratePhysicalDevices) F(vkGetPhysicalDeviceProperties) \
    F(vkCreateWin32SurfaceKHR) F(vkGetPhysicalDeviceSurfaceSupportKHR) \
    F(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) F(vkCreateDevice) F(vkGetDeviceQueue) \
    F(vkCreateSwapchainKHR) F(vkGetSwapchainImagesKHR) F(vkAcquireNextImageKHR) \
    F(vkCreateCommandPool) F(vkAllocateCommandBuffers) F(vkBeginCommandBuffer) \
    F(vkCmdPipelineBarrier) F(vkCmdClearColorImage) F(vkEndCommandBuffer) F(vkQueueSubmit) \
    F(vkQueuePresentKHR) F(vkCreateFence) F(vkWaitForFences) F(vkResetFences) \
    F(vkResetCommandBuffer) F(vkDeviceWaitIdle) F(vkDestroySwapchainKHR) F(vkDestroyDevice) \
    F(vkDestroySurfaceKHR) F(vkDestroyInstance) F(vkDestroyFence) F(vkDestroyCommandPool)
#define F(name) static PFN_##name p##name;
FUNCS
#undef F

int main(void)
{
    const char *inst_ext[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
    const char *dev_ext[] = { "VK_KHR_swapchain" };
    PFN_vkGetInstanceProcAddr get_proc;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance inst;
    VkPhysicalDevice pd = VK_NULL_HANDLE, pds[8];
    VkPhysicalDeviceProperties props;
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceKHR surface;
    VkDevice dev;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkImage images[8];
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkBool32 supported = 0;
    uint32_t npd = 8, nimg = 8;
    WNDCLASSA wc;
    HMODULE vulkan;
    HWND hwnd;
    RECT rc;
    char line[200];

    wm_fd = (int)linux3(LINUX_OPEN, (LONGLONG)(INT_PTR)"/dev/wm", 2, 0);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "NovarisVK";
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "NovarisVK", "Vulkan", WS_OVERLAPPEDWINDOW,
                           240, 180, 320, 260, NULL, NULL, wc.hInstance, NULL);
    check("a window", hwnd != NULL);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    pump(300);

    vulkan = LoadLibraryA("vulkan-1.dll");
    check("vulkan-1.dll loads", vulkan != NULL);
    if (failed) return failed;
    get_proc = (PFN_vkGetInstanceProcAddr)GetProcAddress(vulkan, "vkGetInstanceProcAddr");
    pvkCreateInstance = (PFN_vkCreateInstance)get_proc(NULL, "vkCreateInstance");

    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_ext;
    check("an instance with Win32 surfaces", pvkCreateInstance && pvkCreateInstance(&ici, NULL, &inst) == VK_SUCCESS);
    if (failed) return failed;
#define F(name) p##name = (PFN_##name)get_proc(inst, #name);
    FUNCS
#undef F

    pvkEnumeratePhysicalDevices(inst, &npd, pds);
    for (uint32_t i = 0; i < npd; i++)
    {
        pvkGetPhysicalDeviceProperties(pds[i], &props);
        if (!strncmp(props.deviceName, "llvmpipe", 8)) pd = pds[i];
    }
    check("the device is Mesa's lavapipe", pd != VK_NULL_HANDLE);
    if (failed) return failed;
    {
        VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        sci.hinstance = wc.hInstance;
        sci.hwnd = hwnd;
        check("a surface on the window", pvkCreateWin32SurfaceKHR(inst, &sci, NULL, &surface) == VK_SUCCESS);
    }
    pvkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surface, &supported);
    check("queue 0 presents to it", supported);
    pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
    GetClientRect(hwnd, &rc);
    snprintf(line, sizeof(line), "     surface extent %ux%u, client %ldx%ld", caps.currentExtent.width,
             caps.currentExtent.height, rc.right - rc.left, rc.bottom - rc.top);
    puts(line);
    fflush(stdout);
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
        check("a device", pvkCreateDevice(pd, &dci, NULL, &dev) == VK_SUCCESS);
    }
    if (failed) return failed;
    pvkGetDeviceQueue(dev, 0, 0, &queue);
    {
        VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        sci.surface = surface;
        sci.minImageCount = caps.minImageCount > 2 ? caps.minImageCount : 2;
        sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        sci.imageExtent.width = rc.right - rc.left;
        sci.imageExtent.height = rc.bottom - rc.top;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        check("a swapchain the size of the client area",
              pvkCreateSwapchainKHR(dev, &sci, NULL, &swapchain) == VK_SUCCESS);
    }
    if (failed) return failed;
    pvkGetSwapchainImagesKHR(dev, swapchain, &nimg, images);
    {
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pvkCreateCommandPool(dev, &pci, NULL, &pool);
        cai.commandPool = pool;
        cai.commandBufferCount = 1;
        pvkAllocateCommandBuffers(dev, &cai, &cmd);
        pvkCreateFence(dev, &fci, NULL, &fence);
    }

    for (int frame = 0; frame < 2; frame++)
    {
        static const VkClearColorValue colours[2] = {
            { .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } },
            { .float32 = { 1.0f, 0.0f, 0.0f, 1.0f } },
        };
        static const int want[2] = { 0x4080bf, 0xff0000 };
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        uint32_t index = 0;
        VkResult res;
        POINT mid;

        res = pvkAcquireNextImageKHR(dev, swapchain, 5000000000ull, VK_NULL_HANDLE, fence, &index);
        check("an image is acquired", res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR);
        pvkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
        pvkResetFences(dev, 1, &fence);

        pvkResetCommandBuffer(cmd, 0);
        pvkBeginCommandBuffer(cmd, &begin);
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[index];
        b.subresourceRange = range;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 0, NULL, 1, &b);
        pvkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colours[frame], 1, &range);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              0, 0, NULL, 0, NULL, 1, &b);
        pvkEndCommandBuffer(cmd);
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        pvkQueueSubmit(queue, 1, &si, fence);
        pvkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
        pvkResetFences(dev, 1, &fence);

        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        res = pvkQueuePresentKHR(queue, &pi);
        check("the frame is presented", res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR);
        pump(500);

        mid.x = (rc.right - rc.left) / 2;
        mid.y = (rc.bottom - rc.top) / 2;
        ClientToScreen(hwnd, &mid);
        snprintf(line, sizeof(line), "     frame %d on screen at (%ld,%ld): #%06x", frame, mid.x, mid.y,
                 peek(mid.x, mid.y));
        puts(line);
        fflush(stdout);
        snprintf(line, sizeof(line), "frame %d is on the screen, in its colour", frame);
        check(line, peek(mid.x, mid.y) == want[frame]);
    }

    pvkDeviceWaitIdle(dev);
    pvkDestroyFence(dev, fence, NULL);
    pvkDestroyCommandPool(dev, pool, NULL);
    pvkDestroySwapchainKHR(dev, swapchain, NULL);
    pvkDestroyDevice(dev, NULL);
    pvkDestroySurfaceKHR(inst, surface, NULL);
    pvkDestroyInstance(inst, NULL);
    DestroyWindow(hwnd);
    if (wm_fd >= 0) linux3(LINUX_CLOSE, wm_fd, 0, 0);

    if (failed) printf("winevk64: check %d did not hold\n", failed);
    else puts("winevk64: Vulkan presented into a window, through Mesa");
    fflush(stdout);
    return failed;
}
