/*
 * Local patch (not upstream): tracepoints of the vidmem reserve / eviction
 * patch, system "nvidia_vidmem". RM side: src/nvidia/inc/kernel/mem_mgr/
 * vidmem_trace.h (event types, argument order); recorder and report:
 * local/trace/nvtrace.py. Fields are key=value so the text trace parses
 * without the format files. Times in microseconds, sizes in KB or MB as
 * named.
 */

#include "conftest.h"

#undef TRACE_SYSTEM
#define TRACE_SYSTEM nvidia_vidmem

#if !defined(_TRACE_NV_VIDMEM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_NV_VIDMEM_H

#include <linux/tracepoint.h>

#define NV_VMT_ARGS 14

#define NV_VMT_WHY                                                           \
    { 0, "manual" }, { 1, "auto" }, { 2, "pressure" }, { 3, "alloc" },       \
    { 4, "restore" }, { 5, "room" }, { 6, "idle-back" }, { 7, "hot" },       \
    { 8, "access" }, { 9, "born" }

#define NV_VMT_HINT { 0, "-" }, { 1, "h" }, { 2, "v" }, { 3, "f" }

// One evict (op=evict) or restore (op=restore) batch of process pid.
// a: op(1 evict, 2 restore), why, bytes, allocs, failed, shared, channels,
//    total ns, quiesce, sysmem alloc, copy, remap, pma, sysmem free (ns)
TRACE_EVENT(vidmem_move,
    TP_PROTO(u32 pid, const char *comm, const u64 *a),
    TP_ARGS(pid, comm, a),
    TP_STRUCT__entry(
        __field(u32, pid)
        __array(char, comm, TASK_COMM_LEN)
        __field(u32, op)
        __field(u32, why)
        __field(u64, kb)
        __field(u32, allocs)
        __field(u32, failed)
        __field(u32, shared)
        __field(u32, channels)
        __field(u64, total_us)
        __field(u64, quiesce_us)
        __field(u64, sysalloc_us)
        __field(u64, copy_us)
        __field(u64, remap_us)
        __field(u64, pma_us)
        __field(u64, sysfree_us)
    ),
    TP_fast_assign(
        __entry->pid = pid;
        memcpy(__entry->comm, comm, TASK_COMM_LEN);
        __entry->op = a[0];
        __entry->why = a[1];
        __entry->kb = a[2] >> 10;
        __entry->allocs = a[3];
        __entry->failed = a[4];
        __entry->shared = a[5];
        __entry->channels = a[6];
        __entry->total_us = a[7] / 1000;
        __entry->quiesce_us = a[8] / 1000;
        __entry->sysalloc_us = a[9] / 1000;
        __entry->copy_us = a[10] / 1000;
        __entry->remap_us = a[11] / 1000;
        __entry->pma_us = a[12] / 1000;
        __entry->sysfree_us = a[13] / 1000;
    ),
    TP_printk("op=%s pid=%u comm=%s why=%s kb=%llu allocs=%u failed=%u shared=%u channels=%u "
              "total_us=%llu quiesce_us=%llu sysalloc_us=%llu copy_us=%llu remap_us=%llu "
              "pma_us=%llu sysfree_us=%llu",
              (__entry->op == 1) ? "evict" : "restore", __entry->pid, __entry->comm,
              __print_symbolic(__entry->why, NV_VMT_WHY), __entry->kb, __entry->allocs,
              __entry->failed, __entry->shared, __entry->channels, __entry->total_us,
              __entry->quiesce_us, __entry->sysalloc_us, __entry->copy_us, __entry->remap_us,
              __entry->pma_us, __entry->sysfree_us)
);

// Channels of process pid were off for paused_us (its GPU work stood still).
// a: quiesce mode, channels, tsgs, preempt ok, preempt refused, stop ns,
//    paused ns, status
TRACE_EVENT(vidmem_pause,
    TP_PROTO(u32 pid, const char *comm, const u64 *a),
    TP_ARGS(pid, comm, a),
    TP_STRUCT__entry(
        __field(u32, pid)
        __array(char, comm, TASK_COMM_LEN)
        __field(u32, mode)
        __field(u32, channels)
        __field(u32, tsgs)
        __field(u32, preempt_ok)
        __field(u32, preempt_refused)
        __field(u64, stop_us)
        __field(u64, paused_us)
        __field(u32, status)
    ),
    TP_fast_assign(
        __entry->pid = pid;
        memcpy(__entry->comm, comm, TASK_COMM_LEN);
        __entry->mode = a[0];
        __entry->channels = a[1];
        __entry->tsgs = a[2];
        __entry->preempt_ok = a[3];
        __entry->preempt_refused = a[4];
        __entry->stop_us = a[5] / 1000;
        __entry->paused_us = a[6] / 1000;
        __entry->status = a[7];
    ),
    TP_printk("pid=%u comm=%s mode=%u channels=%u tsgs=%u preempt_ok=%u preempt_refused=%u "
              "stop_us=%llu paused_us=%llu status=0x%x",
              __entry->pid, __entry->comm, __entry->mode, __entry->channels, __entry->tsgs,
              __entry->preempt_ok, __entry->preempt_refused, __entry->stop_us,
              __entry->paused_us, __entry->status)
);

// The user reserve turned away an allocation of pid (the RM client's process).
// a: bytes, free bytes, of it in the reserve block, outcome, kernel client
TRACE_EVENT(vidmem_reserve,
    TP_PROTO(u32 pid, const char *comm, const u64 *a),
    TP_ARGS(pid, comm, a),
    TP_STRUCT__entry(
        __field(u32, pid)
        __array(char, comm, TASK_COMM_LEN)
        __field(u64, kb)
        __field(u64, free_mb)
        __field(u64, block_free_mb)
        __field(u32, outcome)
        __field(u32, kernel)
    ),
    TP_fast_assign(
        __entry->pid = pid;
        memcpy(__entry->comm, comm, TASK_COMM_LEN);
        __entry->kb = a[0] >> 10;
        __entry->free_mb = a[1] >> 20;
        __entry->block_free_mb = a[2] >> 20;
        __entry->outcome = a[3];
        __entry->kernel = a[4];
    ),
    TP_printk("pid=%u comm=%s kb=%llu free_mb=%llu block_free_mb=%llu outcome=%s kernel=%u",
              __entry->pid, __entry->comm, __entry->kb, __entry->free_mb, __entry->block_free_mb,
              __print_symbolic(__entry->outcome, { 0, "denied" }, { 1, "room" }, { 2, "sysmem" }),
              __entry->kernel)
);

// vidmemEvictMakeRoom() for an allocation of pid: the allocating thread
// waited us for it. a: bytes, wanted MB, avail MB after, ok, ns, level
TRACE_EVENT(vidmem_makeroom,
    TP_PROTO(u32 pid, const char *comm, const u64 *a),
    TP_ARGS(pid, comm, a),
    TP_STRUCT__entry(
        __field(u32, pid)
        __array(char, comm, TASK_COMM_LEN)
        __field(u64, kb)
        __field(u64, want_mb)
        __field(u64, avail_mb)
        __field(u32, ok)
        __field(u64, us)
        __field(u32, level)
    ),
    TP_fast_assign(
        __entry->pid = pid;
        memcpy(__entry->comm, comm, TASK_COMM_LEN);
        __entry->kb = a[0] >> 10;
        __entry->want_mb = a[1];
        __entry->avail_mb = a[2];
        __entry->ok = a[3];
        __entry->us = a[4] / 1000;
        __entry->level = a[5];
    ),
    TP_printk("pid=%u comm=%s kb=%llu want_mb=%llu avail_mb=%llu ok=%u us=%llu level=%u",
              __entry->pid, __entry->comm, __entry->kb, __entry->want_mb, __entry->avail_mb,
              __entry->ok, __entry->us, __entry->level)
);

// One policy step. a: ns, queue ns (1 Hz timer to start: waiting for the API
// lock), avail MB (free outside the block), block free MB, block largest
// contiguous free MB, evicted MB, busiest pid, hints active, processes,
// trigger (0 timer, 1 hints)
TRACE_EVENT(vidmem_tick,
    TP_PROTO(u32 pid, const char *comm, const u64 *a),
    TP_ARGS(pid, comm, a),
    TP_STRUCT__entry(
        __field(u64, us)
        __field(u64, queue_us)
        __field(u64, avail_mb)
        __field(u64, block_free_mb)
        __field(u64, block_largest_mb)
        __field(u64, evicted_mb)
        __field(u32, busiest)
        __field(u32, hints)
        __field(u32, procs)
        __field(u32, trigger)
    ),
    TP_fast_assign(
        __entry->us = a[0] / 1000;
        __entry->queue_us = a[1] / 1000;
        __entry->avail_mb = a[2];
        __entry->block_free_mb = a[3];
        __entry->block_largest_mb = a[4];
        __entry->evicted_mb = a[5];
        __entry->busiest = a[6];
        __entry->hints = a[7];
        __entry->procs = a[8];
        __entry->trigger = a[9];
    ),
    TP_printk("us=%llu queue_us=%llu avail_mb=%llu block_free_mb=%llu block_largest_mb=%llu "
              "evicted_mb=%llu busiest=%u hints=%u procs=%u trigger=%s",
              __entry->us, __entry->queue_us, __entry->avail_mb, __entry->block_free_mb,
              __entry->block_largest_mb, __entry->evicted_mb, __entry->busiest, __entry->hints,
              __entry->procs, __entry->trigger ? "hints" : "timer")
);

// A process with GPU memory at a policy step. a: vidmem bytes, evicted bytes,
// eligible bytes, hint, idle s (GPU samples, ~0 = busy), recent samples,
// s since last work submission, never-evict
TRACE_EVENT(vidmem_proc,
    TP_PROTO(u32 pid, const char *comm, const u64 *a),
    TP_ARGS(pid, comm, a),
    TP_STRUCT__entry(
        __field(u32, pid)
        __array(char, comm, TASK_COMM_LEN)
        __field(u64, vram_mb)
        __field(u64, ram_mb)
        __field(u64, eligible_mb)
        __field(u32, hint)
        __field(u32, inherited)
        __field(u32, idle_s)
        __field(u32, samples)
        __field(u32, submit_s)
        __field(u32, never)
    ),
    TP_fast_assign(
        __entry->pid = pid;
        memcpy(__entry->comm, comm, TASK_COMM_LEN);
        __entry->vram_mb = a[0] >> 20;
        __entry->ram_mb = a[1] >> 20;
        __entry->eligible_mb = a[2] >> 20;
        __entry->hint = a[3] & 0xff;
        __entry->inherited = (a[3] >> 8) & 0x3;
        __entry->idle_s = a[4];
        __entry->samples = a[5];
        __entry->submit_s = a[6];
        __entry->never = a[7];
    ),
    TP_printk("pid=%u comm=%s vram_mb=%llu ram_mb=%llu eligible_mb=%llu hint=%s inherited=%u "
              "idle_s=%u samples=%u submit_s=%u never=%u",
              __entry->pid, __entry->comm, __entry->vram_mb, __entry->ram_mb, __entry->eligible_mb,
              __print_symbolic(__entry->hint, NV_VMT_HINT), __entry->inherited, __entry->idle_s,
              __entry->samples, __entry->submit_s, __entry->never)
);

// An RM lock waited for or held long (>= the evict.sh trace-lock threshold),
// by the current thread. a: lock (0 api, 1 gpu), write, wait ns, hold ns,
// where it was taken, thread holding it when the wait began, where that one
// took it
TRACE_EVENT(rm_lock,
    TP_PROTO(u32 pid, const char *comm, const u64 *a),
    TP_ARGS(pid, comm, a),
    TP_STRUCT__entry(
        __field(u32, lock)
        __field(u32, write)
        __field(u64, wait_us)
        __field(u64, hold_us)
        __field(void *, at)
        __field(u32, holder)
        __field(void *, holder_at)
    ),
    TP_fast_assign(
        __entry->lock = a[0];
        __entry->write = a[1];
        __entry->wait_us = a[2] / 1000;
        __entry->hold_us = a[3] / 1000;
        __entry->at = (void *)(uintptr_t)a[4];
        __entry->holder = a[5];
        __entry->holder_at = (void *)(uintptr_t)a[6];
    ),
    TP_printk("lock=%s mode=%s wait_us=%llu hold_us=%llu at=%pS holder=%u holder_at=%pS",
              __entry->lock ? "gpu" : "api", __entry->write ? "w" : "r", __entry->wait_us,
              __entry->hold_us, __entry->at, __entry->holder, __entry->holder_at)
);

// Free vidmem reported to process pid (NV2080 FB_INFO HEAP_FREE: what its
// Vulkan/DXGI budget and its GL driver's placement are based on).
// a: real free bytes, of it in the reserve block, reclaimable from others
//    (window hints) added for it, reported bytes
TRACE_EVENT(vidmem_budget,
    TP_PROTO(u32 pid, const char *comm, const u64 *a),
    TP_ARGS(pid, comm, a),
    TP_STRUCT__entry(
        __field(u32, pid)
        __array(char, comm, TASK_COMM_LEN)
        __field(u64, free_mb)
        __field(u64, block_free_mb)
        __field(u64, reclaim_mb)
        __field(u64, reported_mb)
    ),
    TP_fast_assign(
        __entry->pid = pid;
        memcpy(__entry->comm, comm, TASK_COMM_LEN);
        __entry->free_mb = a[0] >> 20;
        __entry->block_free_mb = a[1] >> 20;
        __entry->reclaim_mb = a[2] >> 20;
        __entry->reported_mb = a[3] >> 20;
    ),
    TP_printk("pid=%u comm=%s free_mb=%llu block_free_mb=%llu reclaim_mb=%llu reported_mb=%llu",
              __entry->pid, __entry->comm, __entry->free_mb, __entry->block_free_mb,
              __entry->reclaim_mb, __entry->reported_mb)
);

// Window state of a process changed in the compositor's hints (vidmem_hints).
TRACE_EVENT(vidmem_hint,
    TP_PROTO(u32 pid, const char *comm, u32 old_state, u32 new_state),
    TP_ARGS(pid, comm, old_state, new_state),
    TP_STRUCT__entry(
        __field(u32, pid)
        __array(char, comm, TASK_COMM_LEN)
        __field(u32, old_state)
        __field(u32, new_state)
    ),
    TP_fast_assign(
        __entry->pid = pid;
        memcpy(__entry->comm, comm, TASK_COMM_LEN);
        __entry->old_state = old_state;
        __entry->new_state = new_state;
    ),
    TP_printk("pid=%u comm=%s old=%s new=%s", __entry->pid, __entry->comm,
              __print_symbolic(__entry->old_state, NV_VMT_HINT),
              __print_symbolic(__entry->new_state, NV_VMT_HINT))
);

#endif // !defined(_TRACE_NV_VIDMEM_H) || defined(TRACE_HEADER_MULTI_READ)

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE nv-vidmem-trace
#include <trace/define_trace.h>
