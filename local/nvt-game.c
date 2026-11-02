// nvt-game: a synthetic GPU workload for unattended tests of vidmem eviction.
// It holds N MB of device-local buffers filled with a position-dependent
// pattern and, every frame, reads part of it in a compute shader on the
// graphics queue, comparing every word with the pattern. So one program is
//   - a "game": frames per second and frame times show what moving memory
//     (its own or other processes') costs, e.g. PCIe-bound reads from sysmem
//     or a stall while the driver pauses channels;
//   - a background "app": small, low fps, or idle;
//   - an integrity check: any word that comes back wrong after an
//     evict/restore is counted (errors=), the first one is printed.
//
// Build: ./build.sh tools. Name the process with a symlink (the runner uses
// nvt-game, nvt-bg-*, nvt-idle-*), so evict.sh and the policy can tell them
// apart by name.
//
// Usage: nvt-game [--mb N] [--chunk-mb N] [--read-mb N] [--fps N] [--idle]
//                 [--verify-s S] [--duration S] [--floor-mb N]
//   --mb N        memory to hold (default 1024); allocation stops early, with
//                 a note, when the driver refuses or free vidmem would fall
//                 under --floor-mb (default 256)
//   --chunk-mb N  size of each buffer (default 128)
//   --read-mb N   memory read and checked per frame, a window moving through
//                 all buffers (default: all of it every frame)
//   --fps N       frame cap, 0 = as fast as possible (default 0)
//   --idle        no frames at all (an app in the background): memory is only
//                 checked every --verify-s seconds and on SIGUSR1
//   --verify-s S  full check every S seconds (default 0 = only at the end)
//   --duration S  stop after S seconds (default 0 = until SIGINT/SIGTERM)
//   --gfx         frames are graphics work: a full-screen draw whose fragment
//                 shader does the checking (and --write), into a 1024x1024
//                 colour attachment; graphics is preempted more coarsely than
//                 compute, like the work of Chrome or a game
//   --write       every frame also rewrites what it reads with the next
//                 generation of the pattern (whole buffers), so writes lost
//                 while memory moves show up at the next check
//
// Output, one line each:
//   START allocated_mb=.. chunks=.. fill_ms=..
//   T t=.. fps=.. avg_ms=.. p99_ms=.. max_ms=.. errors=..     every second
//   VERIFY t=.. mb=.. ms=.. errors=..                            full checks
//   ERROR first_bad_elem=.. got=.. want=..   once (generation found/expected)
//   RESULT ... errors=..                                         at the end
// Exit status: 0 no errors, 1 errors, 2 setup failure.

#define _GNU_SOURCE
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "nvml-lite.h"
#include "nvt-game.spv.h"
#include "nvt-game.vert.spv.h"
#include "nvt-game.frag.spv.h"

#define MAX_CHUNKS   1024
#define MAX_GROUPS   32768u           // per dispatch (limit is 65535)
#define ELEM         16u              // bytes per uvec4
#define MAX_SAMPLES  100000

struct pc {
    uint32_t seed, base, count, mode, gen_old, gen_new;
    uint32_t width, height;   // --gfx only
};

#define RT_SIZE 1024u

struct result {
    uint32_t errors, first_bad, got_x, want_x;
};

static VkDevice dev;
static VkQueue queue;
static VkPipeline pipeline;
static VkPipelineLayout layout;
static VkCommandPool pool;
static VkCommandBuffer cb;
static VkFence fence;
static VkDescriptorSet sets[MAX_CHUNKS];
static uint32_t chunk_elems[MAX_CHUNKS];
static unsigned nchunks;
static uint32_t gens[MAX_CHUNKS];   // generation each buffer holds now
static int write_mode, gfx_mode;
static VkPipeline gfx_pipeline;
static VkPipelineLayout gfx_layout;
static VkRenderPass render_pass;
static VkFramebuffer framebuffer;
static volatile struct result *res;
static volatile sig_atomic_t stop_flag, verify_flag;

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void check(VkResult r, const char *what)
{
    if (r != VK_SUCCESS) {
        fprintf(stderr, "%s failed: %d\n", what, r);
        exit(2);
    }
}

static void on_stop(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static void on_usr1(int sig)
{
    (void)sig;
    verify_flag = 1;
}

static uint32_t find_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits,
                          VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid))
            return i;
    }
    fprintf(stderr, "no memory type with flags 0x%x\n", want);
    exit(2);
}

// Graphics version of record_range() for frames: one full-screen draw.
static void record_range_gfx(unsigned c, uint32_t first, uint32_t n, uint32_t m)
{
    uint32_t gen_old = gens[c], gen_new = m == 2 ? gens[c] + 1 : gens[c];
    VkMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = render_pass, .framebuffer = framebuffer,
                                  .renderArea = { { 0, 0 }, { RT_SIZE, RT_SIZE } } };
    struct pc pc = { .seed = 0x9e3779b9u * (c + 1), .base = first, .count = n, .mode = m,
                     .gen_old = gen_old, .gen_new = gen_new, .width = RT_SIZE, .height = RT_SIZE };

    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &b, 0, NULL, 0, NULL);
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_layout, 0, 1, &sets[c], 0, NULL);
    vkCmdPushConstants(cb, gfx_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    gens[c] = gen_new;
}

// Records dispatches over elements [first, first + n) of chunk c, mode m
// (mode 2 must cover the whole chunk: it moves the chunk to the next generation).
static void record_range(unsigned c, uint32_t first, uint32_t n, uint32_t m)
{
    uint32_t gen_old = gens[c], gen_new = m == 2 ? gens[c] + 1 : (m == 0 ? 0 : gens[c]);

    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[c], 0, NULL);
    if (m == 2) {
        // The previous pass over this buffer (same frame) must have landed.
        VkMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &b, 0, NULL, 0, NULL);
    }
    while (n > 0) {
        uint32_t step = n < MAX_GROUPS * 256u ? n : MAX_GROUPS * 256u;
        struct pc pc = { .seed = 0x9e3779b9u * (c + 1), .base = first, .count = step, .mode = m,
                         .gen_old = gen_old, .gen_new = gen_new };

        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cb, (step + 255) / 256, 1, 1);
        first += step;
        n -= step;
    }
    if (m == 0 || m == 2)
        gens[c] = gen_new;
}

static void frame_range(unsigned c, uint32_t first, uint32_t n, uint32_t m)
{
    if (gfx_mode)
        record_range_gfx(c, first, n, m);
    else
        record_range(c, first, n, m);
}

static void begin(void)
{
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };

    check(vkResetCommandPool(dev, pool, 0), "vkResetCommandPool");
    check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
}

static void submit_wait(void)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                        .pCommandBuffers = &cb };

    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    check(vkResetFences(dev, 1, &fence), "vkResetFences");
    check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
    // A lost device (Xid) must end the test with a clear status, not hang it.
    VkResult r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 30ull * 1000000000ull);
    if (r == VK_TIMEOUT) {
        printf("FATAL gpu did not finish a frame in 30 s\n");
        fflush(stdout);
        exit(2);
    }
    check(r, "vkWaitForFences");
}

static uint32_t last_errors;

static void report_first_error(void)
{
    if (res->errors && !last_errors) {
        printf("ERROR first_bad_elem=%u got_gen=%u want_gen=%u\n", res->first_bad, res->got_x, res->want_x);
        fflush(stdout);
    }
    last_errors = res->errors;
}

// Fill (m = 0) or check (m = 1) everything; returns milliseconds.
static double whole(uint32_t m)
{
    double t0 = now_s();

    for (unsigned c = 0; c < nchunks; c++) {
        begin();
        record_range(c, 0, chunk_elems[c], m);
        submit_wait();
    }
    report_first_error();
    return (now_s() - t0) * 1e3;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;

    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    unsigned long mb = 1024, chunk_mb = 128, read_mb = 0, floor_mb = 256;
    double fps_cap = 0, verify_s = 0, duration = 0;
    int idle = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;

        if (!strcmp(a, "--idle")) {
            idle = 1;
            continue;
        }
        if (!strcmp(a, "--write")) {
            write_mode = 1;
            continue;
        }
        if (!strcmp(a, "--gfx")) {
            gfx_mode = 1;
            continue;
        }
        if (!v)
            goto usage;
        if (!strcmp(a, "--mb")) mb = strtoul(v, NULL, 0);
        else if (!strcmp(a, "--chunk-mb")) chunk_mb = strtoul(v, NULL, 0);
        else if (!strcmp(a, "--read-mb")) read_mb = strtoul(v, NULL, 0);
        else if (!strcmp(a, "--fps")) fps_cap = atof(v);
        else if (!strcmp(a, "--verify-s")) verify_s = atof(v);
        else if (!strcmp(a, "--duration")) duration = atof(v);
        else if (!strcmp(a, "--floor-mb")) floor_mb = strtoul(v, NULL, 0);
        else goto usage;
        i++;
    }
    if (!mb || !chunk_mb || chunk_mb > 4096)
        goto usage;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    signal(SIGUSR1, on_usr1);
    nvml_open();

    // --- instance, device ---------------------------------------------------
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "nvt-game", .apiVersion = VK_API_VERSION_1_1 };
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
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    // Family 0 on NVIDIA is graphics+compute: the work runs where a game's does.
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceFeatures feats = { .fragmentStoresAndAtomics = VK_TRUE };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .pEnabledFeatures = &feats };
    check(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice");
    vkGetDeviceQueue(dev, 0, 0, &queue);

    // --- pipeline -------------------------------------------------------------
    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = sizeof(nvt_game_spv), .pCode = nvt_game_spv };
    VkShaderModule sm;
    check(vkCreateShaderModule(dev, &smci, NULL, &sm), "vkCreateShaderModule");

    VkDescriptorSetLayoutBinding binds[2] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .bindingCount = 2, .pBindings = binds };
    VkDescriptorSetLayout dsl;
    check(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl), "vkCreateDescriptorSetLayout");
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct pc) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &dsl,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    check(vkCreatePipelineLayout(dev, &plci, NULL, &layout), "vkCreatePipelineLayout");
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
        .layout = layout,
    };
    check(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline), "vkCreateComputePipelines");

    if (gfx_mode) {
        // Render target: colour attachment, device-local (compressible kind on NVIDIA).
        VkImageCreateInfo imci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { RT_SIZE, RT_SIZE, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VkImage img;
        check(vkCreateImage(dev, &imci, NULL, &img), "vkCreateImage");
        VkMemoryRequirements ireq;
        vkGetImageMemoryRequirements(dev, img, &ireq);
        VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img };
        VkMemoryAllocateInfo imai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded, .allocationSize = ireq.size,
            .memoryTypeIndex = find_type(&mp, ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
        };
        VkDeviceMemory imem;
        check(vkAllocateMemory(dev, &imai, NULL, &imem), "vkAllocateMemory(rt)");
        check(vkBindImageMemory(dev, img, imem, 0), "vkBindImageMemory");
        VkImageViewCreateInfo ivci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = imci.format, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VkImageView view;
        check(vkCreateImageView(dev, &ivci, NULL, &view), "vkCreateImageView");

        VkAttachmentDescription att = {
            .format = imci.format, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        VkAttachmentReference aref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     .colorAttachmentCount = 1, .pColorAttachments = &aref };
        VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                        .attachmentCount = 1, .pAttachments = &att,
                                        .subpassCount = 1, .pSubpasses = &sub };
        check(vkCreateRenderPass(dev, &rpci, NULL, &render_pass), "vkCreateRenderPass");
        VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                         .renderPass = render_pass, .attachmentCount = 1, .pAttachments = &view,
                                         .width = RT_SIZE, .height = RT_SIZE, .layers = 1 };
        check(vkCreateFramebuffer(dev, &fbci, NULL, &framebuffer), "vkCreateFramebuffer");

        VkShaderModule vsm, fsm;
        VkShaderModuleCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                         .codeSize = sizeof(nvt_vert_spv), .pCode = nvt_vert_spv };
        VkShaderModuleCreateInfo fci2 = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                          .codeSize = sizeof(nvt_frag_spv), .pCode = nvt_frag_spv };
        check(vkCreateShaderModule(dev, &vci, NULL, &vsm), "vkCreateShaderModule(vert)");
        check(vkCreateShaderModule(dev, &fci2, NULL, &fsm), "vkCreateShaderModule(frag)");
        VkPushConstantRange gpcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct pc) };
        VkPipelineLayoutCreateInfo gplci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                             .setLayoutCount = 1, .pSetLayouts = &dsl,
                                             .pushConstantRangeCount = 1, .pPushConstantRanges = &gpcr };
        check(vkCreatePipelineLayout(dev, &gplci, NULL, &gfx_layout), "vkCreatePipelineLayout(gfx)");

        VkPipelineShaderStageCreateInfo stages[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
              .module = vsm, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
              .module = fsm, .pName = "main" },
        };
        VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkViewport vp = { 0, 0, RT_SIZE, RT_SIZE, 0, 1 };
        VkRect2D sc = { { 0, 0 }, { RT_SIZE, RT_SIZE } };
        VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1, .pViewports = &vp,
                                                  .scissorCount = 1, .pScissors = &sc };
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                      .polygonMode = VK_POLYGON_MODE_FILL,
                                                      .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
        VkPipelineColorBlendStateCreateInfo cb2 = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .attachmentCount = 1, .pAttachments = &cba };
        VkGraphicsPipelineCreateInfo gpci = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
            .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vps,
            .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb2,
            .layout = gfx_layout, .renderPass = render_pass, .subpass = 0,
        };
        check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &gfx_pipeline),
              "vkCreateGraphicsPipelines");
    }

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    check(vkCreateCommandPool(dev, &cpi, NULL, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1 };
    check(vkAllocateCommandBuffers(dev, &cbai, &cb), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    check(vkCreateFence(dev, &fci, NULL, &fence), "vkCreateFence");

    // --- result buffer: host-visible, so errors are read without a copy -------
    VkBufferCreateInfo rbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof(struct result),
                                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer rbuf;
    check(vkCreateBuffer(dev, &rbci, NULL, &rbuf), "vkCreateBuffer(result)");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, rbuf, &req);
    VkMemoryAllocateInfo rmai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = find_type(&mp, req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0),
    };
    VkDeviceMemory rmem;
    check(vkAllocateMemory(dev, &rmai, NULL, &rmem), "vkAllocateMemory(result)");
    check(vkBindBufferMemory(dev, rbuf, rmem, 0), "vkBindBufferMemory(result)");
    void *rmap;
    check(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, &rmap), "vkMapMemory(result)");
    res = rmap;
    memset((void *)res, 0, sizeof(*res));

    // --- the memory under test ------------------------------------------------
    unsigned want_chunks = (mb + chunk_mb - 1) / chunk_mb;
    if (want_chunks > MAX_CHUNKS)
        want_chunks = MAX_CHUNKS;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * want_chunks };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                        .maxSets = want_chunks, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dpool;
    check(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool), "vkCreateDescriptorPool");

    unsigned long allocated_mb = 0;
    const char *stop_reason = "complete";
    for (unsigned c = 0; c < want_chunks; c++) {
        unsigned long this_mb = mb - allocated_mb < chunk_mb ? mb - allocated_mb : chunk_mb;
        long f = real_free_mb();

        if (f >= 0 && (unsigned long)f < floor_mb + this_mb) {
            stop_reason = "floor";
            break;
        }
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                   .size = (VkDeviceSize)this_mb << 20,
                                   .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        VkBuffer buf;
        check(vkCreateBuffer(dev, &bci, NULL, &buf), "vkCreateBuffer");
        vkGetBufferMemoryRequirements(dev, buf, &req);
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
            .memoryTypeIndex = find_type(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
        };
        VkDeviceMemory mem;
        if (vkAllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS) {
            vkDestroyBuffer(dev, buf, NULL);
            stop_reason = "denied";
            break;
        }
        check(vkBindBufferMemory(dev, buf, mem, 0), "vkBindBufferMemory");

        VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                             .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
        check(vkAllocateDescriptorSets(dev, &dsai, &sets[c]), "vkAllocateDescriptorSets");
        VkDescriptorBufferInfo dbi[2] = { { buf, 0, VK_WHOLE_SIZE }, { rbuf, 0, VK_WHOLE_SIZE } };
        VkWriteDescriptorSet w[2] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[c], .dstBinding = 0,
              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[0] },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[c], .dstBinding = 1,
              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[1] },
        };
        vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
        chunk_elems[c] = (uint32_t)((this_mb << 20) / ELEM);
        allocated_mb += this_mb;
        nchunks = c + 1;
    }
    if (!nchunks) {
        printf("FATAL no memory allocated (%s)\n", stop_reason);
        return 2;
    }

    double fill_ms = whole(0);
    printf("START pid=%d allocated_mb=%lu wanted_mb=%lu chunks=%u stop=%s fill_ms=%.0f mode=%s%s%s\n",
           getpid(), allocated_mb, mb, nchunks, stop_reason, fill_ms, idle ? "idle" : "frames",
           gfx_mode ? "+gfx" : "", write_mode ? "+write" : "");

    // --- main loop ------------------------------------------------------------
    static double samples[MAX_SAMPLES], per_s_fps[100000];
    unsigned nsamples = 0, nseconds = 0;
    unsigned long frames = 0, frames_50 = 0, frames_250 = 0;
    double worst_ms = 0;
    unsigned verifies = 0;
    uint64_t window_elems = read_mb ? ((uint64_t)read_mb << 20) / ELEM : 0;
    unsigned cur_chunk = 0;
    uint32_t cur_elem = 0;

    double t_start = now_s(), t_report = t_start, t_verify = t_start, t_next = t_start;
    while (!stop_flag && (duration <= 0 || now_s() - t_start < duration)) {
        double t = now_s();

        if (verify_flag || (verify_s > 0 && t - t_verify >= verify_s)) {
            verify_flag = 0;
            double ms = whole(1);
            t_verify = now_s();
            verifies++;
            printf("VERIFY t=%.1f mb=%lu ms=%.0f errors=%u\n", t_verify - t_start, allocated_mb, ms, res->errors);
        }

        if (idle) {
            usleep(100000);
        } else {
            if (fps_cap > 0) {
                if (t < t_next) {
                    usleep((useconds_t)((t_next - t) * 1e6));
                }
                t_next += 1.0 / fps_cap;
                if (t_next < now_s() - 0.5)  // fell far behind: do not catch up in a burst
                    t_next = now_s();
            }
            double f0 = now_s();
            begin();
            if (!window_elems) {
                for (unsigned c = 0; c < nchunks; c++)
                    frame_range(c, 0, chunk_elems[c], write_mode ? 2 : 1);
            } else if (write_mode) {
                // Whole buffers only: each pass moves a buffer to its next generation.
                uint64_t left = window_elems;
                while (left > 0) {
                    frame_range(cur_chunk, 0, chunk_elems[cur_chunk], 2);
                    left = left > chunk_elems[cur_chunk] ? left - chunk_elems[cur_chunk] : 0;
                    cur_chunk = (cur_chunk + 1) % nchunks;
                }
            } else {
                uint64_t left = window_elems;
                while (left > 0) {
                    uint32_t avail = chunk_elems[cur_chunk] - cur_elem;
                    uint32_t take = left < avail ? (uint32_t)left : avail;
                    frame_range(cur_chunk, cur_elem, take, 1);
                    left -= take;
                    cur_elem += take;
                    if (cur_elem == chunk_elems[cur_chunk]) {
                        cur_elem = 0;
                        cur_chunk = (cur_chunk + 1) % nchunks;
                    }
                }
            }
            submit_wait();
            report_first_error();
            double ms = (now_s() - f0) * 1e3;
            frames++;
            if (ms > 50) frames_50++;
            if (ms > 250) frames_250++;
            if (ms > worst_ms) worst_ms = ms;
            if (nsamples < MAX_SAMPLES)
                samples[nsamples++] = ms;
        }

        t = now_s();
        if (t - t_report >= 1.0) {
            double span = t - t_report;
            if (idle) {
                printf("T t=%.0f idle errors=%u\n", t - t_start, res->errors);
            } else {
                double sum = 0, p99 = 0, mx = 0;
                for (unsigned i = 0; i < nsamples; i++) {
                    sum += samples[i];
                    if (samples[i] > mx) mx = samples[i];
                }
                if (nsamples) {
                    qsort(samples, nsamples, sizeof(double), cmp_double);
                    p99 = samples[(unsigned)floor(0.99 * (nsamples - 1))];
                }
                double fps = nsamples / span;
                if (nseconds < sizeof(per_s_fps) / sizeof(per_s_fps[0]))
                    per_s_fps[nseconds++] = fps;
                printf("T t=%.0f fps=%.1f avg_ms=%.2f p99_ms=%.2f max_ms=%.1f errors=%u\n", t - t_start, fps,
                       nsamples ? sum / nsamples : 0, p99, mx, res->errors);
            }
            nsamples = 0;
            t_report = t;
        }
    }

    // Final full check: the content must have survived everything.
    double ms = whole(1);
    verifies++;
    printf("VERIFY t=%.1f mb=%lu ms=%.0f errors=%u final\n", now_s() - t_start, allocated_mb, ms, res->errors);

    double elapsed = now_s() - t_start, min_fps = 0, avg_fps = elapsed > 0 ? frames / elapsed : 0;
    if (nseconds) {
        qsort(per_s_fps, nseconds, sizeof(double), cmp_double);
        min_fps = per_s_fps[0];
    }
    printf("RESULT allocated_mb=%lu seconds=%.0f frames=%lu avg_fps=%.1f min_1s_fps=%.1f "
           "frames_over_50ms=%lu frames_over_250ms=%lu worst_ms=%.0f verifies=%u errors=%u\n",
           allocated_mb, elapsed, frames, avg_fps, min_fps, frames_50, frames_250, worst_ms, verifies, res->errors);
    return res->errors ? 1 : 0;

usage:
    fprintf(stderr, "usage: %s [--mb N] [--chunk-mb N] [--read-mb N] [--fps N] [--idle] [--gfx] [--write] [--verify-s S] "
                    "[--duration S] [--floor-mb N]\n", argv[0]);
    return 2;
}
