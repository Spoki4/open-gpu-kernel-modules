/*
 * Local patch (not upstream): trace events of the vidmem reserve / eviction
 * patch and of RM lock waits, for finding where a stutter comes from.
 *
 * RM fills up to VMT_ARGS 64-bit values per event; kernel-open turns them
 * into Linux tracepoints (kernel-open/nvidia/nv-vidmem-trace.h, system
 * "nvidia_vidmem"), which are free while disabled. Record them with
 * local/trace/nvtrace.py. The argument order of each type is documented
 * there and must match TP_fast_assign in nv-vidmem-trace.h.
 */
#ifndef VIDMEM_TRACE_H
#define VIDMEM_TRACE_H

#include "nvtypes.h"

#define VMT_ARGS 14

// Event types (os_vidmem_trace(), kernel-open/nvidia/nv-vidmem-trace.c)
#define VMT_MOVE      1   // one evict/restore batch of a process
#define VMT_PAUSE     2   // a process's channels were off: how long and how they stopped
#define VMT_RESERVE   3   // the user reserve turned an allocation away, and what happened then
#define VMT_MAKEROOM  4   // a visible process's allocation moved others out (it waited for it)
#define VMT_TICK      5   // one policy step (API + GPU locks held for all of it)
#define VMT_PROC      6   // per-process state at a policy step
#define VMT_LOCK      7   // a long RM lock wait or hold
#define VMT_BUDGET    8   // free vidmem reported to a process (HEAP_FREE: its Vulkan/GL budget)
#define VMT_FRAG      9   // how fragmented free vidmem is (periodic, and after each move batch)
#define VMT_NUM_TYPES 10

// VMT_MOVE why (the caller of the evict engine)
#define VMT_WHY_MANUAL    0   // procfs vidmem_evict (evict.sh)
#define VMT_WHY_AUTO      1   // sample policy (no hints)
#define VMT_WHY_PRESSURE  2   // hints: hidden processes out under pressure
#define VMT_WHY_ALLOC     3   // hints: a visible process's allocation (vidmemEvictMakeRoom)
#define VMT_WHY_RESTORE   4   // hints: a foreground/visible process back to vidmem
#define VMT_WHY_ROOM      5   // hints: others out to make room for that restore
#define VMT_WHY_IDLE_BACK 6   // restored after pressure is gone
#define VMT_WHY_HOT       7   // access counters: one hot allocation back
#define VMT_WHY_ACCESS    8   // restore on access (CPU map, scanout, ...)
#define VMT_WHY_BORN      9   // new allocation created in sysmem
#define VMT_WHY_COMPACT   10  // background compaction: moved inside vidmem to lengthen the free run

// VMT_RESERVE outcome
#define VMT_RES_DENIED    0
#define VMT_RES_ROOM      1   // room was made, allocation went ahead in vidmem
#define VMT_RES_SYSMEM    2   // allocation created in sysmem instead

// VMT_LOCK lock
#define VMT_LOCK_API      0
#define VMT_LOCK_GPU      1

// Clock of every traced duration: osGetMonotonicTimeNs() counts jiffies (1 ms
// at HZ=1000), too coarse for lock waits and short pauses.
NvU64 portTimeGetUptimeNanosecondsHighPrecision(void);
#define VMT_NOW() portTimeGetUptimeNanosecondsHighPrecision()

NvBool osVidmemTraceOn(NvU32 type);
void   osVidmemTrace(NvU32 type, NvU32 pid, const NvU64 *pArgs);

// Lock waits/holds shorter than this are not traced (evict.sh trace-lock US).
extern NvU64 vidmemTraceLockNs;

#endif // VIDMEM_TRACE_H
