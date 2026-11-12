/* vk64.c - Vulkan with no window system (Milestone 110).
 *
 * Mesa's lavapipe: an instance, the llvmpipe physical device, a queue,
 * an image cleared on the device and copied into host memory, read back.
 * Vulkan is what DXVK and vkd3d-proton turn Direct3D 9 to 12 into, so it
 * is what most Windows games under Wine end up calling. Run on the Linux
 * host and on NovarisOS, and the two must agree.
 *
 * Exits 111 when everything held.
 */

#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

static int failures;

static void ok(const char *what, int cond)
{
    printf("%-4s vk: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) failures++;
}

#define W 16
#define H 16

static uint32_t memory_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

int main(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance inst;
    VkPhysicalDevice pds[8], pd = VK_NULL_HANDLE;
    uint32_t npd = 8, qf = UINT32_MAX, nqf = 16;
    VkQueueFamilyProperties qfp[16];
    VkPhysicalDeviceProperties props;
    VkDevice dev;
    VkQueue queue;
    VkImage image;
    VkBuffer buffer;
    VkDeviceMemory image_mem, buffer_mem;
    VkMemoryRequirements req;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    unsigned char *px;

    setvbuf(stdout, NULL, _IONBF, 0);

    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    ok("an instance", vkCreateInstance(&ici, NULL, &inst) == VK_SUCCESS);
    if (failures) return 1;
    ok("physical devices", vkEnumeratePhysicalDevices(inst, &npd, pds) >= 0 && npd > 0);
    for (uint32_t i = 0; i < npd; i++) {
        vkGetPhysicalDeviceProperties(pds[i], &props);
        if (!strncmp(props.deviceName, "llvmpipe", 8)) { pd = pds[i]; break; }
    }
    ok("one of them is llvmpipe", pd != VK_NULL_HANDLE);
    if (failures) return 1;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("     api %u.%u\n", VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion));

    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfp);
    for (uint32_t i = 0; i < nqf; i++)
        if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }
    ok("a graphics queue family", qf != UINT32_MAX);
    {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        qci.queueFamilyIndex = qf;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        ok("a device", vkCreateDevice(pd, &dci, NULL, &dev) == VK_SUCCESS);
    }
    if (failures) return 1;
    vkGetDeviceQueue(dev, qf, 0, &queue);

    {
        VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent.width = W; ii.extent.height = H; ii.extent.depth = 1;
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ok("an image", vkCreateImage(dev, &ii, NULL, &image) == VK_SUCCESS);
        vkGetImageMemoryRequirements(dev, image, &req);
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ok("device memory for it", vkAllocateMemory(dev, &ai, NULL, &image_mem) == VK_SUCCESS &&
                                   vkBindImageMemory(dev, image, image_mem, 0) == VK_SUCCESS);
    }
    {
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        bi.size = W * H * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        ok("a buffer", vkCreateBuffer(dev, &bi, NULL, &buffer) == VK_SUCCESS);
        vkGetBufferMemoryRequirements(dev, buffer, &req);
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(pd, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        ok("host-visible memory for it", vkAllocateMemory(dev, &ai, NULL, &buffer_mem) == VK_SUCCESS &&
                                         vkBindBufferMemory(dev, buffer, buffer_mem, 0) == VK_SUCCESS);
    }
    if (failures) return 1;

    {
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        VkClearColorValue colour = { .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } };
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkBufferImageCopy copy = { 0 };
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

        pci.queueFamilyIndex = qf;
        vkCreateCommandPool(dev, &pci, NULL, &pool);
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &cai, &cmd);
        vkBeginCommandBuffer(cmd, &begin);

        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = range;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = W; copy.imageExtent.height = H; copy.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        vkEndCommandBuffer(cmd);

        vkCreateFence(dev, &fci, NULL, &fence);
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        ok("the commands are submitted", vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS);
        ok("and finish", vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull) == VK_SUCCESS);
    }

    ok("the buffer maps", vkMapMemory(dev, buffer_mem, 0, W * H * 4, 0, (void **)&px) == VK_SUCCESS);
    if (!failures) {
        const unsigned char *p = &px[(5 * W + 7) * 4];
        printf("     pixel (7,5) = %02x%02x%02x%02x\n", p[0], p[1], p[2], p[3]);
        ok("the device cleared the image to the colour asked for",
           p[0] == 0x40 && p[1] == 0x80 && p[2] == 0xbf && p[3] == 0xff);
        vkUnmapMemory(dev, buffer_mem);
    }

    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyBuffer(dev, buffer, NULL);
    vkFreeMemory(dev, buffer_mem, NULL);
    vkDestroyImage(dev, image, NULL);
    vkFreeMemory(dev, image_mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);

    printf("vk: %s\n", failures ? "FAILED" : "all held");
    return failures ? 1 : 111;
}
