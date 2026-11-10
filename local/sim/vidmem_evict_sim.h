/*
 * What vidmem_evict_policy.c takes from RM, for the simulator (sim.c): the
 * policy file includes this instead of the RM headers when built with
 * VIDMEM_EVICT_SIM. Time is the simulator's clock.
 */
#ifndef VIDMEM_EVICT_SIM_H
#define VIDMEM_EVICT_SIM_H

#include <stdlib.h>
#include <string.h>
#include "nvtypes.h"
#include "nvmisc.h"
#include "nvstatus.h"

typedef struct OBJGPU OBJGPU;

#define LEVEL_ERROR 4
void simPrintf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define NV_PRINTF(level, ...) simPrintf(level, __VA_ARGS__)

NvU64 osGetMonotonicTimeNs(void);

static inline void *portMemAllocNonPaged(size_t n) { return calloc(1, n); }
static inline void portMemFree(void *p) { free(p); }
static inline void portMemSet(void *p, NvU8 v, size_t n) { memset(p, v, n); }
static inline void *portMemCopy(void *d, size_t dn, const void *s, size_t sn) { return memcpy(d, s, sn < dn ? sn : dn); }
static inline int portStringCompare(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static inline void portStringCopy(char *dst, size_t dstSize, const char *src, size_t srcSize)
{
    size_t n = strnlen(src, srcSize);

    if (dstSize == 0)
        return;
    if (n >= dstSize)
        n = dstSize - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

#endif // VIDMEM_EVICT_SIM_H
