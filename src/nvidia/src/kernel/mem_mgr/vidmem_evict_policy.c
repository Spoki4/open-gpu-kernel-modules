/*
 * Local patch (not upstream): the eviction policy (window hints, the GPU
 * activity of each process, who gives up memory for whom), split out of
 * vidmem_evict.c so that the same code builds in the driver and in the
 * policy simulator (local/sim/). Everything that touches RM objects or the
 * GPU goes through the vepPlat*() functions of vidmem_evict_policy.h.
 */

#include "vidmem_evict_policy.h"

// Never evicted automatically: the desktop itself.
static const char * const _vidmemEvictNeverNames[] =
{
    "plasmashell",
    // The login screen: its windows belong to the greeter's own compositor,
    // which reports no hints, so it would look like a windowless process.
    "sddm-greeter-qt",
    "sddm-greeter",
};

//
// The busiest process of the last VIDMEM_EVICT_BUSY_STICKY_SEC is "the game":
// never evicted and never given sysmem, even when it hangs or loads and so
// drops out of the GR samples for a while (2026-11-02 02:21: the game, stuck
// after an Xid, looked idle and got evicted).
//
#define VIDMEM_EVICT_BUSY_STICKY_SEC  120
//
// ...unless another process has been the busiest for VIDMEM_EVICT_BUSY_TAKEOVER_SEC
// in a row since: then it is the game now and the previous one (e.g. Chrome
// used right before the game started, 2026-11-02 02:34: it stayed protected
// for 2 minutes and the game ran at 55-60 fps instead of 70) is fair game.
//
#define VIDMEM_EVICT_BUSY_TAKEOVER_SEC 15

//
// A process active on the GPU holding this much vidmem is a game too, whoever
// tops the GR samples (2026-11-02 02:44: the Steam overlay's steamwebhelper
// often did, the game lost its protection and got a 200 MB allocation in
// sysmem: 18 fps).
//
#define VIDMEM_EVICT_GAME_BYTES (1024ULL << 20)
// "Drawing now": GPU work within the last two policy steps.
#define VIDMEM_EVICT_DRAWING_SEC      4

// A process is not evicted in its first seconds, nor right after it needed a
// buffer back (it is still working with them: no evict/restore ping-pong).
#define VIDMEM_EVICT_NEW_PROC_SEC     30
#define VIDMEM_EVICT_AFTER_ACCESS_SEC 60

// Moves made by the policy (a second client walk in a step only if any, problem #1).
static NvU32 _vidmemPolicyMoves;

static NvU32 _vidmemBounces;
static NvU64 _vidmemBounceBytes;

//
// A move out (mode 1) or back (mode 2) of `moved` bytes of `pid`: what undoes
// a move of the other direction made less than VIDMEM_EVICT_BOUNCE_SEC ago
// was copied twice for nothing (host, 2026-11-12 00:42: 1.5 GB of Chrome out,
// 869 MB back two seconds later, 975 MB out again, found by reading the log).
//
static void
_veNoteMove(NvU32 mode, NvU32 pid, NvU64 moved)
{
    VIDMEM_EVICT_ACTIVITY *pAct;
    NvU64  nowNs = osGetMonotonicTimeNs(), undone;
    NvU64 *pDoneNs, *pDoneBytes, *pOtherNs, *pOtherBytes;

    if ((moved == 0) || ((mode != 1) && (mode != 2)))
        return;
    pAct        = _vidmemActivityFind(pid, NV_TRUE);
    pDoneNs     = (mode == 1) ? &pAct->outNs     : &pAct->backNs;
    pDoneBytes  = (mode == 1) ? &pAct->outBytes  : &pAct->backBytes;
    pOtherNs    = (mode == 1) ? &pAct->backNs    : &pAct->outNs;
    pOtherBytes = (mode == 1) ? &pAct->backBytes : &pAct->outBytes;

    if (VE_AGO_NS(nowNs, *pOtherNs) >= VIDMEM_EVICT_BOUNCE_SEC * 1000000000ULL)
        *pOtherBytes = 0;
    if (VE_AGO_NS(nowNs, *pDoneNs) >= VIDMEM_EVICT_BOUNCE_SEC * 1000000000ULL)
        *pDoneBytes = 0;
    undone = NV_MIN(moved, *pOtherBytes);
    if (undone != 0)
    {
        *pOtherBytes       -= undone;
        pAct->bounces++;
        pAct->bounceBytes  += 2 * undone;
        _vidmemBounces++;
        _vidmemBounceBytes += 2 * undone;
    }
    *pDoneBytes += moved - undone;
    *pDoneNs     = nowNs;
}

void
vepBounces(NvU32 pid, NvU32 *pCount, NvU64 *pBytes)
{
    VIDMEM_EVICT_ACTIVITY *pAct = (pid != 0) ? _vidmemActivityFind(pid, NV_FALSE) : NULL;

    *pCount = (pid == 0) ? _vidmemBounces     : ((pAct != NULL) ? pAct->bounces     : 0);
    *pBytes = (pid == 0) ? _vidmemBounceBytes : ((pAct != NULL) ? pAct->bounceBytes : 0);
}

static NV_STATUS
_veMove(OBJGPU *pGpu, NvU32 mode, NvU32 pid, NvU64 *pMovedBytes)
{
    NvU64     moved = 0;
    NV_STATUS status;

    _vidmemPolicyMoves++;
    status = vepPlatMovePid(pGpu, mode, pid, &moved);
    if (pMovedBytes != NULL)
        *pMovedBytes = moved;
    _veNoteMove(mode, pid, moved);
    return status;
}

//
// The per-process totals of the last policy step's walk, for the driver's
// trace of the same step (one walk instead of two, problem #1).
//
static VIDMEM_EVICT_PROC _vidmemLastProcs[VIDMEM_EVICT_MAX_PROCS];
static NvU32             _vidmemLastProcsCount;
static NvU64             _vidmemLastProcsNs;

NvU32
vepLastProcs(VIDMEM_EVICT_PROC **ppProcs, NvU64 *pAtNs)
{
    *ppProcs = _vidmemLastProcs;
    *pAtNs   = _vidmemLastProcsNs;
    return _vidmemLastProcsCount;
}

static void
_vidmemEvictKeepProcs(VIDMEM_EVICT_PROC *pProcs, NvU32 count)
{
    portMemCopy(_vidmemLastProcs, sizeof(*pProcs) * count, pProcs, sizeof(*pProcs) * count);
    _vidmemLastProcsCount = count;
    _vidmemLastProcsNs    = osGetMonotonicTimeNs();
}

NvU32 _vidmemHint(NvU32 pid, NvBool *pbActive)
{
    NvU32 gen = 0;
    NvU32 hint = osGetVidmemHint(pid, &gen);

    if (pbActive != NULL)
        *pbActive = (gen != 0);
    return hint;
}

NvBool _vidmemHintsActive(void)
{
    NvU32 gen = 0;

    (void)osGetVidmemHint(0, &gen);
    return gen != 0;
}

VIDMEM_EVICT_ACTIVITY _vidmemActivity[VIDMEM_EVICT_MAX_PROCS];
NvU64  _vidmemActivityNewestTs;
//
// 0 off, 1 on (window hints when present, GPU samples otherwise), 2 (default)
// only while window hints are present: the sample-based policy alone
// mistakes visible, lightly drawing apps for idle ones.
//
NvU32  _vidmemAutoEnabled = NV_U32_MAX;  // NV_U32_MAX: not initialised

const char *_vidmemAutoName(void)
{
    return (_vidmemAutoEnabled == 0) ? "off" :
           ((_vidmemAutoEnabled == 1) ? "on" : "with window hints only");
}

// Is the automatic policy running right now?
NvBool _vidmemAutoActive(void)
{
    if ((_vidmemAutoEnabled == 0) || (_vidmemAutoEnabled == NV_U32_MAX))
        return NV_FALSE;
    return (_vidmemAutoEnabled == 1) || _vidmemHintsActive();
}
NvU32  _vidmemLowMB = 1024;
NvU32  _vidmemHighMB = 2560;
NvU64  _vidmemLastPressureNs;
NvU32  _vidmemCompactRunMB = VIDMEM_EVICT_COMPACT_RUN_MB;
NvU32  _vidmemCompactBig = 1;          // the step follows who pays for it (0: 32 MB for everyone)
static NvU64 _vidmemCompactRestNs;   // no compaction step before this (monotonic ns)
static NvU32 _vidmemCompactIdleSteps; // steps in a row that had nothing to do (the rest doubles)
static NvU32 _vidmemCompactFgPid;     // the focus at the last step (a change ends the rest)
static NvU64 _vidmemCompactTryNs;     // the last step that looked for something to move
static NvU64 _vidmemCompactWindowMB;  // the window the last moving step worked on
static NvU32 _vidmemCompactStalled;   // moving steps in a row whose window did not grow
// The biggest contiguous request in this half of VIDMEM_EVICT_COMPACT_NEED_SEC and in the one before.
static NvU64 _vidmemContigAskBytes[2];
static NvU64 _vidmemContigAskNs;      // when this half began
static NvU32 _vidmemCompactRunSeenMB; // the knob at the last step (a new value: try at once)
NvU32  _vidmemBusiestPid;          // updated every tick
NvU64  _vidmemBusiestSinceNs;      // since when it is the busiest without a break
NvU64  _vidmemBornBytes;           // allocations created in sysmem
NvU32  _vidmemBornAllocs, _vidmemBornFailed;

VIDMEM_EVICT_ACTIVITY *
_vidmemActivityFind(NvU32 pid, NvBool bCreate)
{
    VIDMEM_EVICT_ACTIVITY *pOldest = NULL;
    NvU32 i;

    for (i = 0; i < VIDMEM_EVICT_MAX_PROCS; i++)
    {
        if (_vidmemActivity[i].pid == pid)
            return &_vidmemActivity[i];
        if ((pOldest == NULL) ||
            (NV_MAX(_vidmemActivity[i].lastActiveTs, _vidmemActivity[i].firstSeenTs) <
             NV_MAX(pOldest->lastActiveTs, pOldest->firstSeenTs)))
            pOldest = &_vidmemActivity[i];
    }
    if (!bCreate)
        return NULL;

    portMemSet(pOldest, 0, sizeof(*pOldest));
    pOldest->pid = pid;
    pOldest->firstSeenTs = _vidmemActivityNewestTs;
    pOldest->firstSeenNs = osGetMonotonicTimeNs();
    return pOldest;
}

//
// Seconds since `pid` last did GPU work (NV_U32_MAX: never seen doing any):
// the shorter of "last GR/NVDEC/NVENC sample" and "last change of its
// channels' GP_PUT". The samples name only the busiest process per sample,
// so a light renderer next to a game never shows up in them (nvtest.sh
// fgprio-anim: a 60 fps Chrome looked idle and was moved out); a moved
// GP_PUT means work was submitted, however light.
//
NvU32
_vidmemActivityIdleSec(NvU32 pid)
{
    VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pid, NV_FALSE);
    NvU64 since;
    NvU32 idle;

    if (pAct == NULL)
        return NV_U32_MAX;
    since = NV_MAX(pAct->lastActiveTs, pAct->firstSeenTs);
    if ((since == 0) || (_vidmemActivityNewestTs < since))
        idle = (pAct->lastActiveTs == 0) ? NV_U32_MAX : 0;
    else
        idle = (NvU32)NV_MIN((_vidmemActivityNewestTs - since) / 1000000000ULL, NV_U32_MAX - 1);
    if (pAct->lastSubmitNs != 0)
        idle = NV_MIN(idle, (NvU32)NV_MIN((osGetMonotonicTimeNs() - pAct->lastSubmitNs) / 1000000000ULL,
                                          NV_U32_MAX - 1));
    return idle;
}

NvU32
_vidmemActivityRecent(NvU32 pid)
{
    VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pid, NV_FALSE);
    return (pAct != NULL) ? pAct->recentSamples : 0;
}

static NvU32
_vidmemActivityBusiest(void)
{
    NvU32 best = 0, bestSamples = 0, i;

    for (i = 0; i < VIDMEM_EVICT_MAX_PROCS; i++)
    {
        if (_vidmemActivity[i].recentSamples > bestSamples)
        {
            bestSamples = _vidmemActivity[i].recentSamples;
            best = _vidmemActivity[i].pid;
        }
    }
    return best;
}

void
_vidmemEvictNoteRestoreOnAccess(NvU32 pid)
{
    VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pid, NV_TRUE);
    pAct->lastRestoreNs = osGetMonotonicTimeNs();
}

// Was the busiest process in the last VIDMEM_EVICT_BUSY_STICKY_SEC.
static NvBool
_vidmemEvictWasBusiest(NvU32 pid, NvU64 nowNs)
{
    VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pid, NV_FALSE);

    if (pid == _vidmemBusiestPid)
        return NV_TRUE;
    // Superseded: someone else has been the busiest long enough.
    if ((_vidmemBusiestPid != 0) &&
        (VE_AGO_NS(nowNs, _vidmemBusiestSinceNs) >= VIDMEM_EVICT_BUSY_TAKEOVER_SEC * 1000000000ULL))
        return NV_FALSE;
    return (pAct != NULL) && (pAct->lastBusiestNs != 0) &&
           (VE_AGO_NS(nowNs, pAct->lastBusiestNs) < VIDMEM_EVICT_BUSY_STICKY_SEC * 1000000000ULL);
}

// The game, too new, or needed one of its buffers back recently.
static NvBool
_vidmemEvictIsProtected(NvU32 pid, NvU64 nowNs)
{
    VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pid, NV_TRUE);

    if (_vidmemEvictWasBusiest(pid, nowNs))
        return NV_TRUE;
    // Holding >= 1 GB: the game, even when it hangs and drops out of the
    // samples (10:09:55: the game, stuck after an Xid, was evicted, 7.2 GB).
    if (pAct->vidmemBytes >= VIDMEM_EVICT_GAME_BYTES)
        return NV_TRUE;

    if ((pAct->lastRestoreNs != 0) &&
        (VE_AGO_NS(nowNs, pAct->lastRestoreNs) < VIDMEM_EVICT_AFTER_ACCESS_SEC * 1000000000ULL))
        return NV_TRUE;
    if ((pAct->firstSeenTs != 0) && (_vidmemActivityNewestTs >= pAct->firstSeenTs) &&
        (_vidmemActivityNewestTs - pAct->firstSeenTs < VIDMEM_EVICT_NEW_PROC_SEC * 1000000000ULL))
        return NV_TRUE;
    return NV_FALSE;
}

NvBool
_vidmemEvictIsNeverName(const char *name)
{
    NvU32 i;

    if (vidmemIsReserveExemptName(name))
        return NV_TRUE;
    for (i = 0; i < NV_ARRAY_ELEMENTS(_vidmemEvictNeverNames); i++)
    {
        if (portStringCompare(name, _vidmemEvictNeverNames[i], NV_PROC_NAME_MAX_LENGTH) == 0)
            return NV_TRUE;
    }
    return NV_FALSE;
}

//
// Per-process totals (vepPlatGatherProcs()) with each one's window hint; the
// activity records follow them. API lock RW, GPU lock held. Returns the
// number of entries filled.
//
NvU32
_vidmemEvictGatherProcs(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 maxProcs)
{
    NvU32 count = vepPlatGatherProcs(pGpu, pProcs, maxProcs);
    NvU32 k;

    for (k = 0; k < count; k++)
    {
        VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pProcs[k].pid, NV_TRUE);
        pAct->vidmemBytes = pProcs[k].vidmemBytes;
        pAct->vidmemAtNs  = osGetMonotonicTimeNs();
        pProcs[k].hint    = _vidmemHint(pProcs[k].pid, NULL);
        if (VE_HINT_STATE(pProcs[k].hint) >= VE_HINT_VISIBLE)
            pAct->lastVisibleNs = pAct->vidmemAtNs;
        if (VE_HINT_STATE(pProcs[k].hint) == VE_HINT_FG)
        {
            pAct->lastFgNs = pAct->vidmemAtNs;
            if (pAct->fgSinceNs == 0)
                pAct->fgSinceNs = pAct->vidmemAtNs;
        }
        else
        {
            pAct->fgSinceNs = 0;
        }
        if (VE_HINT_STATE(pProcs[k].hint) != VE_HINT_HIDDEN)
            pAct->hiddenSinceNs = 0;
        else if (pAct->hiddenSinceNs == 0)
            pAct->hiddenSinceNs = pAct->vidmemAtNs;
    }
    // The window focused before the current one: the newest lastFgNs of a
    // process that is not the foreground now.
    _vidmemPrevFgNs = 0;
    for (k = 0; k < count; k++)
    {
        VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pProcs[k].pid, NV_FALSE);

        if ((pAct != NULL) && (VE_HINT_STATE(pProcs[k].hint) != VE_HINT_FG))
            _vidmemPrevFgNs = NV_MAX(_vidmemPrevFgNs, pAct->lastFgNs);
    }
    return count;
}

//
// The previous foreground (rules R, the user's choice 2026-11-06: the
// focused window and the one before it are kept, so alt-tab back and forth is
// instant): focused last among the processes not focused now, give or take
// the second the policy needs to see a whole app (browser and GPU process)
// in one walk.
//
NvU64 _vidmemPrevFgNs;
// The foreground is a game (as of the last policy step; idea I7).
static NvBool _vidmemFgIsGame;

static NvBool
_vidmemEvictIsPrevFg(VIDMEM_EVICT_PROC *p, VIDMEM_EVICT_ACTIVITY *pAct)
{
    return (VE_HINT_STATE(p->hint) != VE_HINT_FG) && (pAct->lastFgNs != 0) && (_vidmemPrevFgNs != 0) &&
           (pAct->lastFgNs + 1000000000ULL >= _vidmemPrevFgNs);
}

//
// A game (rules R, the user's choice: a fullscreen window that keeps the GPU
// busy, or anything started by Steam or GameMode); without those hints from
// the KWin script, a process holding >= 1 GB as before.
//
static NvBool
_vidmemEvictIsGame(VIDMEM_EVICT_PROC *p)
{
    if (p->hint & VE_HINT_LAUNCHED_GAME)
        return NV_TRUE;
    if ((p->hint & VE_HINT_FULLSCREEN) &&
        ((_vidmemActivityIdleSec(p->pid) < VIDMEM_EVICT_DRAWING_SEC) || (p->pid == _vidmemBusiestPid)))
        return NV_TRUE;
    return p->vidmemBytes >= VIDMEM_EVICT_GAME_BYTES;
}

// The process with the most recent engine samples: the one to keep.
NvU32
_vidmemEvictBusiestPid(VIDMEM_EVICT_PROC *pProcs, NvU32 count)
{
    NvU32 best = 0, bestSamples = 0, i;

    for (i = 0; i < count; i++)
    {
        NvU32 n = _vidmemActivityRecent(pProcs[i].pid);
        if (n > bestSamples)
        {
            bestSamples = n;
            best = pProcs[i].pid;
        }
    }
    return best;
}

//
// Foreground priority (2026-11-02 12:45, CoD with Chrome, ghostty and the
// Steam window open on the second monitor: 30 fps, 60 once they were
// closed; the policy could take only 212 MB of hidden apps). For the
// foreground process a visible window whose process has been idle on the
// GPU for VIDMEM_EVICT_VISIBLE_IDLE_SEC may give up its memory too: kwin
// keeps showing it (window buffers stay in vidmem), only the app's own
// textures move. It comes back when it is activated, or after
// VIDMEM_EVICT_FG_HOLD_SEC when there is room without moving anything.
//
#define VIDMEM_EVICT_VISIBLE_IDLE_SEC 10
#define VIDMEM_EVICT_FG_HOLD_SEC      60

//
// Who may give up memory (VE_LEVEL_*):
//   HIDDEN  hidden and windowless processes (pressure, visible apps)
//   FG_IDLE + visible ones (the foreground's budget, allocations, restore)
// A process that is drawing where it is not shown is never moved out: it
// would go on drawing from sysmem, saturate PCIe and hold GR with slow draws
// (2026-11-02 13:15, CoD: 1 fps after Chrome, busy on the second monitor,
// and the Steam window were moved out for the game's allocations: PCIe
// 21 GB/s, SM 100%). Visible and windowless processes: only when idle on the
// GPU for VIDMEM_EVICT_IDLE_SEC. Hidden ones: unless they drew after they
// were hidden (alt-tab: the app left behind drew a moment ago, as the
// foreground; it stops when hidden; an X11 app behind a fullscreen game
// does not).
//
//
// RESTORE: + visible ones still drawing, only to bring back the foreground's
// own memory (bounded; the user just switched to it, e.g. clicked Chrome on
// the second monitor while the game keeps drawing on the first: the game
// draws partly from sysmem until it is the foreground again, ~0.1 s). Never
// for allocations or pressure (13:15: that took a drawing Chrome for the
// game's streaming, 1 fps).
//
//
// COMPOSITOR: anyone but the desktop itself, the foreground included (last):
// the compositor's allocation must not fail (no reserve), a game moved out
// in part only draws slower for a moment (vidmemEvictMakeRoomCompositor()).
//

//
// A process that was the foreground a moment ago (the game the user alt-tabbed
// away from, or is alt-tabbing back to: the hints say hidden for a moment) is
// not moved out for anybody but the foreground's immediate needs and the
// compositor (2026-11-04 death scenario: 241 MB of the game moved out to bring
// back a minimized launcher's 21 MB at the switch back, a 215 ms frame). It
// used to be "focused in the last 30 s"; now it is the window focused before
// the current one, however long ago (_vidmemEvictIsPrevFg()).
//
//
// "Drawing again" brings a moved-out process back only when it has drawn at
// every policy step for this long: a minimized launcher drawing a frame now
// and then was brought back and moved out again six times (ping-pong).
//
#define VIDMEM_EVICT_STEADY_SEC    6
// Margin on top of what a restore of a non-foreground process needs.

//
// May `p` be moved out to make room, by the window hints? Hidden (own
// windows, or inherited after the grace period), or no window anywhere in
// its ancestry and idle on the GPU; for the foreground process also visible
// ones (see VE_LEVEL_*). Never the foreground, the caller (`exceptPid`), the
// desktop, or a process that needed a buffer back a moment ago.
//
NvBool
_vidmemEvictHintVictimOk(VIDMEM_EVICT_PROC *p, NvU32 exceptPid, NvU64 nowNs, NvU32 level)
{
    VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(p->pid, NV_TRUE);
    NvU32 st = VE_HINT_STATE(p->hint);
    NvU32 idle = _vidmemActivityIdleSec(p->pid);

    if (p->bNever || (p->pid == exceptPid) || (p->eligibleBytes < (4ULL << 20)))
        return NV_FALSE;
    if (level == VE_LEVEL_COMPOSITOR)
        return NV_TRUE;
    if (st == VE_HINT_FG)
        return NV_FALSE;
    // The previous foreground stays for the background and for the
    // foreground's headroom and pressure; it gives what the foreground needs
    // right now or to come back (FG_ALLOC, FG_RESTORE), and to the compositor.
    if (((level == VE_LEVEL_HIDDEN) || (level == VE_LEVEL_FG_IDLE)) && _vidmemEvictIsPrevFg(p, pAct))
        return NV_FALSE;
    if ((pAct->lastRestoreNs != 0) &&
        (VE_AGO_NS(nowNs, pAct->lastRestoreNs) < VIDMEM_EVICT_AFTER_ACCESS_SEC * 1000000000ULL))
        return NV_FALSE;

    switch (st)
    {
        case VE_HINT_VISIBLE:
            // Drawing: only to bring back the foreground's own memory, or
            // for what an allocation of it needs (a game on the other
            // monitor gives the focused WebGPU page its few MB, problem #7;
            // a video gives the focused game what nobody else has, R5).
            if ((level == VE_LEVEL_FG_RESTORE) || (level == VE_LEVEL_FG_ALLOC))
                return NV_TRUE;
            return (level == VE_LEVEL_FG_IDLE) && (idle >= VIDMEM_EVICT_VISIBLE_IDLE_SEC);

        case VE_HINT_HIDDEN:
            //
            // Drawing now while hidden (work in the last two steps, after it
            // was hidden): no. Work only before it was hidden (alt-tab) or a
            // short burst now and then (a check every 20 s): yes.
            //
            if ((idle < VIDMEM_EVICT_DRAWING_SEC) && (pAct->hiddenSinceNs != 0) &&
                ((VE_AGO_NS(nowNs, pAct->hiddenSinceNs)) / 1000000000ULL > (NvU64)idle + 1))
                return NV_FALSE;
            if ((p->hint & VE_HINT_OTHER_APP) &&
                (VE_AGO_NS(nowNs, pAct->firstSeenNs) < VIDMEM_EVICT_INHERIT_GRACE_SEC * 1000000000ULL))
                return NV_FALSE;
            return NV_TRUE;

        default:
            //
            // No window anywhere in its ancestry: only when idle. For the
            // foreground, when not drawing right now (as hidden ones): a
            // background app with a little work now and then (a notification,
            // a check every 30 s) was never 10 s idle at the moment the game
            // allocated, so the memory promised to the game in its budget was
            // not there and the game's GL driver put textures in sysmem for
            // the whole run (2026-11-04, bench.py apps-evict: 8.8 fps vs 52).
            //
            if (level >= VE_LEVEL_FG_IDLE)
                return (idle >= VIDMEM_EVICT_DRAWING_SEC) && !_vidmemEvictIsProtected(p->pid, nowNs);
            return (idle >= VIDMEM_EVICT_IDLE_SEC) && !_vidmemEvictIsProtected(p->pid, nowNs);
    }
}

//
// Order: hidden and idle on the GPU first, then hidden ones still drawing
// (an X11 app such as Steam keeps rendering behind a fullscreen game; moved
// out it draws from sysmem over PCIe), then windowless idle ones, then
// visible idle and visible drawing ones (only the foreground may take
// those). Within a class: the one focused longest ago (never focused first;
// rule R3, the order in which the user activated windows), then the one
// hidden longest, then the biggest.
//
static VIDMEM_EVICT_PROC *
_vidmemEvictHintPickVictim(VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU32 exceptPid, NvU64 nowNs, NvU32 level)
{
    VIDMEM_EVICT_PROC *pPick = NULL;
    NvU64 pickFg = 0, pickVisible = 0;
    NvU32 pickClass = 0;
    NvU32 i;

    for (i = 0; i < count; i++)
    {
        VIDMEM_EVICT_ACTIVITY *pA;
        NvU64 fg, vis;
        NvU32 cls;

        if (!_vidmemEvictHintVictimOk(&pProcs[i], exceptPid, nowNs, level))
            continue;
        pA  = _vidmemActivityFind(pProcs[i].pid, NV_TRUE);
        fg  = pA->lastFgNs;
        vis = pA->lastVisibleNs;
        if (VE_HINT_STATE(pProcs[i].hint) == VE_HINT_FG)
            cls = 6;    // the compositor's last resort
        else if ((level == VE_LEVEL_COMPOSITOR) && _vidmemEvictIsProtected(pProcs[i].pid, nowNs))
            cls = 5;    // the game in the background, a process just started or just restored
        else if (VE_HINT_STATE(pProcs[i].hint) == VE_HINT_VISIBLE)
            // Visible: only for the foreground, idle ones before drawing ones.
            cls = (_vidmemActivityIdleSec(pProcs[i].pid) >= VIDMEM_EVICT_VISIBLE_IDLE_SEC) ? 3 : 4;
        else if (VE_HINT_STATE(pProcs[i].hint) == VE_HINT_UNKNOWN)
            cls = 2;
        else
            cls = (_vidmemActivityIdleSec(pProcs[i].pid) < VIDMEM_EVICT_IDLE_SEC) ? 1 : 0;
        if ((pPick == NULL) || (cls < pickClass) ||
            ((cls == pickClass) &&
             ((fg < pickFg) ||
              ((fg == pickFg) && ((vis < pickVisible) ||
                                  ((vis == pickVisible) && (pProcs[i].eligibleBytes > pPick->eligibleBytes)))))))
        {
            pPick = &pProcs[i];
            pickFg = fg;
            pickVisible = vis;
            pickClass = cls;
        }
    }
    return pPick;
}

// The process whose window is in focus (0: none).
static NvU32
_vidmemEvictFgPid(VIDMEM_EVICT_PROC *pProcs, NvU32 count)
{
    NvU32 i;

    for (i = 0; i < count; i++)
    {
        if (VE_HINT_STATE(pProcs[i].hint) == VE_HINT_FG)
            return pProcs[i].pid;
    }
    return 0;
}

// Margin on top of what is missing, per victim (the compositor asks for less).
static NvU64 _vidmemEvictVictimMarginMB = 64;

//
// Move hidden processes out until `wantMB` is free outside the reserve block
// (or nothing is left to move). Returns the free MB afterwards.
//
NvU64
_vidmemEvictHintFree(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 wantMB,
                     NvU32 exceptPid, NvU32 level, const char *why)
{
    NvU64 nowNs = osGetMonotonicTimeNs();
    NvU64 avail = vepPlatAvailMB(pGpu);

    while (avail < wantMB)
    {
        VIDMEM_EVICT_PROC *pPick = _vidmemEvictHintPickVictim(pProcs, count, exceptPid, nowNs, level);
        NvU64 moved = 0;

        if (pPick == NULL)
            break;
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: %s: free %llu MB, want %llu MB, evicting up to %llu MB of %s (pid %u, %s%s, %llu MB movable)\n",
                  why, avail, wantMB, (wantMB - avail) + _vidmemEvictVictimMarginMB, pPick->name, pPick->pid,
                  (VE_HINT_STATE(pPick->hint) == VE_HINT_HIDDEN) ? "hidden" :
                  ((VE_HINT_STATE(pPick->hint) == VE_HINT_VISIBLE) ? "visible, for the foreground" :
                   ((VE_HINT_STATE(pPick->hint) == VE_HINT_FG) ? "foreground, for the compositor" : "no window")),
                  (pPick->hint & VE_HINT_INHERITED) ? " (parent)" : "", pPick->eligibleBytes >> 20);
        // Only what is missing (+ a margin, 64 MB), not the whole process.
        _vidmemEvictBudgetBytes = ((wantMB - avail) + _vidmemEvictVictimMarginMB) << 20;
        (void)_veMove(pGpu, 1, pPick->pid, &moved);
        //
        // Making chunks free prefers allocations on 2 MB pages, within one
        // victim: one that has none gives what it has before the next one
        // in line is touched (nvtest comproom: the hidden app's 64 KB-page
        // buffers were skipped and the drawing game gave first).
        //
        if ((moved == 0) && _vidmemEvictBigPagesOnly)
        {
            _vidmemEvictBigPagesOnly = NV_FALSE;
            (void)_veMove(pGpu, 1, pPick->pid, &moved);
            _vidmemEvictBigPagesOnly = NV_TRUE;
        }
        _vidmemEvictBudgetBytes = 0;
        pPick->eligibleBytes = (moved != 0 && pPick->eligibleBytes > moved) ? pPick->eligibleBytes - moved : 0;
        pPick->evictedBytes += moved;
        if (VE_HINT_STATE(pPick->hint) == VE_HINT_VISIBLE)
            _vidmemActivityFind(pPick->pid, NV_TRUE)->evictedForFgNs = osGetMonotonicTimeNs();
        if (moved != 0)
        {
            VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pPick->pid, NV_TRUE);

            // Who was in focus: it stays out while that one is (step 3, problem #6).
            pAct->outForPid   = _vidmemEvictFgPid(pProcs, count);
            if (pAct->outForPid == pPick->pid)
                pAct->outForPid = 0;
            pAct->roomSinceNs = 0;
            //
            // "Drawing steadily" (step 1b) is about what it does once it is
            // out: the frames it drew until a moment ago, as a visible
            // window, said nothing about that and brought it back in the
            // step it was moved out in (host, 2026-11-12 00:42, Chrome
            // covered by the window the user switched to).
            //
            pAct->drawingSinceNs = 0;
        }
        avail = vepPlatAvailMB(pGpu);
    }
    return avail;
}

//
// What hidden processes hold and could give up now (bytes), refreshed by every
// policy step. Added to the free memory a visible process is told about
// (HEAP_FREE, i.e. its Vulkan/DXGI budget, vidmemEvictReclaimableFor()):
// otherwise a game never asks for more than is free and streams the rest
// from sysmem itself, and the hidden apps keep the VRAM.
//
NvU64 _vidmemReclaimableBytes;      // for visible processes: hidden/windowless ones
NvU64 _vidmemReclaimableFgBytes;    // for the foreground: + visible ones (FG_ALLOC)
// What could move per level at the last walk, without the margin (problem #8).
static NvU64 _vidmemMovableHiddenBytes = NV_U64_MAX, _vidmemMovableFgIdleBytes = NV_U64_MAX,
             _vidmemMovableFgAllocBytes = NV_U64_MAX;
// What the foreground itself could give for its own allocation (0: a game, or nothing; problem #7).
static NvU64 _vidmemMovableFgOwnBytes = NV_U64_MAX;

//
// The foreground's allocation fits nowhere and nobody is left to give: an
// app that is not a game gets it anyway, and as much of its own older memory
// goes to sysmem instead (problem #7, the user's choice 2026-11-06: "memory
// in sysmem for the focused window rather than a denial"; a WebGPU page
// denied next to a game died). A game is turned away as before: its engine
// places by its budget and falls back by itself (vkd3d), and a move inside
// its allocation is a stall of its frame (problem #5). Registry-less knob for
// tests: evict.sh fgown on|off.
//
NvU32 _vidmemFgOwnEnabled = 1;

//
// Free-memory targets with hints. Smaller than lowMB/hysteresis of the
// sample policy: a visible process's allocation makes room on demand
// (vidmemEvictMakeRoom), so a big free cushion only makes every switch move
// more (2026-11-02 12:10: 1.4 GB of the game moved to bring back Chrome's
// 345 MB).
//
#define VIDMEM_EVICT_RECLAIM_MARGIN (256ULL << 20)

void
_vidmemEvictUpdateReclaimable(VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 nowNs)
{
    NvU64 sum = 0, sumFg = 0, sumFgIdle = 0;
    NvU32 i;

    for (i = 0; i < count; i++)
    {
        if (_vidmemEvictHintVictimOk(&pProcs[i], 0, nowNs, VE_LEVEL_HIDDEN))
            sum += pProcs[i].eligibleBytes;
        if (_vidmemEvictHintVictimOk(&pProcs[i], 0, nowNs, VE_LEVEL_FG_IDLE))
            sumFgIdle += pProcs[i].eligibleBytes;
        // The foreground is told what an allocation of it can take, a game
        // on the other monitor included (FG_ALLOC, problem #7): a WebGPU page
        // focused next to a game that fills vidmem saw no room in its budget
        // and stopped (nvtest focus).
        if (_vidmemEvictHintVictimOk(&pProcs[i], 0, nowNs, VE_LEVEL_FG_ALLOC))
            sumFg += pProcs[i].eligibleBytes;
    }
    _vidmemReclaimableBytes = (sum > VIDMEM_EVICT_RECLAIM_MARGIN) ? (sum - VIDMEM_EVICT_RECLAIM_MARGIN) : 0;
    _vidmemReclaimableFgBytes = (sumFg > VIDMEM_EVICT_RECLAIM_MARGIN) ? (sumFg - VIDMEM_EVICT_RECLAIM_MARGIN) : 0;
    _vidmemMovableHiddenBytes = sum;
    _vidmemMovableFgIdleBytes = sumFgIdle;
    _vidmemMovableFgAllocBytes = sumFg;
    _vidmemMovableFgOwnBytes = 0;
    for (i = 0; i < count; i++)
    {
        if ((VE_HINT_STATE(pProcs[i].hint) == VE_HINT_FG) && !pProcs[i].bNever && !_vidmemEvictIsGame(&pProcs[i]))
            _vidmemMovableFgOwnBytes += pProcs[i].eligibleBytes;
    }
}

//
// Could anybody be moved out for `level` at all (as of the last step or
// make-room)? An allocation that finds nothing to move must not walk every
// client and every allocation under the locks again (problem #8: 4-11 ms per
// allocation of the game near its limit, each one coming back empty).
//
static NvBool
_vidmemEvictAnyMovable(NvU32 level)
{
    switch (level)
    {
        case VE_LEVEL_HIDDEN:   return _vidmemMovableHiddenBytes >= (4ULL << 20);
        case VE_LEVEL_FG_IDLE:  return _vidmemMovableFgIdleBytes >= (4ULL << 20);
        case VE_LEVEL_FG_ALLOC: return _vidmemMovableFgAllocBytes >= (4ULL << 20);
        default:                return NV_TRUE;
    }
}

NvU64
vepReclaimableFor(NvU32 pid)
{
    if (!_vidmemAutoActive() || !_vidmemHintsActive() || (pid == 0))
        return 0;
    switch (VE_HINT_STATE(_vidmemHint(pid, NULL)))
    {
        case VE_HINT_FG:      return _vidmemReclaimableFgBytes;
        case VE_HINT_VISIBLE: return _vidmemReclaimableBytes;
        default:              return 0;
    }
}

//
// Free memory a restore of anyone but the foreground must leave behind
// (problems #6, #9): nothing comes back while the foreground still has memory
// in sysmem, and while the foreground is a game (>= 1 GB) it keeps its
// headroom (RmVidmemFgHeadroomMB), so its next allocations do not move the
// same apps out again (Claude and Bottles came back every 30-60 s next to
// WoW and went out at the game's next allocation, a 30-40 ms frame each
// time). Otherwise the usual low mark + hysteresis.
//
#define VIDMEM_EVICT_KEEP_BLOCKED NV_U64_MAX
// The smallest piece of the foreground's memory worth a restore call.
#define VIDMEM_EVICT_MIN_RESTORE_MB 16
//
// A restore pauses the process it brings back for as long as the copy takes
// (its channels are stopped). The foreground's memory comes back in pieces
// that take about this long at the restore speed seen so far: more right
// after the user switched to it (the switch is a moment of change anyway),
// little while it runs (a frame or a few, not a stall).
//
#define VIDMEM_EVICT_FG_RESTORE_SWITCH_MS 400
#define VIDMEM_EVICT_FG_RESTORE_RUN_MS    100
#define VIDMEM_EVICT_FG_SWITCH_SEC        4
static NvU64 _vidmemRestoreMBps = 4000;    // estimate, updated by every restore of the foreground

static NvU64
_vidmemEvictFgRestoreCapMB(VIDMEM_EVICT_ACTIVITY *pAct, NvU64 nowNs)
{
    NvU64 ms = ((pAct->fgSinceNs != 0) &&
                (VE_AGO_NS(nowNs, pAct->fgSinceNs) < VIDMEM_EVICT_FG_SWITCH_SEC * 1000000000ULL)) ?
               VIDMEM_EVICT_FG_RESTORE_SWITCH_MS : VIDMEM_EVICT_FG_RESTORE_RUN_MS;

    return NV_MAX(_vidmemRestoreMBps * ms / 1000, 32ULL);
}

static NvU64
_vidmemEvictKeepForFgMB(VIDMEM_EVICT_PROC *pProcs, NvU32 count)
{
    NvU32 i;

    for (i = 0; i < count; i++)
    {
        if (VE_HINT_STATE(pProcs[i].hint) != VE_HINT_FG)
            continue;
        if (pProcs[i].evictedBytes != 0)
            return VIDMEM_EVICT_KEEP_BLOCKED;
        //
        // Its headroom and a piece more, game or not: the headroom is
        // refilled for any focused window (vepHeadroomRun()), so whatever
        // came back leaving less went out again at the window's next
        // allocation. Only a game counted here, and next to any other focus
        // the low mark (512 MB) was enough to come back: Chrome, 1.5 GB out
        // for Claude's headroom, got 869 MB back two seconds later, gave 975
        // again and got 264 back (host, 2026-11-12 00:42; any amount below
        // headroom - 512 MB went round like that).
        //
        if (_vidmemFgHeadroomMB != 0)
            return NV_MAX((NvU64)_vidmemFgHeadroomMB + VIDMEM_EVICT_HINT_HYST_MB,
                          VIDMEM_EVICT_HINT_LOW_MB + VIDMEM_EVICT_HINT_HYST_MB);
    }
    return VIDMEM_EVICT_HINT_LOW_MB + VIDMEM_EVICT_HINT_HYST_MB;
}

// MB that could be moved out for `level`, `exceptPid` aside (problem #11).
static NvU64
_vidmemEvictFreeableMB(VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU32 exceptPid, NvU32 level, NvU64 nowNs)
{
    NvU64 sum = 0;
    NvU32 i;

    for (i = 0; i < count; i++)
    {
        if (_vidmemEvictHintVictimOk(&pProcs[i], exceptPid, nowNs, level))
            sum += pProcs[i].eligibleBytes;
    }
    return sum >> 20;
}

// Was `pAct` moved out for the foreground less than VIDMEM_EVICT_FG_HOLD_SEC ago?
static NvBool
_vidmemEvictInFgHold(VIDMEM_EVICT_ACTIVITY *pAct, NvU64 nowNs)
{
    return (pAct->evictedForFgNs != 0) &&
           (VE_AGO_NS(nowNs, pAct->evictedForFgNs) < VIDMEM_EVICT_FG_HOLD_SEC * 1000000000ULL);
}

// What the foreground keeps free, as of the last policy step (KEEP_BLOCKED: nothing comes back).
static NvU64 _vidmemLastKeepMB = VIDMEM_EVICT_HINT_LOW_MB + VIDMEM_EVICT_HINT_HYST_MB;

//
// A hot evicted allocation of `pid` (access counters, I5): how much must stay
// free after it comes back (MB), or NV_U64_MAX for not now. The same rules as
// for whole processes (problems #6, #9): the foreground's own always; others
// not while the foreground has memory in sysmem, not within the hold after
// they were moved out for it, and only leaving the game its headroom
// (nvtest fgprio-anim with counters on: a visible app's 6 MB buffer came back
// and was taken by the focused game, again and again).
//
NvU64
vepHotRestoreKeepMB(NvU32 pid)
{
    VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pid, NV_FALSE);

    if (VE_HINT_STATE(_vidmemHint(pid, NULL)) == VE_HINT_FG)
        return VIDMEM_EVICT_RESTORE_MARGIN_MB;
    if (_vidmemLastKeepMB == VIDMEM_EVICT_KEEP_BLOCKED)
        return NV_U64_MAX;
    if ((pAct != NULL) && _vidmemEvictInFgHold(pAct, osGetMonotonicTimeNs()))
        return NV_U64_MAX;
    // The focused window's headroom when there is one; else a piece needs only a margin.
    return (_vidmemLastKeepMB > VIDMEM_EVICT_HINT_LOW_MB + VIDMEM_EVICT_HINT_HYST_MB) ?
           _vidmemLastKeepMB : VIDMEM_EVICT_RESTORE_MARGIN_MB;
}

//
// A process about to get its memory back has contiguous buffers in sysmem:
// free bytes are no place for them (host, 2026-11-10 20:45: 379 MB of the
// game on the other monitor moved out for Chrome's 130 MB buffers, which then
// did not fit: "restore failed 4"). A window is cleared for the biggest one,
// one a step, by moving what is in it to other vidmem pages first, from those
// who may give at `level` (problem #15, I11). After a failure no new try for
// 10 s. NV_TRUE: a window is there.
//
static NvBool
_vidmemEvictContigWindow(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU32 i, NvU32 level, NvU64 nowNs)
{
    VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pProcs[i].pid, NV_TRUE);
    NvU64  windowMB = 0;
    NvBool bWindow;

    if ((pProcs[i].evictedContigBytes == 0) ||
        ((pAct->contigFailNs != 0) && (VE_AGO_NS(nowNs, pAct->contigFailNs) < 10 * 1000000000ULL)))
        return NV_FALSE;
    _vidmemEvictWhy = VMT_WHY_ALLOC;
    bWindow = vepPlatMakeContig(pGpu, pProcs, count, pProcs[i].evictedContigMax, pProcs[i].evictedContigAlign,
                                pProcs[i].pid, level, &windowMB);
    pAct->contigFailNs = bWindow ? 0 : nowNs;
    return bWindow;
}

// The policy step when window hints are on. API lock RW, GPU lock held.
static void
_vidmemEvictPolicyHints(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 nowNs)
{
    NvU64 avail, keepMB;
    NvU32 i;

    //
    // 1. The foreground gets its memory back now; others (hidden ones first,
    // visible ones still drawing if need be) make room. What fits comes back
    // even when not all of it does (problem #10: CoD's 1 GB waited in sysmem
    // for 8 minutes while the game filled every MB that came free with new
    // data, 1353 failed all-or-nothing restores, 12-16 GB/s of PCIe, a third
    // of the fps), the rest at the next steps as room appears.
    //
    for (i = 0; i < count; i++)
    {
        VIDMEM_EVICT_ACTIVITY *pAct;
        NvU64 needMB = pProcs[i].evictedBytes >> 20, budgetMB, moved = 0;
        NvU64 contigMB = NV_MIN(pProcs[i].evictedContigBytes >> 20, needMB);
        NvBool bWindow = NV_FALSE;

        if ((pProcs[i].evictedBytes == 0) || (VE_HINT_STATE(pProcs[i].hint) != VE_HINT_FG))
            continue;
        pAct = _vidmemActivityFind(pProcs[i].pid, NV_TRUE);
        _vidmemEvictWhy = VMT_WHY_ROOM;
        //
        // Room for all of it plus the low mark if others can give it. Not for
        // its contiguous buffers: they get a window (_vidmemEvictContigWindow()).
        //
        avail = _vidmemEvictHintFree(pGpu, pProcs, count, needMB - contigMB + VIDMEM_EVICT_HINT_LOW_MB,
                                     pProcs[i].pid, VE_LEVEL_FG_RESTORE, "foreground again");
        if (contigMB != 0)
        {
            bWindow = _vidmemEvictContigWindow(pGpu, pProcs, count, i, VE_LEVEL_FG_RESTORE, nowNs);
            avail = vepPlatAvailMB(pGpu);
        }
        //
        // Down to a small margin: a game that fills vidmem must get all of
        // it back (nvtest fgprio: the game wants exactly what is there). The
        // compositor and new contexts make room for themselves when needed.
        //
        //
        // Down to a small margin: a game that fills vidmem must get all of
        // it back (nvtest fgprio: the game wants exactly what is there). The
        // compositor and new contexts make room for themselves when needed.
        //
        // All of it when it fits with 16 MB to spare (as before #10): with
        // the 64 MB margin a game that fills vidmem kept its last few MB in
        // sysmem for good (nvtest fgprio, 2026-11-07: 1482 of 1488 MB).
        if (avail >= needMB + 16)
            budgetMB = needMB;
        else
            budgetMB = (avail > VIDMEM_EVICT_RESTORE_MARGIN_MB) ? avail - VIDMEM_EVICT_RESTORE_MARGIN_MB : 0;
        budgetMB = NV_MIN(budgetMB, _vidmemEvictFgRestoreCapMB(pAct, nowNs));
        // Nothing fitted last time and there is no more room now: no new try.
        if ((pAct->restoreStuckMB != 0) && (avail <= pAct->restoreStuckMB) && !bWindow)
            budgetMB = 0;
        if (budgetMB < VIDMEM_EVICT_MIN_RESTORE_MB)
        {
            // Said once a minute, not every step (problem #11).
            if ((pAct->lastStuckLogNs == 0) ||
                (VE_AGO_NS(nowNs, pAct->lastStuckLogNs) >= 60 * 1000000000ULL))
            {
                NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: %s (pid %u) is the foreground, %llu MB in sysmem, only %llu MB free\n",
                          pProcs[i].name, pProcs[i].pid, needMB, avail);
                pAct->lastStuckLogNs = nowNs;
            }
            continue;
        }
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: restoring %s (pid %u, foreground, %llu of %llu MB)\n",
                  pProcs[i].name, pProcs[i].pid, NV_MIN(budgetMB, needMB), needMB);
        _vidmemEvictWhy = VMT_WHY_RESTORE;
        _vidmemEvictBudgetBytes = (budgetMB < needMB) ? (budgetMB << 20) : 0;
        {
            NvU64 t0 = osGetMonotonicTimeNs(), tookNs;

            (void)_veMove(pGpu, 2, pProcs[i].pid, &moved);
            tookNs = VE_AGO_NS(osGetMonotonicTimeNs(), t0);
            if ((moved >= (16ULL << 20)) && (tookNs >= 1000000ULL))
                _vidmemRestoreMBps = (_vidmemRestoreMBps +
                                      NV_MAX((moved >> 20) * 1000000000ULL / tookNs, 100ULL)) / 2;
        }
        _vidmemEvictBudgetBytes = 0;
        pProcs[i].evictedBytes -= NV_MIN(pProcs[i].evictedBytes, moved);
        // No allocation fitted in the budget (the smallest is bigger): wait for more room.
        pAct->restoreStuckMB = (moved == 0) ? avail : 0;
        if (pProcs[i].evictedBytes == 0)
        {
            pAct->evictedForFgNs = 0;
            pAct->lastStuckLogNs = 0;
        }
    }
    keepMB = _vidmemEvictKeepForFgMB(pProcs, count);
    _vidmemLastKeepMB = keepMB;

    //
    // 1a. Other visible processes come back when there is room for them and
    // what the foreground keeps; only hidden processes make room. One moved
    // out for the foreground stays out for VIDMEM_EVICT_FG_HOLD_SEC.
    //
    for (i = 0; (i < count) && (keepMB != VIDMEM_EVICT_KEEP_BLOCKED); i++)
    {
        VIDMEM_EVICT_ACTIVITY *pAct;
        NvU64 needMB = pProcs[i].evictedBytes >> 20;

        if ((pProcs[i].evictedBytes == 0) || (VE_HINT_STATE(pProcs[i].hint) != VE_HINT_VISIBLE))
            continue;
        pAct = _vidmemActivityFind(pProcs[i].pid, NV_TRUE);
        if (_vidmemEvictInFgHold(pAct, nowNs))
            continue;
        avail = vepPlatAvailMB(pGpu);
        if (avail < needMB + keepMB)
        {
            // Nobody moves unless that makes it fit (problem #11).
            if (avail + _vidmemEvictFreeableMB(pProcs, count, pProcs[i].pid, VE_LEVEL_HIDDEN, nowNs) <
                needMB + keepMB)
                continue;
            _vidmemEvictWhy = VMT_WHY_ROOM;
            avail = _vidmemEvictHintFree(pGpu, pProcs, count, needMB + keepMB,
                                         pProcs[i].pid, VE_LEVEL_HIDDEN, "visible again");
            if (avail < needMB + keepMB)
                continue;
        }
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: restoring %s (pid %u, visible, %llu MB)\n",
                  pProcs[i].name, pProcs[i].pid, needMB);
        (void)_vidmemEvictContigWindow(pGpu, pProcs, count, i, VE_LEVEL_HIDDEN, nowNs);
        _vidmemEvictWhy = VMT_WHY_RESTORE;
        (void)_veMove(pGpu, 2, pProcs[i].pid, NULL);
        pProcs[i].evictedBytes = 0;
        pAct->evictedForFgNs = 0;
    }

    //
    // 1b. A process moved out that draws steadily again comes back: from
    // sysmem it would load PCIe and GR for everyone. After the foreground,
    // which the user is working with, and with what it keeps free; one moved
    // out for the foreground also waits out the hold (problem #9: ghostty
    // came back through here in the step it was moved out in). A visible one
    // gets room made by idle hidden processes; a hidden one (minimized, behind
    // the game) only takes memory that is free anyway: it is slowed down, not
    // the others (a minimized launcher drawing a frame now and then
    // ping-ponged six times and took 241 MB of the game at a switch,
    // 2026-11-04).
    //
    for (i = 0; (i < count) && (keepMB != VIDMEM_EVICT_KEEP_BLOCKED); i++)
    {
        VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pProcs[i].pid, NV_TRUE);
        NvU64 needMB = pProcs[i].evictedBytes >> 20;
        NvBool bVisible = (VE_HINT_STATE(pProcs[i].hint) == VE_HINT_VISIBLE);

        if ((pProcs[i].evictedBytes == 0) || pProcs[i].bNever ||
            (VE_HINT_STATE(pProcs[i].hint) == VE_HINT_FG) ||
            (pAct->drawingSinceNs == 0) ||
            (VE_AGO_NS(nowNs, pAct->drawingSinceNs) < VIDMEM_EVICT_STEADY_SEC * 1000000000ULL) ||
            _vidmemEvictInFgHold(pAct, nowNs))
            continue;
        if (bVisible)
        {
            avail = vepPlatAvailMB(pGpu);
            if (avail + _vidmemEvictFreeableMB(pProcs, count, pProcs[i].pid, VE_LEVEL_HIDDEN, nowNs) <
                needMB + keepMB)
                continue;
            _vidmemEvictWhy = VMT_WHY_ROOM;
            avail = _vidmemEvictHintFree(pGpu, pProcs, count, needMB + keepMB,
                                         pProcs[i].pid, VE_LEVEL_HIDDEN, "drawing again");
            if (avail < needMB + keepMB)
                continue;
        }
        else if (vepPlatAvailMB(pGpu) < needMB + keepMB)
        {
            continue;
        }
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: restoring %s (pid %u, drawing again, %s, %llu MB)\n",
                  pProcs[i].name, pProcs[i].pid, bVisible ? "visible" : "not visible", needMB);
        if (bVisible)
            (void)_vidmemEvictContigWindow(pGpu, pProcs, count, i, VE_LEVEL_HIDDEN, nowNs);
        _vidmemEvictWhy = VMT_WHY_RESTORE;
        (void)_veMove(pGpu, 2, pProcs[i].pid, NULL);
        pProcs[i].evictedBytes = 0;
        pAct->evictedForFgNs = 0;
    }

    // 1c. Evicted allocations in use (access counters) come back one by one.
    vepPlatHotRestore(pGpu, pProcs, count);

    //
    // 2. Pressure: hidden processes out until lowMB + hysteresis is free.
    //
    avail = vepPlatAvailMB(pGpu);
    if (avail < VIDMEM_EVICT_HINT_LOW_MB)
    {
        NvU32 level = VE_LEVEL_HIDDEN, fgPid = 0;

        //
        // A game in the foreground (>= 1 GB) is what fills vidmem: visible
        // but idle apps give up memory too. Not every app asks beyond its
        // budget (GL keeps textures in sysmem itself once vidmem looks
        // full, nvtest.sh fgprio), so waiting for its allocations is not enough.
        //
        for (i = 0; i < count; i++)
        {
            if ((VE_HINT_STATE(pProcs[i].hint) == VE_HINT_FG) && _vidmemEvictIsGame(&pProcs[i]))
            {
                level = VE_LEVEL_FG_IDLE;
                fgPid = pProcs[i].pid;
                break;
            }
        }
        _vidmemLastPressureNs = nowNs;
        _vidmemEvictWhy = VMT_WHY_PRESSURE;
        (void)_vidmemEvictHintFree(pGpu, pProcs, count, VIDMEM_EVICT_HINT_LOW_MB + VIDMEM_EVICT_HINT_HYST_MB, fgPid,
                                   level, (level == VE_LEVEL_HIDDEN) ? "pressure" : "pressure from the foreground game");
        return;
    }

    //
    // 3. Plenty free for a while: bring hidden processes back too, so they
    // are ready when shown (one per step, as without hints). They are in no
    // hurry: shown, they come back at once (steps 1, 1a), and drawing while
    // hidden, through 1b. So not while the window they made room for is
    // still in focus, and only after they would have fitted, with what the
    // foreground keeps, for VIDMEM_EVICT_HIDDEN_BACK_SEC without a break
    // (problem #6, host 2026-11-08: the focused window grew and wanted its
    // headroom, Battle.net, Claude and Steam went out, 37-81 s later it
    // shrank, after 30 quiet seconds they came back, and at its next growth
    // went out again: 1.8 GB of copies a round, two 0.75 s freezes with it).
    //
    {
        NvU32  fgPid  = _vidmemEvictFgPid(pProcs, count);
        NvBool bQuiet = (VE_AGO_NS(nowNs, _vidmemLastPressureNs)) >= (VIDMEM_EVICT_QUIET_SEC * 1000000000ULL);
        NvBool bMoved = NV_FALSE;

        for (i = 0; (i < count) && (_vidmemEvictBytes != 0); i++)
        {
            VIDMEM_EVICT_ACTIVITY *pAct;
            NvU64 needMB = pProcs[i].evictedBytes >> 20;

            if ((pProcs[i].evictedBytes == 0) || pProcs[i].bNever)
                continue;
            pAct = _vidmemActivityFind(pProcs[i].pid, NV_TRUE);
            // The wait starts when that window leaves the focus: a look at another one is not the end of the game.
            if ((keepMB == VIDMEM_EVICT_KEEP_BLOCKED) ||
                ((pAct->outForPid != 0) && (pAct->outForPid == fgPid)) ||
                (avail < needMB + NV_MAX((NvU64)_vidmemHighMB, keepMB)))
            {
                pAct->roomSinceNs = 0;
                continue;
            }
            if (pAct->roomSinceNs == 0)
                pAct->roomSinceNs = nowNs;
            if (bMoved || !bQuiet ||
                (VE_AGO_NS(nowNs, pAct->roomSinceNs) < VIDMEM_EVICT_HIDDEN_BACK_SEC * 1000000000ULL))
                continue;
            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: plenty free (%llu MB), restoring %s (pid %u, %llu MB)\n",
                      avail, pProcs[i].name, pProcs[i].pid, needMB);
            _vidmemEvictWhy = VMT_WHY_IDLE_BACK;
            (void)_veMove(pGpu, 2, pProcs[i].pid, NULL);
            pAct->outForPid = 0;
            bMoved = NV_TRUE;
        }
    }
}

//
// Background compaction: the longest window that `budgetFrames` of moving
// turns into one free run, among the stretches without a fixed frame that
// are longer than the run there is now (shorter ones can never beat it).
// Step after step the same window grows: what is free in it costs nothing
// the next time.
//
// A stretch apart from the run can be cleared only into the free chunks
// outside the run (the run is kept), so it gets as long as those at most:
// when that is no longer than the run, clearing it is work for nothing. The
// simulator showed it at once with a game's 256 MB blocks as walls 1.1 GB
// apart: the run at its 1056 MB, 640 MB free elsewhere, and eight more steps
// moved 600 MB of the game into a second window that could never be longer.
//
static void
_veContigAskAge(NvU64 nowNs)
{
    NvU64 halfNs = (NvU64)VIDMEM_EVICT_COMPACT_NEED_SEC * 1000000000ULL / 2;

    if (VE_AGO_NS(nowNs, _vidmemContigAskNs) < halfNs)
        return;
    // Two halves ago is forgotten; after a long silence both are.
    _vidmemContigAskBytes[1] = (VE_AGO_NS(nowNs, _vidmemContigAskNs) < 2 * halfNs) ? _vidmemContigAskBytes[0] : 0;
    _vidmemContigAskBytes[0] = 0;
    _vidmemContigAskNs       = nowNs;
}

void
vepNoteContig(NvU64 size)
{
    _veContigAskAge(osGetMonotonicTimeNs());
    _vidmemContigAskBytes[0] = NV_MAX(_vidmemContigAskBytes[0], size);
}

NvU64
vepCompactNeedMB(void)
{
    _veContigAskAge(osGetMonotonicTimeNs());
    return NV_ALIGN_UP64(NV_MAX(_vidmemContigAskBytes[0], _vidmemContigAskBytes[1]), 1ULL << 20) >> 20;
}

NvU64
vepCompactWantMB(NvU64 freeMB)
{
    NvU64 wantMB;

    if (_vidmemCompactRunMB == 0)
        return 0;
    //
    // The knob, or three quarters of what is free when that is more. A fixed
    // 1 GB was "reached" on a desktop with 10.6 GB free in 88 runs, the
    // longest 1.2 GB, and room for a 32 MB buffer of the compositor then took
    // moving 137 MB (2026-11-10, nvtest focus and comproom failed).
    //
    wantMB = NV_MAX((NvU64)_vidmemCompactRunMB, freeMB * VIDMEM_EVICT_COMPACT_FREE_PCT / 100);
    // Next to a game no more than the requests seen need (vidmem_evict_policy.h).
    if (_vidmemFgIsGame)
        wantMB = NV_MIN(wantMB, NV_MAX(vepCompactNeedMB() * VIDMEM_EVICT_COMPACT_NEED_TIMES,
                                       (NvU64)VIDMEM_EVICT_COMPACT_NEED_MIN_MB));
    return wantMB;
}

//
// And what is in the window must fit into the free chunks outside it (and
// outside the run, when the window is apart from it and the run is kept):
// the window grows step after step, and without this it grew past all the
// free memory there is. The simulator, placing memory as the driver does:
// 1698 MB free, the run at 1168 MB, the window 1952 MB with 250 MB to move
// and 2 MB free outside it; every step after that found "no room", and the
// run stayed 100 MB short of what was wanted.
//
// What a window costs is what the step will copy: an allocation moves whole,
// so one with a corner in the window brings the rest of itself (pOwners: per
// chunk the allocation of more than a chunk that holds most of it, and how
// much of that allocation is in the chunk). Counted by the frames inside
// only, the window above ended on the edge of a 256 MB block of the game:
// 2 MB of it inside, no room for 256 MB outside, and no shorter window was
// tried. pOwners NULL: by the frames inside, as before.
//
NvBool
vepCompactWindow(const NvU32 *pCost, const VEP_COMPACT_OWNERS *pOwners, NvU64 chunks, NvU64 chunkFrames,
                 NvU64 runStart, NvU64 runLen, NvU64 budgetFrames, NvU64 *pLo, NvU64 *pHi)
{
    NvU64 seg = 0, best = 0, bestCost = 0, lo = 0, hi = 0, freeChunks = 0, c;

    for (c = 0; c < chunks; c++)
        freeChunks += (pCost[c] == 0);
    for (c = 0; c <= chunks; c++)
    {
        NvU64 l, r, cost, drag, freeIn;

        if ((c < chunks) && (pCost[c] != VEP_COMPACT_FIXED))
            continue;
        if ((c - seg > runLen) &&
            (((runStart >= seg) && (runStart < c)) || (freeChunks - NV_MIN(freeChunks, runLen) > runLen)))
        {
            for (l = r = seg, cost = 0, drag = 0, freeIn = 0; r <= c; r++)
            {
                NvU32 o;

                // Past the stretch: only takes the chunks left out of the window (pCount back to zeros).
                if (r < c)
                {
                    cost   += pCost[r];
                    freeIn += (pCost[r] == 0);
                    o = (pOwners != NULL) ? pOwners->pOwner[r] : 0;
                    if (o != 0)
                    {
                        if (pOwners->pCount[o]++ == 0)
                            drag += pOwners->pFrames[o];
                        drag -= NV_MIN(drag, (NvU64)pOwners->pOwn[r]);
                    }
                }
                for (;;)
                {
                    // Free chunks for what moves out: not in the window, not in the run when it is kept.
                    NvBool bKeep = (runStart >= r + 1) || (runStart + runLen <= l);
                    NvU64  room = freeChunks - freeIn;

                    room -= bKeep ? NV_MIN(room, runLen) : 0;
                    if ((l > r) || (l == c) ||
                        ((r < c) && (cost + drag <= budgetFrames) && (cost + drag <= room * chunkFrames)))
                        break;
                    cost   -= pCost[l];
                    freeIn -= (pCost[l] == 0);
                    o = (pOwners != NULL) ? pOwners->pOwner[l] : 0;
                    if (o != 0)
                    {
                        drag += pOwners->pOwn[l];
                        if (--pOwners->pCount[o] == 0)
                            drag -= NV_MIN(drag, (NvU64)pOwners->pFrames[o]);
                    }
                    l++;
                }
                if ((l > r) || (r == c))
                    continue;
                if ((cost != 0) && ((r + 1 - l > best) || ((r + 1 - l == best) && (cost + drag < bestCost))))
                {
                    best     = r + 1 - l;
                    bestCost = cost + drag;
                    lo       = l;
                    hi       = r + 1;
                }
            }
        }
        seg = c + 1;
    }
    *pLo = lo;
    *pHi = hi;
    return hi != lo;
}

//
// One policy step, after the platform sampled the GPU activity
// (_vidmemActivityNote(), lastSubmitNs). API lock RW, GPU lock held.
//
void
vepPolicyStep(OBJGPU *pGpu)
{
    VIDMEM_EVICT_PROC *pProcs;
    NvU64 nowNs = osGetMonotonicTimeNs();
    NvU64 avail;
    NvU32 count, busiest, done = 0, i;

    // Drawing streaks ("drawing again" wants a steady one, not a frame now and then).
    for (i = 0; i < VIDMEM_EVICT_MAX_PROCS; i++)
    {
        VIDMEM_EVICT_ACTIVITY *pAct = &_vidmemActivity[i];

        if (pAct->pid == 0)
            continue;
        if (_vidmemActivityIdleSec(pAct->pid) >= VIDMEM_EVICT_TICK_SEC)
            pAct->drawingSinceNs = 0;
        else if (pAct->drawingSinceNs == 0)
            pAct->drawingSinceNs = nowNs;
    }
    {
        NvU32 busiest = _vidmemActivityBusiest();

        if (busiest != _vidmemBusiestPid)
            _vidmemBusiestSinceNs = osGetMonotonicTimeNs();
        _vidmemBusiestPid = busiest;
    }
    if (_vidmemBusiestPid != 0)
        _vidmemActivityFind(_vidmemBusiestPid, NV_TRUE)->lastBusiestNs = osGetMonotonicTimeNs();
    if (!_vidmemAutoActive())
        return;

    // Common case without hints: no pressure, nothing to restore; skip the
    // client walk. With hints every step walks them: the reclaimable total
    // (budget of visible processes) must stay current.
    avail = vepPlatAvailMB(pGpu);
    if (!_vidmemHintsActive() && (avail >= _vidmemLowMB) && (_vidmemEvictBytes == 0))
        return;

    pProcs = portMemAllocNonPaged(sizeof(*pProcs) * VIDMEM_EVICT_MAX_PROCS);
    if (pProcs == NULL)
        return;

    count   = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);
    if (_vidmemHintsActive())
    {
        NvU32 moves = _vidmemPolicyMoves;
        NvU64 evicted = _vidmemEvictBytes;

        _vidmemFgIsGame = NV_FALSE;
        for (i = 0; i < count; i++)
        {
            if ((VE_HINT_STATE(pProcs[i].hint) == VE_HINT_FG) && _vidmemEvictIsGame(&pProcs[i]))
                _vidmemFgIsGame = NV_TRUE;
        }
        _vidmemEvictUpdateReclaimable(pProcs, count, nowNs);
        _vidmemEvictPolicyHints(pGpu, pProcs, count, nowNs);
        //
        // A quiet step (nothing moved, no pressure lately): compact a little
        // (problem #16), so a contiguous request finds its window instead of
        // waiting for one to be cleared.
        //
        if (_vidmemCompactRunSeenMB != _vidmemCompactRunMB)
        {
            _vidmemCompactRunSeenMB = _vidmemCompactRunMB;
            _vidmemCompactRestNs    = 0;
            _vidmemCompactIdleSteps = 0;
        }
        //
        // The focus changed: who may move in what pieces is different now
        // (the game the user looked away from gives its big blocks), so the
        // long rest after fruitless steps ends. With 318 changes of focus in
        // five hours next to a game the rest still stood at its 15 minutes
        // (host, 2026-11-11). One look per shortest rest at most: a look
        // walks every allocation under the locks, and alt-tab comes in bursts.
        // The rest keeps its length: a fruitless look is followed by as long
        // a one as before.
        //
        if (_vidmemCompactFgPid != _vidmemEvictFgPid(pProcs, count))
        {
            _vidmemCompactFgPid    = _vidmemEvictFgPid(pProcs, count);
            _vidmemCompactRestNs   = NV_MIN(_vidmemCompactRestNs,
                                            _vidmemCompactTryNs + VIDMEM_EVICT_COMPACT_REST_SEC * 1000000000ULL);
            _vidmemCompactStalled  = 0;
            _vidmemCompactWindowMB = 0;
        }
        if ((moves != _vidmemPolicyMoves) || (evicted != _vidmemEvictBytes))
        {
            // The layout changed: the next quiet step looks again without the long rest.
            _vidmemCompactIdleSteps = 0;
        }
        else if ((_vidmemCompactRunMB != 0) && (nowNs >= _vidmemCompactRestNs) &&
                 (VE_AGO_NS(nowNs, _vidmemLastPressureNs) >= (VIDMEM_EVICT_QUIET_SEC * 1000000000ULL)))
        {
            NvU64 stepMB = VIDMEM_EVICT_COMPACT_STEP_MB, fgStepMB = VIDMEM_EVICT_COMPACT_STEP_MB;
            NvU64 windowMB = 0, movedMB;

            if (_vidmemCompactBig)
            {
                fgStepMB = VIDMEM_EVICT_COMPACT_FG_STEP_MB;
                stepMB   = _vidmemFgIsGame ? VIDMEM_EVICT_COMPACT_FG_STEP_MB : VIDMEM_EVICT_COMPACT_BIG_STEP_MB;
            }
            _vidmemEvictWhy = VMT_WHY_COMPACT;
            _vidmemCompactTryNs = nowNs;
            movedMB = vepPlatCompact(pGpu, pProcs, count, stepMB, fgStepMB, &windowMB);
            if ((movedMB != 0) && (windowMB > _vidmemCompactWindowMB))
            {
                // Getting somewhere.
                _vidmemCompactIdleSteps = 0;
                _vidmemCompactStalled   = 0;
            }
            else if ((movedMB == 0) || (++_vidmemCompactStalled >= VIDMEM_EVICT_COMPACT_STALL_STEPS))
            {
                if (movedMB != 0)
                    NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: compaction: %u steps moved memory and the window (%llu MB) did not grow, resting\n",
                              _vidmemCompactStalled, windowMB);
                // Nothing to do: 14 s, 28 s, ... up to 15 min while nothing changes.
                _vidmemCompactRestNs = nowNs + ((VIDMEM_EVICT_COMPACT_REST_SEC * 1000000000ULL) <<
                                                NV_MIN(_vidmemCompactIdleSteps, 6));
                _vidmemCompactIdleSteps++;
                _vidmemCompactStalled = 0;
            }
            if (movedMB != 0)
                _vidmemCompactWindowMB = windowMB;
        }
        // Moves change what is left to reclaim: walk again only after some.
        if ((moves != _vidmemPolicyMoves) || (evicted != _vidmemEvictBytes))
        {
            count = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);
            _vidmemEvictUpdateReclaimable(pProcs, count, osGetMonotonicTimeNs());
        }
        _vidmemEvictKeepProcs(pProcs, count);
        portMemFree(pProcs);
        return;
    }
    busiest = _vidmemEvictBusiestPid(pProcs, count);

    if (avail < _vidmemLowMB)
    {
        _vidmemLastPressureNs = nowNs;

        // Least recently active first; "never active" counts as oldest.
        while ((done < VIDMEM_EVICT_MAX_PER_TICK) && (avail < _vidmemLowMB + VIDMEM_EVICT_HYST_MB))
        {
            VIDMEM_EVICT_PROC *pPick = NULL;
            NvU32 pickIdle = 0;
            NvU64 moved = 0;

            for (i = 0; i < count; i++)
            {
                NvU32 idle = _vidmemActivityIdleSec(pProcs[i].pid);

                if (pProcs[i].bNever || (pProcs[i].pid == busiest) ||
                    (pProcs[i].eligibleBytes < (4ULL << 20)) || (idle < VIDMEM_EVICT_IDLE_SEC) ||
                    _vidmemEvictIsProtected(pProcs[i].pid, nowNs))
                    continue;
                if ((pPick == NULL) || (idle > pickIdle) ||
                    ((idle == pickIdle) && (pProcs[i].eligibleBytes > pPick->eligibleBytes)))
                {
                    pPick = &pProcs[i];
                    pickIdle = idle;
                }
            }
            if (pPick == NULL)
                break;

            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: auto: avail %llu MB < %u MB, evicting %s (pid %u, %llu MB, idle %s%u s)\n",
                      avail, _vidmemLowMB, pPick->name, pPick->pid, pPick->eligibleBytes >> 20,
                      (pickIdle == NV_U32_MAX) ? ">" : "", (pickIdle == NV_U32_MAX) ? 10 : pickIdle);
            _vidmemEvictWhy = VMT_WHY_AUTO;
            (void)_veMove(pGpu, 1, pPick->pid, &moved);
            pPick->eligibleBytes = 0;
            avail += moved >> 20;
            done++;
        }
    }
    else if (_vidmemEvictBytes != 0)
    {
        NvBool bQuiet = (VE_AGO_NS(nowNs, _vidmemLastPressureNs)) >= (VIDMEM_EVICT_QUIET_SEC * 1000000000ULL);
        VIDMEM_EVICT_PROC *pPick = NULL;
        NvU32 pickIdle = NV_U32_MAX;

        // Most recently active first.
        for (i = 0; i < count; i++)
        {
            NvU32 idle = _vidmemActivityIdleSec(pProcs[i].pid);
            NvU64 needMB = pProcs[i].evictedBytes >> 20;
            NvBool bActive = (idle < VIDMEM_EVICT_IDLE_SEC);

            if ((pProcs[i].evictedBytes == 0) || pProcs[i].bNever)
                continue;
            // Active: as soon as it fits with headroom. Idle: only when the
            // pressure is long gone and highMB stays free afterwards (a game
            // loading a level frees memory for a moment; no ping-pong).
            if (avail < needMB + _vidmemLowMB + VIDMEM_EVICT_HYST_MB)
                continue;
            if (!bActive && !(bQuiet && (avail >= needMB + _vidmemHighMB)))
                continue;
            if ((pPick == NULL) || (idle < pickIdle))
            {
                pPick = &pProcs[i];
                pickIdle = idle;
            }
        }
        if (pPick != NULL)
        {
            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: auto: avail %llu MB, restoring %s (pid %u, %llu MB)\n",
                      avail, pPick->name, pPick->pid, pPick->evictedBytes >> 20);
            _vidmemEvictWhy = VMT_WHY_AUTO;
            (void)_veMove(pGpu, 2, pPick->pid, NULL);
        }
    }

    portMemFree(pProcs);
}

//
// Step 2: the user reserve is about to deny a vidmem allocation of `pid`.
// Should it be created in sysmem instead (vidmemEvictBornAllocation)? Only
// for idle processes, never the game or the desktop. The driver's wrapper
// (vidmemEvictWantBorn) adds what eviction supports (no contiguous/ISO/
// fixed-address requests, up to 512 MB) and whether the free memory can hold
// it for the moment it takes to move it. measureVidmem(ctx): the process's
// vidmem right now, when the last walk is older than 2 s.
//
NvBool
vepWantBorn(NvU32 pid, const char *name, NvU64 (*measureVidmem)(void *ctx), void *ctx)
{
    if (!_vidmemAutoActive())
        return NV_FALSE;
    //
    // With window hints: only for hidden processes (and idle ones without
    // windows). A visible one gets room made instead (vidmemEvictMakeRoom).
    //
    if (_vidmemHintsActive())
    {
        NvU32 st = VE_HINT_STATE(_vidmemHint(pid, NULL));

        if (st >= VE_HINT_VISIBLE)
            return NV_FALSE;
        if ((st == VE_HINT_UNKNOWN) && (_vidmemActivityIdleSec(pid) < VIDMEM_EVICT_IDLE_SEC))
            return NV_FALSE;
    }
    //
    // Everyone but the game (busiest in the last two minutes) and the desktop:
    // a denial breaks visible apps too (ghostty rendered garbage at 02:21),
    // sysmem only makes them slower.
    //
    //
    // Idle processes only: an active one (Steam playing a trailer, 02:50) would
    // render from sysmem and saturate PCIe for everyone. A denial is the
    // lesser evil for it.
    //
    if ((pid == 0) || _vidmemEvictIsNeverName(name) ||
        _vidmemEvictWasBusiest(pid, osGetMonotonicTimeNs()) ||
        (_vidmemActivityIdleSec(pid) < VIDMEM_EVICT_IDLE_SEC))
        return NV_FALSE;
    {
        //
        // Never for a process holding >= 1 GB of vidmem (a game, or a busy
        // browser): measured right here from the calling client (cached for
        // 2 s), since the policy tick may not have walked it yet (02:49: the
        // game got 128 MB in sysmem 40 s after it started).
        //
        VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pid, NV_TRUE);
        NvU64 nowNs = osGetMonotonicTimeNs();

        if ((pAct->vidmemAtNs == 0) || (VE_AGO_NS(nowNs, pAct->vidmemAtNs) > 2000000000ULL))
        {
            NvU64 bytes = measureVidmem(ctx);

            pAct->vidmemBytes = NV_MAX(bytes, (pAct->vidmemAtNs != 0) ? pAct->vidmemBytes : 0);
            pAct->vidmemAtNs  = nowNs;
        }
        if (pAct->vidmemBytes >= VIDMEM_EVICT_GAME_BYTES)
            return NV_FALSE;
    }
    return NV_TRUE;
}

//
// No reserve: free vidmem a background process may not take (registry
// RmVidmemBgCushionMB, 0: off). A hidden process, or a windowless one idle on
// the GPU, gets its new allocations in sysmem once less than this would be
// left (vidmemEvictWantBorn()), and is told that much less free memory: the
// last of vidmem goes to what the user works with and to the compositor.
// Not for what would then be denied instead of moved (the game in the
// background, a process with >= 1 GB, one drawing), and not for a process
// that was the foreground a moment ago.
//
NvU32 _vidmemBgCushionMB = 256;

NvU64
vepBackgroundCushion(NvU32 pid, const char *name)
{
    VIDMEM_EVICT_ACTIVITY *pAct;
    NvU64 nowNs;
    NvU32 st;

    if ((_vidmemBgCushionMB == 0) || !_vidmemAutoActive() || !_vidmemHintsActive() ||
        (pid == 0) || _vidmemEvictIsNeverName(name))
        return 0;
    st = VE_HINT_STATE(_vidmemHint(pid, NULL));
    if (st >= VE_HINT_VISIBLE)
        return 0;
    pAct = _vidmemActivityFind(pid, NV_FALSE);
    if (pAct == NULL)
        return 0;
    nowNs = osGetMonotonicTimeNs();
    if (_vidmemEvictIsProtected(pid, nowNs) ||
        (_vidmemActivityIdleSec(pid) < VIDMEM_EVICT_DRAWING_SEC) ||
        ((pAct->lastFgNs != 0) && (_vidmemPrevFgNs != 0) && (pAct->lastFgNs + 1000000000ULL >= _vidmemPrevFgNs)))
        return 0;
    //
    // A game in the foreground: the background is kept off its headroom too
    // (idea I7, as WDDM shrinks the budgets of background processes
    // first): a hidden app's new allocations go to sysmem while less than the
    // game's headroom is free, instead of taking memory the game's next
    // allocation would then have to move out again.
    //
    // As much as the headroom refill frees (vepHeadroomRun()), or the
    // background takes the refill's margin and is moved out at the next one.
    if (_vidmemFgIsGame && (_vidmemFgHeadroomMB > _vidmemBgCushionMB))
        return ((NvU64)_vidmemFgHeadroomMB + VIDMEM_EVICT_RESTORE_MARGIN_MB) << 20;
    return (NvU64)_vidmemBgCushionMB << 20;
}

//
// The user reserve is about to deny an allocation of `pid`. If it is a
// visible process (window hints), move hidden processes out right now and
// let the caller check again: the foreground app gets the memory, not a
// denial (a game would drop textures or spill into sysmem) and not sysmem.
// Moving 1 GB takes ~100 ms (2026-11-02, CeUtils page merging).
// vidmemEvictMakeRoom() in the driver: vepMakeRoomAllowed(), then the locks,
// then vepMakeRoomRun().
//
//
// Real free vidmem kept ahead of the foreground process's allocations
// (registry RmVidmemFgHeadroomMB, 0: off). A GL driver places its textures
// by the vidmem actually free while the game loads: told about reclaimable
// memory but finding 0.6 GB free, Superposition took the free 4.8 GB in 2 s
// and put the rest (~1.6 GB) in sysmem for good, 8 fps instead of 52 at 11
// GB/s of PCIe (2026-11-04, bench.py apps-evict), although its budget said
// 5 GB free all along and not one allocation was denied. So when an
// allocation of the foreground leaves less than this free, hidden and idle
// windowless processes move out first.
//
NvU32 _vidmemFgHeadroomMB = 2048;

// Nothing to move a moment ago: do not walk every client per allocation.
static NvU64 _vidmemMakeRoomFruitlessNs;

static NvBool _veMakeRoomAllowed(NvU32 pid, NvBool bContig);

NvBool
vepMakeRoomAllowed(NvU32 pid)
{
    return _veMakeRoomAllowed(pid, NV_FALSE);
}

//
// bContig: a contiguous request that found no place. A window for it is also
// cleared by moves inside vidmem of processes that may not be moved out (the
// ones that draw, problem #16), so "nobody to move out as of the last walk"
// does not turn it away (contigloop-test on the RTX 5000, 2026-11-11: the
// second 224 MB buffer of the focused app was denied with 7 GB free in pieces,
// the hidden apps around it checking their memory every 2 s, "drawing").
// At most one fruitless try a second, as for the rest.
//
static NvBool
_veMakeRoomAllowed(NvU32 pid, NvBool bContig)
{
    NvU64 nowNs = osGetMonotonicTimeNs();

    if (!_vidmemAutoActive() || (pid == 0) || !_vidmemHintsActive())
        return NV_FALSE;
    if (VE_HINT_STATE(_vidmemHint(pid, NULL)) < VE_HINT_VISIBLE)
        return NV_FALSE;
    if ((_vidmemMakeRoomFruitlessNs != 0) && (VE_AGO_NS(nowNs, _vidmemMakeRoomFruitlessNs) < 1000000000ULL))
        return NV_FALSE;
    if (bContig)
        return NV_TRUE;
    // Nobody to move for it as of the last walk: no walk now (#8).
    if (!_vidmemEvictAnyMovable((VE_HINT_STATE(_vidmemHint(pid, NULL)) == VE_HINT_FG) ?
                                VE_LEVEL_FG_ALLOC : VE_LEVEL_HIDDEN) &&
        !((VE_HINT_STATE(_vidmemHint(pid, NULL)) == VE_HINT_FG) && _vidmemFgOwnEnabled &&
          //
          // Its own memory is counted at a walk, and an app that took it since
          // the last one had none then: it was turned away (fgown-test on the
          // RTX 5000, 2026-11-11: 288 MB in 2 ms, then a denial; with "a walk
          // older than 250 ms" it passed or failed by how long the app took to
          // start). So a foreground that is not a game always gets its walk;
          // a fruitless one keeps the next away for a second (above).
          //
          ((_vidmemMovableFgOwnBytes >= (4ULL << 20)) || !_vidmemFgIsGame)))
        return NV_FALSE;
    return NV_TRUE;
}

// API lock RW, GPU lock held. *pWantMB, *pAvail: for the trace.
NvBool
vepMakeRoomRun(OBJGPU *pGpu, NvU32 pid, NvU64 size, NvU64 headroomMB, const char *why,
               NvU64 *pWantMB, NvU64 *pAvail)
{
    VIDMEM_EVICT_PROC *pProcs;
    NvU64              nowNs = osGetMonotonicTimeNs();
    NvU64              wantMB, avail;
    NvU32              count;
    NvBool             bOk, bFg;

    pProcs = portMemAllocNonPaged(sizeof(*pProcs) * VIDMEM_EVICT_MAX_PROCS);
    if (pProcs == NULL)
        return NV_FALSE;
    count = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);

    bFg    = (VE_HINT_STATE(_vidmemHint(pid, NULL)) == VE_HINT_FG);
    //
    // Some headroom, so the next allocations do not each come here. The
    // foreground waits only for what this allocation needs plus a margin;
    // its headroom follows in the background (vepHeadroomRun(), problem #5).
    //
    if (bFg && (_vidmemFgHeadroomMB != 0))
        headroomMB = NV_MIN(headroomMB, (NvU64)VIDMEM_EVICT_RESTORE_MARGIN_MB);
    wantMB = (size >> 20) + 1 + headroomMB;
    _vidmemEvictWhy = VMT_WHY_ALLOC;
    avail  = _vidmemEvictHintFree(pGpu, pProcs, count, wantMB, pid,
                                  bFg ? VE_LEVEL_FG_IDLE : VE_LEVEL_HIDDEN, why);
    //
    // The foreground's allocation still does not fit: visible processes that
    // draw (a game on the other monitor, a video) and the previous foreground
    // give exactly what it needs plus a margin, nothing for headroom
    // (problem #7: the WebGPU page focused next to CoD was denied and died;
    // R5: the focused game wins over a video, as the last resort).
    //
    if (bFg && (avail <= (size >> 20)))
        avail = _vidmemEvictHintFree(pGpu, pProcs, count, (size >> 20) + 1 + VIDMEM_EVICT_RESTORE_MARGIN_MB, pid,
                                     VE_LEVEL_FG_ALLOC, "allocation of the foreground");
    bOk    = (avail > (size >> 20));
    // Nobody left to give: its own older memory, as much as this allocation takes.
    if (bFg && !bOk && _vidmemFgOwnEnabled && (size <= (512ULL << 20)))
    {
        NvU32 k;

        for (k = 0; k < count; k++)
        {
            NvU64 moved = 0;

            if ((pProcs[k].pid != pid) || pProcs[k].bNever || _vidmemEvictIsGame(&pProcs[k]) ||
                (pProcs[k].eligibleBytes < size + (16ULL << 20)))
                continue;
            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: allocation of the foreground: free %llu MB, nobody left to give, %llu MB of %s's own memory (pid %u) to sysmem for its %llu KB\n",
                      avail, (size >> 20) + 16, pProcs[k].name, pid, size >> 10);
            _vidmemEvictWhy = VMT_WHY_ALLOC;
            _vidmemEvictBudgetBytes = size + (16ULL << 20);
            (void)_veMove(pGpu, 1, pid, &moved);
            _vidmemEvictBudgetBytes = 0;
            pProcs[k].eligibleBytes -= NV_MIN(pProcs[k].eligibleBytes, moved);
            avail = vepPlatAvailMB(pGpu);
            bOk   = (avail > (size >> 20));
            break;
        }
    }
    _vidmemEvictUpdateReclaimable(pProcs, count, nowNs);
    _vidmemMakeRoomFruitlessNs = bOk ? 0 : nowNs;
    if (bOk)
        _vidmemLastPressureNs = nowNs;

    portMemFree(pProcs);
    *pWantMB = wantMB;
    *pAvail  = avail;
    return bOk;
}

//
// The foreground's headroom is refilled in the background, not inside its
// allocation (problem #5: WoW froze for 0.9 s while 290 MB of Battle.net
// were copied out within one of its allocations that fitted anyway). The
// allocation path only asks (vepHeadroomRequest()); the driver then runs
// vepHeadroomRun() from a work item, again and again until the headroom is
// there, each run moving at most VIDMEM_EVICT_HEADROOM_STEP_MB, so the
// locks come free in between and the game's next RM call waits for one
// piece, not for all of it.
//
#define VIDMEM_EVICT_HEADROOM_STEP_MB 256
static NvU32 _vidmemHeadroomPid;

// NV_TRUE: a refill run should be queued (none pending).
NvBool
vepHeadroomRequest(NvU32 pid)
{
    NvBool bQueue = (_vidmemHeadroomPid == 0);

    _vidmemHeadroomPid = pid;
    return bQueue;
}

// One piece of the refill. API lock RW, GPU lock held. NV_TRUE: run again.
NvBool
vepHeadroomRun(OBJGPU *pGpu)
{
    VIDMEM_EVICT_PROC *pProcs;
    NvU32  pid = _vidmemHeadroomPid, count;
    NvU64  avail, target, before;
    NvBool bMore;

    if ((pid == 0) || !_vidmemAutoActive() || !_vidmemHintsActive() || (_vidmemFgHeadroomMB == 0) ||
        (VE_HINT_STATE(_vidmemHint(pid, NULL)) != VE_HINT_FG))
    {
        _vidmemHeadroomPid = 0;
        return NV_FALSE;
    }
    target = (NvU64)_vidmemFgHeadroomMB + VIDMEM_EVICT_RESTORE_MARGIN_MB;
    before = vepPlatAvailMB(pGpu);
    if ((before >= target) || !_vidmemEvictAnyMovable(VE_LEVEL_FG_IDLE))
    {
        _vidmemHeadroomPid = 0;
        return NV_FALSE;
    }
    pProcs = portMemAllocNonPaged(sizeof(*pProcs) * VIDMEM_EVICT_MAX_PROCS);
    if (pProcs == NULL)
        return NV_FALSE;
    count = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);
    _vidmemEvictWhy = VMT_WHY_ALLOC;
    avail = _vidmemEvictHintFree(pGpu, pProcs, count,
                                 NV_MIN(target, before + VIDMEM_EVICT_HEADROOM_STEP_MB), pid,
                                 VE_LEVEL_FG_IDLE, "foreground headroom");
    _vidmemEvictUpdateReclaimable(pProcs, count, osGetMonotonicTimeNs());
    _vidmemLastPressureNs = osGetMonotonicTimeNs();
    portMemFree(pProcs);
    bMore = (avail > before) && (avail < target);
    if (!bMore)
        _vidmemHeadroomPid = 0;
    return bMore;
}

// Should an allocation of `pid` of `size` make room to keep the foreground headroom?
NvBool
vepKeepFgHeadroomWanted(OBJGPU *pGpu, NvU32 pid, NvU64 size)
{
    if ((_vidmemFgHeadroomMB == 0) || !_vidmemAutoActive() || !_vidmemHintsActive() || (pid == 0) ||
        (VE_HINT_STATE(_vidmemHint(pid, NULL)) != VE_HINT_FG))
        return NV_FALSE;
    // The headroom only takes idle ones; none left at the last walk: no walk (#8).
    if (!_vidmemEvictAnyMovable(VE_LEVEL_FG_IDLE))
        return NV_FALSE;
    return vepPlatAvailMB(pGpu) < (size >> 20) + _vidmemFgHeadroomMB;
}

//
// No reserve: an allocation just failed in PMA although the free memory
// looked enough (a 2 MB-page allocation cannot use 64 KB holes), or the
// compositor's failed at all. Move others to sysmem until it fits: for the
// compositor (kwin_wayland, Xwayland, a kernel thread) hidden and idle ones
// first and the foreground last (VE_LEVEL_COMPOSITOR); for the foreground
// what an allocation of it may take (VE_LEVEL_FG_ALLOC: a game on the other
// monitor included, problem #7); for a visible process hidden ones. For a
// contiguous one clear a window (vepPlatMakeContig()). The caller tries
// again. API lock RW, GPU lock held. *pWantMB, *pAvail, *pMovedMB: for the
// log and the trace.
//
static NvBool
_vidmemEvictMakeChunks(OBJGPU *pGpu, NvU64 size, NvBool bContig, NvU64 align, NvU32 exceptPid, NvU32 level,
                       NvU64 marginMB, const char *why, NvU64 *pWantMB, NvU64 *pAvail, NvU64 *pMovedMB)
{
    VIDMEM_EVICT_PROC *pProcs;
    NvU64              avail, wantMB, movedMB = 0;
    NvU32              count;
    NvBool             bOk;

    pProcs = portMemAllocNonPaged(sizeof(*pProcs) * VIDMEM_EVICT_MAX_PROCS);
    if (pProcs == NULL)
        return NV_FALSE;
    count = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);

    //
    // Free 2 MB pages, not free bytes: a 2 MB-page allocation cannot use
    // 64 KB holes. Each round asks for what is still missing (whole
    // allocations move, and some of them may sit in holes).
    //
    _vidmemEvictWhy = VMT_WHY_ALLOC;
    wantMB = 2 * NV_DIV_AND_CEIL(size, 2ULL << 20) + marginMB;
    avail  = vepPlatAvailMB(pGpu);
    {
        NvU64  freeChunks = 0, longest = 0, round;

        //
        // Allocations on 2 MB pages first (each MB moved frees chunks;
        // problem #12: 284 MB of the focused game moved in 531 ms for kwin's
        // 32 MB), per victim, in the usual victim order. At most 4 rounds.
        //
        NvBool bBig = NV_TRUE;

        for (round = 0; round < 4; round++)
        {
            NvU64 before = avail;

            if (!vepPlatChunkInfo(pGpu, &freeChunks, &longest) || (2 * freeChunks >= wantMB))
                break;
            _vidmemEvictBigPagesOnly = bBig;
            _vidmemEvictVictimMarginMB = marginMB;
            //
            // Each round asks for twice what the last one did: whole chunks
            // come from 2 MB-page allocations, and once a victim has none left
            // the bytes it gives sit in 64 KB pieces between others (a scanout
            // stress run, churn of 8 buffers: 213 MB moved in 4 rounds of "what is
            // missing" for a 32 MB buffer, still short of 16 free chunks).
            //
            avail = _vidmemEvictHintFree(pGpu, pProcs, count, avail + ((wantMB - 2 * freeChunks) << round),
                                         exceptPid, level, why);
            _vidmemEvictVictimMarginMB = 64;
            _vidmemEvictBigPagesOnly = NV_FALSE;
            movedMB += (avail > before) ? (avail - before) : 0;
            if (avail <= before)
                break;      // nothing left to move (each victim gave any pages it had)
        }
        bOk = vepPlatChunkInfo(pGpu, &freeChunks, &longest) &&
              (2 * freeChunks > (size >> 20));
    }
    if (bOk && bContig)
    {
        NvU64 contigMB = 0;

        // What moved changed what the victims hold.
        count = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);
        bOk = vepPlatMakeContig(pGpu, pProcs, count, size, align, exceptPid, level, &contigMB);
        movedMB += contigMB;
    }
    _vidmemEvictUpdateReclaimable(pProcs, count, osGetMonotonicTimeNs());
    _vidmemLastPressureNs = osGetMonotonicTimeNs();

    portMemFree(pProcs);
    *pWantMB  = wantMB;
    *pAvail   = avail;
    *pMovedMB = movedMB;
    return bOk;
}

NvBool
vepMakeRoomCompositorRun(OBJGPU *pGpu, NvU64 size, NvBool bContig, NvU64 align,
                         NvU64 *pWantMB, NvU64 *pAvail, NvU64 *pMovedMB)
{
    //
    // Only what it asked for plus a little (problem #12, the user's choice):
    // kwin's buffers come one at a time, a margin of 16 MB does not move
    // a game for buffers nobody asked for.
    //
    return _vidmemEvictMakeChunks(pGpu, size, bContig, align, 0, VE_LEVEL_COMPOSITOR, 16, "compositor",
                                  pWantMB, pAvail, pMovedMB);
}

// Why vepMakeRoomAllowed() says no, for the log of a contiguous request left without a window.
const char *
vepMakeRoomDeniedWhy(NvU32 pid)
{
    NvU64 nowNs = osGetMonotonicTimeNs();

    if (!_vidmemAutoActive() || !_vidmemHintsActive())
        return "the policy is off (no window hints)";
    if (VE_HINT_STATE(_vidmemHint(pid, NULL)) < VE_HINT_VISIBLE)
        return "its window is not visible";
    if ((_vidmemMakeRoomFruitlessNs != 0) && (VE_AGO_NS(nowNs, _vidmemMakeRoomFruitlessNs) < 1000000000ULL))
        return "nothing could be moved a moment ago";
    NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: movable as of the last walk (MB): hidden %llu, for an idle foreground %llu, for a foreground's allocation %llu, the foreground's own %llu\n",
              _vidmemMovableHiddenBytes >> 20, _vidmemMovableFgIdleBytes >> 20, _vidmemMovableFgAllocBytes >> 20,
              _vidmemMovableFgOwnBytes >> 20);
    return "nobody may be moved for it as of the last walk";
}

// The level an ordinary process's failed placement makes room at (~0: none).
NvU32
vepMakeRoomPlaceLevel(NvU32 pid, NvBool bContig)
{
    if (!_veMakeRoomAllowed(pid, bContig))
        return NV_U32_MAX;
    return (VE_HINT_STATE(_vidmemHint(pid, NULL)) == VE_HINT_FG) ? VE_LEVEL_FG_ALLOC : VE_LEVEL_HIDDEN;
}

//
// Context buffers of a new channel (ctxBufPoolReserve(), problem #13) found
// no room: the level to make it at for the process creating the channel
// (~0: none). Any process may ask (a windowless portal too); only hidden
// ones give for it, and for the foreground what its allocations may take.
//
NvU32
vepMakeRoomContextsLevel(NvU32 pid)
{
    NvU64 nowNs = osGetMonotonicTimeNs();
    NvU32 level;

    if (!_vidmemAutoActive() || !_vidmemHintsActive())
        return NV_U32_MAX;
    if ((_vidmemMakeRoomFruitlessNs != 0) && (VE_AGO_NS(nowNs, _vidmemMakeRoomFruitlessNs) < 1000000000ULL))
        return NV_U32_MAX;
    level = ((pid != 0) && (VE_HINT_STATE(_vidmemHint(pid, NULL)) == VE_HINT_FG)) ?
            VE_LEVEL_FG_ALLOC : VE_LEVEL_HIDDEN;
    return _vidmemEvictAnyMovable(level) ? level : NV_U32_MAX;
}

NvBool
vepMakeRoomPlaceRun(OBJGPU *pGpu, NvU32 pid, NvU64 size, NvBool bContig, NvU64 align, NvU32 level,
                    NvU64 *pWantMB, NvU64 *pAvail, NvU64 *pMovedMB)
{
    // A margin of 64 MB: a game streaming textures comes back for more.
    NvBool bOk = _vidmemEvictMakeChunks(pGpu, size, bContig, align, pid, level, VIDMEM_EVICT_RESTORE_MARGIN_MB,
                                        (level == VE_LEVEL_FG_ALLOC) ? "placing an allocation of the foreground" :
                                                                       "placing an allocation",
                                        pWantMB, pAvail, pMovedMB);

    //
    // Worth another try only if something moved: PMA can refuse with free
    // 2 MB chunks to spare (memory still being scrubbed after a free, a
    // range or kind it cannot use), and a WebGL page streaming textures
    // asks again at once; without this every one of its allocations walked
    // the clients for nothing (nvtest focus, 2026-11-06).
    //
    bOk = bOk && (*pMovedMB != 0);
    _vidmemMakeRoomFruitlessNs = bOk ? 0 : osGetMonotonicTimeNs();
    return bOk;
}
