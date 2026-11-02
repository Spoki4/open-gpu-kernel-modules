// vram-hog: fills NVIDIA vidmem through Vulkan to test the vidmem user reserve
// patch (RmVidmemUserReserveMB). On a stock driver this can freeze the desktop,
// so it refuses to run unless the reserve is configured (override: --force).
//
// Build: ./build.sh tools (needs Khronos Vulkan-Headers, links
//        /usr/lib/x86_64-linux-gnu/libvulkan.so.1 directly).
// Usage: vram-hog [--chunk-mb N] [--threads N] [--churn S] [--load]
//                 [--hold S] [--floor-mb N] [--start-at T] [--force]
//
//   --chunk-mb N  allocation size while filling (default 256)
//   --threads N   N threads fill at the same time; tests the race between the
//                 reserve check and the PMA allocation (default 1)
//   --churn S     after filling, keep freeing and allocating random
//                 1..chunk MB blocks at the limit for S seconds, like a game
//                 streaming textures
//   --load        keep the GPU busy with vkCmdFillBuffer the whole time
//   --hold S      keep everything allocated for S seconds before freeing
//   --floor-mb N  safety stop: no allocation is attempted while NVML reports
//                 less than N MB + its size free (default 128), so the desktop
//                 survives even if the reserve does not hold
//   --start-at T  start filling at Unix time T (seconds, may be fractional), so
//                 several processes started a moment apart fill together
//
// The last line before "holding" is "RESULT key=value ..." for scripts.

#define _GNU_SOURCE
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "nvml-lite.h"

#define MAX_ALLOCS  4096
#define MAX_THREADS 64
#define LOAD_MB     256

static VkDevice dev;
static VkPhysicalDevice phys;
static uint32_t mem_type, mem_heap;
static unsigned long chunk_mb = 256, floor_mb = 128;
static unsigned churn_s;
static atomic_int stop_all;
static atomic_long min_free_mb = LONG_MAX;
static pthread_barrier_t start_barrier;

struct worker {
    pthread_t tid;
    unsigned id, seed;
    VkDeviceMemory mem[MAX_ALLOCS];
    unsigned long size_mb[MAX_ALLOCS];
    unsigned count;         // slots in use; churn can leave some VK_NULL_HANDLE
    unsigned long held_mb, filled_mb;
    unsigned long denied, churn_ok, churn_denied, guard_stops;
};

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void note_free(long f)
{
    long cur = atomic_load(&min_free_mb);

    while (f >= 0 && f < cur && !atomic_compare_exchange_weak(&min_free_mb, &cur, f))
        ;
}

// True if there is room for want_mb above the safety floor.
static int guard_ok(unsigned long want_mb)
{
    long f = real_free_mb();

    note_free(f);
    return f >= 0 && (unsigned long)f >= floor_mb + want_mb;
}

static VkResult alloc_mb(VkDeviceMemory *m, unsigned long mb)
{
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = (VkDeviceSize)mb << 20,
        .memoryTypeIndex = mem_type,
    };

    return vkAllocateMemory(dev, &mai, NULL, m);
}

// Puts a new random-sized block into slot i (which must be empty).
static void churn_alloc(struct worker *w, unsigned i)
{
    unsigned long mb = 1 + rand_r(&w->seed) % chunk_mb;

    if (!guard_ok(mb)) {
        w->guard_stops++;
        usleep(10000);
        return;
    }
    if (alloc_mb(&w->mem[i], mb) == VK_SUCCESS) {
        w->size_mb[i] = mb;
        w->held_mb += mb;
        w->churn_ok++;
    } else {
        w->mem[i] = VK_NULL_HANDLE;
        w->churn_denied++;
    }
}

// Free a random block, re-allocate one of random size in its place, and try
// one extra block, so the total keeps pressing against the limit.
static void churn(struct worker *w)
{
    double end = now_s() + churn_s;

    while (!atomic_load(&stop_all) && now_s() < end && w->count > 0) {
        unsigned i = rand_r(&w->seed) % w->count;

        if (w->mem[i] != VK_NULL_HANDLE) {
            vkFreeMemory(dev, w->mem[i], NULL);
            w->mem[i] = VK_NULL_HANDLE;
            w->held_mb -= w->size_mb[i];
        }
        churn_alloc(w, i);
        if (w->count < MAX_ALLOCS) {
            w->mem[w->count] = VK_NULL_HANDLE;
            churn_alloc(w, w->count);
            if (w->mem[w->count] != VK_NULL_HANDLE)
                w->count++;
        }
        usleep(500);
    }
}

static void *fill_thread(void *arg)
{
    struct worker *w = arg;

    pthread_barrier_wait(&start_barrier);
    while (!atomic_load(&stop_all) && w->count < MAX_ALLOCS) {
        if (!guard_ok(chunk_mb)) {
            w->guard_stops++;
            break;
        }
        if (alloc_mb(&w->mem[w->count], chunk_mb) != VK_SUCCESS) {
            w->denied++;
            note_free(real_free_mb());
            break;
        }
        w->size_mb[w->count++] = chunk_mb;
        w->held_mb += chunk_mb;
    }
    w->filled_mb = w->held_mb;
    if (churn_s)
        churn(w);
    return NULL;
}

static void *sampler_thread(void *arg)
{
    (void)arg;
    while (!atomic_load(&stop_all)) {
        note_free(real_free_mb());
        usleep(5000);
    }
    return NULL;
}

// Keeps the GPU busy: repeated fills of a LOAD_MB buffer allocated up front.
static void *load_thread(void *arg)
{
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb;
    VkFence fence = VK_NULL_HANDLE;
    VkQueue q;
    VkMemoryRequirements req;
    unsigned long *submits = arg;

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize)LOAD_MB << 20,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev, &bci, NULL, &buf) != VK_SUCCESS)
        goto fail;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    if (!(req.memoryTypeBits & (1u << mem_type)))
        goto fail;
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mem_type,
    };
    if (vkAllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS ||
        vkBindBufferMemory(dev, buf, mem, 0) != VK_SUCCESS)
        goto fail;

    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0,
    };
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandBufferCount = 1,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    };
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateCommandPool(dev, &pci, NULL, &pool) != VK_SUCCESS)
        goto fail;
    cbai.commandPool = pool;
    if (vkAllocateCommandBuffers(dev, &cbai, &cb) != VK_SUCCESS ||
        vkCreateFence(dev, &fci, NULL, &fence) != VK_SUCCESS)
        goto fail;
    vkBeginCommandBuffer(cb, &bi);
    for (unsigned i = 0; i < 16; i++)
        vkCmdFillBuffer(cb, buf, 0, VK_WHOLE_SIZE, i);
    vkEndCommandBuffer(cb);
    vkGetDeviceQueue(dev, 0, 0, &q);

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    while (!atomic_load(&stop_all)) {
        if (vkQueueSubmit(q, 1, &si, fence) != VK_SUCCESS)
            break;
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence);
        (*submits)++;
    }
    goto done;
fail:
    fprintf(stderr, "load: setup failed, running without GPU load\n");
done:
    if (fence)
        vkDestroyFence(dev, fence, NULL);
    if (pool)
        vkDestroyCommandPool(dev, pool, NULL);
    if (buf)
        vkDestroyBuffer(dev, buf, NULL);
    if (mem)
        vkFreeMemory(dev, mem, NULL);
    return NULL;
}

// What the driver tells applications (VK_EXT_memory_budget, which DXGI's
// QueryVideoMemoryInfo maps to under vkd3d-proton): this process's usage of
// the device-local heap and the budget it may grow to.
static void print_budget(const char *when)
{
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
    };
    VkPhysicalDeviceMemoryProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
        .pNext = &budget,
    };

    vkGetPhysicalDeviceMemoryProperties2(phys, &props);
    printf("BUDGET %s: usage %llu MB, budget %llu MB, heap %llu MB, nvml free %ld MB, pma free %ld MB\n", when,
           (unsigned long long)(budget.heapUsage[mem_heap] >> 20),
           (unsigned long long)(budget.heapBudget[mem_heap] >> 20),
           (unsigned long long)(props.memoryProperties.memoryHeaps[mem_heap].size >> 20),
           nvml_free_mb(), pma_free_mb());
}

static long reserve_mb_from_params(void)
{
    FILE *f = fopen("/proc/driver/nvidia/params", "r");
    char line[512];
    long mb = -1;

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "RmVidmemUserReserveMB=");
        if (strncmp(line, "RegistryDwords:", 15) == 0 && p)
            mb = strtol(p + strlen("RmVidmemUserReserveMB="), NULL, 0);
    }
    fclose(f);
    return mb;
}

static int setup_vulkan(VkInstance *inst)
{
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vram-hog",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    if (vkCreateInstance(&ici, NULL, inst) != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed\n");
        return -1;
    }

    uint32_t n = 16;
    VkPhysicalDevice pds[16], pd = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(*inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.vendorID == 0x10de) {
            pd = pds[i];
            phys = pd;
            printf("device: %s\n", p.deviceName);
            break;
        }
    }
    if (pd == VK_NULL_HANDLE) {
        fprintf(stderr, "no NVIDIA Vulkan device\n");
        return -1;
    }

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mem_type = i;
            break;
        }
    }
    if (mem_type == UINT32_MAX) {
        fprintf(stderr, "no device-local memory type\n");
        return -1;
    }
    mem_heap = mp.memoryTypes[mem_type].heapIndex;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDevice failed\n");
        return -1;
    }
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [--chunk-mb N] [--threads N] [--churn S] [--load] [--hold S] "
                    "[--floor-mb N] [--start-at T] [--force]\n", argv0);
    exit(2);
}

int main(int argc, char **argv)
{
    unsigned hold = 15, nthreads = 1;
    int force = 0, load = 0;
    double start_at = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chunk-mb") && i + 1 < argc)
            chunk_mb = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            nthreads = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--churn") && i + 1 < argc)
            churn_s = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc)
            hold = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--floor-mb") && i + 1 < argc)
            floor_mb = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--start-at") && i + 1 < argc)
            start_at = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--load"))
            load = 1;
        else if (!strcmp(argv[i], "--force"))
            force = 1;
        else
            usage(argv[0]);
    }
    if (chunk_mb == 0 || nthreads == 0 || nthreads > MAX_THREADS)
        usage(argv[0]);

    long reserve = reserve_mb_from_params();
    if (reserve <= 0 && !force) {
        fprintf(stderr, "RmVidmemUserReserveMB is not set in /proc/driver/nvidia/params.\n"
                        "Refusing to fill vidmem on a driver without the reserve (--force overrides).\n");
        return 1;
    }
    if (nvml_open() != 0) {
        fprintf(stderr, "cannot load NVML (libnvidia-ml.so.1); the safety floor needs it\n");
        return 1;
    }
    // If every thread passes the check at the same moment, free memory can
    // drop to reserve - (threads - 1) * chunk; the floor check races the same way.
    if ((long)((nthreads - 1) * chunk_mb) + 128 > reserve)
        fprintf(stderr, "warning: %u threads x %lu MB in flight could leave under 128 MB free "
                        "if they all race past the %ld MB reserve\n", nthreads, chunk_mb, reserve);

    VkInstance inst;
    if (setup_vulkan(&inst) != 0)
        return 1;

    if (start_at > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        double wait = start_at - (ts.tv_sec + ts.tv_nsec / 1e9);
        if (wait > 0 && wait < 600)
            usleep((useconds_t)(wait * 1e6));
    }

    print_budget("start");
    long start_free = real_free_mb();
    printf("driver reserve: %ld MB, free %ld MB, floor %lu MB, %u thread(s) x %lu MB%s%s\n",
           reserve, start_free, floor_mb, nthreads, chunk_mb,
           churn_s ? ", churn" : "", load ? ", GPU load" : "");
    fflush(stdout);

    static struct worker w[MAX_THREADS];
    pthread_t sampler, loader;
    unsigned long submits = 0;
    double t0 = now_s();

    pthread_create(&sampler, NULL, sampler_thread, NULL);
    if (load)
        pthread_create(&loader, NULL, load_thread, &submits);
    pthread_barrier_init(&start_barrier, NULL, nthreads);
    for (unsigned i = 0; i < nthreads; i++) {
        w[i].id = i;
        w[i].seed = (unsigned)time(NULL) ^ (i * 2654435761u);
        pthread_create(&w[i].tid, NULL, fill_thread, &w[i]);
    }

    unsigned long held = 0, filled = 0, denied = 0, churn_ok = 0, churn_denied = 0, guards = 0;
    for (unsigned i = 0; i < nthreads; i++) {
        pthread_join(w[i].tid, NULL);
        held += w[i].held_mb;
        filled += w[i].filled_mb;
        denied += w[i].denied;
        churn_ok += w[i].churn_ok;
        churn_denied += w[i].churn_denied;
        guards += w[i].guard_stops;
    }
    long end_free = real_free_mb();
    note_free(end_free);

    printf("filled %lu MB in %.1f s, holding %lu MB\n", filled, now_s() - t0, held);
    print_budget("full");
    printf("RESULT reserve_mb=%ld threads=%u chunk_mb=%lu start_free_mb=%ld filled_mb=%lu held_mb=%lu "
           "end_free_mb=%ld min_free_mb=%ld denied=%lu churn_ok=%lu churn_denied=%lu guard_stops=%lu "
           "load_submits=%lu\n",
           reserve, nthreads, chunk_mb, start_free, filled, held, end_free,
           atomic_load(&min_free_mb), denied, churn_ok, churn_denied, guards, submits);
    printf("holding for %u s, check that the desktop still responds...\n", hold);
    fflush(stdout);
    sleep(hold);

    atomic_store(&stop_all, 1);
    pthread_join(sampler, NULL);
    if (load)
        pthread_join(loader, NULL);
    for (unsigned i = 0; i < nthreads; i++)
        for (unsigned j = 0; j < w[i].count; j++)
            if (w[i].mem[j] != VK_NULL_HANDLE)
                vkFreeMemory(dev, w[i].mem[j], NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("freed\n");
    return 0;
}
