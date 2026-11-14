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
//
// A step moves whole allocations, and one bigger than the step is a wall.
// With 32 MB for everyone a game stood still: WoW held 6.3 GB of 9.4 GB in
// use in blocks of 64-256 MB, no stretch between them was longer than the
// free run, 107 steps in five hours found nothing to do and Chrome was
// denied eight contiguous buffers of 130-192 MB with 0.8-1.4 GB free (host,
// 2026-11-11). So the step follows who pays for it (knob "compactbig", 0:
// 32 MB for everyone as before):
//   - the memory of the window in focus moves in allocations of FG_STEP_MB at
//     most: a pause of about 9 ms at the 10 MB/ms a move makes, less than a
//     frame at 60 Hz, and the 64 MB blocks most engines allocate in are in;
//   - the others' in allocations of BIG_STEP_MB at most (26 ms, for a window
//     the user is not working in);
//   - a step moves BIG_STEP_MB in all, FG_STEP_MB while a game is in focus:
//     the RM locks are held for the copy, and the game's own calls wait.
//
#define VIDMEM_EVICT_COMPACT_RUN_MB      1024
#define VIDMEM_EVICT_COMPACT_STEP_MB     32
#define VIDMEM_EVICT_COMPACT_FG_STEP_MB  96
#define VIDMEM_EVICT_COMPACT_BIG_STEP_MB 256
// (not a round number: a 30 s rest met a process that works every 30 s each time)
#define VIDMEM_EVICT_COMPACT_REST_SEC 14
//
// Steps in a row that moved memory while the window being cleared did not
// grow: then compaction rests as after a step with nothing to do. What moves
// goes wherever the allocator puts it, the next window may be over it.
//
#define VIDMEM_EVICT_COMPACT_STALL_STEPS 16
//
// The run compaction gathers while a game is in focus: room for the biggest
// contiguous buffer anybody asked for in the last NEED_SEC, NEED_TIMES over
// (a page asks for several), NEED_MIN_MB when nobody did. Not the three
// quarters of the free memory that are wanted next to any other window: every
// step is a pause of the game, and what the run is for is those buffers (host,
// 2026-11-11: Chrome's were 130-192 MB, the target 1.4 GB of the 1.9 GB free,
// never reached between the game's blocks).
//
#define VIDMEM_EVICT_COMPACT_NEED_SEC    1800
#define VIDMEM_EVICT_COMPACT_NEED_TIMES  3
#define VIDMEM_EVICT_COMPACT_NEED_MIN_MB 256
#define VIDMEM_EVICT_COMPACT_FREE_PCT    75
extern NvU32 _vidmemCompactRunMB;
extern NvU32 _vidmemCompactBig;
//
// Sides (evict.sh placesides): while a game runs, the others' new memory is
// placed from the bottom of vidmem and the game's from the top, as all of it
// was. The game allocates blocks of 64-256 MB for hours while a browser next
// to it allocates and frees small buffers; placed from the same end, those
// landed between the blocks, and what they left when freed were holes
// between walls (host, 2026-11-11: 1.9 GB free, the longest run 260 MB,
// problem #17). On separate sides the free memory is what is left between
// the two, and the holes of the desktop side are between memory that moves
// without a pause of the game. A game: started by a launcher or with a
// fullscreen window (the hints), holding vidmem. The compositor's stays at
// the top.
//
extern NvU32 _vidmemPlaceSides;
NvBool vepPlaceLow(NvU32 pid, const char *name);
NvBool vepGameSideNow(void);
// A chunk of the cost map of vepCompactWindow() that holds a frame nobody can move.
#define VEP_COMPACT_FIXED NV_U32_MAX
#define VIDMEM_EVICT_HYST_MB        512
#define VIDMEM_EVICT_MAX_PROCS      128
#define VIDMEM_EVICT_MAX_PER_TICK   2

//
// How long ago `then` was, seen from `now` (ns); 0 when `then` is not before
// `now`. A policy step takes `now` once at its start, and a move during the
// step sets timestamps after it: a plain `now - then` wraps to ~584 years
// there, and "moved out a moment ago" read as "long ago" (problem #9: ghostty
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
    NvU64 outNs, outBytes;   // the policy's moves out in the last VIDMEM_EVICT_BOUNCE_SEC: the last one, bytes not yet back
    NvU64 backNs, backBytes; // and its moves back: the last one, bytes not yet out again
    NvU32 bounces;           // moves that undid one of the other direction made less than that long ago
    NvU64 bounceBytes;       // bytes copied there and back for nothing by them
} VIDMEM_EVICT_ACTIVITY;

//
// Memory the policy moved one way and back the other within this long was
// copied twice for nothing (vepBounces(), `evict.sh status`): the number a
// change of the rules is judged by.
//
#define VIDMEM_EVICT_BOUNCE_SEC 60

//
// A contiguous buffer this big comes back through a cleared window, one a
// step; smaller ones as any other memory, room for them is made at once.
// Without the limit every contiguous buffer counted: a Chrome whose WebGL
// memory is 6 MB contiguous buffers (1.3 GB of them) got room for 256 MB a
// step and came back in 11 s instead of 0.7 s, its last 30 MB never (nvtest
// fgprio on the RTX 5000, 2026-11-11; the host's Chrome buffers that looped
// were 130-192 MB, problem #15).
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
//               needs right now (problem #7, R): numbered after COMPOSITOR,
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
// Moves undone within VIDMEM_EVICT_BOUNCE_SEC and the bytes they copied, of `pid` (0: of everyone, since load).
void   vepBounces(NvU32 pid, NvU32 *pCount, NvU64 *pBytes);
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
// Background compaction (problem #16), one step: clear the window of 2 MB
// chunks that moving makes the longest free run, by moving what is in it to
// other vidmem pages. Whose memory: anyone's but the compositor's. Whole
// allocations move: one of the window in focus up to `fgStepMB`, of the
// others up to `stepMB`, bigger ones count as fixed; `stepMB` in all per
// step. Nothing to do when the run already has what vepCompactWantMB() says
// for the free memory there is. Returns the MB moved (0: nothing to do or
// nothing possible now); *pWindowMB: the window it worked on (0: none), for
// the policy to see whether the steps get anywhere.
NvU64  vepPlatCompact(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 stepMB, NvU64 fgStepMB,
                      NvU64 *pWindowMB);
// The free run compaction wants with `freeMB` free in whole 2 MB chunks (0:
// compaction is off), as of the last policy step.
NvU64  vepCompactWantMB(NvU64 freeMB);
// A contiguous allocation of `size` bytes was asked for (granted or not).
void   vepNoteContig(NvU64 size);
// The biggest one asked for lately, in MB (0: none), for the status.
NvU64  vepCompactNeedMB(void);
// The window for a step, shared by the driver and the simulator. pCost: per
// 2 MB chunk the frames in use that may move (0: free), VEP_COMPACT_FIXED
// when it holds one that may not; the longest free run is `runLen` chunks
// at `runStart`; a chunk is `chunkFrames` frames. pOwners (or NULL): the
// allocations of more than a chunk that may move, numbered from 1; pOwner[c]
// is the one holding most of chunk c (0: none) with pOwn[c] of its frames
// there, pFrames[n] is all of allocation n, pCount[n] is scratch, zeros
// before and after. NV_TRUE: chunks [*pLo, *pHi).
typedef struct
{
    const NvU32 *pOwner;
    const NvU32 *pOwn;
    const NvU32 *pFrames;
    NvU32       *pCount;
} VEP_COMPACT_OWNERS;
// Allocations numbered at most; the rest count by their frames in the window only.
#define VEP_COMPACT_MAX_OWNERS 8192
NvBool vepCompactWindow(const NvU32 *pCost, const VEP_COMPACT_OWNERS *pOwners, NvU64 chunks, NvU64 chunkFrames,
                        NvU64 runStart, NvU64 runLen, NvU64 budgetFrames, NvU64 *pLo, NvU64 *pHi);
// Total evicted bytes right now (the engine's count).
extern NvU64 _vidmemEvictBytes;
// kwin_wayland / Xwayland (video_mem.c).
NvBool vidmemIsReserveExemptName(const char *name);

#endif // VIDMEM_EVICT_POLICY_H
