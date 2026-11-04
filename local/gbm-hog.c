// gbm-hog: allocates window-sized buffers through GBM (nvidia-drm_gbm.so) or as
// DRM dumb buffers, to check whether these paths are bound by the vidmem user
// reserve (RmVidmemUserReserveMB). Allocations that nvidia-drm makes through
// nvkms run with kernel privilege, which the reserve exempts.
//
// Build: ./build.sh tools (no gbm.h needed; links libgbm.so.1 directly).
// Usage: gbm-hog [--node PATH] [--size WxH] [--scanout] [--linear] [--dumb]
//                [--max N] [--floor-mb N] [--hold S]
//
//   --node PATH   DRM node (default: the NVIDIA render node; --dumb defaults
//                 to the NVIDIA primary node, dumb buffers need it)
//   --size WxH    buffer size (default 3840x2160, ARGB8888)
//   --scanout     GBM_BO_USE_SCANOUT (what a compositor or fullscreen app asks for)
//   --linear      GBM_BO_USE_LINEAR
//   --dumb        DRM_IOCTL_MODE_CREATE_DUMB instead of GBM
//   --max N       stop after N buffers (default 400)
//
// It also stops when 5 buffers in a row did not take vidmem: nvidia-drm has
// fallen back to sysmem (NO_SCANOUT only), and going on would fill system RAM.
//   --floor-mb N  safety stop: never allocate while NVML reports less than
//                 N MB + one buffer free (default 150; keep it below the
//                 reserve so a bypass shows up as "floor" instead of a denial)
//
// Prints "RESULT key=value ..." before holding.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "nvml-lite.h"

// The parts of gbm.h used here (stable ABI).
struct gbm_device;
struct gbm_bo;
struct gbm_device *gbm_create_device(int fd);
void gbm_device_destroy(struct gbm_device *gbm);
struct gbm_bo *gbm_bo_create(struct gbm_device *gbm, uint32_t width, uint32_t height,
                             uint32_t format, uint32_t flags);
void gbm_bo_destroy(struct gbm_bo *bo);
uint32_t gbm_bo_get_stride(struct gbm_bo *bo);
#define GBM_FORMAT_ARGB8888 0x34325241u /* 'AR24' */
#define GBM_BO_USE_SCANOUT   (1u << 0)
#define GBM_BO_USE_RENDERING (1u << 2)
#define GBM_BO_USE_LINEAR    (1u << 4)

// The parts of drm_mode.h used here.
struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags;
    uint32_t handle, pitch;
    uint64_t size;
};
#define DRM_IOCTL_MODE_CREATE_DUMB _IOWR('d', 0xB2, struct drm_mode_create_dumb)

#define NV_RENDER "/dev/dri/by-path/pci-0000:01:00.0-render"
#define NV_CARD   "/dev/dri/by-path/pci-0000:01:00.0-card"

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

int main(int argc, char **argv)
{
    const char *node = NULL;
    unsigned w = 3840, h = 2160, max = 400, hold = 10;
    unsigned long floor_mb = 150;
    uint32_t flags = GBM_BO_USE_RENDERING;
    int dumb = 0, force = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--node") && i + 1 < argc)
            node = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &w, &h) != 2)
                goto usage;
        } else if (!strcmp(argv[i], "--scanout"))
            flags |= GBM_BO_USE_SCANOUT;
        else if (!strcmp(argv[i], "--linear"))
            flags |= GBM_BO_USE_LINEAR;
        else if (!strcmp(argv[i], "--dumb"))
            dumb = 1;
        else if (!strcmp(argv[i], "--max") && i + 1 < argc)
            max = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--floor-mb") && i + 1 < argc)
            floor_mb = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc)
            hold = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--force"))
            force = 1;
        else
            goto usage;
    }
    if (!node)
        node = dumb ? NV_CARD : NV_RENDER;

    long reserve = reserve_mb_from_params();
    if (reserve <= 0 && !force) {
        fprintf(stderr, "RmVidmemUserReserveMB is not set; refusing to run (--force overrides)\n");
        return 1;
    }
    if (nvml_open() != 0) {
        fprintf(stderr, "cannot load NVML (libnvidia-ml.so.1); the safety floor needs it\n");
        return 1;
    }

    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", node, strerror(errno));
        return 1;
    }
    struct gbm_device *gbm = NULL;
    if (!dumb && !(gbm = gbm_create_device(fd))) {
        fprintf(stderr, "gbm_create_device failed\n");
        return 1;
    }

    struct gbm_bo **bos = calloc(max, sizeof(*bos));
    unsigned long bo_mb = ((unsigned long)w * h * 4 + (1 << 20) - 1) >> 20;
    long start_free = real_free_mb(), min_free = start_free;
    unsigned count = 0, no_vram = 0;
    int guard = 0, err = 0;
    const char *why = "max";

    printf("reserve %ld MB, free %ld MB, floor %lu MB, %s %ux%u%s%s on %s (~%lu MB each)\n",
           reserve, start_free, floor_mb, dumb ? "dumb" : "gbm", w, h,
           (flags & GBM_BO_USE_SCANOUT) ? " scanout" : "",
           (flags & GBM_BO_USE_LINEAR) ? " linear" : "", node, bo_mb);
    fflush(stdout);

    while (count < max) {
        long f = real_free_mb();
        if (f < min_free)
            min_free = f;
        // --floor-mb 0: no guard at all (no reserve: the driver has to make room).
        if (floor_mb != 0 && (f < 0 || (unsigned long)f < floor_mb + bo_mb)) {
            guard = 1;
            why = "floor";
            break;
        }
        if (dumb) {
            struct drm_mode_create_dumb req = { .height = h, .width = w, .bpp = 32 };
            if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &req) != 0) {
                err = errno;
                why = "denied";
                break;
            }
            if (count == 0)
                printf("dumb: pitch %u, size %llu KB\n", req.pitch,
                       (unsigned long long)(req.size >> 10));
        } else {
            bos[count] = gbm_bo_create(gbm, w, h, GBM_FORMAT_ARGB8888, flags);
            if (!bos[count]) {
                err = errno;
                why = "denied";
                break;
            }
            if (count == 0)
                printf("gbm: stride %u\n", gbm_bo_get_stride(bos[0]));
        }
        count++;
        // nvidia-drm falls back to sysmem for NO_SCANOUT buffers once vidmem
        // is refused; stop then instead of filling system RAM. Not for scanout
        // buffers (no fallback for them; without the reserve the driver frees
        // more than each takes by moving others out).
        long after = real_free_mb();
        if (!(flags & GBM_BO_USE_SCANOUT) && after >= 0 && f - after < (long)bo_mb / 2) {
            if (++no_vram >= 5) {
                why = "sysmem-fallback";
                break;
            }
        } else {
            no_vram = 0;
        }
        if (count % 20 == 0) {
            printf("  %u buffers, free %ld MB\n", count, real_free_mb());
            fflush(stdout);
        }
    }
    long end_free = real_free_mb();
    if (end_free < min_free)
        min_free = end_free;

    printf("stopped after %u buffers: %s%s%s\n", count, why, err ? ", " : "", err ? strerror(err) : "");
    printf("RESULT mode=%s scanout=%d linear=%d reserve_mb=%ld start_free_mb=%ld end_free_mb=%ld "
           "min_free_mb=%ld used_mb=%ld count=%u stop=%s guard=%d\n",
           dumb ? "dumb" : "gbm", !!(flags & GBM_BO_USE_SCANOUT), !!(flags & GBM_BO_USE_LINEAR),
           reserve, start_free, end_free, min_free, start_free - end_free, count, why, guard);
    printf("holding for %u s...\n", hold);
    fflush(stdout);
    sleep(hold);

    if (!dumb) {
        for (unsigned i = 0; i < count; i++)
            gbm_bo_destroy(bos[i]);
        gbm_device_destroy(gbm);
    }
    close(fd); // also frees dumb buffers
    printf("freed\n");
    return 0;

usage:
    fprintf(stderr, "usage: %s [--node PATH] [--size WxH] [--scanout] [--linear] [--dumb] "
                    "[--max N] [--floor-mb N] [--hold S]\n", argv[0]);
    return 2;
}
