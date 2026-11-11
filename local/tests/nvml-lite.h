// nvml-lite.h: the three NVML calls the test tools need, loaded with dlopen so
// no NVML headers or link-time dependency are required. Single GPU (index 0).
#ifndef NVML_LITE_H
#define NVML_LITE_H

#include <dlfcn.h>
#include <glob.h>
#include <stdio.h>

typedef struct {
    unsigned long long total, free, used;
} nvml_lite_memory_t;

static void *nvml_lite_dev;
static int (*nvml_lite_get_memory)(void *dev, nvml_lite_memory_t *mem);

// Returns 0 on success.
static int nvml_open(void)
{
    void *h = dlopen("libnvidia-ml.so.1", RTLD_NOW);
    int (*init)(void);
    int (*by_index)(unsigned, void **);

    if (!h)
        return -1;
    init = (int (*)(void))dlsym(h, "nvmlInit_v2");
    by_index = (int (*)(unsigned, void **))dlsym(h, "nvmlDeviceGetHandleByIndex_v2");
    nvml_lite_get_memory = (int (*)(void *, nvml_lite_memory_t *))dlsym(h, "nvmlDeviceGetMemoryInfo");
    if (!init || !by_index || !nvml_lite_get_memory)
        return -1;
    if (init() != 0 || by_index(0, &nvml_lite_dev) != 0)
        return -1;
    return 0;
}

// Free vidmem in MB as NVML reports it, or -1.
static long nvml_free_mb(void)
{
    nvml_lite_memory_t m;

    if (!nvml_lite_get_memory || nvml_lite_get_memory(nvml_lite_dev, &m) != 0)
        return -1;
    return (long)(m.free >> 20);
}

// Free vidmem as PMA sees it, from the patched module's world-readable
// /proc/driver/nvidia/gpus/*/vidmem_pma (first field). NVML reports less to
// reserve-bound processes since the reserve is kept out of HEAP_FREE, so the
// tools' safety floors use this when it exists. -1 if unavailable.
static long pma_free_mb(void)
{
    static char path[256];
    long mb = -1;
    FILE *f;

    if (!path[0]) {
        glob_t g;
        if (glob("/proc/driver/nvidia/gpus/*/vidmem_pma", 0, NULL, &g) == 0 && g.gl_pathc > 0)
            snprintf(path, sizeof(path), "%s", g.gl_pathv[0]);
        else
            path[0] = '-';
        globfree(&g);
    }
    if (path[0] == '-' || !(f = fopen(path, "r")))
        return -1;
    if (fscanf(f, "%ld", &mb) != 1)
        mb = -1;
    fclose(f);
    return mb;
}

// Real free vidmem: PMA if the patched module provides it, else NVML.
static long real_free_mb(void)
{
    long mb = pma_free_mb();

    return mb >= 0 ? mb : nvml_free_mb();
}

#endif
