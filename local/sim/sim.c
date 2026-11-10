/*
 * Policy simulator: runs the driver's eviction policy
 * (src/nvidia/src/kernel/mem_mgr/vidmem_evict_policy.c, the same file the
 * driver builds) against a memory model instead of a GPU.
 *
 * The model: a fixed amount of vidmem for processes (`cap`), processes with
 * allocations that are in vidmem or moved out by the driver (evicted). An
 * allocation the driver turns away is counted ("denied") and not kept: the
 * app falls back to sysmem by itself, which a recording does not show either
 * (2026-11-06 CoD: 249 denials in the trace, none in its vram/ram).
 * The platform functions of vidmem_evict_policy.h move allocations between
 * those, and an allocation goes through the same steps as in video_mem.c
 * (background cushion, make room for visible processes, created in sysmem
 * for background ones, foreground headroom, compositor make room).
 *
 * Input: a scenario, written by hand or converted from a gamerec recording
 * by replay.py (README.md has the commands). Output: what moved and why, per process what sat in sysmem while it was focused or
 * visible, ping-pong, denials, stalls; `expect` lines make it a test.
 *
 *   sim [-v] [-cmp PID[,PID]] [-set KNOB=VALUE]... SCENARIO
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <ctype.h>
#include "vidmem_evict_policy.h"

#define MAXP 512

enum { W_VID, W_EVICT };

typedef struct
{
    NvU64 kb;
    NvU8  where;      // W_*
    NvU8  movable;    // the driver could move it (eligible)
    NvU8  contig;
} Alloc;

typedef struct
{
    NvU32  pid;
    char   name[NV_PROC_NAME_MAX_LENGTH];
    int    never, exempt, old;
    NvU32  hint;              // VE_HINT_* | flags
    int    idle;              // seconds since GPU work, -1 never
    NvU32  samples;
    NvU32  chunkMB;           // allocation size when it grows (target)
    Alloc *a;
    int    na, cap;
    // stats
    double sysMBs, sysFgMBs, sysVisMBs, sysFgMaxMB;
    double fgS, visS;
    NvU32  evicts, restores, restoreFails, denied, born, pingpong, hintSwitches;
    NvU64  evictKB, restoreKB, deniedKB, bornKB;
    double lastEvictS, lastRestoreS;
    double stallMs, maxStallMs;   // moves made inside its own allocations
    NvU64  windowKB;              // a contiguous window cleared for it and not used yet
    NvU32  windows;               // windows cleared for it
} Proc;

static Proc   gProcs[MAXP];
static int    gNumProcs;
static NvU64  gCapKB = 11000ULL << 10;
// The clock at scenario time 0: > 0 (0 means "never" to the policy) and past
// any idle time a scenario gives (`act PID 14522`: Xwayland in a replay); at
// 100 s an idle time above 100 s wrapped and the process looked busy now.
#define T0 1000000.0
static double gNowS = T0;         // the clock (s)
static double gNextTickS = T0 + VIDMEM_EVICT_TICK_SEC;
static int    gAutoTick = 1;
static int    gHintsActive;
static int    gVerbose;
static double gCopyGBs = 3.5;     // copy speed of a move (2026-11-06 CoD: 494 MB in 139 ms)
static double gFrag = 1.0;        // share of free memory in free 2 MB chunks
static NvU64  gLongestMB;         // longest free run (0: no limit): a bigger contiguous buffer comes back only into a cleared window
static long   gClockOffset;       // display: wall clock (unix s) at scenario time 0
static int    gHaveClock;
static const char *gScenario;
static int    gLine;
static int    gFailures, gChecks;

// Who the current moves are for (an allocation of gCtxProc, or the policy step).
static Proc  *gCtxProc;
static double gCtxMs;
static const char *gCtxWhat = "step";
static double gStepMs, gStepMaxMs, gStepSumMs, gStepMaxAt;
static NvU32  gSteps, gMoves;
static NvU64  gMovedKB;

// Knobs given with -set win over the scenario's own `set` lines.
static const char *gCmdKnobs[32];
static int    gNumCmdKnobs;

static NvU32  gCmp[16];
static int    gNumCmp;
static double gCmpLastMin[16];

static const char *whyName(NvU32 why)
{
    static const char *n[] = { "manual", "auto", "pressure", "alloc", "restore", "room", "idle-back", "hot",
                               "access", "born" };
    return (why < NV_ARRAY_ELEMENTS(n)) ? n[why] : "?";
}

static const char *clockStr(double s)
{
    static char buf[4][32];
    static int  k;
    char *b = buf[k++ & 3];

    if (gHaveClock)
    {
        time_t t = (time_t)(s - T0 + gClockOffset);
        struct tm tm;

        localtime_r(&t, &tm);
        snprintf(b, 32, "%02d:%02d:%02d.%01d", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)((s - (long)s) * 10));
    }
    else
    {
        snprintf(b, 32, "%8.1f", s - T0);
    }
    return b;
}

void simPrintf(int level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    if (!gVerbose)
        return;
    printf("%s  ", clockStr(gNowS));
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

NvU64 osGetMonotonicTimeNs(void)
{
    return (NvU64)(gNowS * 1e9);
}

static Proc *findProc(NvU32 pid)
{
    int i;

    for (i = 0; i < gNumProcs; i++)
        if (gProcs[i].pid == pid)
            return &gProcs[i];
    return NULL;
}

NvU32 osGetVidmemHint(NvU32 pid, NvU32 *pGeneration)
{
    Proc *p = (pid != 0) ? findProc(pid) : NULL;

    if (pGeneration != NULL)
        *pGeneration = gHintsActive ? 1 : 0;
    return (p != NULL) ? p->hint : 0;
}

NvBool vidmemIsReserveExemptName(const char *name)
{
    return (strcmp(name, "kwin_wayland") == 0) || (strcmp(name, "Xwayland") == 0);
}

// The engine's globals (vidmem_evict.c in the driver).
NvU64 _vidmemEvictBytes;
NvU64 _vidmemEvictBudgetBytes;
NvBool _vidmemEvictBigPagesOnly;     // the simulator has no page sizes: ignored
NvU32 _vidmemEvictWhy;

static NvU64 sumKB(const Proc *p, int where)
{
    NvU64 s = 0;
    int   i;

    for (i = 0; i < p->na; i++)
        if (p->a[i].where == where)
            s += p->a[i].kb;
    return s;
}

static NvU64 usedKB(void)
{
    NvU64 s = 0;
    int   i;

    for (i = 0; i < gNumProcs; i++)
        s += sumKB(&gProcs[i], W_VID);
    return s;
}

static NvU64 freeKB(void)
{
    NvU64 u = usedKB();

    return (u < gCapKB) ? gCapKB - u : 0;
}

static void accumulate(double toS);

// --- the platform (vidmem_evict_policy.h) ---------------------------------

NvU64 vepPlatAvailMB(OBJGPU *pGpu)
{
    (void)pGpu;
    return freeKB() >> 10;
}

static unsigned gWalks;           // client walks (each one costs the driver ms under its locks)
static double   gHeadroomAtS;     // when the driver's headroom work item runs next (0: none queued)

NvU32 vepPlatGatherProcs(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 maxProcs)
{
    gWalks++;
    NvU32 n = 0;
    int   i, k;

    (void)pGpu;
    for (i = 0; (i < gNumProcs) && (n < maxProcs); i++)
    {
        Proc              *p = &gProcs[i];
        VIDMEM_EVICT_PROC *q = &pProcs[n++];

        memset(q, 0, sizeof(*q));
        q->pid = p->pid;
        portStringCopy(q->name, sizeof(q->name), p->name, sizeof(p->name));
        q->clients = 1;
        q->bNever = p->never || _vidmemEvictIsNeverName(p->name);
        for (k = 0; k < p->na; k++)
        {
            if (p->a[k].where == W_VID)
            {
                q->vidmemBytes += p->a[k].kb << 10;
                if (p->a[k].movable)
                    q->eligibleBytes += p->a[k].kb << 10;
            }
            else if (p->a[k].where == W_EVICT)
            {
                q->evictedBytes += p->a[k].kb << 10;
                if (p->a[k].contig)
                {
                    q->evictedContigBytes += p->a[k].kb << 10;
                    q->evictedContigMax    = NV_MAX(q->evictedContigMax, p->a[k].kb << 10);
                    q->evictedContigAlign  = 2ULL << 20;
                }
            }
        }
    }
    return n;
}

static double moveMs(NvU64 kb, NvU32 allocs)
{
    if (kb == 0)
        return 0;
    return 1.0 + (double)kb / 1024.0 / (gCopyGBs * 1024.0) * 1000.0 + allocs * 0.02;
}

NV_STATUS vepPlatMovePid(OBJGPU *pGpu, NvU32 mode, NvU32 pid, NvU64 *pMovedBytes)
{
    Proc  *p = findProc(pid);
    NvU64  kb = 0;
    NvU32  n = 0, failed = 0;
    double ms;
    int    i;

    (void)pGpu;
    if (pMovedBytes != NULL)
        *pMovedBytes = 0;
    if (p == NULL)
        return NV_ERR_INVALID_ARGUMENT;
    for (i = 0; i < p->na; i++)
    {
        Alloc *a = &p->a[i];

        if (mode == 1)
        {
            if ((a->where != W_VID) || !a->movable)
                continue;
            if ((_vidmemEvictBudgetBytes != 0) && ((kb << 10) >= _vidmemEvictBudgetBytes))
                continue;
            a->where = W_EVICT;
            kb += a->kb;
            n++;
        }
        else if (mode == 2)
        {
            if (a->where != W_EVICT)
                continue;
            // Partial restore: no more than the budget (allocations that do
            // not fit in what is left are skipped, smaller ones may follow).
            if ((_vidmemEvictBudgetBytes != 0) && (((kb + a->kb) << 10) > _vidmemEvictBudgetBytes))
                continue;
            if (freeKB() < a->kb)
            {
                failed++;
                continue;
            }
            // Contiguous and longer than any free run: only into a window cleared for it.
            if (a->contig && (gLongestMB != 0) && (a->kb > (gLongestMB << 10)))
            {
                if (p->windowKB < a->kb)
                {
                    failed++;
                    continue;
                }
                p->windowKB = 0;
            }
            a->where = W_VID;
            kb += a->kb;
            n++;
        }
    }
    if (mode == 1)
        _vidmemEvictBytes += kb << 10;
    else
        _vidmemEvictBytes -= NV_MIN(_vidmemEvictBytes, kb << 10);

    ms = moveMs(kb, n);
    gCtxMs += ms;
    // The clock runs on while memory moves, as in the driver (a step takes
    // nowNs once at its start; what it sets after a move is later).
    accumulate(gNowS + ms / 1000.0);
    gMoves += (kb != 0);
    gMovedKB += kb;
    if (mode == 1)
    {
        if (kb != 0)
        {
            if (gNowS - p->lastRestoreS <= 4.0)
                p->pingpong++;
            p->evicts++;
            p->evictKB += kb;
            p->lastEvictS = gNowS;
        }
    }
    else
    {
        if (kb != 0)
        {
            if (gNowS - p->lastEvictS <= 4.0)
                p->pingpong++;
            p->restores++;
            p->restoreKB += kb;
            p->lastRestoreS = gNowS;
        }
        if (failed != 0)
            p->restoreFails++;
    }
    if (gVerbose)
        printf("%s  MOVE %s %s(%u) %llu MB in %u allocs%s why=%s for=%s%s%s, %.0f ms; free %llu MB\n",
               clockStr(gNowS), (mode == 1) ? "evict" : "restore", p->name, p->pid,
               (unsigned long long)(kb >> 10), n,
               failed ? " (some failed)" : "", whyName(_vidmemEvictWhy), gCtxWhat,
               gCtxProc ? " of " : "", gCtxProc ? gCtxProc->name : "", ms,
               (unsigned long long)(freeKB() >> 10));
    if (pMovedBytes != NULL)
        *pMovedBytes = kb << 10;
    return (failed != 0) ? NV_ERR_NO_MEMORY : NV_OK;
}

void vepPlatHotRestore(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count)
{
    (void)pGpu; (void)pProcs; (void)count;
}

NvBool vepPlatChunkInfo(OBJGPU *pGpu, NvU64 *pFreeChunks, NvU64 *pLongest)
{
    (void)pGpu;
    *pFreeChunks = (NvU64)((double)(freeKB() >> 11) * gFrag);
    *pLongest = *pFreeChunks;
    return NV_TRUE;
}

NvBool vepPlatMakeContig(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 size, NvU64 align,
                         NvU32 forPid, NvU32 level, NvU64 *pMovedMB)
{
    Proc *p = (forPid != 0) ? findProc(forPid) : NULL;

    (void)pGpu; (void)pProcs; (void)count; (void)align; (void)level;
    *pMovedMB = 0;
    // Always found, by moving what is in it to other vidmem pages (no cost modelled).
    if ((p != NULL) && (p->windowKB < (size >> 10)))
    {
        p->windowKB = size >> 10;
        p->windows++;
    }
    return NV_TRUE;
}

// Fragmentation is not modelled: nothing to compact.
NvU64 vepPlatCompact(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 budgetMB, NvU64 wantMB)
{
    (void)pGpu; (void)pProcs; (void)count; (void)budgetMB; (void)wantMB;
    return 0;
}

// --- processes and allocations -------------------------------------------

static Proc *getProc(NvU32 pid, const char *name)
{
    Proc *p = findProc(pid);

    if (p != NULL)
        return p;
    if (gNumProcs >= MAXP)
    {
        fprintf(stderr, "%s:%d: too many processes\n", gScenario, gLine);
        exit(2);
    }
    p = &gProcs[gNumProcs++];
    memset(p, 0, sizeof(*p));
    p->pid = pid;
    portStringCopy(p->name, sizeof(p->name), name ? name : "?", NV_PROC_NAME_MAX_LENGTH);
    p->idle = -1;
    p->chunkMB = 32;
    p->lastEvictS = p->lastRestoreS = -1e9;
    p->exempt = vidmemIsReserveExemptName(p->name);
    return p;
}

static void addAlloc(Proc *p, NvU64 kb, int where, int movable, int contig)
{
    if (p->na == p->cap)
    {
        p->cap = p->cap ? 2 * p->cap : 64;
        p->a = realloc(p->a, sizeof(Alloc) * p->cap);
    }
    p->a[p->na].kb = kb;
    p->a[p->na].where = (NvU8)where;
    p->a[p->na].movable = (NvU8)movable;
    p->a[p->na].contig = (NvU8)contig;
    p->na++;
    if (where == W_EVICT)
        _vidmemEvictBytes += kb << 10;
}

static NvU64 measureVidmem(void *ctx)
{
    return sumKB((Proc *)ctx, W_VID) << 10;
}

static void ctxBegin(Proc *p, const char *what)
{
    gCtxProc = p;
    gCtxWhat = what;
    gCtxMs = 0;
}

static void ctxEnd(void)
{
    if (gCtxProc != NULL)
    {
        gCtxProc->stallMs += gCtxMs;
        if (gCtxMs > gCtxProc->maxStallMs)
            gCtxProc->maxStallMs = gCtxMs;
    }
    gCtxProc = NULL;
    gCtxWhat = "step";
    gCtxMs = 0;
}

// One allocation through the steps of vidmemConstruct() (video_mem.c, no reserve).
static void simAlloc(Proc *p, NvU64 kb, int movable, int contig)
{
    NvU64 size = kb << 10;
    NvU64 wantMB, avail, moved;
    int   ok, born = 0;

    ctxBegin(p, "alloc");
    if (p->exempt)
    {
        // The compositor: never turned away; others move out until it fits.
        ok = freeKB() >= kb;
        if (!ok && _vidmemAutoActive())
        {
            ctxBegin(p, "compositor");
            (void)vepMakeRoomCompositorRun(NULL, size, contig, 2ULL << 20, &wantMB, &avail, &moved);
            ok = freeKB() >= kb;
        }
    }
    else
    {
        NvU64 cushion = vepBackgroundCushion(p->pid, p->name);
        NvU64 fb = freeKB() << 10;

        ok = ((fb > cushion) ? fb - cushion : 0) >= size;
        if (!ok && vepMakeRoomAllowed(p->pid) &&
            vepMakeRoomRun(NULL, p->pid, size, VIDMEM_EVICT_HINT_LOW_MB, "allocation", &wantMB, &avail))
        {
            cushion = vepBackgroundCushion(p->pid, p->name);
            fb = freeKB() << 10;
            ok = ((fb > cushion) ? fb - cushion : 0) >= size;
        }
        if (!ok && vepWantBorn(p->pid, p->name, measureVidmem, p) && (size <= (512ULL << 20)) && !contig &&
            ((freeKB() << 10) >= size + (64ULL << 20)))
        {
            ok = born = 1;
        }
        if (!ok && ((freeKB() << 10) >= size))
            ok = 1;     // only the cushion said no
#ifdef VEP_HAS_HEADROOM_RUN
        // The driver refills the foreground's headroom in a work item (pieces
        // a few ms apart), not inside the allocation.
        if (ok && !born && vepKeepFgHeadroomWanted(NULL, p->pid, size) && vepHeadroomRequest(p->pid) &&
            (gHeadroomAtS == 0))
            gHeadroomAtS = gNowS + 0.005;
#else
        if (ok && !born && vepKeepFgHeadroomWanted(NULL, p->pid, size) && vepMakeRoomAllowed(p->pid))
        {
            gCtxWhat = "headroom";
            (void)vepMakeRoomRun(NULL, p->pid, size, _vidmemFgHeadroomMB, "foreground headroom", &wantMB, &avail);
        }
#endif
        if (ok && !born)
            ok = freeKB() >= kb;    // PMA
    }
    if (born)
    {
        addAlloc(p, kb, W_EVICT, movable, contig);
        _vidmemBornAllocs++;
        _vidmemBornBytes += size;
        p->born++;
        p->bornKB += kb;
    }
    else if (ok)
    {
        addAlloc(p, kb, W_VID, movable, contig);
    }
    else
    {
        p->denied++;
        p->deniedKB += kb;
        if (gVerbose)
            printf("%s  DENIED %s(%u) %llu MB (free %llu MB)\n", clockStr(gNowS), p->name, p->pid,
                   (unsigned long long)(kb >> 10), (unsigned long long)(freeKB() >> 10));
    }
    ctxEnd();
}

// Frees the newest allocations first.
static void simFree(Proc *p, NvU64 kb)
{
    while ((kb != 0) && (p->na != 0))
    {
        Alloc *a = &p->a[p->na - 1];
        NvU64  take = NV_MIN(kb, a->kb);

        if (a->where == W_EVICT)
            _vidmemEvictBytes -= NV_MIN(_vidmemEvictBytes, take << 10);
        a->kb -= take;
        kb -= take;
        if (a->kb == 0)
            p->na--;
    }
}

static NvU64 totalKB(const Proc *p)
{
    return sumKB(p, W_VID) + sumKB(p, W_EVICT);
}

// Grow or shrink to `mb` in total, with about `elig` of it movable.
//
// Allocations and frees waiting for their time (target ... spread=S spreads
// a process's growth over the next S seconds, as it happens between two
// policy steps: all at one instant they would meet the driver's one-second
// pause after a fruitless make-room and be denied).
//
typedef struct { double t; int proc; NvU64 kb; NvU8 movable, bFree; } Event;
static Event *gEv;
static int    gNumEv, gCapEv;

static void queueEvent(double t, Proc *p, NvU64 kb, int movable, int bFree)
{
    int i;

    if (gNumEv == gCapEv)
    {
        gCapEv = gCapEv ? 2 * gCapEv : 256;
        gEv = realloc(gEv, sizeof(Event) * gCapEv);
    }
    for (i = gNumEv; (i > 0) && (gEv[i - 1].t > t); i--)
        gEv[i] = gEv[i - 1];
    gEv[i].t = t;
    gEv[i].proc = (int)(p - gProcs);
    gEv[i].kb = kb;
    gEv[i].movable = (NvU8)movable;
    gEv[i].bFree = (NvU8)bFree;
    gNumEv++;
}

static void runEvent(void)
{
    Event e = gEv[0];

    memmove(gEv, gEv + 1, sizeof(Event) * --gNumEv);
    if (e.bFree)
        simFree(&gProcs[e.proc], e.kb);
    else
        simAlloc(&gProcs[e.proc], e.kb, e.movable, 0);
}

// Grow or shrink to `mb` in total, with about `elig` of it movable, now or
// spread over the next `spread` seconds.
static void simTarget(Proc *p, NvU64 mb, double elig, double spread)
{
    NvU64 cur = totalKB(p), want = mb << 10;
    int   i;

    // What is still queued for it counts as done.
    for (i = 0; i < gNumEv; i++)
    {
        if (gEv[i].proc != (int)(p - gProcs))
            continue;
        cur = gEv[i].bFree ? cur - NV_MIN(cur, gEv[i].kb) : cur + gEv[i].kb;
    }
    if (want > cur + 1024)
    {
        NvU64 grow = want - cur, chunk = (NvU64)p->chunkMB << 10;
        NvU64 movKB = 0, n = (grow + chunk - 1) / chunk, k = 0;

        for (i = 0; i < p->na; i++)
            movKB += p->a[i].movable ? p->a[i].kb : 0;
        while (grow != 0)
        {
            NvU64 kb = NV_MIN(grow, chunk);
            int   mov = ((double)(movKB + kb) <= elig * (double)(cur + kb) + (double)chunk / 2);

            if (spread > 0)
                queueEvent(gNowS + spread * (double)(++k) / (double)(n + 1), p, kb, mov, 0);
            else
                simAlloc(p, kb, mov, 0);
            movKB += mov ? kb : 0;
            cur += kb;
            grow -= kb;
        }
    }
    else if (want + 1024 < cur)
    {
        if (spread > 0)
            queueEvent(gNowS + spread / 2, p, cur - want, 0, 1);
        else
            simFree(p, cur - want);
    }
}

// --- time and steps ------------------------------------------------------

static void accumulate(double toS)
{
    double dt = toS - gNowS;
    int    i;

    if (dt <= 0)
        return;
    for (i = 0; i < gNumProcs; i++)
    {
        Proc  *p = &gProcs[i];
        double sys = (double)sumKB(p, W_EVICT) / 1024.0;
        NvU32  st = VE_HINT_STATE(p->hint);

        p->sysMBs += sys * dt;
        if (st == VE_HINT_FG)
        {
            p->sysFgMBs += sys * dt;
            p->fgS += dt;
            if (sys > p->sysFgMaxMB)
                p->sysFgMaxMB = sys;
        }
        else if (st == VE_HINT_VISIBLE)
        {
            p->sysVisMBs += sys * dt;
            p->visS += dt;
        }
    }
    gNowS = toS;
}

static void step(void)
{
    int i;

    // The activity the driver would have sampled.
    _vidmemActivityNewestTs = osGetMonotonicTimeNs();
    for (i = 0; i < gNumProcs; i++)
    {
        Proc                  *p = &gProcs[i];
        VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(p->pid, NV_TRUE);

        if (p->old && (pAct->firstSeenNs == osGetMonotonicTimeNs()))
        {
            // Seen long before the recording started.
            pAct->firstSeenTs = 1;
            pAct->firstSeenNs = 1;
        }
        if (p->idle < 0)
            pAct->lastActiveTs = 0;
        else if ((NvU64)p->idle * 1000000000ULL >= osGetMonotonicTimeNs())
            pAct->lastActiveTs = 1;     // before the clock began: idle as long as it gets
        else
            pAct->lastActiveTs = osGetMonotonicTimeNs() - (NvU64)p->idle * 1000000000ULL;
        pAct->recentSamples = p->samples;
    }
    ctxBegin(NULL, "step");
    vepPolicyStep(NULL);
    gStepMs = gCtxMs;
    gStepSumMs += gStepMs;
    if (gStepMs > gStepMaxMs)
    {
        gStepMaxMs = gStepMs;
        gStepMaxAt = gNowS;
    }
    gSteps++;
    ctxEnd();
}

static void advance(double toS)
{
    for (;;)
    {
        double tick = gAutoTick ? gNextTickS : 1e300;
        double ev = gNumEv ? gEv[0].t : 1e300;
        double hr = (gHeadroomAtS != 0) ? gHeadroomAtS : 1e300;

        if ((tick > toS) && (ev > toS) && (hr > toS))
            break;
        if ((hr <= ev) && (hr <= tick))
        {
            accumulate(hr);
            gHeadroomAtS = 0;
#ifdef VEP_HAS_HEADROOM_RUN
            ctxBegin(NULL, "headroom");
            if (vepHeadroomRun(NULL))
                gHeadroomAtS = gNowS + 0.005;
            ctxEnd();
#endif
            continue;
        }
        if (ev <= tick)
        {
            accumulate(ev);
            runEvent();
        }
        else
        {
            accumulate(tick);
            step();
            gNextTickS += VIDMEM_EVICT_TICK_SEC;
        }
    }
    accumulate(toS);
}

// --- scenario -----------------------------------------------------------

static NvU32 parseHint(const char *s)
{
    switch (s[0])
    {
        case 'f': return VE_HINT_FG;
        case 'v': return VE_HINT_VISIBLE;
        case 'h': return VE_HINT_HIDDEN;
        default:  return VE_HINT_UNKNOWN;
    }
}

static double metric(const char *m, Proc *p)
{
    if (strcmp(m, "free") == 0)
        return (double)(freeKB() >> 10);
    if (p == NULL)
    {
        fprintf(stderr, "%s:%d: metric %s needs a pid\n", gScenario, gLine, m);
        exit(2);
    }
    if (strcmp(m, "vid") == 0)        return (double)(sumKB(p, W_VID) >> 10);
    if (strcmp(m, "evicted") == 0)    return (double)(sumKB(p, W_EVICT) >> 10);
    if (strcmp(m, "sys") == 0)        return (double)(sumKB(p, W_EVICT) >> 10);
    if (strcmp(m, "denied_mb") == 0)  return (double)(p->deniedKB >> 10);
    if (strcmp(m, "moved_out") == 0)  return (double)(p->evictKB >> 10);    // MB copied out by evictions, all told
    if (strcmp(m, "evicts") == 0)     return p->evicts;
    if (strcmp(m, "restores") == 0)   return p->restores;
    if (strcmp(m, "restorefails") == 0) return p->restoreFails;
    if (strcmp(m, "windows") == 0)    return p->windows;
    if (strcmp(m, "pingpong") == 0)   return p->pingpong;
    if (strcmp(m, "denied") == 0)     return p->denied;
    if (strcmp(m, "born") == 0)       return p->born;
    if (strcmp(m, "stallmax") == 0)   return p->maxStallMs;
    if (strcmp(m, "sysfg_mbs") == 0)  return p->sysFgMBs;
    fprintf(stderr, "%s:%d: unknown metric %s\n", gScenario, gLine, m);
    exit(2);
}

static void expect(const char *m, const char *pidStr, const char *op, double v)
{
    Proc  *p = (strcmp(pidStr, "-") == 0) ? NULL : findProc((NvU32)strtoul(pidStr, NULL, 10));
    double x = metric(m, p);
    int    ok;

    if (strcmp(op, "<=") == 0)      ok = x <= v;
    else if (strcmp(op, ">=") == 0) ok = x >= v;
    else if (strcmp(op, "<") == 0)  ok = x < v;
    else if (strcmp(op, ">") == 0)  ok = x > v;
    else if (strcmp(op, "==") == 0) ok = x == v;
    else { fprintf(stderr, "%s:%d: bad operator %s\n", gScenario, gLine, op); exit(2); }
    gChecks++;
    if (!ok)
        gFailures++;
    printf("%s  %s %s %s(%s) = %.0f, want %s %.0f  (line %d)\n", clockStr(gNowS), ok ? "PASS" : "FAIL", m,
           p ? p->name : "", pidStr, x, op, v, gLine);
}

static int setKnob(const char *k, const char *v)
{
    double d = atof(v);

    if (strcmp(k, "fg_headroom") == 0)       _vidmemFgHeadroomMB = (NvU32)d;
    else if (strcmp(k, "bg_cushion") == 0)   _vidmemBgCushionMB = (NvU32)d;
    else if (strcmp(k, "auto") == 0)         _vidmemAutoEnabled = (NvU32)d;
    else if (strcmp(k, "low") == 0)          _vidmemLowMB = (NvU32)d;
    else if (strcmp(k, "high") == 0)         _vidmemHighMB = (NvU32)d;
    else if (strcmp(k, "copy_gbs") == 0)     gCopyGBs = d;
    else if (strcmp(k, "frag") == 0)         gFrag = d;
    else if (strcmp(k, "longest") == 0)      gLongestMB = (NvU64)d;
    else if (strcmp(k, "fg_own") == 0)       _vidmemFgOwnEnabled = (NvU32)d;
    else if (strcmp(k, "autotick") == 0)     gAutoTick = (int)d;
    else if (strcmp(k, "hints") == 0)        gHintsActive = (int)d;
    else return 0;
    return 1;
}

static void cmpRow(Proc *p, NvU64 recVram, NvU64 recRam, int force)
{
    int i;

    for (i = 0; i < gNumCmp; i++)
    {
        if (gCmp[i] != p->pid)
            continue;
        if (!force && (gNowS - gCmpLastMin[i] < 60.0))
            return;
        gCmpLastMin[i] = gNowS;
        printf("%s  CMP %-14.14s(%u) %c  recorded vram %6llu ram %5llu | sim vram %6llu evicted %5llu (denied so far %llu MB) | free %llu\n",
               clockStr(gNowS), p->name, p->pid, "-hvf"[VE_HINT_STATE(p->hint) & 3],
               (unsigned long long)recVram, (unsigned long long)recRam,
               (unsigned long long)(sumKB(p, W_VID) >> 10), (unsigned long long)(sumKB(p, W_EVICT) >> 10),
               (unsigned long long)(p->deniedKB >> 10), (unsigned long long)(freeKB() >> 10));
    }
}

static void runScenario(FILE *f)
{
    char line[1024];

    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *tok[16];
        int   n = 0;
        char *s = line, *c = strchr(line, '#');

        gLine++;
        if (c != NULL)
            *c = '\0';
        while ((n < 16) && ((s = strtok(n ? NULL : s, " \t\r\n")) != NULL))
            tok[n++] = s;
        if (n == 0)
            continue;

        if (strcmp(tok[0], "cap") == 0 && n >= 2)
        {
            gCapKB = (NvU64)atof(tok[1]) << 10;
        }
        else if (strcmp(tok[0], "set") == 0 && n >= 3)
        {
            int i, given = 0;

            for (i = 0; i < gNumCmdKnobs; i++)
                given |= (strcmp(gCmdKnobs[i], tok[1]) == 0);
            if (!given && !setKnob(tok[1], tok[2]))
            {
                fprintf(stderr, "%s:%d: unknown knob %s\n", gScenario, gLine, tok[1]);
                exit(2);
            }
        }
        else if (strcmp(tok[0], "clock") == 0 && n >= 2)
        {
            gClockOffset = atol(tok[1]);
            gHaveClock = 1;
        }
        else if (strcmp(tok[0], "proc") == 0 && n >= 3)
        {
            Proc *p = getProc((NvU32)strtoul(tok[1], NULL, 10), tok[2]);
            int   i;

            for (i = 3; i < n; i++)
            {
                if (strcmp(tok[i], "never") == 0)            p->never = 1;
                else if (strcmp(tok[i], "exempt") == 0)      p->exempt = 1;
                else if (strcmp(tok[i], "old") == 0)         p->old = 1;
                else if (strncmp(tok[i], "chunk=", 6) == 0)  p->chunkMB = (NvU32)atoi(tok[i] + 6);
            }
        }
        else if (strcmp(tok[0], "at") == 0 && n >= 2)
        {
            advance(T0 + atof(tok[1]));
        }
        else if (strcmp(tok[0], "step") == 0)
        {
            step();
        }
        else if (strcmp(tok[0], "hint") == 0 && n >= 3)
        {
            Proc *p = getProc((NvU32)strtoul(tok[1], NULL, 10), NULL);
            NvU32 h = parseHint(tok[2]);
            int   i;

            for (i = 3; i < n; i++)
            {
                if (strcmp(tok[i], "inh") == 0)        h |= VE_HINT_INHERITED;
                else if (strcmp(tok[i], "other") == 0) h |= VE_HINT_OTHER_APP;
#ifdef VE_HINT_FULLSCREEN
                else if (strcmp(tok[i], "full") == 0)  h |= VE_HINT_FULLSCREEN;
                else if (strcmp(tok[i], "launched") == 0) h |= VE_HINT_LAUNCHED_GAME;
#endif
            }
            if (VE_HINT_STATE(h) != VE_HINT_STATE(p->hint))
                p->hintSwitches++;
            p->hint = h;
            gHintsActive = 1;
        }
        else if (strcmp(tok[0], "act") == 0 && n >= 4)
        {
            Proc *p = getProc((NvU32)strtoul(tok[1], NULL, 10), NULL);

            p->idle = (tok[2][0] == '-') ? -1 : atoi(tok[2]);
            p->samples = (NvU32)atoi(tok[3]);
        }
        else if (strcmp(tok[0], "alloc") == 0 && n >= 3)
        {
            Proc *p = getProc((NvU32)strtoul(tok[1], NULL, 10), NULL);
            NvU64 kb = (NvU64)(atof(tok[2]) * 1024);
            int   count = 1, fixed = 0, contig = 0, i;

            for (i = 3; i < n; i++)
            {
                if (strncmp(tok[i], "n=", 2) == 0)       count = atoi(tok[i] + 2);
                else if (strcmp(tok[i], "fixed") == 0)   fixed = 1;
                else if (strcmp(tok[i], "contig") == 0)  contig = 1;
            }
            for (i = 0; i < count; i++)
                simAlloc(p, kb / (NvU64)count, !fixed, contig);
        }
        else if (strcmp(tok[0], "place") == 0 && n >= 5)
        {
            // Initial state as recorded, without the allocation path: VRAM MB
            // in vidmem (ELIG of it movable), RAM MB moved out by the driver.
            Proc  *p = getProc((NvU32)strtoul(tok[1], NULL, 10), NULL);
            NvU64  vram = (NvU64)atof(tok[2]) << 10, ram = (NvU64)atof(tok[3]) << 10;
            NvU64  chunk = (NvU64)p->chunkMB << 10, movKB = (NvU64)(atof(tok[4]) * (double)vram), k;

            while (vram != 0)
            {
                k = NV_MIN(vram, chunk);
                addAlloc(p, k, W_VID, movKB >= k, 0);
                movKB -= NV_MIN(movKB, k);
                vram -= k;
            }
            while (ram != 0)
            {
                k = NV_MIN(ram, chunk);
                addAlloc(p, k, W_EVICT, 1, 0);
                ram -= k;
            }
        }
        else if (strcmp(tok[0], "free") == 0 && n >= 3)
        {
            simFree(getProc((NvU32)strtoul(tok[1], NULL, 10), NULL), (NvU64)(atof(tok[2]) * 1024));
        }
        else if (strcmp(tok[0], "target") == 0 && n >= 4)
        {
            double spread = ((n >= 5) && (strncmp(tok[4], "spread=", 7) == 0)) ? atof(tok[4] + 7) : 0;

            simTarget(getProc((NvU32)strtoul(tok[1], NULL, 10), NULL), (NvU64)atof(tok[2]), atof(tok[3]), spread);
        }
        else if (strcmp(tok[0], "comp") == 0 && n >= 2)
        {
            // An allocation of the compositor side that no tracked process
            // holds (a kernel thread, udev-worker probing a display): it
            // makes room as the driver would, and is not kept.
            NvU64 kb = (NvU64)(atof(tok[1]) * 1024), wantMB, avail, moved;

            if (freeKB() < kb + (NvU64)VIDMEM_EVICT_RESTORE_MARGIN_MB * 1024 && _vidmemAutoActive())
            {
                ctxBegin(NULL, "compositor");
                (void)vepMakeRoomCompositorRun(NULL, kb << 10, (n >= 3) && (strcmp(tok[2], "contig") == 0),
                                               2ULL << 20, &wantMB, &avail, &moved);
                ctxEnd();
            }
        }
        else if (strcmp(tok[0], "rec") == 0 && n >= 4)
        {
            Proc *p = findProc((NvU32)strtoul(tok[1], NULL, 10));

            if (p != NULL)
                cmpRow(p, strtoull(tok[2], NULL, 10), strtoull(tok[3], NULL, 10), 0);
        }
        else if (strcmp(tok[0], "mark") == 0)
        {
            int i;

            printf("%s  MARK", clockStr(gNowS));
            for (i = 1; i < n; i++)
                printf(" %s", tok[i]);
            printf("\n");
        }
        else if (strcmp(tok[0], "expect") == 0 && n >= 5)
        {
            expect(tok[1], tok[2], tok[3], atof(tok[4]));
        }
        else
        {
            fprintf(stderr, "%s:%d: cannot parse: %s\n", gScenario, gLine, tok[0]);
            exit(2);
        }
    }
}

static void summary(void)
{
    int i;

    printf("\n%u client walks\n", gWalks);
    printf("%u policy steps, %u moves, %llu MB moved; moves inside steps: %.0f ms in all, longest step %.0f ms at %s\n",
           gSteps, gMoves, (unsigned long long)(gMovedKB >> 10), gStepSumMs, gStepMaxMs, clockStr(gStepMaxAt));
    printf("%-8s %-16s %6s %6s %6s | %8s %8s %6s | %5s %5s %5s %5s %5s %5s | %7s %7s\n",
           "pid", "name", "vid", "evict", "denied", "fg_sys", "vis_sys", "fg_max",
           "evict", "rest", "rfail", "pingp", "deny", "born", "stall", "stallmx");
    printf("%-8s %-16s %6s %6s %6s | %8s %8s %6s | %5s %5s %5s %5s %5s %5s | %7s %7s\n",
           "", "", "MB", "MB", "MB", "MB*s", "MB*s", "MB", "", "", "", "", "", "", "ms", "ms");
    for (i = 0; i < gNumProcs; i++)
    {
        Proc *p = &gProcs[i];

        if ((p->na == 0) && (p->evicts == 0) && (p->denied == 0))
            continue;
        printf("%-8u %-16.16s %6llu %6llu %6llu | %8.0f %8.0f %6.0f | %5u %5u %5u %5u %5u %5u | %7.0f %7.0f\n",
               p->pid, p->name, (unsigned long long)(sumKB(p, W_VID) >> 10),
               (unsigned long long)(sumKB(p, W_EVICT) >> 10), (unsigned long long)(p->deniedKB >> 10),
               p->sysFgMBs, p->sysVisMBs, p->sysFgMaxMB,
               p->evicts, p->restores, p->restoreFails, p->pingpong, p->denied, p->born,
               p->stallMs, p->maxStallMs);
    }
    printf("vid / evict: at the end; denied: MB of allocations turned away (the app's own sysmem fallback);\n"
           "fg_sys / vis_sys: MB moved out integrated over the time the process was focused / visible;\n"
           "stall: moves made inside the process's own allocations (it waited for them).\n");
    if (gChecks != 0)
        printf("\nexpect: %d of %d passed\n", gChecks - gFailures, gChecks);
}

int main(int argc, char **argv)
{
    FILE *f;
    int   i;

    _vidmemAutoEnabled = VIDMEM_AUTO_HINTS;
    for (i = 1; i < argc - 1; i++)
    {
        if (strcmp(argv[i], "-v") == 0)
        {
            gVerbose = 1;
        }
        else if ((strcmp(argv[i], "-cmp") == 0) && (i + 1 < argc - 1))
        {
            char *s = argv[++i], *t;

            for (t = strtok(s, ","); (t != NULL) && (gNumCmp < 16); t = strtok(NULL, ","))
            {
                gCmpLastMin[gNumCmp] = -1e9;
                gCmp[gNumCmp++] = (NvU32)strtoul(t, NULL, 10);
            }
        }
        else if ((strcmp(argv[i], "-set") == 0) && (i + 1 < argc - 1))
        {
            char *kv = argv[++i], *eq = strchr(kv, '=');

            if ((eq == NULL) || (*eq = '\0', !setKnob(kv, eq + 1)) || (gNumCmdKnobs >= 32))
            {
                fprintf(stderr, "bad -set %s\n", kv);
                return 2;
            }
            gCmdKnobs[gNumCmdKnobs++] = kv;
        }
        else
        {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (argc < 2)
    {
        fprintf(stderr, "usage: sim [-v] [-cmp PID[,PID]] [-set KNOB=VALUE]... SCENARIO\n");
        return 2;
    }
    gScenario = argv[argc - 1];
    f = (strcmp(gScenario, "-") == 0) ? stdin : fopen(gScenario, "r");
    if (f == NULL)
    {
        perror(gScenario);
        return 2;
    }
    runScenario(f);
    summary();
    return gFailures ? 1 : 0;
}
