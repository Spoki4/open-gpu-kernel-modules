/*
 * Local patch (not upstream): the eviction policy of vidmem_evict.c, split
 * out so that the same code also builds outside the kernel, in the policy
 * simulator (local/sim/): who is moved out, when it comes back, how much is
 * taken. It decides; the platform (vidmem_evict.c in the driver, the memory
 * model in the simulator) walks the clients, moves memory and reports free
 * memory through the vepPlat*() functions below.
 *
 * Built with VIDMEM_EVICT_SIM defined, it takes the RM types and helpers it
 * uses (NV_PRINTF, portMem*, osGetMonotonicTimeNs, ...) from the
 * simulator's shim header instead of the RM headers.
 */
#ifndef VIDMEM_EVICT_POLICY_H
#define VIDMEM_EVICT_POLICY_H

#ifdef VIDMEM_EVICT_SIM
#include "vidmem_evict_sim.h"
#else
#include "core/core.h"
#include "os/os.h"
#include "gpu/gpu.h"
#endif
#include "nvlimits.h"
#include "mem_mgr/vidmem_trace.h"

#define VIDMEM_EVICT_TICK_SEC       2
#define VIDMEM_EVICT_IDLE_SEC       10
#define VIDMEM_EVICT_QUIET_SEC      30
// A hidden process comes back by itself only after it would have fitted for this long.
#define VIDMEM_EVICT_HIDDEN_BACK_SEC 300
// Background compaction: the free run to keep (MB; knob "compact", 0: off;
// three quarters of the free memory when that is more), MB moved per policy
// step (each step holds the RM locks for its copy and pauses the owners), and
// the pause after a step that had nothing to do (doubles up to 15 min while
// nothing changes).
#define VIDMEM_EVICT_COMPACT_RUN_MB   1024
#define VIDMEM_EVICT_COMPACT_STEP_MB  32
// (not a round number: a 30 s rest met a process that works every 30 s each time)
#define VIDMEM_EVICT_COMPACT_REST_SEC 14
extern NvU32 _vidmemCompactRunMB;
#define VIDMEM_EVICT_HYST_MB        512
#define VIDMEM_EVICT_MAX_PROCS      128
#define VIDMEM_EVICT_MAX_PER_TICK   2

//
// How long ago `then` was, seen from `now` (ns); 0 when `then` is not before
// `now`. A policy step takes `now` once at its start, and a move during the
// step sets timestamps after it: a plain `now - then` wraps to ~584 years
// there, and "moved out a moment ago" read as "long ago" (BACKLOG #9: ghostty
// moved out for the game and brought back in the same step, 449 times).
//
#define VE_AGO_NS(now, then) (((then) >= (now)) ? 0ULL : ((now) - (then)))

typedef struct
{
    NvU32 pid;
    NvU64 lastActiveTs;   // GPU timestamp (ns) of the last sample on an engine
    NvU64 firstSeenTs;    // GPU timestamp when the process was first seen
    NvU64 lastRestoreNs;  // monotonic ns of its last restore on access
    NvU64 lastBusiestNs;  // monotonic ns when it was last the busiest process
    NvU64 vidmemBytes;    // its vidmem at the last client walk (policy tick)
    NvU64 vidmemAtNs;     // when vidmemBytes was taken (monotonic ns)
    NvU32 recentSamples;  // samples in the last ~10 s (decayed per tick)
    NvU64 firstSeenNs;    // monotonic ns when the policy first saw it
    NvU64 lastVisibleNs;  // monotonic ns when a hint last said visible/foreground
    NvU64 evictedForFgNs; // monotonic ns when it was moved out (while visible) for the foreground
    NvU64 hiddenSinceNs;  // monotonic ns when its hint last turned hidden (0: not hidden)
    NvU64 submitSig;      // signature of its channels' GP_PUT at the last check
    NvU64 lastSubmitNs;   // monotonic ns when that signature last changed (work submitted)
    NvU64 lastFgNs;       // monotonic ns when a hint last said foreground
    NvU64 drawingSinceNs; // monotonic ns since when it has drawn at every policy step (0: not drawing)
    NvU64 lastStuckLogNs; // monotonic ns of the last "foreground, only N MB free" line
    NvU64 fgSinceNs;      // monotonic ns since when a hint says foreground without a break (0: not)
    NvU64 restoreStuckMB; // free MB when a restore of it last moved nothing (0: not stuck)
    NvU64 contigFailNs;   // monotonic ns when no window could be cleared for its contiguous buffer (0: not)
    NvU32 outForPid;      // the foreground when the policy last moved it out (0: there was none)
    NvU64 roomSinceNs;    // monotonic ns since when it would fit back with room to spare (0: it would not)
} VIDMEM_EVICT_ACTIVITY;

//
// A contiguous buffer this big comes back through a cleared window, one a
// step; smaller ones as any other memory, room for them is made at once.
// Without the limit every contiguous buffer counted: a Chrome whose WebGL
// memory is 6 MB contiguous buffers (1.3 GB of them) got room for 256 MB a
// step and came back in 11 s instead of 0.7 s, its last 30 MB never (nvtest
// fgprio on the RTX 5000, 2026-11-11; the host's Chrome buffers that looped
// were 130-192 MB, BACKLOG #15).
//
#define VIDMEM_EVICT_CONTIG_WINDOW_BYTES (32ULL << 20)

typedef struct
{
    NvU32 pid;
    char  name[NV_PROC_NAME_MAX_LENGTH];
    NvU32 clients;
    NvU64 vidmemBytes;    // owned, in vidmem
    NvU64 evictedBytes;   // owned, moved to sysmem
    NvU64 eligibleBytes;  // could be moved now
    NvU64 evictedContigBytes; // of evictedBytes: contiguous buffers of VIDMEM_EVICT_CONTIG_WINDOW_BYTES or more
    NvU64 evictedContigMax;   // the biggest of them, and the alignment it comes back with
    NvU64 evictedContigAlign;
    NvBool bNever;
    NvU32 hint;           // VE_HINT_*, | VE_HINT_INHERITED (from an ancestor's windows)
} VIDMEM_EVICT_PROC;

//
// Window state hints (procfs vidmem_hints, written by the KWin script
// local/kwin-vram-hints; os_get_vidmem_hint()). Once a snapshot has been
// written the policy runs on them instead of the GPU samples:
//   - foreground and visible processes are never moved out; when one has
//     memory in sysmem it is restored at once, hidden ones making room;
//   - hidden processes (and idle ones without any window) are moved out
//     under pressure, the one hidden longest first;
//   - an allocation of a visible process that the reserve would deny moves
//     hidden processes out first (vidmemEvictMakeRoom).
//
#define VE_HINT_UNKNOWN   0
#define VE_HINT_HIDDEN    1
#define VE_HINT_VISIBLE   2
#define VE_HINT_FG        3
#define VE_HINT_STATE(h)  ((h) & 0xff)
#define VE_HINT_INHERITED 0x100
#define VE_HINT_OTHER_APP 0x200   // inherited from an ancestor with another name
#define VE_HINT_FULLSCREEN 0x400  // has a fullscreen window (the KWin script's "g")
#define VE_HINT_LAUNCHED_GAME 0x800 // started by a game launcher (Steam's reaper) or GameMode
// A process that inherits "hidden" from an ancestor of another name (no
// window of its own) is left alone in its first two minutes: a game started
// from Steam has no window while it loads. Helper processes of the same app
// (Chrome's GPU process under the browser) get no such grace.
#define VIDMEM_EVICT_INHERIT_GRACE_SEC 120

NvU32 osGetVidmemHint(NvU32 pid, NvU32 *pGeneration);   // arch/nvalloc/unix/src/os.c

//
// Who may give up memory (VE_LEVEL_*), see _vidmemEvictHintVictimOk():
//   HIDDEN      hidden and windowless processes (pressure, visible apps)
//   FG_IDLE     + visible idle ones (the foreground's budget, allocations)
//   FG_RESTORE  + visible ones still drawing, only to bring back the
//               foreground's own memory
//   COMPOSITOR  anyone but the desktop itself, the foreground last
//   FG_ALLOC    + visible ones still drawing (a game on the other monitor,
//               a video), only for what an allocation of the foreground
//               needs right now (BACKLOG #7, R): numbered after COMPOSITOR,
//               whose 3 is in recorded traces
//
#define VE_LEVEL_HIDDEN     0
#define VE_LEVEL_FG_IDLE    1
#define VE_LEVEL_FG_RESTORE 2
#define VE_LEVEL_COMPOSITOR 3
#define VE_LEVEL_FG_ALLOC   4

// Free-memory targets with hints and the margin of a restore (policy.c).
#define VIDMEM_EVICT_HINT_LOW_MB  256
#define VIDMEM_EVICT_HINT_HYST_MB 256
#define VIDMEM_EVICT_RESTORE_MARGIN_MB 64

// 0 off, 1 on, 2 (default) only while window hints are present.
#define VIDMEM_AUTO_HINTS 2

//
// Policy state (vidmem_evict_policy.c), also read by the driver's status
// report and set by its registry/procfs knobs.
//
extern VIDMEM_EVICT_ACTIVITY _vidmemActivity[VIDMEM_EVICT_MAX_PROCS];
extern NvU64  _vidmemActivityNewestTs;
extern NvU32  _vidmemAutoEnabled;
extern NvU32  _vidmemLowMB, _vidmemHighMB;
extern NvU64  _vidmemLastPressureNs;
extern NvU32  _vidmemBusiestPid;
extern NvU64  _vidmemBusiestSinceNs;
extern NvU64  _vidmemBornBytes;
extern NvU32  _vidmemBornAllocs, _vidmemBornFailed;
extern NvU64  _vidmemReclaimableBytes, _vidmemReclaimableFgBytes;
extern NvU32  _vidmemFgHeadroomMB, _vidmemBgCushionMB, _vidmemFgOwnEnabled;
extern NvU64  _vidmemPrevFgNs;          // lastFgNs of the window focused before the current one
// Set by the policy around a move, read by the engine (vidmem_evict.c).
extern NvU32  _vidmemEvictWhy;          // VMT_WHY_* of the next evict/restore call
extern NvU64  _vidmemEvictBudgetBytes;  // partial eviction: stop after this many bytes (0: all)
extern NvBool _vidmemEvictBigPagesOnly; // evict only allocations on 2 MB pages (making chunks free)

// Policy functions the driver calls.
NvU32  _vidmemHint(NvU32 pid, NvBool *pbActive);
NvBool _vidmemHintsActive(void);
const char *_vidmemAutoName(void);
NvBool _vidmemAutoActive(void);
VIDMEM_EVICT_ACTIVITY *_vidmemActivityFind(NvU32 pid, NvBool bCreate);
NvU32  _vidmemActivityIdleSec(NvU32 pid);
NvU32  _vidmemActivityRecent(NvU32 pid);
void   _vidmemEvictNoteRestoreOnAccess(NvU32 pid);
NvBool _vidmemEvictIsNeverName(const char *name);
NvU32  _vidmemEvictGatherProcs(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 maxProcs);
NvU32  _vidmemEvictBusiestPid(VIDMEM_EVICT_PROC *pProcs, NvU32 count);
NvBool _vidmemEvictHintVictimOk(VIDMEM_EVICT_PROC *p, NvU32 exceptPid, NvU64 nowNs, NvU32 level);
NvU64  _vidmemEvictHintFree(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 wantMB,
                            NvU32 exceptPid, NvU32 level, const char *why);
void   _vidmemEvictUpdateReclaimable(VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 nowNs);

// One policy step after the platform has sampled activity. API lock RW, GPU lock held.
void   vepPolicyStep(OBJGPU *pGpu);
// The per-process totals of the last step's walk (with hints) and when it was taken.
NvU32  vepLastProcs(VIDMEM_EVICT_PROC **ppProcs, NvU64 *pAtNs);
// A hot evicted allocation of `pid`: free MB to keep after it comes back (NV_U64_MAX: not now).
NvU64  vepHotRestoreKeepMB(NvU32 pid);
// The cores of the driver's entry points (vidmemEvict*() in vidmem_evict.c).
NvU64  vepReclaimableFor(NvU32 pid);
NvU64  vepBackgroundCushion(NvU32 pid, const char *name);
NvBool vepWantBorn(NvU32 pid, const char *name, NvU64 (*measureVidmem)(void *ctx), void *ctx);
NvBool vepMakeRoomAllowed(NvU32 pid);
NvBool vepMakeRoomRun(OBJGPU *pGpu, NvU32 pid, NvU64 size, NvU64 headroomMB, const char *why,
                      NvU64 *pWantMB, NvU64 *pAvail);
NvBool vepKeepFgHeadroomWanted(OBJGPU *pGpu, NvU32 pid, NvU64 size);
// The foreground's headroom, refilled in the background: request (NV_TRUE:
// queue a run), and one run (NV_TRUE: run again).
NvBool vepHeadroomRequest(NvU32 pid);
NvBool vepHeadroomRun(OBJGPU *pGpu);
#define VEP_HAS_HEADROOM_RUN 1
NvBool vepMakeRoomCompositorRun(OBJGPU *pGpu, NvU64 size, NvBool bContig, NvU64 align,
                                NvU64 *pWantMB, NvU64 *pAvail, NvU64 *pMovedMB);
const char *vepMakeRoomDeniedWhy(NvU32 pid);
NvU32  vepMakeRoomPlaceLevel(NvU32 pid, NvBool bContig);
NvU32  vepMakeRoomContextsLevel(NvU32 pid);
NvBool vepMakeRoomPlaceRun(OBJGPU *pGpu, NvU32 pid, NvU64 size, NvBool bContig, NvU64 align, NvU32 level,
                           NvU64 *pWantMB, NvU64 *pAvail, NvU64 *pMovedMB);

//
// The platform: implemented by vidmem_evict.c in the driver and by the
// memory model in the simulator. API lock RW, GPU lock held for all of them.
//
// Free vidmem outside the reserve block, in MB.
NvU64  vepPlatAvailMB(OBJGPU *pGpu);
// Per-process totals over every non-kernel client: pid, name, clients,
// vidmemBytes, evictedBytes, eligibleBytes, bNever (_vidmemEvictIsNeverName()
// of any of its clients). Returns the number of entries filled.
NvU32  vepPlatGatherProcs(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 maxProcs);
// mode 1 evict, 2 restore what is evicted of `pid` (both up to
// _vidmemEvictBudgetBytes if set); *pMovedBytes (may be NULL): bytes moved.
NV_STATUS vepPlatMovePid(OBJGPU *pGpu, NvU32 mode, NvU32 pid, NvU64 *pMovedBytes);
// Evicted allocations in use (access counters) back, one by one.
void   vepPlatHotRestore(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count);
// Free 2 MB chunks and the longest run of them (NV_FALSE: unknown).
NvBool vepPlatChunkInfo(OBJGPU *pGpu, NvU64 *pFreeChunks, NvU64 *pLongest);
// Clear a contiguous window of `size` (aligned): for the compositor (forPid
// 0, VE_LEVEL_COMPOSITOR), or for an allocation of `forPid` from those who
// may give memory at `level`. forPid's own memory never leaves vidmem for it.
NvBool vepPlatMakeContig(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 size, NvU64 align,
                         NvU32 forPid, NvU32 level, NvU64 *pMovedMB);
// Background compaction (BACKLOG #16): lengthen the longest run of free 2 MB
// chunks by moving up to `budgetMB` of memory next to it (of processes not
// drawing now, never the foreground) to other vidmem pages. Nothing to do when the run already has `wantMB`.
// Returns the MB moved (0: nothing to do or nothing possible now).
NvU64  vepPlatCompact(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 budgetMB, NvU64 wantMB);
// Total evicted bytes right now (the engine's count).
extern NvU64 _vidmemEvictBytes;
// kwin_wayland / Xwayland (video_mem.c).
NvBool vidmemIsReserveExemptName(const char *name);

#endif // VIDMEM_EVICT_POLICY_H
