// compr-test: keeps a colour-attachment image filled with a known pattern and
// verifies it through the GPU, for testing what happens to a vidmem
// allocation underneath a running process (e.g. moving it to system memory
// and back). The NVIDIA driver normally gives such an image a compressible
// PTE kind; check with census.sh (look for a dedicated allocation of about
// size*size*4 bytes with verdict "compressed").
//
// Build: ./build.sh tools
// Usage: compr-test [--size N] [--interval-ms M] [--mode write|hold] [--iterations K]
//
//   write (default) every iteration: full clear (vkCmdClearColorImage, usually a
//                   fast clear), four rectangle clears inside a render pass
//                   (vkCmdClearAttachments), one 256x256 data block upload, then
//                   a full readback that is compared with a CPU model.
//   hold            writes iteration 0 once, then only reads back and compares,
//                   so the content must survive whatever happens to the memory
//                   in between.
//
// One line per iteration: "OK <n>" or "MISMATCH <n>: <count> pixels, first at
// x,y got 0x... want 0x...". Exit status 1 after the first mismatch unless
// --keep-going is given.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define BLOCK 256
#define RECTS 4

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties memprops;

static void check(VkResult r, const char *what)
{
    if (r != VK_SUCCESS) {
        fprintf(stderr, "%s failed: %d\n", what, r);
        exit(2);
    }
}

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = memprops.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid))
            return i;
    }
    fprintf(stderr, "no memory type for bits 0x%x\n", bits);
    exit(2);
}

// --- the CPU model of what the image must contain after iteration k ---------

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

static uint32_t full_colour(uint32_t k) { return mix(k * 4 + 1) | 0xff000000u; }
static uint32_t rect_colour(uint32_t k, int i) { return mix(k * 4 + 2 + (uint32_t)i * 977) | 0xff000000u; }

static VkRect2D rect_of(uint32_t k, int i, uint32_t size)
{
    uint32_t w = 64 + mix(k * 31 + i) % (size / 4);
    uint32_t h = 64 + mix(k * 37 + i + 100) % (size / 4);
    VkRect2D r = {
        .offset = { (int32_t)(mix(k * 41 + i + 200) % (size - w)), (int32_t)(mix(k * 43 + i + 300) % (size - h)) },
        .extent = { w, h },
    };
    return r;
}

static void block_origin(uint32_t k, uint32_t size, uint32_t *x, uint32_t *y)
{
    *x = mix(k * 47 + 400) % (size - BLOCK);
    *y = mix(k * 53 + 500) % (size - BLOCK);
}

static uint32_t block_pixel(uint32_t k, uint32_t x, uint32_t y)
{
    return mix((k << 20) ^ (y << 10) ^ x) | 0xff000000u;
}

static void model(uint32_t *img, uint32_t size, uint32_t k)
{
    uint32_t c = full_colour(k), bx, by;

    for (size_t i = 0; i < (size_t)size * size; i++)
        img[i] = c;
    for (int i = 0; i < RECTS; i++) {
        VkRect2D r = rect_of(k, i, size);
        uint32_t rc = rect_colour(k, i);
        for (uint32_t y = 0; y < r.extent.height; y++)
            for (uint32_t x = 0; x < r.extent.width; x++)
                img[(size_t)(r.offset.y + y) * size + r.offset.x + x] = rc;
    }
    block_origin(k, size, &bx, &by);
    for (uint32_t y = 0; y < BLOCK; y++)
        for (uint32_t x = 0; x < BLOCK; x++)
            img[(size_t)(by + y) * size + bx + x] = block_pixel(k, x, y);
}

static VkClearColorValue unpack(uint32_t c)
{
    VkClearColorValue v;
    // R8G8B8A8_UNORM, little-endian: byte 0 = R.
    v.float32[0] = (float)((c >> 0) & 0xff) / 255.0f;
    v.float32[1] = (float)((c >> 8) & 0xff) / 255.0f;
    v.float32[2] = (float)((c >> 16) & 0xff) / 255.0f;
    v.float32[3] = (float)((c >> 24) & 0xff) / 255.0f;
    return v;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

int main(int argc, char **argv)
{
    uint32_t size = 2048, interval_ms = 500;
    long iterations = -1;
    int hold = 0, keep_going = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc)
            size = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc)
            interval_ms = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--iterations") && i + 1 < argc)
            iterations = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "hold"))
                hold = 1;
            else if (strcmp(m, "write"))
                goto usage;
        } else if (!strcmp(argv[i], "--keep-going"))
            keep_going = 1;
        else
            goto usage;
    }
    if (size < 4 * BLOCK)
        goto usage;

    // --- instance, device, queue ---------------------------------------------
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "compr-test", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    check(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");

    uint32_t n = 16;
    VkPhysicalDevice pds[16], pd = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.vendorID == 0x10de) {
            pd = pds[i];
            break;
        }
    }
    if (pd == VK_NULL_HANDLE) {
        fprintf(stderr, "no NVIDIA Vulkan device\n");
        return 2;
    }
    vkGetPhysicalDeviceMemoryProperties(pd, &memprops);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    check(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice");
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    // --- the image under test: dedicated, device-local, optimal tiling --------
    VkImageCreateInfo imci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { size, size, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img;
    check(vkCreateImage(dev, &imci, NULL, &img), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img, &req);
    VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded, .allocationSize = req.size,
        .memoryTypeIndex = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
    };
    VkDeviceMemory imgmem;
    check(vkAllocateMemory(dev, &mai, NULL, &imgmem), "vkAllocateMemory(image)");
    check(vkBindImageMemory(dev, img, imgmem, 0), "vkBindImageMemory");

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = imci.format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkImageView view;
    check(vkCreateImageView(dev, &ivci, NULL, &view), "vkCreateImageView");

    VkAttachmentDescription att = {
        .format = imci.format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference aref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &aref };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                    .attachmentCount = 1, .pAttachments = &att,
                                    .subpassCount = 1, .pSubpasses = &sub };
    VkRenderPass rp;
    check(vkCreateRenderPass(dev, &rpci, NULL, &rp), "vkCreateRenderPass");
    VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
                                     .attachmentCount = 1, .pAttachments = &view,
                                     .width = size, .height = size, .layers = 1 };
    VkFramebuffer fb;
    check(vkCreateFramebuffer(dev, &fbci, NULL, &fb), "vkCreateFramebuffer");

    // --- host-visible buffers: readback (whole image) and upload (one block) --
    VkDeviceSize rb_size = (VkDeviceSize)size * size * 4, up_size = BLOCK * BLOCK * 4;
    VkBuffer bufs[2];
    VkDeviceMemory bufmem[2];
    void *maps[2];
    VkDeviceSize sizes[2] = { rb_size, up_size };
    VkBufferUsageFlags usages[2] = { VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    for (int i = 0; i < 2; i++) {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizes[i],
                                   .usage = usages[i], .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        check(vkCreateBuffer(dev, &bci, NULL, &bufs[i]), "vkCreateBuffer");
        VkMemoryRequirements br;
        vkGetBufferMemoryRequirements(dev, bufs[i], &br);
        VkMemoryAllocateInfo bai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = br.size,
            .memoryTypeIndex = find_type(br.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0),
        };
        check(vkAllocateMemory(dev, &bai, NULL, &bufmem[i]), "vkAllocateMemory(buffer)");
        check(vkBindBufferMemory(dev, bufs[i], bufmem[i], 0), "vkBindBufferMemory");
        check(vkMapMemory(dev, bufmem[i], 0, VK_WHOLE_SIZE, 0, &maps[i]), "vkMapMemory");
    }

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = 0 };
    VkCommandPool pool;
    check(vkCreateCommandPool(dev, &pci, NULL, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1 };
    VkCommandBuffer cb;
    check(vkAllocateCommandBuffers(dev, &cbai, &cb), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    check(vkCreateFence(dev, &fci, NULL, &fence), "vkCreateFence");

    uint32_t *want = malloc(rb_size);
    if (!want)
        return 2;

    printf("compr-test pid %d: %ux%u RGBA8 image, %llu KB dedicated allocation, mode %s\n",
           getpid(), size, size, (unsigned long long)(req.size >> 10), hold ? "hold" : "write");
    fflush(stdout);

    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    int failures = 0;

    for (long it = 0; iterations < 0 || it < iterations; it++) {
        uint32_t k = hold ? 0 : (uint32_t)it;
        int write = !hold || it == 0;
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

        if (write) {
            uint32_t *up = maps[1];
            for (uint32_t y = 0; y < BLOCK; y++)
                for (uint32_t x = 0; x < BLOCK; x++)
                    up[y * BLOCK + x] = block_pixel(k, x, y);
        }

        check(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer");
        check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
        if (write) {
            VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkClearColorValue c = unpack(full_colour(k));

            barrier(cb, img, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &all);

            barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                          .framebuffer = fb, .renderArea = { { 0, 0 }, { size, size } } };
            vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            for (int i = 0; i < RECTS; i++) {
                VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
                                         .clearValue = { .color = unpack(rect_colour(k, i)) } };
                VkClearRect cr = { .rect = rect_of(k, i, size), .baseArrayLayer = 0, .layerCount = 1 };
                vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
            }
            vkCmdEndRenderPass(cb);

            uint32_t bx, by;
            block_origin(k, size, &bx, &by);
            barrier(cb, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy upr = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                      .imageOffset = { (int32_t)bx, (int32_t)by, 0 },
                                      .imageExtent = { BLOCK, BLOCK, 1 } };
            vkCmdCopyBufferToImage(cb, bufs[1], img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upr);
            barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        VkBufferImageCopy rbr = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                  .imageExtent = { size, size, 1 } };
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bufs[0], 1, &rbr);
        check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");

        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        check(vkQueueSubmit(q, 1, &si, fence), "vkQueueSubmit");
        check(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        check(vkResetFences(dev, 1, &fence), "vkResetFences");

        if (write || it == 1)
            model(want, size, k);

        const uint32_t *got = maps[0];
        size_t bad = 0, first = 0;
        for (size_t i = 0; i < (size_t)size * size; i++) {
            if (got[i] != want[i]) {
                if (bad++ == 0)
                    first = i;
            }
        }
        if (bad == 0) {
            printf("OK %ld\n", it);
        } else {
            printf("MISMATCH %ld: %zu pixels, first at %zu,%zu got 0x%08x want 0x%08x\n", it, bad,
                   first % size, first / size, got[first], want[first]);
            failures++;
        }
        fflush(stdout);
        if (failures && !keep_going)
            return 1;
        if (interval_ms)
            usleep(interval_ms * 1000u);
    }
    return failures ? 1 : 0;

usage:
    fprintf(stderr, "usage: %s [--size N>=1024] [--interval-ms M] [--mode write|hold] [--iterations K] [--keep-going]\n",
            argv[0]);
    return 2;
}
