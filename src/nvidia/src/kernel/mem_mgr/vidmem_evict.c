/*
 * Local patch (not upstream): move the vidmem of an idle process to system
 * memory while the process keeps running, and back.
 *
 * Only NV01_MEMORY_LOCAL_USER allocations that are private to one RM client
 * are moved: PMA-owned (contiguous or not), not duplicated, not CPU-
 * mapped, generic PTE kinds only (pitch/generic, compressed or not; no Z/S
 * kinds), and only GPU-mapped through VirtualMemory objects. See
 * _vidmemEvictCheck() for the full list.
 *
 * Evict, per allocation, with every channel of the owning process disabled
 * and preempted (FIFO_DISABLE_CHANNELS RPC to GSP):
 *   1. allocate sysmem in chunks of the vidmem page size (64K/2M), so every
 *      existing mapping keeps its GPU page size;
 *   2. copy with the memory manager's CeUtils. On GA102 it runs in virtual
 *      mode (one-to-one comptags) and reads vidmem through an FB alias mapped
 *      with a compressible kind, so compressed data arrives decompressed; RM
 *      relies on this for suspend with PreserveVideoMemoryAllocations;
 *   3. for each GPU mapping: dmaFreeMap() + dmaAllocMap() at the same VA with
 *      a sub-memdesc of the sysmem memdesc and the uncompressed kind (exactly
 *      what an unmap + map by the application would do);
 *   4. free the PMA pages. The vidmem memdesc keeps its (stale) page array
 *      and its PMA_ALLOC_INFO; pMemDesc->pLocalEvictState marks it.
 * Restore is the reverse with freshly allocated PMA pages; the copy back goes
 * through the compressible FB alias again and the mappings get their original
 * kinds back.
 *
 * While evicted, the stale physical pages must never be used:
 *   - memIsReady() (dup, controls, GPU and CPU mapping, memGetByHandle*,
 *     kernel mappings) restores first: vidmemEvictRestoreOnAccess();
 *   - memmgrFree() frees the sysmem copy instead of the PMA pages:
 *     vidmemEvictFreeEvicted().
 */

#include "core/core.h"
#include "core/locks.h"
#include "os/os.h"
#include "gpu/gpu.h"
#include "gpu/mem_mgr/mem_mgr.h"
#include "gpu/mem_mgr/mem_desc.h"
#include "gpu/mem_mgr/heap.h"
#include "gpu/mem_mgr/ce_utils.h"
#include "gpu/mem_mgr/virt_mem_allocator.h"
#include "gpu/mem_mgr/phys_mem_allocator/phys_mem_allocator.h"
#include "gpu/mem_sys/kern_mem_sys.h"
#include "gpu/bus/kern_bus.h"
#include "kernel/gpu/fifo/kernel_channel.h"
#include "mem_mgr/mem.h"
#include "mem_mgr/vaspace.h"
#include "mem_mgr/virtual_mem.h"
#include "mem_mgr/vidmem_trace.h"
#include "vidmem_evict_policy.h"
#include "rmapi/client.h"
#include "rmapi/mapping_list.h"
#include "rmapi/rs_utils.h"
#include "vgpu/rpc.h"
#include "class/cl0040.h" // NV01_MEMORY_LOCAL_USER
#include "ctrl/ctrl2080/ctrl2080fifo.h"
#include "ctrl/ctrla06c.h"
#include "kernel/gpu/fifo/kernel_channel_group_api.h"
#include "kernel/gpu/fifo/kernel_channel_group.h"
#include "kernel/gpu/fifo/kernel_fifo.h"
#include "gpu/device/device.h"
#include "rmapi/rmapi_utils.h"
#include "class/clc365.h"
#include "ctrl/ctrlc365.h"
#include "alloc/alloc_access_counter_buffer.h"

// Ampere PTE kinds (same table on Turing..Hopper, dev_mmu.h)
#define VIDMEM_EVICT_KIND_PITCH          0x00
#define VIDMEM_EVICT_KIND_GENERIC_MEMORY 0x06

#define VIDMEM_EVICT_MAX_MAPPINGS 64
#define VIDMEM_EVICT_MAX_CHANNELS 1024

typedef struct
{
    CLI_DMA_MAPPING_INFO *pDmaMappingInfo;
    NvU32                 origKind;
} VIDMEM_EVICT_MAPPING;

// Where an evicted allocation is mapped (values: the mapping may go away).
typedef struct
{
    void  *pVas;     // OBJVASPACE *, compared only
    NvU64  vaStart;
    NvU64  vaEnd;    // inclusive
} VIDMEM_EVICT_VARANGE;

typedef struct VIDMEM_EVICT_STATE
{
    MEMORY_DESCRIPTOR    *pSysMemDesc;
    NvU64                 bytes;
    NvU64                 restoreAlign;   // contiguous: alignment of the new block
    NvU32                 staticBar1Refs; // static BAR1 references dropped at evict
    NvU32                 numMappings;
    VIDMEM_EVICT_MAPPING  mappings[VIDMEM_EVICT_MAX_MAPPINGS];
    // Registry of evicted allocations (hot memory, see _vidmemHotPoll()).
    struct VIDMEM_EVICT_STATE *pNext, *pPrev;
    MEMORY_DESCRIPTOR    *pMemDesc;       // the (now sysmem-backed) vidmem allocation
    NvU32                 ownerPid;
    NvU32                 numRanges;
    VIDMEM_EVICT_VARANGE  ranges[VIDMEM_EVICT_MAX_MAPPINGS];
    NvU64                 hotNs;          // last access-counter notification for it
    NvU32                 hotCount;
} VIDMEM_EVICT_STATE;

static VIDMEM_EVICT_STATE *_vidmemEvictList;   // committed evictions, GPU lock held

static void
_vidmemEvictListAdd(VIDMEM_EVICT_STATE *pState)
{
    pState->pPrev = NULL;
    pState->pNext = _vidmemEvictList;
    if (_vidmemEvictList != NULL)
        _vidmemEvictList->pPrev = pState;
    _vidmemEvictList = pState;
}

static void
_vidmemEvictListRemove(VIDMEM_EVICT_STATE *pState)
{
    if ((pState->pPrev == NULL) && (_vidmemEvictList != pState))
        return;     // not registered (aborted before commit)
    if (pState->pPrev != NULL)
        pState->pPrev->pNext = pState->pNext;
    else
        _vidmemEvictList = pState->pNext;
    if (pState->pNext != NULL)
        pState->pNext->pPrev = pState->pPrev;
    pState->pNext = pState->pPrev = NULL;
}

typedef struct
{
    NvU32    count;
    NvHandle hClient[VIDMEM_EVICT_MAX_CHANNELS];
    NvHandle hChannel[VIDMEM_EVICT_MAX_CHANNELS];
    NvHandle hTsg[VIDMEM_EVICT_MAX_CHANNELS];     // channel group of each channel (0: none)
    NvHandle hDevice[VIDMEM_EVICT_MAX_CHANNELS];  // device the channel belongs to (IDLE_CHANNELS)
    NvU8     bImplicitTsg[VIDMEM_EVICT_MAX_CHANNELS]; // TSG allocated by RM around a bare channel
    // Trace (VMT_PAUSE): whose channels, when they went off, how the stop went.
    NvU32    vmtPid;
    NvU64    vmtOffNs;
    NvU64    vmtStopArgs[5];   // mode, tsgs, preempt ok, preempt refused, stop ns
} VIDMEM_EVICT_CHANNELS;

typedef struct
{
    VirtualMemory        *pVirtualMemory;
    OBJVASPACE           *pVas;
    CLI_DMA_MAPPING_INFO *pDmaMappingInfo;
    Memory               *pHolder;      // the Memory object this mapping maps
    NvBool                bCompositor;  // the holder belongs to the compositor
} VIDMEM_EVICT_MAPREF;

// The only other process an evicted buffer may be shared with: it reads
// window buffers and never writes them.
#define VIDMEM_EVICT_COMPOSITOR "kwin_wayland"

NvBool vidmemUserReserveBoundRangeAny(PMA *pPma, NvU64 pageSize, NvU64 *pBegin, NvU64 *pEnd); // video_mem.c

// Totals for the log and the procfs summary.
NvU64 _vidmemEvictBytes;               // also read by the policy
static NvU32 _vidmemEvictAllocs;

// "trace 1": log every remap (VA range, target) to match against Xid addresses.
static NvU32 _vidmemEvictTrace;

//
// "windows 1": also evict buffers shared with the compositor (window
// buffers). Off by default: a visible window's buffer is read by kwin every
// frame, from sysmem over PCIe (2026-11-02 02:50: Chrome fullscreen and
// ghostty on the 4K@120 second monitor evicted, the game dropped below 1 fps).
//
static NvU32 _vidmemEvictWindows;

//
// "quiesce N": how a process's channels are stopped before its memory moves
// (experiment knob, see _vidmemEvictSetChannels()):
//   0 scheduling off, preempt each TSG (GSP refuses implicit ones), 20 ms wait
//   1 FIFO_DISABLE_CHANNELS with preemption (2026-11-02 02:21: Xid 109 in a game)
//   2 scheduling off, then IDLE_CHANNELS (engines only) for the listed channels
//   3 scheduling off, then IDLE_CHANNELS (engines + push buffer)
//   4 IDLE_CHANNELS (engines + push buffer) while still scheduled, then off
//
#define VIDMEM_EVICT_QUIESCE_MAX 4
//
// Default 1 since 2026-11-02 12:28 (nvtest.sh quiesce, an app with ~50 ms
// draws that write, next to a game-like 60 fps app with heavy draws): modes
// 0, 2, 3 and 4 moved memory under a draw still running (Xid 31 MMU fault
// in the moved app); only 1 waits until the listed channels are really off
// the GPU, in 1-3 ms, with no Xid in the other app. (The Xid 109 in CoD at
// 02:21 with mode 1 stays unexplained; the bench did not reproduce it.)
//
static NvU32 _vidmemEvictQuiesce = 1;

// What the last stop did, for the per-evict report.
typedef struct
{
    NvU32     mode;
    NvU32     channels, tsgs, implicitTsgs;
    NvU32     preemptOk, preemptRefused;
    NV_STATUS firstRefusal;
    NV_STATUS idleStatus;
    NvU64     disableNs, idleNs, totalNs;
} VIDMEM_EVICT_QUIESCE_STATS;
static VIDMEM_EVICT_QUIESCE_STATS _vidmemEvictLastStop;

// Time per phase during one evict/restore call (GPU lock held, so plain globals).
enum
{
    VE_PH_QUIESCE, VE_PH_SYSALLOC, VE_PH_COPY, VE_PH_REMAP, VE_PH_PMA, VE_PH_SYSFREE, VE_PH_COUNT
};
static NvU64 _vidmemEvictPhaseNs[VE_PH_COUNT];
#define VE_PHASE_BEGIN() NvU64 _vePhaseT0 = VMT_NOW()
#define VE_PHASE_END(ph)                                                      \
    do {                                                                    \
        NvU64 _vePhaseT1 = VMT_NOW();                                       \
        _vidmemEvictPhaseNs[ph] += _vePhaseT1 - _vePhaseT0;                 \
        _vePhaseT0 = _vePhaseT1;                                            \
    } while (0)
#define VE_PHASE_SKIP() (_vePhaseT0 = VMT_NOW())

//
// Partial eviction: when non-zero, an evict call stops once it has moved this
// many bytes (set around one call by _vidmemEvictHintFree()). Making room
// for 300 MB must not move a hidden game's 10 GB: bringing it back would
// cost ~0.5 s instead of ~20 ms (nvtest.sh switch, 2026-11-02 12:05).
//
NvU64 _vidmemEvictBudgetBytes;
// Making whole 2 MB chunks free: only allocations on 2 MB pages move (policy sets it).
NvBool _vidmemEvictBigPagesOnly;

// Process the current evict/restore call works on (recorded in the state).
static NvU32 _vidmemEvictCurPid;

// Who asked for the next evict/restore call (VMT_WHY_*), set by each caller.
NvU32 _vidmemEvictWhy;
static NvU64 _vidmemRelocateMaxBytes;  // mode 3: bigger allocations stay where they are (0: no limit)
static NvU64 _vidmemRelocateKeepLo, _vidmemRelocateKeepHi;  // mode 3: no new pages in [lo, hi) either (hi 0: none)

// Lock waits/holds shorter than this are not traced ("trace-lock US").
NvU64 vidmemTraceLockNs = 1000000ULL;

// When set, an evict/restore call handles only this allocation (hot memory).
static MEMORY_DESCRIPTOR *_vidmemEvictOnlyMemDesc;
// Only allocations with a page in [lo, hi) (a contiguous window for the
// compositor, _vidmemEvictMakeContig()); hi 0: no filter.
static NvU64 _vidmemEvictRangeLo, _vidmemEvictRangeHi;

// Hot memory (access counters), defined further down.
static NvBool _vidmemHotIsRecent(MEMORY_DESCRIPTOR *pMemDesc);
static void   _vidmemHotResetCounters(void);
void          vidmemEvictHotTeardown(OBJGPU *pGpu);

// Thread currently inside the engine (memIsReady() must not recurse).
static volatile OS_THREAD_HANDLE _vidmemEvictBusyThread;

#define EVICT_PRINTF(...)                                                   \
    do {                                                                    \
        if ((buf != NULL) && (*pLen + 1 < bufSize))                         \
        {                                                                   \
            int written = nvDbgSnprintf(buf + *pLen, bufSize - *pLen, __VA_ARGS__); \
            if (written > 0)                                                \
                *pLen = NV_MIN(*pLen + (NvU32)written, bufSize - 1);        \
        }                                                                   \
    } while (0)

static NvBool
_vidmemEvictIsBusyThread(void)
{
    OS_THREAD_HANDLE self = 0;

    if (osGetCurrentThread(&self) != NV_OK)
        return NV_FALSE;
    return (_vidmemEvictBusyThread != 0) && (_vidmemEvictBusyThread == self);
}

static void
_vidmemEvictSetBusy(NvBool bBusy)
{
    OS_THREAD_HANDLE self = 0;

    if (bBusy)
        (void)osGetCurrentThread(&self);
    _vidmemEvictBusyThread = self;
}

static NvBool
_vidmemEvictKindAllowed(OBJGPU *pGpu, MemoryManager *pMemoryManager, NvU32 kind)
{
    if (memmgrIsKind_HAL(pMemoryManager, FB_IS_KIND_Z, kind))
        return NV_FALSE;
    if (kind == VIDMEM_EVICT_KIND_PITCH)
        return NV_TRUE;
    return memmgrGetUncompressedKind_HAL(pGpu, pMemoryManager, kind, NV_FALSE) ==
           VIDMEM_EVICT_KIND_GENERIC_MEMORY;
}

//
// CeUtils in virtual mode reads/writes vidmem through an FB alias with a
// compressible kind, which is what makes a compressed copy come out right.
// (CeUtils.bUseVasForCeCopy is never set; the channel holds the real flag.)
//
static NvBool
_vidmemEvictCeIsVirtual(MemoryManager *pMemoryManager)
{
    return (pMemoryManager->pCeUtils != NULL) &&
           (pMemoryManager->pCeUtils->pChannel != NULL) &&
           pMemoryManager->pCeUtils->pChannel->bUseVasForCeCopy;
}

static NvU32
_vidmemEvictUncompressedKind(OBJGPU *pGpu, MemoryManager *pMemoryManager, NvU32 kind)
{
    if (!memmgrIsKind_HAL(pMemoryManager, FB_IS_KIND_COMPRESSIBLE, kind))
        return kind;
    return memmgrGetUncompressedKind_HAL(pGpu, pMemoryManager, kind, NV_FALSE);
}

static NvBool
_vidmemEvictIsCompositor(RmClient *pRmClient)
{
    return portStringCompare(pRmClient->name, VIDMEM_EVICT_COMPOSITOR, NV_PROC_NAME_MAX_LENGTH) == 0;
}

//
// Appends the GPU mappings of one holder of pMemDesc to pRefs. With bCheck,
// also verifies that every mapping can be remapped (else returns why not).
//
static const char *
_vidmemEvictAddHolderMaps
(
    OBJGPU              *pGpu,
    MEMORY_DESCRIPTOR   *pMemDesc,
    RsResourceRef       *pRef,
    NvBool               bCompositor,
    NvBool               bCheck,
    VIDMEM_EVICT_MAPREF *pRefs,
    NvU32               *pNumRefs
)
{
    MemoryManager  *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    PMA_ALLOC_INFO *pPmaInfo = pMemDesc->pPmaAllocInfo;
    RsInterMapping *pMapping;

    for (pMapping = listHead(&pRef->interBackRefsMappable);
         pMapping != NULL;
         pMapping = listNext(&pRef->interBackRefsMappable, pMapping))
    {
        VirtualMemory        *pVirtualMemory;
        CLI_DMA_MAPPING_INFO *pInfo;
        OBJVASPACE           *pVas = NULL;

        if (*pNumRefs >= VIDMEM_EVICT_MAX_MAPPINGS)
            return "too-many-maps";

        pVirtualMemory = dynamicCast(pMapping->pMapperRef->pResource, VirtualMemory);
        if (pVirtualMemory == NULL)
            return "map-not-virtmem";
        //
        // NV50_MEMORY_VIRTUAL reserves its VA at alloc (unmap only invalidates
        // PTEs); NV01_MEMORY_VIRTUAL allocates VA per mapping, so the unmap
        // frees it and the DMA_OFFSET_FIXED remap takes the same VA back
        // (nobody else can take it: API and GPU locks are held).
        //
        if (staticCast(pVirtualMemory, Memory)->bRpcAlloc &&
            !gpuIsSplitVasManagementServerClientRmEnabled(pGpu))
            return "map-rpc";
        if ((pMapping->pContextRef == NULL) ||
            (vaspaceGetByHandleOrDeviceDefault(pRef->pClient, pMapping->pContextRef->hResource,
                                               pVirtualMemory->hVASpace, &pVas) != NV_OK) ||
            (pVas == NULL))
            return "map-vas";

        pInfo = intermapGetDmaMapping(pVirtualMemory, pMapping->dmaOffset);
        if (pInfo == NULL)
            return "map-info";

        if (bCheck)
        {
            if (vaspaceGetFlags(pVas) & (VASPACE_FLAGS_BAR | VASPACE_FLAGS_FLA))
                return "map-bar-fla";
            if ((pInfo->pMemDesc == NULL) || (pInfo->pMemDesc->_pParentDescriptor != pMemDesc) ||
                (pInfo->pMemDesc->childDescriptorCnt != 0) || (pInfo->pMemDesc->RefCount != 1))
                return "map-memdesc";
            if ((pInfo->KernelVAddr != NULL) || (pInfo->FbApertureLen != 0) ||
                pInfo->bP2P || (pInfo->aperture != GMMU_APERTURE_VIDEO) ||
                (pInfo->pBar1P2PVirtMemDesc != NULL) || (pInfo->pBar1P2PPhysMemDesc != NULL) ||
                (DRF_VAL(OS46, _FLAGS, _P2P_ENABLE, pInfo->Flags) != NVOS46_FLAGS_P2P_ENABLE_NONE) ||
                FLD_TEST_DRF(OS46, _FLAGS, _KERNEL_MAPPING, _ENABLE, pInfo->Flags))
                return "map-kind-of-mapping";
            if ((pInfo->mapPageSize == 0) || (pInfo->mapPageSize > RM_PAGE_SIZE_HUGE) ||
                (!pPmaInfo->bContig && (pInfo->mapPageSize > pPmaInfo->pageSize)))
                return "map-page-size";
            if (!_vidmemEvictKindAllowed(pGpu, pMemoryManager, memdescGetPteKind(pInfo->pMemDesc)))
                return "map-kind";
        }

        pRefs[*pNumRefs].pVirtualMemory  = pVirtualMemory;
        pRefs[*pNumRefs].pVas            = pVas;
        pRefs[*pNumRefs].pDmaMappingInfo = pInfo;
        pRefs[*pNumRefs].pHolder         = dynamicCast(pRef->pResource, Memory);
        pRefs[*pNumRefs].bCompositor     = bCompositor;
        (*pNumRefs)++;
    }
    return NULL;
}

//
// Collects the GPU mappings of every Memory object that points at pMemDesc
// (the allocation itself and its dups in any client). pPrimary is a holder
// the caller already has. With bCheck (eviction of process `pid`), the other
// holders may only be: more objects of `pid` (pPrimary must be the first one
// found, so a buffer is handled once per process), the compositor, or kernel
// clients without any mapping (e.g. the nvidia-drm GEM object).
//
//
// Holders of shared allocations (DupCount > 1: window buffers kwin also
// holds, browser buffers held by two processes), indexed once per client
// walk (BACKLOG #1). Without it every shared allocation looked for its other
// holders by walking every client and every resource again: a policy step
// took ~20 ms under the RM locks on the desktop (Chrome WebGPU and kwin lost
// 2-4 frames every 2 s). Built by vepPlatGatherProcs() and
// _vidmemEvictPidLocked(); entries keep the walk order, so the first holder
// of a process is the same as found by a walk. Off (NULL) elsewhere.
//
typedef struct
{
    MEMORY_DESCRIPTOR *pMemDesc;
    RsResourceRef     *pRef;
    NvU32              seq;
} VE_HOLDER;

#define VE_HOLDER_MAX 32768

static VE_HOLDER *_veHolders;
static NvU32      _veNumHolders;
static NvU32      _veHolderUsers;

static NvBool
_veHolderLess(const VE_HOLDER *a, const VE_HOLDER *b)
{
    if (a->pMemDesc != b->pMemDesc)
        return (NvUPtr)a->pMemDesc < (NvUPtr)b->pMemDesc;
    return a->seq < b->seq;
}

static void
_veHolderSift(VE_HOLDER *v, NvU32 root, NvU32 n)
{
    for (;;)
    {
        NvU32 c = 2 * root + 1;
        VE_HOLDER t;

        if (c >= n)
            return;
        if ((c + 1 < n) && _veHolderLess(&v[c], &v[c + 1]))
            c++;
        if (!_veHolderLess(&v[root], &v[c]))
            return;
        t = v[root]; v[root] = v[c]; v[c] = t;
        root = c;
    }
}

// Heap sort by (memdesc, walk order).
static void
_veHolderSort(VE_HOLDER *v, NvU32 n)
{
    NvU32 i;

    if (n < 2)
        return;
    for (i = n / 2; i-- > 0; )
        _veHolderSift(v, i, n);
    for (i = n - 1; i > 0; i--)
    {
        VE_HOLDER t = v[0]; v[0] = v[i]; v[i] = t;
        _veHolderSift(v, 0, i);
    }
}

// Start using the index (built if nobody uses it yet). Pair with _veHolderIndexEnd().
static void
_veHolderIndexBegin(void)
{
    RmClient **ppClient;
    NvU32      n = 0;

    if (_veHolderUsers++ != 0)
        return;
    _veHolders = portMemAllocNonPaged(sizeof(*_veHolders) * VE_HOLDER_MAX);
    if (_veHolders == NULL)
        return;
    for (ppClient = serverutilGetFirstClientUnderLock();
         ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RsClient   *pRsClient = staticCast(*ppClient, RsClient);
        RS_ITERATOR iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0,
                                             RS_ITERATE_DESCENDANTS, NV_TRUE);

        while (clientRefIterNext(iter.pClient, &iter))
        {
            Memory *pMemory = dynamicCast(iter.pResourceRef->pResource, Memory);

            if ((pMemory == NULL) || (pMemory->pMemDesc == NULL) || (pMemory->pMemDesc->DupCount <= 1))
                continue;
            if (n >= VE_HOLDER_MAX)
            {
                // Too many to index: the walks as before.
                portMemFree(_veHolders);
                _veHolders = NULL;
                _veNumHolders = 0;
                return;
            }
            _veHolders[n].pMemDesc = pMemory->pMemDesc;
            _veHolders[n].pRef     = iter.pResourceRef;
            _veHolders[n].seq      = n;
            n++;
        }
    }
    _veHolderSort(_veHolders, n);
    _veNumHolders = n;
}

static void
_veHolderIndexEnd(void)
{
    if ((_veHolderUsers == 0) || (--_veHolderUsers != 0))
        return;
    if (_veHolders != NULL)
        portMemFree(_veHolders);
    _veHolders = NULL;
    _veNumHolders = 0;
}

// The holders of pMemDesc in walk order: *pFirst, return value = how many (0: not indexed).
static NvU32
_veHolderRange(MEMORY_DESCRIPTOR *pMemDesc, NvU32 *pFirst)
{
    NvU32 lo = 0, hi = _veNumHolders, n = 0;

    while (lo < hi)
    {
        NvU32 mid = lo + (hi - lo) / 2;

        if ((NvUPtr)_veHolders[mid].pMemDesc < (NvUPtr)pMemDesc)
            lo = mid + 1;
        else
            hi = mid;
    }
    *pFirst = lo;
    while ((lo + n < _veNumHolders) && (_veHolders[lo + n].pMemDesc == pMemDesc))
        n++;
    return n;
}

// _vidmemEvictCollectHolders() for one holder; VE_HOLDER_SKIP: not counted further.
static const char VE_HOLDER_SKIP[] = "skip";

static const char *
_vidmemEvictHolderOne
(
    OBJGPU              *pGpu,
    MEMORY_DESCRIPTOR   *pMemDesc,
    NvU32                pid,
    RsResourceRef       *pPrimary,
    NvBool               bCheck,
    RsResourceRef       *pRef,
    NvBool              *pbSeenPid,
    VIDMEM_EVICT_MAPREF *pRefs,
    NvU32               *pNumRefs,
    NvU32               *pNumHolders
)
{
    RsClient *pRsClient = pRef->pClient;
    RmClient *pRmClient = dynamicCast(pRsClient, RmClient);
    Memory   *pMemory = dynamicCast(pRef->pResource, Memory);
    NvBool    bCompositor = NV_FALSE;

    (*pNumHolders)++;

    if (bCheck)
    {
        if ((listCount(&pRef->cpuMappings) != 0) || (pMemory->KernelVAddr != NvP64_NULL))
            return "cpu-mapped";
        if (pMemory->pGpu != pGpu)
            return "other-gpu";

        if (pRsClient->type == CLIENT_TYPE_KERNEL)
        {
            if (listCount(&pRef->interBackRefsMappable) != 0)
                return "shared-kernel-mapped";
            return VE_HOLDER_SKIP;
        }
        if ((pRmClient != NULL) && (pRmClient->ProcID == pid))
        {
            if (!*pbSeenPid && (pRef != pPrimary))
                return "secondary-holder";
            *pbSeenPid = NV_TRUE;
        }
        else if ((pRmClient != NULL) && _vidmemEvictIsCompositor(pRmClient))
        {
            if (!_vidmemEvictWindows)
                return "window-buffer";
            bCompositor = NV_TRUE;
        }
        else
        {
            return "shared-other-app";
        }
    }
    else
    {
        bCompositor = (pRsClient->type != CLIENT_TYPE_KERNEL) && (pRmClient != NULL) &&
                      _vidmemEvictIsCompositor(pRmClient);
    }

    return _vidmemEvictAddHolderMaps(pGpu, pMemDesc, pRef, bCompositor, bCheck, pRefs, pNumRefs);
}

static const char *
_vidmemEvictCollectHolders
(
    OBJGPU              *pGpu,
    MEMORY_DESCRIPTOR   *pMemDesc,
    NvU32                pid,
    RsResourceRef       *pPrimary,
    NvBool               bCheck,
    VIDMEM_EVICT_MAPREF *pRefs,
    NvU32               *pNumRefs,
    NvU32               *pNumHolders
)
{
    RmClient  **ppClient;
    NvBool      bSeenPid = NV_FALSE;
    const char *reason;

    *pNumRefs = 0;
    *pNumHolders = 0;

    if ((pMemDesc->DupCount <= 1) && (pPrimary != NULL))
    {
        RmClient *pRmClient = dynamicCast(pPrimary->pClient, RmClient);

        *pNumHolders = 1;
        if (bCheck && (listCount(&pPrimary->cpuMappings) != 0))
            return "cpu-mapped";
        return _vidmemEvictAddHolderMaps(pGpu, pMemDesc, pPrimary,
                                         (pRmClient != NULL) && _vidmemEvictIsCompositor(pRmClient),
                                         bCheck, pRefs, pNumRefs);
    }

    if (_veHolders != NULL)
    {
        NvU32 first, n = _veHolderRange(pMemDesc, &first), k;

        for (k = 0; k < n; k++)
        {
            reason = _vidmemEvictHolderOne(pGpu, pMemDesc, pid, pPrimary, bCheck, _veHolders[first + k].pRef,
                                           &bSeenPid, pRefs, pNumRefs, pNumHolders);
            if ((reason != NULL) && (reason != VE_HOLDER_SKIP))
                return reason;
        }
    }
    else
    {
        for (ppClient = serverutilGetFirstClientUnderLock();
             ppClient != NULL;
             ppClient = serverutilGetNextClientUnderLock(ppClient))
        {
            RsClient   *pRsClient = staticCast(*ppClient, RsClient);
            RS_ITERATOR iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0,
                                                 RS_ITERATE_DESCENDANTS, NV_TRUE);

            while (clientRefIterNext(iter.pClient, &iter))
            {
                Memory *pMemory = dynamicCast(iter.pResourceRef->pResource, Memory);

                if ((pMemory == NULL) || (pMemory->pMemDesc != pMemDesc))
                    continue;
                reason = _vidmemEvictHolderOne(pGpu, pMemDesc, pid, pPrimary, bCheck, iter.pResourceRef,
                                               &bSeenPid, pRefs, pNumRefs, pNumHolders);
                if ((reason != NULL) && (reason != VE_HOLDER_SKIP))
                    return reason;
            }
        }
    }

    if (bCheck && (*pNumHolders != pMemDesc->DupCount))
        return "holders";
    return NULL;
}

//
// Fills pRefs with the GPU mappings of the allocation pRef (in any holder);
// returns NULL when it can be moved, else the reason it cannot.
//
#define VE_MODE_NAME(mode) (((mode) == 1) ? "evict" : (((mode) == 2) ? "restore" : "move"))

static const char *
_vidmemEvictCheck
(
    OBJGPU              *pGpu,
    RsResourceRef       *pRef,
    NvU32                pid,
    VIDMEM_EVICT_MAPREF *pRefs,
    NvU32               *pNumRefs
)
{
    MemoryManager     *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    Memory            *pMemory = dynamicCast(pRef->pResource, Memory);
    MEMORY_DESCRIPTOR *pMemDesc;
    PMA_ALLOC_INFO    *pPmaInfo;
    NvU32              numRefs = 0, numHolders = 0;
    const char        *reason;

    *pNumRefs = 0;

    if ((pMemory == NULL) || (pMemory->pMemDesc == NULL))
        return "no-memdesc";
    if ((pRef->externalClassId != NV01_MEMORY_LOCAL_USER) ||
        (pMemory->categoryClassId != NV01_MEMORY_LOCAL_USER))
        return "class";

    pMemDesc = pMemory->pMemDesc;
    pPmaInfo = pMemDesc->pPmaAllocInfo;

    if (pMemDesc->pLocalEvictState != NULL)
        return "evicted";
    if (pMemory->pGpu != pGpu)
        return "other-gpu";
    if (memdescGetAddressSpace(pMemDesc) != ADDR_FBMEM)
        return "not-fbmem";
    if (memdescGetFlag(pMemDesc, MEMDESC_FLAGS_LIST_MEMORY) ||
        memdescGetFlag(pMemDesc, MEMDESC_FLAGS_ALLOC_PER_SUBDEVICE) ||
        memdescGetFlag(pMemDesc, MEMDESC_FLAGS_ENCRYPTED) ||
        memdescGetFlag(pMemDesc, MEMDESC_FLAGS_ALLOC_AS_LOCALIZED) ||
        memdescHasSubDeviceMemDescs(pMemDesc) ||
        (memdescGetCustomHeap(pMemDesc) != MEMDESC_CUSTOM_HEAP_NONE))
        return "memdesc-flags";
    if ((pPmaInfo == NULL) || (pMemDesc->pHeap == NULL))
        return "not-pma";
    if (pPmaInfo->bContig != memdescGetContiguity(pMemDesc, AT_GPU))
        return "contiguity";
    if (pPmaInfo->flags & PMA_ALLOCATE_NO_ZERO)
        return "pma-no-zero";
    if ((pMemory->KernelVAddr != NvP64_NULL) || (pMemDesc->_pInternalMapping != NULL))
        return "cpu-mapped";
    if (!_vidmemEvictKindAllowed(pGpu, pMemoryManager, memdescGetPteKind(pMemDesc)))
        return "kind";
    if (memmgrIsKind_HAL(pMemoryManager, FB_IS_KIND_COMPRESSIBLE, memdescGetPteKind(pMemDesc)) &&
        !_vidmemEvictCeIsVirtual(pMemoryManager))
        return "compressed-no-vas-ce";

    reason = _vidmemEvictCollectHolders(pGpu, pMemDesc, pid, pRef, NV_TRUE, pRefs, &numRefs, &numHolders);
    if (reason != NULL)
        return reason;

    // heapReference() takes one PMA reference per dup, vidmemCopyConstruct() one static BAR1 reference.
    if (pPmaInfo->refCount != pMemDesc->DupCount)
        return "pma-refs";
    if (pMemDesc->staticBar1MappingRefCount > pMemDesc->DupCount)
        return "static-bar1";

    // Nobody else may hold the memdesc: the holders plus our mappings' sub-memdescs.
    if ((pMemDesc->childDescriptorCnt != numRefs) ||
        (pMemDesc->RefCount != pMemDesc->DupCount + numRefs))
        return "memdesc-refs";

    *pNumRefs = numRefs;
    return NULL;
}

static NvBool
_vidmemEvictHasCompositor(VIDMEM_EVICT_MAPREF *pRefs, NvU32 numRefs)
{
    NvU32 i;

    for (i = 0; i < numRefs; i++)
    {
        if (pRefs[i].bCompositor)
            return NV_TRUE;
    }
    return NV_FALSE;
}

static void
_vidmemEvictCollectChannels(RsClient *pRsClient, VIDMEM_EVICT_CHANNELS *pChannels)
{
    RS_ITERATOR iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0,
                                         RS_ITERATE_DESCENDANTS, NV_TRUE);
    NvU32 i;

    // Each client only once.
    for (i = 0; i < pChannels->count; i++)
    {
        if (pChannels->hClient[i] == pRsClient->hClient)
            return;
    }

    while (clientRefIterNext(iter.pClient, &iter))
    {
        KernelChannel *pKernelChannel = dynamicCast(iter.pResourceRef->pResource, KernelChannel);

        if ((pKernelChannel == NULL) || (pChannels->count >= VIDMEM_EVICT_MAX_CHANNELS))
            continue;

        pChannels->hClient[pChannels->count]  = pRsClient->hClient;
        pChannels->hChannel[pChannels->count] = RES_GET_HANDLE(pKernelChannel);
        pChannels->hTsg[pChannels->count]     = (pKernelChannel->pKernelChannelGroupApi != NULL) ?
            RES_GET_HANDLE(pKernelChannel->pKernelChannelGroupApi) : 0;
        pChannels->hDevice[pChannels->count]  = (GPU_RES_GET_DEVICE(pKernelChannel) != NULL) ?
            RES_GET_HANDLE(GPU_RES_GET_DEVICE(pKernelChannel)) : 0;
        pChannels->bImplicitTsg[pChannels->count] =
            (pKernelChannel->pKernelChannelGroupApi != NULL) &&
            (pKernelChannel->pKernelChannelGroupApi->pKernelChannelGroup != NULL) &&
            pKernelChannel->pKernelChannelGroupApi->pKernelChannelGroup->bAllocatedByRm;
        pChannels->count++;
    }
}

//
// Disable or re-enable channels, in batches of 64. GPU lock held. bPreempt:
// also take them off the engines here (FIFO_DISABLE_CHANNELS with preemption
// may preempt more than the listed channels; see _vidmemEvictSetChannels).
// On a failed disable the channels already disabled are re-enabled.
//
static NV_STATUS
_vidmemEvictSetChannelsN(OBJGPU *pGpu, VIDMEM_EVICT_CHANNELS *pChannels, NvU32 count, NvBool bDisable,
                         NvBool bPreempt)
{
    NV2080_CTRL_FIFO_DISABLE_CHANNELS_PARAMS *pParams;
    NV_STATUS status = NV_OK;
    NvU32 done = 0;

    if (count == 0)
        return NV_OK;

    pParams = portMemAllocNonPaged(sizeof(*pParams));
    if (pParams == NULL)
        return NV_ERR_NO_MEMORY;

    while (done < count)
    {
        NvU32 n = NV_MIN(count - done, NV2080_CTRL_FIFO_DISABLE_CHANNELS_MAX_ENTRIES);
        NvU32 i;

        portMemSet(pParams, 0, sizeof(*pParams));
        pParams->bDisable = bDisable ? NV2080_CTRL_FIFO_DISABLE_CHANNEL_TRUE :
                                       NV2080_CTRL_FIFO_DISABLE_CHANNEL_FALSE;
        pParams->bOnlyDisableScheduling = !bPreempt;
        pParams->numChannels = n;
        for (i = 0; i < n; i++)
        {
            pParams->hClientList[i]  = pChannels->hClient[done + i];
            pParams->hChannelList[i] = pChannels->hChannel[done + i];
        }

        NV_RM_RPC_CONTROL(pGpu, pGpu->hInternalClient, pGpu->hInternalSubdevice,
                          NV2080_CTRL_CMD_FIFO_DISABLE_CHANNELS,
                          pParams, sizeof(*pParams), status);
        if (status != NV_OK)
        {
            NV_PRINTF(LEVEL_ERROR, "vidmem evict: %s %u channels failed: 0x%x\n",
                      bDisable ? "disable" : "enable", n, status);
            break;
        }
        done += n;
    }

    portMemFree(pParams);

    // Re-enable the ones already disabled (the first `done`).
    if ((status != NV_OK) && bDisable && (done != 0))
        (void)_vidmemEvictSetChannelsN(pGpu, pChannels, done, NV_FALSE, NV_FALSE);

    return status;
}

// Short: with a game on GR a TSG preempt can take up to its timeout (02:50:
// 300 ms per stop with 200 ms), and the GPU lock is held all that time.
#define VIDMEM_EVICT_PREEMPT_TIMEOUT_US 20000
#define VIDMEM_EVICT_DRAIN_US           20000
// IDLE_CHANNELS: how long GSP may wait for the listed channels' work.
#define VIDMEM_EVICT_IDLE_TIMEOUT_US    500000
#define VIDMEM_EVICT_IDLE_BATCH         256

//
// IDLE_CHANNELS RPC (what NV0080_CTRL_CMD_FIFO_IDLE_CHANNELS sends): GSP
// waits until the listed channels have no pending work on the engines (and,
// with bPushBuffer, nothing left in their push buffers). GPU lock held.
//
static NV_STATUS
_vidmemEvictIdleChannels(OBJGPU *pGpu, VIDMEM_EVICT_CHANNELS *pChannels, NvBool bPushBuffer)
{
    NvU32     idle = NVOS30_FLAGS_IDLE_ALL_ENGINES | (bPushBuffer ? NVOS30_FLAGS_IDLE_PUSH_BUFFER : 0);
    NvU32     flags = DRF_DEF(OS30, _FLAGS, _BEHAVIOR, _SPIN) |
                      DRF_DEF(OS30, _FLAGS, _CHANNEL, _LIST) |
                      DRF_NUM(OS30, _FLAGS, _IDLE, idle);
    NV_STATUS status = NV_OK;
    NvU32     done = 0;

    while ((done < pChannels->count) && (status == NV_OK))
    {
        NvU32 n = NV_MIN(pChannels->count - done, VIDMEM_EVICT_IDLE_BATCH);

        NV_RM_RPC_IDLE_CHANNELS(pGpu, &pChannels->hClient[done], &pChannels->hDevice[done],
                                &pChannels->hChannel[done], n, flags, VIDMEM_EVICT_IDLE_TIMEOUT_US, status);
        done += n;
    }
    return status;
}

// Preempt each TSG of the list once (only that TSG). GPU lock held.
static void
_vidmemEvictPreemptTsgs(OBJGPU *pGpu, VIDMEM_EVICT_CHANNELS *pChannels, VIDMEM_EVICT_QUIESCE_STATS *pStats)
{
    NVA06C_CTRL_PREEMPT_PARAMS preempt;
    NvU32                      i, j;

    for (i = 0; i < pChannels->count; i++)
    {
        NV_STATUS st = NV_ERR_OBJECT_NOT_FOUND;

        for (j = 0; j < i; j++)
        {
            if ((pChannels->hClient[j] == pChannels->hClient[i]) && (pChannels->hTsg[j] == pChannels->hTsg[i]))
                break;
        }
        if (j < i)
            continue;

        pStats->tsgs++;
        if (pChannels->bImplicitTsg[i])
            pStats->implicitTsgs++;
        if (pChannels->hTsg[i] != 0)
        {
            portMemSet(&preempt, 0, sizeof(preempt));
            preempt.bWait          = NV_TRUE;
            preempt.bManualTimeout = NV_TRUE;
            preempt.timeoutUs      = VIDMEM_EVICT_PREEMPT_TIMEOUT_US;
            NV_RM_RPC_CONTROL(pGpu, pChannels->hClient[i], pChannels->hTsg[i], NVA06C_CTRL_CMD_PREEMPT,
                              &preempt, sizeof(preempt), st);
        }
        if (st == NV_OK)
        {
            pStats->preemptOk++;
        }
        else if (pStats->preemptRefused++ == 0)
        {
            pStats->firstRefusal = st;
        }
    }
}

// Trace (VMT_PAUSE) when a channel set goes back on: how long it was off.
static void
_vidmemTracePause(VIDMEM_EVICT_CHANNELS *pChannels, NV_STATUS status)
{
    NvU64 a[VMT_ARGS] = {0};

    if ((pChannels->count != 0) && (pChannels->vmtOffNs != 0) && osVidmemTraceOn(VMT_PAUSE))
    {
        a[0] = pChannels->vmtStopArgs[0];
        a[1] = pChannels->count;
        a[2] = pChannels->vmtStopArgs[1];
        a[3] = pChannels->vmtStopArgs[2];
        a[4] = pChannels->vmtStopArgs[3];
        a[5] = pChannels->vmtStopArgs[4];
        a[6] = VMT_NOW() - pChannels->vmtOffNs;
        a[7] = status;
        osVidmemTrace(VMT_PAUSE, pChannels->vmtPid, a);
    }
    pChannels->vmtOffNs = 0;
}

//
// Stop (bDisable) or resume a process's channels; how depends on the
// "quiesce" knob (see _vidmemEvictQuiesce). Mode 0 (default) never asks the
// GPU to preempt anything but the process's own channel groups: the
// 2026-11-02 02:21 game test showed that FIFO_DISABLE_CHANNELS with
// preemption (mode 1) also forced the running game's context off GR (Xid
// 109 CTX SWITCH TIMEOUT, 3.3 s stall). But mode 0 is not safe either when
// the process has long GPU work in flight: GSP refuses the TSG preempt for
// RM-allocated (implicit) TSGs, the fixed 20 ms wait expires, and memory
// moves under running work (nvtest.sh active: a 50 ms-per-frame app never
// finished its frame). Modes 2-4 wait for the work instead (IDLE_CHANNELS).
// The result of each stop is kept in _vidmemEvictLastStop for the report.
//
static NV_STATUS
_vidmemEvictSetChannels(OBJGPU *pGpu, VIDMEM_EVICT_CHANNELS *pChannels, NvBool bDisable)
{
    VIDMEM_EVICT_QUIESCE_STATS stats = {0};
    NvU64                      startNs = VMT_NOW(), t;
    NV_STATUS                  status = NV_OK;
    VE_PHASE_BEGIN();

    if (!bDisable)
    {
        status = _vidmemEvictSetChannelsN(pGpu, pChannels, pChannels->count, NV_FALSE, NV_FALSE);
        VE_PHASE_END(VE_PH_QUIESCE);
        _vidmemTracePause(pChannels, status);
        return status;
    }
    pChannels->vmtOffNs = 0;
    if (pChannels->count == 0)
        return NV_OK;

    stats.mode     = _vidmemEvictQuiesce;
    stats.channels = pChannels->count;
    stats.idleStatus = NV_OK;

    switch (stats.mode)
    {
        case 1:
            status = _vidmemEvictSetChannelsN(pGpu, pChannels, pChannels->count, NV_TRUE, NV_TRUE);
            stats.disableNs = VMT_NOW() - startNs;
            break;

        case 2:
        case 3:
            status = _vidmemEvictSetChannelsN(pGpu, pChannels, pChannels->count, NV_TRUE, NV_FALSE);
            stats.disableNs = VMT_NOW() - startNs;
            if (status != NV_OK)
                break;
            t = VMT_NOW();
            stats.idleStatus = _vidmemEvictIdleChannels(pGpu, pChannels, stats.mode == 3);
            stats.idleNs = VMT_NOW() - t;
            if (stats.idleStatus != NV_OK)
                osDelayUs(VIDMEM_EVICT_DRAIN_US);
            break;

        case 4:
            t = VMT_NOW();
            stats.idleStatus = _vidmemEvictIdleChannels(pGpu, pChannels, NV_TRUE);
            stats.idleNs = VMT_NOW() - t;
            t = VMT_NOW();
            status = _vidmemEvictSetChannelsN(pGpu, pChannels, pChannels->count, NV_TRUE, NV_FALSE);
            stats.disableNs = VMT_NOW() - t;
            if ((status == NV_OK) && (stats.idleStatus != NV_OK))
                osDelayUs(VIDMEM_EVICT_DRAIN_US);
            break;

        default:
            status = _vidmemEvictSetChannelsN(pGpu, pChannels, pChannels->count, NV_TRUE, NV_FALSE);
            stats.disableNs = VMT_NOW() - startNs;
            if (status != NV_OK)
                break;
            _vidmemEvictPreemptTsgs(pGpu, pChannels, &stats);
            if (stats.preemptRefused != 0)
                osDelayUs(VIDMEM_EVICT_DRAIN_US);
            break;
    }

    stats.totalNs = VMT_NOW() - startNs;
    _vidmemEvictLastStop = stats;
    pChannels->vmtOffNs = startNs;
    pChannels->vmtStopArgs[0] = stats.mode;
    pChannels->vmtStopArgs[1] = stats.tsgs;
    pChannels->vmtStopArgs[2] = stats.preemptOk;
    pChannels->vmtStopArgs[3] = stats.preemptRefused;
    pChannels->vmtStopArgs[4] = stats.totalNs;
    VE_PHASE_END(VE_PH_QUIESCE);

    if (stats.totalNs > 50000000ULL)
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: stopping %u channels (quiesce %u) took %llu ms (disable %llu ms, idle %llu ms: 0x%x, TSG preempt ok %u refused %u)\n",
                  stats.channels, stats.mode, stats.totalNs / 1000000, stats.disableNs / 1000000,
                  stats.idleNs / 1000000, stats.idleStatus, stats.preemptOk, stats.preemptRefused);
    return status;
}

// Channels of every client of the compositor. API lock held.
static void
_vidmemEvictCollectCompositorChannels(VIDMEM_EVICT_CHANNELS *pChannels)
{
    RmClient **ppClient;

    for (ppClient = serverutilGetFirstClientUnderLock();
         ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RsClient *pRsClient = staticCast(*ppClient, RsClient);

        if ((pRsClient->type != CLIENT_TYPE_KERNEL) && _vidmemEvictIsCompositor(*ppClient))
        {
            _vidmemEvictCollectChannels(pRsClient, pChannels);
            pChannels->vmtPid = (*ppClient)->ProcID;
        }
    }
}

//
// Point one existing GPU mapping at a sub-range of pNewParent with PTE kind
// `kind`: the same as an unmap + map at the same VA by the client. On a
// failed map the old sub-memdesc is mapped back.
//
static NV_STATUS
_vidmemEvictRemapOne
(
    OBJGPU              *pGpu,
    VIDMEM_EVICT_MAPREF *pMapRef,
    MEMORY_DESCRIPTOR   *pNewParent,
    NvU32                kind
)
{
    Memory               *pMemory = pMapRef->pHolder;
    VirtMemAllocator     *pDma = GPU_GET_DMA(pGpu);
    CLI_DMA_MAPPING_INFO *pInfo = pMapRef->pDmaMappingInfo;
    MEMORY_DESCRIPTOR    *pOldSub = pInfo->pMemDesc;
    MEMORY_DESCRIPTOR    *pNewSub = NULL;
    NvU32                 savedFlags = pInfo->Flags;
    NvU64                 savedOffset = pInfo->DmaOffset;
    NvU64                 mapPageSize = pInfo->mapPageSize;
    NvU32                 pageSizeFlag;
    NV_STATUS             status;
    NV_STATUS             rollbackStatus;

    if (mapPageSize == RM_PAGE_SIZE)
        pageSizeFlag = NVOS46_FLAGS_PAGE_SIZE_4KB;
    else if (mapPageSize == RM_PAGE_SIZE_HUGE)
        pageSizeFlag = NVOS46_FLAGS_PAGE_SIZE_HUGE;
    else if (mapPageSize == vaspaceGetBigPageSize(pMapRef->pVas))
        pageSizeFlag = NVOS46_FLAGS_PAGE_SIZE_BIG;
    else
        return NV_ERR_NOT_SUPPORTED;

    NV_ASSERT_OK_OR_RETURN(memdescCreateSubMem(&pNewSub, pNewParent, pGpu,
                                               pOldSub->subMemOffset, pOldSub->Size));
    memdescSetPteKind(pNewSub, kind);

    status = dmaFreeMap(pGpu, pDma, pMapRef->pVas, pMapRef->pVirtualMemory, pInfo,
                        DRF_DEF(OS47, _FLAGS, _DEFER_TLB_INVALIDATION, _FALSE));
    if (status != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR, "vidmem evict: unmap at 0x%llx failed: 0x%x\n", savedOffset, status);
        memdescDestroy(pNewSub);
        return status;
    }

    pInfo->pMemDesc = pNewSub;
    pInfo->Flags = FLD_SET_DRF(OS46, _FLAGS, _DMA_OFFSET_FIXED, _TRUE, savedFlags);
    pInfo->Flags = FLD_SET_DRF_NUM(OS46, _FLAGS, _PAGE_SIZE, pageSizeFlag, pInfo->Flags);
    // The client may have mapped with a deferred invalidate; we never defer.
    pInfo->Flags = FLD_SET_DRF(OS46, _FLAGS, _DEFER_TLB_INVALIDATION, _FALSE, pInfo->Flags);

    status = dmaAllocMap(pGpu, pDma, pMapRef->pVas, pMapRef->pVirtualMemory, pMemory, pInfo);
    if ((status == NV_OK) && ((pInfo->DmaOffset != savedOffset) || (pInfo->mapPageSize != mapPageSize)))
    {
        NV_PRINTF(LEVEL_ERROR, "vidmem evict: remap moved 0x%llx/0x%llx -> 0x%llx/0x%llx\n",
                  savedOffset, mapPageSize, pInfo->DmaOffset, pInfo->mapPageSize);
        (void)dmaFreeMap(pGpu, pDma, pMapRef->pVas, pMapRef->pVirtualMemory, pInfo, 0);
        status = NV_ERR_INVALID_STATE;
    }

    if (status != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR, "vidmem evict: map at 0x%llx failed: 0x%x, mapping old memory back\n",
                  savedOffset, status);
        pInfo->pMemDesc = pOldSub;
        pInfo->DmaOffset = savedOffset;
        rollbackStatus = dmaAllocMap(pGpu, pDma, pMapRef->pVas, pMapRef->pVirtualMemory, pMemory, pInfo);
        if (rollbackStatus != NV_OK)
        {
            NV_PRINTF(LEVEL_ERROR, "vidmem evict: mapping 0x%llx is now INVALID (0x%x)\n",
                      savedOffset, rollbackStatus);
        }
        pInfo->Flags = savedFlags;
        pInfo->DmaOffset = savedOffset;
        memdescDestroy(pNewSub);
        return status;
    }

    pInfo->Flags = savedFlags;
    memdescDestroy(pOldSub);

    if (_vidmemEvictTrace)
    {
        RmClient *pRmClient = dynamicCast(RES_GET_CLIENT(pMemory), RmClient);
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict trace: %s(%u) mem 0x%x va 0x%llx-0x%llx page %llu KB %s kind 0x%x -> %s\n",
                  (pRmClient != NULL) ? pRmClient->name : "?", (pRmClient != NULL) ? pRmClient->ProcID : 0,
                  RES_GET_HANDLE(pMemory), savedOffset, savedOffset + pNewSub->Size - 1,
                  mapPageSize >> 10, pMapRef->pVirtualMemory->bReserveVaOnAlloc ? "va-reserved" : "va-per-map",
                  kind, (memdescGetAddressSpace(pNewParent) == ADDR_SYSMEM) ? "sysmem" : "vidmem");
    }
    return NV_OK;
}

static void
_vidmemEvictFlushSysmemL2(OBJGPU *pGpu, FB_CACHE_OP op)
{
    (void)kmemsysCacheOp_HAL(pGpu, GPU_GET_KERNEL_MEMORY_SYSTEM(pGpu), NULL,
                             FB_CACHE_SYSTEM_MEMORY, op);
}

NvBool vidmemGetUserReserveBlockInfo(PMA *pPma, NvU64 *pBlockBytes, NvU64 *pFreeBytes,
                                     NvU64 *pLargestFreeBytes);

// The compositor's reserve block keeps at least 256 MB free after a restore into it.
static NvBool
_vidmemEvictBlockCanTake(PMA *pPma, NvU64 bytes)
{
    NvU64 blockBytes = 0, blockFree = 0, blockLargest = 0;

    if (!vidmemGetUserReserveBlockInfo(pPma, &blockBytes, &blockFree, &blockLargest))
        return NV_TRUE;
    return blockFree >= bytes + (256ULL << 20);
}

// Kind a mapping had before the eviction (pInfo is the mapping being restored).
static NvU32
_vidmemEvictOrigKind(VIDMEM_EVICT_STATE *pState, CLI_DMA_MAPPING_INFO *pInfo, NvU32 defaultKind)
{
    NvU32 j;

    for (j = 0; j < pState->numMappings; j++)
    {
        if (pState->mappings[j].pDmaMappingInfo == pInfo)
            return pState->mappings[j].origKind;
    }
    return defaultKind;
}

//
// Remaps the refs whose bCompositor equals bCompositor onto pTarget (sysmem
// copy: uncompressed original kind; vidmem: original kind). On a failure the
// ones already done in this call are put back onto pOther.
//
static NV_STATUS
_vidmemEvictRemapSet
(
    OBJGPU              *pGpu,
    VIDMEM_EVICT_STATE  *pState,
    MEMORY_DESCRIPTOR   *pMemDesc,
    VIDMEM_EVICT_MAPREF *pRefs,
    NvU32                numRefs,
    NvBool               bCompositor,
    NvBool               bToSysmem
)
{
    MemoryManager     *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    MEMORY_DESCRIPTOR *pTarget = bToSysmem ? pState->pSysMemDesc : pMemDesc;
    MEMORY_DESCRIPTOR *pOther  = bToSysmem ? pMemDesc : pState->pSysMemDesc;
    NvU32              defaultKind = memdescGetPteKind(pMemDesc);
    NV_STATUS          status = NV_OK;
    NvU32              i;

    for (i = 0; i < numRefs; i++)
    {
        NvU32 orig;

        if (pRefs[i].bCompositor != bCompositor)
            continue;
        orig = _vidmemEvictOrigKind(pState, pRefs[i].pDmaMappingInfo, defaultKind);
        status = _vidmemEvictRemapOne(pGpu, &pRefs[i], pTarget,
                     bToSysmem ? _vidmemEvictUncompressedKind(pGpu, pMemoryManager, orig) : orig);
        if (status != NV_OK)
            break;
    }
    if (status == NV_OK)
        return NV_OK;

    while (i-- > 0)
    {
        NvU32 orig;

        if (pRefs[i].bCompositor != bCompositor)
            continue;
        orig = _vidmemEvictOrigKind(pState, pRefs[i].pDmaMappingInfo, defaultKind);
        (void)_vidmemEvictRemapOne(pGpu, &pRefs[i], pOther,
                  bToSysmem ? orig : _vidmemEvictUncompressedKind(pGpu, pMemoryManager, orig));
    }
    return status;
}

static void
_vidmemEvictFreePma(MEMORY_DESCRIPTOR *pMemDesc)
{
    PMA_ALLOC_INFO *pPmaInfo = pMemDesc->pPmaAllocInfo;
    PMA            *pPma = pMemDesc->pHeap->pPmaObject;

    // Same free as vidmemPmaFree().
    if (pPmaInfo->bContig)
        pmaFreePages(pPma, pPmaInfo->pageArray, 1, pPmaInfo->allocSize, 0);
    else
        pmaFreePages(pPma, pPmaInfo->pageArray, pPmaInfo->pageCount, pPmaInfo->pageSize, 0);
}

static void
_vidmemEvictDropStaticBar1(OBJGPU *pGpu, MEMORY_DESCRIPTOR *pMemDesc, NvU32 *pDropped)
{
    while (pMemDesc->staticBar1MappingRefCount != 0)
    {
        if (kbusDecreaseStaticBar1Refcount_HAL(pGpu, GPU_GET_KERNEL_BUS(pGpu), pMemDesc, NULL) != NV_OK)
            break;
        if (pDropped != NULL)
            (*pDropped)++;
    }
}

//
// Batching inside one evict/restore call of a process (channels stopped):
// one L2 writeback of sysmem before the first restore copy, and the sysmem
// copies of restored allocations freed together after one L2 evict at the
// end, instead of an L2 operation per allocation (2026-11-02: 25-30 ms of
// L2 flushes for Chrome's 112 allocations, as long as the copies).
//
#define VIDMEM_EVICT_DEFER_MAX 1024
static NvBool              _vidmemEvictBatch;          // inside _vidmemEvictPidLocked()
static NvBool              _vidmemEvictL2WrittenBack;  // writeback done for this batch
static VIDMEM_EVICT_STATE *_vidmemEvictDeferred[VIDMEM_EVICT_DEFER_MAX];
static NvU32               _vidmemEvictDeferredCount;

static void
_vidmemEvictFreeStateNow(VIDMEM_EVICT_STATE *pState)
{
    memdescFree(pState->pSysMemDesc);
    memdescDestroy(pState->pSysMemDesc);
    portMemFree(pState);
}

static void
_vidmemEvictFreeState(OBJGPU *pGpu, VIDMEM_EVICT_STATE *pState)
{
    if (_vidmemEvictBatch && (_vidmemEvictDeferredCount < VIDMEM_EVICT_DEFER_MAX))
    {
        _vidmemEvictDeferred[_vidmemEvictDeferredCount++] = pState;
        return;
    }
    _vidmemEvictFlushSysmemL2(pGpu, FB_CACHE_EVICT);
    _vidmemEvictFreeStateNow(pState);
}

static void
_vidmemEvictBatchBegin(void)
{
    _vidmemEvictBatch = NV_TRUE;
    _vidmemEvictL2WrittenBack = NV_FALSE;
    _vidmemEvictDeferredCount = 0;
}

static void
_vidmemEvictBatchEnd(OBJGPU *pGpu)
{
    NvU32 i;

    _vidmemEvictBatch = NV_FALSE;
    _vidmemEvictL2WrittenBack = NV_FALSE;
    if (_vidmemEvictDeferredCount == 0)
        return;
    _vidmemEvictFlushSysmemL2(pGpu, FB_CACHE_EVICT);
    for (i = 0; i < _vidmemEvictDeferredCount; i++)
        _vidmemEvictFreeStateNow(_vidmemEvictDeferred[i]);
    _vidmemEvictDeferredCount = 0;
}

//
// Evict, phase 1. Channels of the owning process disabled, GPU lock held:
// copy into a new sysmem allocation and move the owning process's mappings
// there. The compositor's mappings (which only read) still point at the
// vidmem copy, which stays allocated until _vidmemEvictCommit().
//
static NV_STATUS
_vidmemEvictBegin
(
    OBJGPU               *pGpu,
    MEMORY_DESCRIPTOR    *pMemDesc,
    VIDMEM_EVICT_MAPREF  *pRefs,
    NvU32                 numRefs,
    VIDMEM_EVICT_STATE  **ppState
)
{
    MemoryManager      *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    PMA_ALLOC_INFO     *pPmaInfo = pMemDesc->pPmaAllocInfo;
    MEMORY_DESCRIPTOR  *pSys = NULL;
    VIDMEM_EVICT_STATE *pState;
    CEUTILS_MEMCOPY_PARAMS copyParams = {0};
    NvU64               size = memdescGetSize(pMemDesc);
    NvU32               kind = memdescGetPteKind(pMemDesc);
    NvU64               maxMapPageSize = 0;
    NvU64               chunk;
    NvU32               i;
    NV_STATUS           status;
    VE_PHASE_BEGIN();

    *ppState = NULL;

    pState = portMemAllocNonPaged(sizeof(*pState));
    if (pState == NULL)
        return NV_ERR_NO_MEMORY;
    portMemSet(pState, 0, sizeof(*pState));

    for (i = 0; i < numRefs; i++)
    {
        maxMapPageSize = NV_MAX(maxMapPageSize, pRefs[i].pDmaMappingInfo->mapPageSize);
        pState->mappings[i].pDmaMappingInfo = pRefs[i].pDmaMappingInfo;
        pState->mappings[i].origKind        = memdescGetPteKind(pRefs[i].pDmaMappingInfo->pMemDesc);
        pState->ranges[i].pVas    = pRefs[i].pVas;
        pState->ranges[i].vaStart = pRefs[i].pDmaMappingInfo->DmaOffset;
        pState->ranges[i].vaEnd   = pRefs[i].pDmaMappingInfo->DmaOffset +
                                    memdescGetSize(pRefs[i].pDmaMappingInfo->pMemDesc) - 1;
    }
    pState->numMappings = numRefs;
    pState->numRanges   = numRefs;

    //
    // Sysmem chunks must be at least as big as every mapping's GPU page. A
    // non-contiguous allocation uses its own page size (>= every mapping's);
    // a contiguous one may be mapped with pages bigger than its own.
    //
    if (pPmaInfo->bContig)
    {
        chunk = NV_MAX(maxMapPageSize, RM_PAGE_SIZE_64K);
        pState->restoreAlign = NV_MAX(NV_MAX(pPmaInfo->pageSize, maxMapPageSize),
                                      NV_MAX(pMemDesc->Alignment, RM_PAGE_SIZE));
    }
    else
    {
        chunk = pPmaInfo->pageSize;
    }

    status = memdescCreate(&pSys, pGpu, NV_ALIGN_UP64(size, chunk), 0, NV_FALSE,
                           ADDR_SYSMEM, NV_MEMORY_CACHED, MEMDESC_FLAGS_NONE);
    if (status != NV_OK)
        goto fail_state;

    memdescSetPageSize(pSys, AT_GPU, chunk);
    memdescSetPteKind(pSys, _vidmemEvictUncompressedKind(pGpu, pMemoryManager, kind));
    memdescSetCpuCacheSnoop(pSys, MEMDESC_CACHE_SNOOP_ENABLE);

    VE_PHASE_SKIP();
    memdescTagAlloc(status, NV_FB_ALLOC_RM_INTERNAL_OWNER_UNNAMED_TAG_141, pSys);
    VE_PHASE_END(VE_PH_SYSALLOC);
    if (status != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR, "vidmem evict: sysmem alloc of %llu KB (pages %llu KB) failed: 0x%x\n",
                  size >> 10, chunk >> 10, status);
        memdescDestroy(pSys);
        goto fail_state;
    }
    pState->pSysMemDesc = pSys;
    pState->bytes       = size;

    copyParams.pSrcMemDesc = pMemDesc;
    copyParams.pDstMemDesc = pSys;
    copyParams.length      = size;
    VE_PHASE_SKIP();
    status = ceutilsMemcopy(pMemoryManager->pCeUtils, &copyParams);
    VE_PHASE_END(VE_PH_COPY);
    if (status != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR, "vidmem evict: copy to sysmem failed: 0x%x\n", status);
        goto fail_sys;
    }

    status = _vidmemEvictRemapSet(pGpu, pState, pMemDesc, pRefs, numRefs, NV_FALSE, NV_TRUE);
    VE_PHASE_END(VE_PH_REMAP);
    if (status != NV_OK)
        goto fail_sys;

    *ppState = pState;
    return NV_OK;

fail_sys:
    _vidmemEvictFreeState(pGpu, pState);
    return status;
fail_state:
    portMemFree(pState);
    return status;
}

// Undo a _vidmemEvictBegin() that will not be committed.
static void
_vidmemEvictAbort(OBJGPU *pGpu, MEMORY_DESCRIPTOR *pMemDesc, VIDMEM_EVICT_STATE *pState,
                  VIDMEM_EVICT_MAPREF *pRefs, NvU32 numRefs)
{
    NvU32 i;

    for (i = 0; i < numRefs; i++)
    {
        if (!pRefs[i].bCompositor)
            (void)_vidmemEvictRemapOne(pGpu, &pRefs[i], pMemDesc,
                      _vidmemEvictOrigKind(pState, pRefs[i].pDmaMappingInfo, memdescGetPteKind(pMemDesc)));
    }
    _vidmemEvictFreeState(pGpu, pState);
}

//
// Evict, phase 2. Channels of the owner and (if it maps the buffer) of the
// compositor disabled: move the compositor's mappings, release the vidmem.
//
static NV_STATUS
_vidmemEvictCommit(OBJGPU *pGpu, MEMORY_DESCRIPTOR *pMemDesc, VIDMEM_EVICT_STATE *pState,
                   VIDMEM_EVICT_MAPREF *pRefs, NvU32 numRefs)
{
    NV_STATUS status;
    VE_PHASE_BEGIN();

    status = _vidmemEvictRemapSet(pGpu, pState, pMemDesc, pRefs, numRefs, NV_TRUE, NV_TRUE);
    VE_PHASE_END(VE_PH_REMAP);
    if (status != NV_OK)
    {
        _vidmemEvictAbort(pGpu, pMemDesc, pState, pRefs, numRefs);
        return status;
    }

    //
    // Drop the static BAR1 references like vidmemDestruct() does: the last
    // one puts the BAR1 PTEs of these pages back to the default kind before
    // PMA hands them to someone else.
    //
    _vidmemEvictDropStaticBar1(pGpu, pMemDesc, &pState->staticBar1Refs);
    _vidmemEvictFreePma(pMemDesc);
    VE_PHASE_END(VE_PH_PMA);

    pMemDesc->pLocalEvictState = pState;
    pState->pMemDesc = pMemDesc;
    pState->ownerPid = _vidmemEvictCurPid;
    pState->hotNs    = 0;
    pState->hotCount = 0;
    _vidmemEvictListAdd(pState);
    _vidmemEvictBytes += pState->bytes;
    _vidmemEvictAllocs++;
    return NV_OK;
}

//
// Restore, phase 1. Channels of the owner disabled: new vidmem, copy back,
// the owner's mappings move back. The compositor still reads the sysmem copy.
//
static NV_STATUS
_vidmemRestoreBegin(OBJGPU *pGpu, MEMORY_DESCRIPTOR *pMemDesc, VIDMEM_EVICT_MAPREF *pRefs, NvU32 numRefs,
                    NvBool bAllowBlock)
{
    MemoryManager      *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    VIDMEM_EVICT_STATE *pState = pMemDesc->pLocalEvictState;
    PMA_ALLOC_INFO     *pPmaInfo = pMemDesc->pPmaAllocInfo;
    PMA                *pPma = pMemDesc->pHeap->pPmaObject;
    PMA_ALLOCATION_OPTIONS allocOptions = {0};
    CEUTILS_MEMCOPY_PARAMS copyParams = {0};
    NvU64              *pPages;
    NvU32               bar1Refs, i;
    NV_STATUS           status;
    VE_PHASE_BEGIN();

    NV_ASSERT_OR_RETURN(pState != NULL, NV_ERR_INVALID_STATE);

    pPages = portMemAllocNonPaged(sizeof(NvU64) * pPmaInfo->pageCount);
    if (pPages == NULL)
        return NV_ERR_NO_MEMORY;

    //
    // Pinned, as every RM allocation (video_mem.c). Without it PMA marks the
    // restored frames unpinned, i.e. evictable UVM memory: the next allocation
    // that needs them calls the UVM eviction callback, which is NULL when
    // nvidia-uvm has no GPU registered (2026-11-04, scenario.py --bg-fill:
    // NULL pointer call in _pmaEvictContiguous from the game's vidmemConstruct,
    // then soft lockups on every CPU).
    //
    allocOptions.flags     = PMA_ALLOCATE_FORCE_ALIGNMENT | PMA_ALLOCATE_PINNED |
                             (pPmaInfo->flags & PMA_ALLOCATE_PERSISTENT);
    allocOptions.alignment = pPmaInfo->pageSize;
    if (pPmaInfo->bContig)
    {
        allocOptions.flags    |= PMA_ALLOCATE_CONTIGUOUS;
        allocOptions.alignment = pState->restoreAlign;
    }
    // Above the user reserve block, also when the caller is the (kernel thread) worker.
    if (vidmemUserReserveBoundRangeAny(pPma, allocOptions.alignment,
                                       &allocOptions.physBegin, &allocOptions.physEnd))
    {
        allocOptions.flags |= PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE;
    }
    status = pmaAllocatePages(pPma, pPmaInfo->pageCount, pPmaInfo->pageSize, &allocOptions, pPages);
    if ((status != NV_OK) && bAllowBlock && _vidmemEvictBlockCanTake(pPma, pState->bytes))
    {
        //
        // RM itself needs the vidmem pages now (scanout, CPU mapping): a
        // failure would fail the client's call. Use the reserve block too
        // (an explicit whole range, so pmaAllocatePages() does not add the
        // above-the-block one).
        //
        allocOptions.flags    |= PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE;
        allocOptions.physBegin = 0;
        allocOptions.physEnd   = NV_ALIGN_DOWN64(NV_U64_MAX, allocOptions.alignment) - 1;
        status = pmaAllocatePages(pPma, pPmaInfo->pageCount, pPmaInfo->pageSize, &allocOptions, pPages);
    }
    if (status != NV_OK)
    {
        NV_PRINTF(LEVEL_WARNING, "vidmem evict: restore of %llu MB%s: no vidmem (0x%x)\n",
                  pState->bytes >> 20, pPmaInfo->bContig ? " (contiguous)" : "", status);
        portMemFree(pPages);
        return status;
    }

    pPmaInfo->flags = allocOptions.flags & (PMA_ALLOCATE_FORCE_ALIGNMENT | PMA_ALLOCATE_CONTIGUOUS |
                                            PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE | PMA_ALLOCATE_PINNED |
                                            PMA_ALLOCATE_PERSISTENT);
    if (pPmaInfo->bContig)
    {
        // Same layout as the vidmem alloc path: memdescDescribe(pageArray[0]).
        NV_ASSERT(pMemDesc->PteAdjust == 0);
        pPmaInfo->pageArray[0] = pPages[0];
        pMemDesc->_pteArray[0] = pPages[0];
    }
    else
    {
        portMemCopy(pPmaInfo->pageArray, sizeof(NvU64) * pPmaInfo->pageCount,
                    pPages, sizeof(NvU64) * pPmaInfo->pageCount);
        memdescFillPages(pMemDesc, 0, pPmaInfo->pageArray, pPmaInfo->pageCount, pPmaInfo->pageSize);
    }
    portMemFree(pPages);

    // One static BAR1 reference per remaining holder (some may be gone).
    bar1Refs = NV_MIN(pState->staticBar1Refs, pMemDesc->DupCount);
    for (i = 0; i < bar1Refs; i++)
    {
        status = kbusIncreaseStaticBar1Refcount_HAL(pGpu, GPU_GET_KERNEL_BUS(pGpu), pMemDesc,
                                                    BUS_MAP_FB_FLAGS_NONE);
        if ((status != NV_OK) && (status != NV_ERR_NOT_SUPPORTED))
            NV_PRINTF(LEVEL_ERROR, "vidmem evict: static BAR1 reference on restore: 0x%x\n", status);
    }
    VE_PHASE_END(VE_PH_PMA);

    // GPU writes to the sysmem copy may still sit in L2 (once per batch:
    // the channels are stopped, nothing writes in between).
    if (!_vidmemEvictBatch || !_vidmemEvictL2WrittenBack)
    {
        _vidmemEvictFlushSysmemL2(pGpu, FB_CACHE_WRITEBACK);
        _vidmemEvictL2WrittenBack = _vidmemEvictBatch;
    }

    copyParams.pSrcMemDesc = pState->pSysMemDesc;
    copyParams.pDstMemDesc = pMemDesc;
    copyParams.length      = pState->bytes;
    status = ceutilsMemcopy(pMemoryManager->pCeUtils, &copyParams);
    VE_PHASE_END(VE_PH_COPY);
    if (status != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR, "vidmem evict: copy back failed: 0x%x\n", status);
        goto fail_pages;
    }

    status = _vidmemEvictRemapSet(pGpu, pState, pMemDesc, pRefs, numRefs, NV_FALSE, NV_FALSE);
    VE_PHASE_END(VE_PH_REMAP);
    if (status != NV_OK)
        goto fail_pages;

    return NV_OK;

fail_pages:
    // Still evicted: the new pages go back to PMA, so drop their BAR1 refs too.
    _vidmemEvictDropStaticBar1(pGpu, pMemDesc, NULL);
    _vidmemEvictFreePma(pMemDesc);
    return status;
}

// Undo a _vidmemRestoreBegin() that will not be committed.
static void
_vidmemRestoreAbort(OBJGPU *pGpu, MEMORY_DESCRIPTOR *pMemDesc, VIDMEM_EVICT_MAPREF *pRefs, NvU32 numRefs)
{
    VIDMEM_EVICT_STATE *pState = pMemDesc->pLocalEvictState;
    MemoryManager      *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    NvU32               i;

    for (i = 0; i < numRefs; i++)
    {
        if (!pRefs[i].bCompositor)
            (void)_vidmemEvictRemapOne(pGpu, &pRefs[i], pState->pSysMemDesc,
                      _vidmemEvictUncompressedKind(pGpu, pMemoryManager,
                          _vidmemEvictOrigKind(pState, pRefs[i].pDmaMappingInfo, memdescGetPteKind(pMemDesc))));
    }
    _vidmemEvictDropStaticBar1(pGpu, pMemDesc, NULL);
    _vidmemEvictFreePma(pMemDesc);
}

// Restore, phase 2: the compositor's mappings move back, sysmem is freed.
static NV_STATUS
_vidmemRestoreCommit(OBJGPU *pGpu, MEMORY_DESCRIPTOR *pMemDesc, VIDMEM_EVICT_MAPREF *pRefs, NvU32 numRefs)
{
    VIDMEM_EVICT_STATE *pState = pMemDesc->pLocalEvictState;
    NV_STATUS           status;
    VE_PHASE_BEGIN();

    status = _vidmemEvictRemapSet(pGpu, pState, pMemDesc, pRefs, numRefs, NV_TRUE, NV_FALSE);
    VE_PHASE_END(VE_PH_REMAP);
    if (status != NV_OK)
    {
        _vidmemRestoreAbort(pGpu, pMemDesc, pRefs, numRefs);
        return status;
    }

    _vidmemEvictBytes -= NV_MIN(_vidmemEvictBytes, pState->bytes);
    _vidmemEvictAllocs -= NV_MIN(_vidmemEvictAllocs, 1);
    _vidmemEvictListRemove(pState);
    pMemDesc->pLocalEvictState = NULL;
    _vidmemEvictFreeState(pGpu, pState);
    VE_PHASE_END(VE_PH_SYSFREE);
    return NV_OK;
}

NvU64 *vidmemPmaUsedMap(PMA *pPma, NvU64 *pBase, NvU64 *pFrames, NvU64 *pLargestFree); // video_mem.c

//
// Where a moved allocation goes: the smallest free run that holds `bytes`
// (in pages of `align`), outside the two avoided ranges (hi 0: none). Best
// fit keeps the long free runs whole: with "anywhere outside the window" the
// desktop's small long-lived buffers ended up sprinkled over all of vidmem
// and the longest free run of an idle desktop fell from 8.6 GB to 2 GB
// (2026-11-10, nvtest fgprio failed on it). NV_FALSE: no single run fits.
//
static NvBool
_vidmemBestFitRange(PMA *pPma, NvU64 bytes, NvU64 align, NvU64 aLo, NvU64 aHi, NvU64 bLo, NvU64 bHi,
                    NvU64 *pBegin, NvU64 *pEnd, NvU64 *pRegionBase)
{
    NvU64  base = 0, frames = 0, largest = 0, f, start = 0, best = NV_U64_MAX;
    NvU64 *pUsed = vidmemPmaUsedMap(pPma, &base, &frames, &largest);
    NvBool bIn = NV_FALSE;

    if (pUsed == NULL)
        return NV_FALSE;
    *pRegionBase = base;
    for (f = 0; f <= frames; f++)
    {
        NvU64  addr = base + (f << PMA_PAGE_SHIFT);
        NvBool bFree = (f < frames) && !((pUsed[f >> 6] >> (f & 63)) & 1) &&
                       !((aHi != 0) && (addr >= aLo) && (addr < aHi)) &&
                       !((bHi != 0) && (addr >= bLo) && (addr < bHi));

        if (bFree && !bIn)
        {
            start = addr;
            bIn   = NV_TRUE;
        }
        else if (!bFree && bIn)
        {
            NvU64 lo = NV_ALIGN_UP64(start, align), hi = NV_ALIGN_DOWN64(addr, align);

            bIn = NV_FALSE;
            if ((hi > lo) && (hi - lo >= bytes) && (hi - lo < best))
            {
                best    = hi - lo;
                *pBegin = lo;
                *pEnd   = hi - 1;
            }
        }
    }
    portMemFree(pUsed);
    return best != NV_U64_MAX;
}

//
// Move an allocation to other vidmem pages without the detour through sysmem
// (BACKLOG #16: compaction, and room for contiguous requests without taking
// the memory out of vidmem). Channels of the owner disabled, no mapping of
// the compositor (the caller skips those). New pages outside [avoidLo,
// avoidHi) when that is given (the window being cleared), one CE copy from
// the old pages to the new ones through the FB alias, the mappings remade at
// their VAs on the new pages with their kinds, the old pages back to PMA.
//
static NV_STATUS
_vidmemRelocate
(
    OBJGPU              *pGpu,
    MEMORY_DESCRIPTOR   *pMemDesc,
    VIDMEM_EVICT_MAPREF *pRefs,
    NvU32                numRefs,
    NvU64                avoidLo,
    NvU64                avoidHi
)
{
    MemoryManager      *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    PMA_ALLOC_INFO     *pPmaInfo = pMemDesc->pPmaAllocInfo;
    PMA                *pPma = pMemDesc->pHeap->pPmaObject;
    PMA_ALLOCATION_OPTIONS allocOptions = {0};
    CEUTILS_MEMCOPY_PARAMS copyParams = {0};
    MEMORY_DESCRIPTOR  *pOld = NULL;
    NvU64              *pNewPages = NULL, *pOldPages = NULL;
    NvU32              *pKinds = NULL;
    NvU32               numPages = pPmaInfo->bContig ? 1 : pPmaInfo->pageCount;
    NvU64               maxMapPageSize = 0;
    NvU32               bar1Refs = 0, i, try;
    NvBool              bRemapFailed = NV_FALSE;
    NV_STATUS           status;
    VE_PHASE_BEGIN();

    pNewPages = portMemAllocNonPaged(sizeof(NvU64) * pPmaInfo->pageCount);
    pOldPages = portMemAllocNonPaged(sizeof(NvU64) * numPages);
    pKinds    = portMemAllocNonPaged(sizeof(NvU32) * NV_MAX(numRefs, 1));
    if ((pNewPages == NULL) || (pOldPages == NULL) || (pKinds == NULL))
    {
        status = NV_ERR_NO_MEMORY;
        goto done;
    }
    for (i = 0; i < numRefs; i++)
    {
        maxMapPageSize = NV_MAX(maxMapPageSize, pRefs[i].pDmaMappingInfo->mapPageSize);
        pKinds[i] = memdescGetPteKind(pRefs[i].pDmaMappingInfo->pMemDesc);
    }

    // New pages, as a restore takes them (pinned; see _vidmemRestoreBegin()).
    allocOptions.flags     = PMA_ALLOCATE_FORCE_ALIGNMENT | PMA_ALLOCATE_PINNED |
                             (pPmaInfo->flags & PMA_ALLOCATE_PERSISTENT);
    allocOptions.alignment = pPmaInfo->pageSize;
    if (pPmaInfo->bContig)
    {
        allocOptions.flags    |= PMA_ALLOCATE_CONTIGUOUS;
        allocOptions.alignment = NV_MAX(NV_MAX(pPmaInfo->pageSize, maxMapPageSize),
                                        NV_MAX(pMemDesc->Alignment, RM_PAGE_SIZE));
    }
    //
    // Where the new pages go: the smallest free run that holds them all,
    // outside the window being cleared and outside the free run compaction is
    // keeping. If no single run does (a scattered allocation bigger than
    // every hole), the gaps around those two, lowest first.
    //
    status = NV_ERR_NO_MEMORY;
    {
        NvU64 aLo = avoidLo, aHi = avoidHi, bLo = _vidmemRelocateKeepLo, bHi = _vidmemRelocateKeepHi;
        NvU64 gapLo[3], gapHi[3];
        PMA_ALLOCATION_OPTIONS opt = allocOptions;
        NvU64 resBegin = 0, resEnd = 0, regionBase = 0;

        // With a user reserve: never into its block (as a restore).
        if ((bHi == 0) && vidmemUserReserveBoundRangeAny(pPma, allocOptions.alignment, &resBegin, &resEnd) &&
            (resBegin != 0))
        {
            bLo = 0;
            bHi = resBegin;
        }
        if (_vidmemBestFitRange(pPma, (NvU64)pPmaInfo->pageCount * pPmaInfo->pageSize, allocOptions.alignment,
                                aLo, aHi, bLo, bHi, &opt.physBegin, &opt.physEnd, &regionBase))
        {
            opt.flags |= PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE;
            status = pmaAllocatePages(pPma, pPmaInfo->pageCount, pPmaInfo->pageSize, &opt, pNewPages);
        }
        if (aHi == 0)
        {
            aLo = bLo;
            aHi = bHi;
        }
        if (bHi == 0)
        {
            bLo = aLo;
            bHi = aHi;
        }
        if (bLo < aLo)
        {
            NvU64 t;

            t = aLo; aLo = bLo; bLo = t;
            t = aHi; aHi = bHi; bHi = t;
        }
        //
        // From the region's base, not 0: PMA clips a range to the region, and
        // the base is not 2 MB aligned (the regmap.c range assert on the host,
        // 2026-11-10 20:45, a 2 MB-page allocation of the game moved for a window).
        //
        gapLo[0] = regionBase;        gapHi[0] = (aHi != 0) ? aLo : NV_ALIGN_DOWN64(NV_U64_MAX, allocOptions.alignment);
        gapLo[1] = aHi;               gapHi[1] = bLo;
        gapLo[2] = NV_MAX(aHi, bHi);  gapHi[2] = NV_ALIGN_DOWN64(NV_U64_MAX, allocOptions.alignment);
        for (try = 0; (try < 3) && (status != NV_OK); try++)
        {
            opt = allocOptions;
            opt.flags    |= PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE;
            opt.physBegin = NV_ALIGN_UP64(gapLo[try], opt.alignment);
            opt.physEnd   = NV_ALIGN_DOWN64(gapHi[try], opt.alignment);
            if ((opt.physEnd <= opt.physBegin) || ((try != 0) && (aHi == 0)))
                continue;
            opt.physEnd -= 1;
            status = pmaAllocatePages(pPma, pPmaInfo->pageCount, pPmaInfo->pageSize, &opt, pNewPages);
        }
    }
    VE_PHASE_END(VE_PH_PMA);
    if (status != NV_OK)
        goto done;

    // The old pages: a snapshot memdesc to copy from, the list to free.
    status = memdescCreateSubMem(&pOld, pMemDesc, pGpu, 0, memdescGetSize(pMemDesc));
    if (status != NV_OK)
    {
        pmaFreePages(pPma, pNewPages, numPages, pPmaInfo->bContig ? pPmaInfo->allocSize : pPmaInfo->pageSize, 0);
        goto done;
    }
    portMemCopy(pOldPages, sizeof(NvU64) * numPages, pPmaInfo->pageArray, sizeof(NvU64) * numPages);
    _vidmemEvictDropStaticBar1(pGpu, pMemDesc, &bar1Refs);

    // The allocation is the new pages from here on.
    if (pPmaInfo->bContig)
    {
        NV_ASSERT(pMemDesc->PteAdjust == 0);
        pPmaInfo->pageArray[0] = pNewPages[0];
        pMemDesc->_pteArray[0] = pNewPages[0];
    }
    else
    {
        portMemCopy(pPmaInfo->pageArray, sizeof(NvU64) * numPages, pNewPages, sizeof(NvU64) * numPages);
        memdescFillPages(pMemDesc, 0, pPmaInfo->pageArray, pPmaInfo->pageCount, pPmaInfo->pageSize);
    }
    for (i = 0; i < bar1Refs; i++)
    {
        status = kbusIncreaseStaticBar1Refcount_HAL(pGpu, GPU_GET_KERNEL_BUS(pGpu), pMemDesc,
                                                    BUS_MAP_FB_FLAGS_NONE);
        if ((status != NV_OK) && (status != NV_ERR_NOT_SUPPORTED))
            NV_PRINTF(LEVEL_ERROR, "vidmem evict: static BAR1 reference on move: 0x%x\n", status);
    }
    VE_PHASE_END(VE_PH_PMA);

    copyParams.pSrcMemDesc = pOld;
    copyParams.pDstMemDesc = pMemDesc;
    copyParams.length      = memdescGetSize(pMemDesc);
    status = ceutilsMemcopy(pMemoryManager->pCeUtils, &copyParams);
    VE_PHASE_END(VE_PH_COPY);
    if (status != NV_OK)
    {
        // Nothing points at the new pages yet: back to the old ones.
        NV_PRINTF(LEVEL_ERROR, "vidmem evict: move copy failed: 0x%x\n", status);
        _vidmemEvictDropStaticBar1(pGpu, pMemDesc, NULL);
        _vidmemEvictFreePma(pMemDesc);
        if (pPmaInfo->bContig)
        {
            pPmaInfo->pageArray[0] = pOldPages[0];
            pMemDesc->_pteArray[0] = pOldPages[0];
        }
        else
        {
            portMemCopy(pPmaInfo->pageArray, sizeof(NvU64) * numPages, pOldPages, sizeof(NvU64) * numPages);
            memdescFillPages(pMemDesc, 0, pPmaInfo->pageArray, pPmaInfo->pageCount, pPmaInfo->pageSize);
        }
        for (i = 0; i < bar1Refs; i++)
            (void)kbusIncreaseStaticBar1Refcount_HAL(pGpu, GPU_GET_KERNEL_BUS(pGpu), pMemDesc, BUS_MAP_FB_FLAGS_NONE);
        goto done;
    }

    //
    // Old and new pages hold the same data now (the channels are stopped), so
    // a mapping may point at either until all are remade. One that could not
    // be remade keeps the old pages: then they are not given back (a leak,
    // not a mapping of somebody else's memory).
    //
    for (i = 0; i < numRefs; i++)
    {
        NV_STATUS st = _vidmemEvictRemapOne(pGpu, &pRefs[i], pMemDesc, pKinds[i]);

        if (st != NV_OK)
        {
            bRemapFailed = NV_TRUE;
            status = st;
        }
    }
    VE_PHASE_END(VE_PH_REMAP);
    if (bRemapFailed)
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: move: a mapping was not remade (0x%x), %llu KB of old pages are not freed\n",
                  status, memdescGetSize(pMemDesc) >> 10);
    else
        pmaFreePages(pPma, pOldPages, numPages, pPmaInfo->bContig ? pPmaInfo->allocSize : pPmaInfo->pageSize, 0);
    VE_PHASE_END(VE_PH_PMA);

done:
    if (pOld != NULL)
        memdescDestroy(pOld);
    portMemFree(pNewPages);
    portMemFree(pOldPages);
    portMemFree(pKinds);
    return status;
}

//
// memmgrFree() of a holder of an evicted allocation (the client freed it, or
// exited). Like vidmemPmaFree(): one PMA reference per holder; with the last
// one the sysmem copy goes instead of the (long gone) PMA pages. GPU lock held.
//
void
vidmemEvictFreeEvicted(OBJGPU *pGpu, MEMORY_DESCRIPTOR *pMemDesc)
{
    VIDMEM_EVICT_STATE *pState = pMemDesc->pLocalEvictState;
    PMA_ALLOC_INFO     *pPmaInfo = pMemDesc->pPmaAllocInfo;

    if (pState == NULL)
        return;

    if ((pPmaInfo != NULL) && (pPmaInfo->refCount > 1))
    {
        pPmaInfo->refCount--;
        return;
    }

    _vidmemEvictBytes -= NV_MIN(_vidmemEvictBytes, pState->bytes);
    _vidmemEvictAllocs -= NV_MIN(_vidmemEvictAllocs, 1);
    _vidmemEvictListRemove(pState);
    pMemDesc->pLocalEvictState = NULL;
    _vidmemEvictFreeState(pGpu, pState);

    portMemFree(pPmaInfo);
    pMemDesc->pPmaAllocInfo = NULL;
}

// GPU mappings of an evicted allocation map its sysmem copy (memGetMemInterMapParams).
MEMORY_DESCRIPTOR *
vidmemEvictGetSysMemDesc(MEMORY_DESCRIPTOR *pMemDesc)
{
    VIDMEM_EVICT_STATE *pState = pMemDesc->pLocalEvictState;
    return (pState != NULL) ? pState->pSysMemDesc : pMemDesc;
}

// A dup of an evicted allocation: its static BAR1 reference is taken on restore.
NV_STATUS
vidmemEvictNoteDupBar1(MEMORY_DESCRIPTOR *pMemDesc)
{
    VIDMEM_EVICT_STATE *pState = pMemDesc->pLocalEvictState;

    if (pState != NULL)
        pState->staticBar1Refs++;
    return NV_OK;
}


//
// memIsReady() hook: an evicted allocation is about to be used by RM (map,
// dup, control, lookup by handle): bring it back first. API lock held (read
// or write); the GPU lock is taken here if the caller does not hold it.
// Every client holding a mapping of it is paused for the whole restore.
//
NV_STATUS
vidmemEvictRestoreOnAccess(Memory *pMemory)
{
    OBJGPU                *pGpu = pMemory->pGpu;
    MEMORY_DESCRIPTOR     *pMemDesc = pMemory->pMemDesc;
    VIDMEM_EVICT_CHANNELS *pChannels = NULL;
    VIDMEM_EVICT_MAPREF   *pRefs = NULL;
    NvU32                  numRefs = 0, numHolders = 0, i;
    NvBool                 bLocked = NV_FALSE;
    NV_STATUS              status = NV_OK;
    NvU64                  startNs = VMT_NOW();

    if (_vidmemEvictIsBusyThread() || (pGpu == NULL))
        return NV_OK;

    if (!rmGpuLockIsOwner())
    {
        status = rmGpuLocksAcquire(GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_MEM);
        if (status != NV_OK)
            return status;
        bLocked = NV_TRUE;
    }

    // Someone else may have restored it while we waited for the lock.
    if (pMemDesc->pLocalEvictState == NULL)
        goto done;

    pChannels = portMemAllocNonPaged(sizeof(*pChannels));
    pRefs = portMemAllocNonPaged(sizeof(*pRefs) * VIDMEM_EVICT_MAX_MAPPINGS);
    if ((pChannels == NULL) || (pRefs == NULL))
    {
        status = NV_ERR_NO_MEMORY;
        goto done;
    }
    pChannels->count = 0;
    pChannels->vmtOffNs = 0;

    if (_vidmemEvictCollectHolders(pGpu, pMemDesc, 0, RES_GET_REF(pMemory), NV_FALSE,
                                   pRefs, &numRefs, &numHolders) != NULL)
    {
        status = NV_ERR_INVALID_STATE;
        goto done;
    }
    _vidmemEvictCollectChannels(RES_GET_CLIENT(pMemory), pChannels);
    for (i = 0; i < numRefs; i++)
        _vidmemEvictCollectChannels(RES_GET_CLIENT(pRefs[i].pHolder), pChannels);
    {
        RmClient *pOwner = dynamicCast(RES_GET_CLIENT(pMemory), RmClient);

        pChannels->vmtPid = (pOwner != NULL) ? pOwner->ProcID : 0;
    }
    portMemSet(_vidmemEvictPhaseNs, 0, sizeof(_vidmemEvictPhaseNs));

    _vidmemEvictSetBusy(NV_TRUE);
    status = _vidmemEvictSetChannels(pGpu, pChannels, NV_TRUE);
    if (status == NV_OK)
    {
        status = _vidmemRestoreBegin(pGpu, pMemDesc, pRefs, numRefs, NV_TRUE);
        if (status == NV_OK)
            status = _vidmemRestoreCommit(pGpu, pMemDesc, pRefs, numRefs);
        (void)_vidmemEvictSetChannels(pGpu, pChannels, NV_FALSE);
    }
    _vidmemEvictSetBusy(NV_FALSE);

    {
        RmClient *pRmClient = dynamicCast(RES_GET_CLIENT(pMemory), RmClient);
        static NvU32 failures;
        NvU32 n = (status == NV_OK) ? 0 : ++failures;

        if (pRmClient != NULL)
            _vidmemEvictNoteRestoreOnAccess(pRmClient->ProcID);
        if (osVidmemTraceOn(VMT_MOVE))
        {
            // The calling thread (CPU map, scanout, ...) waited all of it.
            NvU64 a[VMT_ARGS] = { 2, VMT_WHY_ACCESS, (status == NV_OK) ? memdescGetSize(pMemDesc) : 0,
                                  status == NV_OK, status != NV_OK, numHolders, pChannels->count,
                                  VMT_NOW() - startNs,
                                  _vidmemEvictPhaseNs[VE_PH_QUIESCE], _vidmemEvictPhaseNs[VE_PH_SYSALLOC],
                                  _vidmemEvictPhaseNs[VE_PH_COPY], _vidmemEvictPhaseNs[VE_PH_REMAP],
                                  _vidmemEvictPhaseNs[VE_PH_PMA], _vidmemEvictPhaseNs[VE_PH_SYSFREE] };
            osVidmemTrace(VMT_MOVE, (pRmClient != NULL) ? pRmClient->ProcID : 0, a);
        }
        if ((status == NV_OK) || ((n & (n - 1)) == 0))
            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: restore on access, %s(%u) memory 0x%x (%llu KB, %u holders): 0x%x%s\n",
                      (pRmClient != NULL) ? pRmClient->name : "?", (pRmClient != NULL) ? pRmClient->ProcID : 0,
                      RES_GET_HANDLE(pMemory), memdescGetSize(pMemDesc) >> 10, numHolders, status,
                      (status != NV_OK) ? " (failures logged at powers of two)" : "");
    }

done:
    portMemFree(pChannels);
    portMemFree(pRefs);
    if (bLocked)
        rmGpuLocksRelease(GPUS_LOCK_FLAGS_NONE, NULL);
    return (status == NV_OK) ? NV_OK : NV_ERR_NO_MEMORY;
}

//
// Does a PMA-backed allocation have a page in [lo, hi)? With pBits (a frame
// map from `base`, `frames` frames of PMA_PAGE_SHIFT) it marks its frames
// there instead and returns NV_TRUE if it marked any.
//
static NvBool
_vidmemMemDescFrames(MEMORY_DESCRIPTOR *pMemDesc, NvU64 lo, NvU64 hi, NvU64 *pBits, NvU64 base, NvU64 frames)
{
    PMA_ALLOC_INFO *pInfo = pMemDesc->pPmaAllocInfo;
    NvU64 pageBytes, n, i, f;
    NvBool bAny = NV_FALSE;

    if ((pInfo == NULL) || (memdescGetAddressSpace(pMemDesc) != ADDR_FBMEM))
        return NV_FALSE;
    n         = pInfo->bContig ? 1 : pInfo->pageCount;
    pageBytes = pInfo->bContig ? pInfo->allocSize : pInfo->pageSize;
    for (i = 0; i < n; i++)
    {
        NvU64 a = pInfo->pageArray[i];

        if (pBits == NULL)
        {
            if ((a < hi) && (a + pageBytes > lo))
                return NV_TRUE;
            continue;
        }
        if (a < base)
            continue;
        for (f = (a - base) >> PMA_PAGE_SHIFT; (f < frames) && (f < ((a - base + pageBytes) >> PMA_PAGE_SHIFT)); f++)
        {
            pBits[f >> 6] |= NVBIT64(f & 63);
            bAny = NV_TRUE;
        }
    }
    return bAny;
}

static NvBool
_vidmemMemDescInRange(MEMORY_DESCRIPTOR *pMemDesc, NvU64 lo, NvU64 hi)
{
    return _vidmemMemDescFrames(pMemDesc, lo, hi, NULL, 0, 0);
}

// A buffer waiting for phase 2 (the compositor maps it).
typedef struct
{
    MEMORY_DESCRIPTOR   *pMemDesc;
    VIDMEM_EVICT_STATE  *pState;      // evict only
    VIDMEM_EVICT_MAPREF *pRefs;
    NvU32                numRefs;
    NvHandle             hMemory;
} VIDMEM_EVICT_PENDING;

#define VIDMEM_EVICT_MAX_PENDING 256

NvU64 *vidmemPmaUsedMap(PMA *pPma, NvU64 *pBase, NvU64 *pFrames, NvU64 *pLargestFree); // video_mem.c
extern NvU64 g_ceutilsCopyLaunches, g_ceutilsCopySubmitNs, g_ceutilsCopyWaitNs;             // ce_utils.c

//
// How fragmented free vidmem is (BACKLOG #16): the PMA region user
// allocations come from, in 2 MB chunks (physically aligned). A chunk is free
// when all its frames are; a run is free chunks in a row, what a contiguous
// or 2 MB-page allocation can use. Free frames in chunks partly in use are
// holes: only 64 KB-page allocations fit there.
//
typedef struct
{
    NvU64 totalBytes;
    NvU64 freeBytes;
    NvU64 chunkBytes;        // free in whole chunks
    NvU64 longestBytes;      // longest run of free chunks
    NvU64 partialUsedBytes;  // in use in partly used chunks
    NvU32 runs, runs32, runs128;
    NvU32 partial;           // partly used chunks
} VIDMEM_FRAG;

// Frames in use among [f, f + n) of a frame map (bit set: in use).
static NvU32
_vidmemFramesUsed(const NvU64 *pMap, NvU64 f, NvU64 n)
{
    NvU32 used = 0;

    while (n != 0)
    {
        NvU64 s = f & 63;
        NvU64 k = NV_MIN(n, 64 - s);
        NvU64 m = (k == 64) ? ~0ULL : (((1ULL << k) - 1) << s);

        used += nvPopCount64(pMap[f >> 6] & m);
        f += k;
        n -= k;
    }
    return used;
}

static void
_vidmemFragEndRun(VIDMEM_FRAG *pF, NvU64 run)
{
    NvU64 bytes = run << 21;

    if (run == 0)
        return;
    pF->runs++;
    if (bytes >= (32ULL << 20))
        pF->runs32++;
    if (bytes >= (128ULL << 20))
        pF->runs128++;
    pF->longestBytes = NV_MAX(pF->longestBytes, bytes);
}

static NvBool
_vidmemFragStats(OBJGPU *pGpu, VIDMEM_FRAG *pF)
{
    Heap  *pHeap = GPU_GET_HEAP(pGpu);
    NvU64  chunkFrames = (2ULL << 20) >> PMA_PAGE_SHIFT;
    NvU64  base = 0, frames = 0, largest = 0, words, w, off, f, run = 0;
    NvU64 *pUsed;

    portMemSet(pF, 0, sizeof(*pF));
    if (pHeap == NULL)
        return NV_FALSE;
    pUsed = vidmemPmaUsedMap(pHeap->pPmaObject, &base, &frames, &largest);
    if (pUsed == NULL)
        return NV_FALSE;

    // Frames past the end are marked in use by vidmemPmaUsedMap().
    words = (frames + 63) >> 6;
    pF->totalBytes = frames << PMA_PAGE_SHIFT;
    for (w = 0; w < words; w++)
        pF->freeBytes += (NvU64)(64 - nvPopCount64(pUsed[w])) << PMA_PAGE_SHIFT;

    off = (NV_ALIGN_UP64(base, 2ULL << 20) - base) >> PMA_PAGE_SHIFT;
    for (f = off; f + chunkFrames <= frames; f += chunkFrames)
    {
        NvU32 used = _vidmemFramesUsed(pUsed, f, chunkFrames);

        if (used == 0)
        {
            pF->chunkBytes += 2ULL << 20;
            run++;
            continue;
        }
        _vidmemFragEndRun(pF, run);
        run = 0;
        if (used < chunkFrames)
        {
            pF->partial++;
            pF->partialUsedBytes += (NvU64)used << PMA_PAGE_SHIFT;
        }
    }
    _vidmemFragEndRun(pF, run);
    portMemFree(pUsed);
    return NV_TRUE;
}

//
// VMT_FRAG: at 0 (a policy step), 1/2 (after an evict/restore batch of pid,
// pBefore taken before it), why as in VMT_MOVE.
//
static void
_vidmemFragTrace(OBJGPU *pGpu, NvU32 at, NvU32 why, NvU32 pid, const VIDMEM_FRAG *pBefore)
{
    VIDMEM_FRAG fr;
    NvU64       t0 = VMT_NOW();

    if (!_vidmemFragStats(pGpu, &fr))
        return;
    {
        NvU64 a[VMT_ARGS] = { at, why, fr.totalBytes, fr.freeBytes, fr.chunkBytes, fr.longestBytes,
                              fr.runs, fr.runs32, fr.runs128, fr.partial, fr.partialUsedBytes,
                              (pBefore != NULL) ? pBefore->longestBytes : 0,
                              (pBefore != NULL) ? pBefore->chunkBytes : 0,
                              VMT_NOW() - t0 };
        osVidmemTrace(VMT_FRAG, pid, a);
    }
}

//
// Evict / restore / dry-run every eligible allocation of process `pid`
// (its own and those it shares only with the compositor).
// API lock held for writing, GPU lock held.
//   mode 0: dry run (list verdicts), 1: evict, 2: restore
// *pMovedBytes (optional) gets the bytes evicted/restored.
//
static NV_STATUS
_vidmemEvictPidLocked
(
    OBJGPU *pGpu,
    NvU32   mode,
    NvU32   pid,
    char   *buf,
    NvU32   bufSize,
    NvU32  *pLen,
    NvU64  *pMovedBytes
)
{
    VIDMEM_EVICT_CHANNELS *pChannels = NULL;
    VIDMEM_EVICT_CHANNELS *pCompChannels = NULL;
    VIDMEM_EVICT_MAPREF   *pRefs = NULL;
    VIDMEM_EVICT_PENDING  *pPending = NULL;
    RmClient             **ppClient;
    NvU64                  startNs = VMT_NOW();
    NvU64                  movedBytes = 0, failedBytes = 0, skippedBytes = 0, sharedBytes = 0;
    NvU32                  moved = 0, failed = 0, skipped = 0, shared = 0, numPending = 0, i;
    NV_STATUS              status;
    VIDMEM_FRAG            fragBefore;
    NvBool                 bFrag = NV_FALSE;
    NvU64                  ceLaunches0 = g_ceutilsCopyLaunches, ceSubmit0 = g_ceutilsCopySubmitNs;
    NvU64                  ceWait0 = g_ceutilsCopyWaitNs;

    //
    // The device lock is enough on this single-GPU path: vidmemConstruct()
    // holds only it when vidmemEvictMakeRoom() moves other processes out.
    //
    NV_ASSERT_OR_RETURN(rmapiLockIsWriteOwner() &&
                        (rmGpuLockIsOwner() || rmDeviceGpuLockIsOwner(pGpu->gpuInstance)),
                        NV_ERR_INVALID_LOCK_STATE);

    pChannels = portMemAllocNonPaged(sizeof(*pChannels));
    pCompChannels = portMemAllocNonPaged(sizeof(*pCompChannels));
    pRefs = portMemAllocNonPaged(sizeof(*pRefs) * VIDMEM_EVICT_MAX_MAPPINGS);
    pPending = portMemAllocNonPaged(sizeof(*pPending) * VIDMEM_EVICT_MAX_PENDING);
    if ((pChannels == NULL) || (pCompChannels == NULL) || (pRefs == NULL) || (pPending == NULL))
    {
        status = NV_ERR_NO_MEMORY;
        goto free_mem;
    }
    pChannels->count = 0;
    pCompChannels->count = 0;
    pChannels->vmtOffNs = 0;
    pCompChannels->vmtOffNs = 0;
    portMemSet(_vidmemEvictPhaseNs, 0, sizeof(_vidmemEvictPhaseNs));
    // Fragmentation before the batch (VMT_FRAG after it, BACKLOG #16).
    bFrag = (mode != 0) && osVidmemTraceOn(VMT_FRAG) && _vidmemFragStats(pGpu, &fragBefore);

    EVICT_PRINTF("%s pid %u\n", (mode == 0) ? "dry" : VE_MODE_NAME(mode), pid);
    _vidmemEvictCurPid = pid;
    pChannels->vmtPid = pid;

    for (ppClient = serverutilGetFirstClientUnderLock();
         ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RsClient *pRsClient = staticCast(*ppClient, RsClient);

        if ((pRsClient->type == CLIENT_TYPE_KERNEL) || ((*ppClient)->ProcID != pid))
            continue;
        _vidmemEvictCollectChannels(pRsClient, pChannels);
    }
    EVICT_PRINTF("channels %u, CE virtual mode %u, static BAR1 %u\n", pChannels->count,
                 _vidmemEvictCeIsVirtual(GPU_GET_MEMORY_MANAGER(pGpu)),
                 kbusIsStaticBar1Enabled(pGpu, GPU_GET_KERNEL_BUS(pGpu)));

    if (mode != 0)
    {
        _vidmemEvictSetBusy(NV_TRUE);
        _vidmemEvictBatchBegin();
        status = _vidmemEvictSetChannels(pGpu, pChannels, NV_TRUE);
        if (status != NV_OK)
        {
            EVICT_PRINTF("disabling channels failed: 0x%x\n", status);
            _vidmemEvictBatchEnd(pGpu);
            _vidmemEvictSetBusy(NV_FALSE);
            goto free_mem;
        }
        if (pChannels->count != 0)
        {
            VIDMEM_EVICT_QUIESCE_STATS *q = &_vidmemEvictLastStop;
            EVICT_PRINTF("quiesce: mode %u, %u channels, %u TSGs (%u implicit), preempt ok %u refused %u (0x%x), "
                         "idle 0x%x in %llu us, disable %llu us, total %llu us\n",
                         q->mode, q->channels, q->tsgs, q->implicitTsgs, q->preemptOk, q->preemptRefused,
                         q->firstRefusal, q->idleStatus, q->idleNs / 1000, q->disableNs / 1000, q->totalNs / 1000);
        }
    }

    // Phase 1, only the process itself paused.
    for (ppClient = serverutilGetFirstClientUnderLock();
         ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RsClient   *pRsClient = staticCast(*ppClient, RsClient);
        RS_ITERATOR iter;

        if ((pRsClient->type == CLIENT_TYPE_KERNEL) || ((*ppClient)->ProcID != pid))
            continue;

        iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0,
                                 RS_ITERATE_DESCENDANTS, NV_TRUE);
        while (clientRefIterNext(iter.pClient, &iter))
        {
            RsResourceRef     *pRef = iter.pResourceRef;
            Memory            *pMemory = dynamicCast(pRef->pResource, Memory);
            MEMORY_DESCRIPTOR *pMemDesc;
            VIDMEM_EVICT_STATE *pState = NULL;
            const char        *reason;
            NvU32              numRefs = 0, numHolders = 0;
            NvU64              bytes;
            NvBool             bPending = NV_FALSE;

            if ((pMemory == NULL) || (pMemory->pMemDesc == NULL) ||
                (pRef->externalClassId != NV01_MEMORY_LOCAL_USER))
                continue;

            pMemDesc = pMemory->pMemDesc;
            bytes = memdescGetSize(pMemDesc);

            // One allocation only (a hot one coming back, _vidmemHotRestore()).
            if ((_vidmemEvictOnlyMemDesc != NULL) && (pMemDesc != _vidmemEvictOnlyMemDesc))
                continue;
            // Only what lies in a window being cleared for the compositor.
            if ((_vidmemEvictRangeHi != 0) &&
                !_vidmemMemDescInRange(pMemDesc, _vidmemEvictRangeLo, _vidmemEvictRangeHi))
                continue;

            if (mode == 2)
            {
                if (pMemDesc->pLocalEvictState == NULL)
                    continue;
                // A second holder in this process of a buffer already in phase 2.
                for (i = 0; i < numPending; i++)
                {
                    if (pPending[i].pMemDesc == pMemDesc)
                        break;
                }
                if (i < numPending)
                    continue;

                //
                // Partial restore (the foreground's memory when not all of it
                // fits, BACKLOG #10): no more than the budget, counting
                // deferred ones; an allocation that does not fit in what is
                // left is skipped, a smaller one after it may still fit.
                //
                if (_vidmemEvictBudgetBytes != 0)
                {
                    NvU64 sofar = movedBytes + bytes;

                    for (i = 0; i < numPending; i++)
                        sofar += memdescGetSize(pPending[i].pMemDesc);
                    if (sofar > _vidmemEvictBudgetBytes)
                        continue;
                }

                if (_vidmemEvictCollectHolders(pGpu, pMemDesc, pid, pRef, NV_FALSE,
                                               pRefs, &numRefs, &numHolders) != NULL)
                {
                    status = NV_ERR_INVALID_STATE;
                }
                else
                {
                    status = _vidmemRestoreBegin(pGpu, pMemDesc, pRefs, numRefs, NV_FALSE);
                    if ((status == NV_OK) && _vidmemEvictHasCompositor(pRefs, numRefs))
                        bPending = NV_TRUE;
                    else if (status == NV_OK)
                        status = _vidmemRestoreCommit(pGpu, pMemDesc, pRefs, numRefs);
                }
            }
            else
            {
                reason = _vidmemEvictCheck(pGpu, pRef, pid, pRefs, &numRefs);
                if (reason != NULL)
                {
                    // Not counted: other holders of buffers handled elsewhere.
                    if ((portStringCompare(reason, "secondary-holder", 17) == 0) ||
                        (portStringCompare(reason, "evicted", 8) == 0))
                        continue;
                    skipped++;
                    skippedBytes += bytes;
                    if (mode == 0)
                        EVICT_PRINTF("0x%x %llu KB kind 0x%x maps %u ref %u child %u dup %u pma %u: %s\n",
                                     RES_GET_HANDLE(pMemory), bytes >> 10, memdescGetPteKind(pMemDesc),
                                     listCount(&pRef->interBackRefsMappable), pMemDesc->RefCount,
                                     pMemDesc->childDescriptorCnt, pMemDesc->DupCount,
                                     (pMemDesc->pPmaAllocInfo != NULL) ? pMemDesc->pPmaAllocInfo->refCount : 0,
                                     reason);
                    continue;
                }

                if (pMemDesc->DupCount > 1)
                {
                    shared++;
                    sharedBytes += bytes;
                }

                if (mode == 0)
                {
                    moved++;
                    movedBytes += bytes;
                    EVICT_PRINTF("0x%x %llu KB kind 0x%x maps %u dup %u page %llu KB%s%s: ok\n",
                                 RES_GET_HANDLE(pMemory), bytes >> 10, memdescGetPteKind(pMemDesc),
                                 numRefs, pMemDesc->DupCount,
                                 pMemDesc->pPmaAllocInfo->pageSize >> 10,
                                 pMemDesc->pPmaAllocInfo->bContig ? " contig" : "",
                                 _vidmemEvictHasCompositor(pRefs, numRefs) ? " compositor" : "");
                    continue;
                }

                // Partial eviction: allocations that were hot lately go last.
                if ((_vidmemEvictBudgetBytes != 0) && _vidmemHotIsRecent(pMemDesc))
                    continue;

                //
                // Making 2 MB chunks free (the compositor, a placement): only
                // allocations on 2 MB pages free whole chunks; one on 64 KB
                // pages may leave every chunk it touched still in use
                // (BACKLOG #12: 284 MB of the game moved for kwin's 32 MB).
                //
                if (_vidmemEvictBigPagesOnly &&
                    ((pMemDesc->pPmaAllocInfo == NULL) || (pMemDesc->pPmaAllocInfo->pageSize < (2ULL << 20))))
                    continue;

                // Partial eviction: enough moved (counting deferred ones).
                if (_vidmemEvictBudgetBytes != 0)
                {
                    NvU64 sofar = movedBytes;

                    for (i = 0; i < numPending; i++)
                        sofar += memdescGetSize(pPending[i].pMemDesc);
                    if (sofar >= _vidmemEvictBudgetBytes)
                        continue;
                }

                if (mode == 3)
                {
                    // What the compositor maps too stays where it is (its channels are not stopped here).
                    if (_vidmemEvictHasCompositor(pRefs, numRefs) ||
                        ((_vidmemRelocateMaxBytes != 0) && (bytes > _vidmemRelocateMaxBytes)))
                    {
                        skipped++;
                        skippedBytes += bytes;
                        continue;
                    }
                    status = _vidmemRelocate(pGpu, pMemDesc, pRefs, numRefs,
                                             _vidmemEvictRangeLo, _vidmemEvictRangeHi);
                }
                else
                {
                    status = _vidmemEvictBegin(pGpu, pMemDesc, pRefs, numRefs, &pState);
                    if ((status == NV_OK) && _vidmemEvictHasCompositor(pRefs, numRefs))
                        bPending = NV_TRUE;
                    else if (status == NV_OK)
                        status = _vidmemEvictCommit(pGpu, pMemDesc, pState, pRefs, numRefs);
                }
            }

            if (bPending)
            {
                VIDMEM_EVICT_MAPREF *pCopy = (numPending < VIDMEM_EVICT_MAX_PENDING) ?
                    portMemAllocNonPaged(sizeof(*pRefs) * numRefs) : NULL;

                if (pCopy != NULL)
                {
                    portMemCopy(pCopy, sizeof(*pRefs) * numRefs, pRefs, sizeof(*pRefs) * numRefs);
                    pPending[numPending].pMemDesc = pMemDesc;
                    pPending[numPending].pState   = pState;
                    pPending[numPending].pRefs    = pCopy;
                    pPending[numPending].numRefs  = numRefs;
                    pPending[numPending].hMemory  = RES_GET_HANDLE(pMemory);
                    numPending++;
                    continue;
                }
                // No room to defer: undo phase 1.
                if (mode == 1)
                    _vidmemEvictAbort(pGpu, pMemDesc, pState, pRefs, numRefs);
                else
                    _vidmemRestoreAbort(pGpu, pMemDesc, pRefs, numRefs);
                status = NV_ERR_NO_MEMORY;
            }

            if (status == NV_OK)
            {
                moved++;
                movedBytes += bytes;
            }
            else
            {
                failed++;
                failedBytes += bytes;
                EVICT_PRINTF("0x%x %llu KB %s failed 0x%x\n", RES_GET_HANDLE(pMemory), bytes >> 10,
                             VE_MODE_NAME(mode), status);
                if (mode == 3)
                {
                    static NvU32 moveFailLogged;

                    // Why a move inside vidmem did not happen (BACKLOG #15: "failed 1" said nothing).
                    if ((moveFailLogged++ < 8) || ((moveFailLogged % 64) == 0))
                        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: move of %llu KB%s of pid %u inside vidmem failed: 0x%x\n",
                                  bytes >> 10,
                                  ((pMemDesc->pPmaAllocInfo != NULL) && pMemDesc->pPmaAllocInfo->bContig) ?
                                  " (contiguous)" : "", pid, status);
                }
            }
        }
    }

    // Phase 2: the compositor paused only for remapping its views of these buffers.
    if (numPending != 0)
    {
        _vidmemEvictCollectCompositorChannels(pCompChannels);
        status = _vidmemEvictSetChannels(pGpu, pCompChannels, NV_TRUE);
        if (status != NV_OK)
            EVICT_PRINTF("disabling compositor channels failed: 0x%x\n", status);

        for (i = 0; i < numPending; i++)
        {
            VIDMEM_EVICT_PENDING *pP = &pPending[i];
            NvU64 bytes = memdescGetSize(pP->pMemDesc);
            NV_STATUS st;

            if (status != NV_OK)
            {
                if (mode == 1)
                    _vidmemEvictAbort(pGpu, pP->pMemDesc, pP->pState, pP->pRefs, pP->numRefs);
                else
                    _vidmemRestoreAbort(pGpu, pP->pMemDesc, pP->pRefs, pP->numRefs);
                st = status;
            }
            else if (mode == 1)
            {
                st = _vidmemEvictCommit(pGpu, pP->pMemDesc, pP->pState, pP->pRefs, pP->numRefs);
            }
            else
            {
                st = _vidmemRestoreCommit(pGpu, pP->pMemDesc, pP->pRefs, pP->numRefs);
            }

            if (st == NV_OK)
            {
                moved++;
                movedBytes += bytes;
            }
            else
            {
                failed++;
                failedBytes += bytes;
                EVICT_PRINTF("0x%x %llu KB %s (shared with compositor) failed 0x%x\n", pP->hMemory,
                             bytes >> 10, VE_MODE_NAME(mode), st);
            }
            portMemFree(pP->pRefs);
        }

        if (status == NV_OK)
            (void)_vidmemEvictSetChannels(pGpu, pCompChannels, NV_FALSE);
    }

    if (mode != 0)
    {
        (void)_vidmemEvictSetChannels(pGpu, pChannels, NV_FALSE);
        // Frees the restored allocations' sysmem copies (one L2 evict).
        {
            VE_PHASE_BEGIN();
            _vidmemEvictBatchEnd(pGpu);
            VE_PHASE_END(VE_PH_SYSFREE);
        }
        _vidmemEvictSetBusy(NV_FALSE);
    }
    status = NV_OK;

    EVICT_PRINTF("summary: %s %u allocs %llu MB (shared %u, %llu MB), failed %u (%llu MB), not eligible %u (%llu MB), %llu ms\n",
                 (mode == 0) ? "eligible" : ((mode == 1) ? "evicted" : ((mode == 2) ? "restored" : "moved")),
                 moved, movedBytes >> 20, shared, sharedBytes >> 20, failed, failedBytes >> 20,
                 skipped, skippedBytes >> 20, (VMT_NOW() - startNs) / 1000000);
    EVICT_PRINTF("phases ms: quiesce %llu, sysmem alloc %llu, copy %llu, remap %llu, pma %llu, sysmem free %llu\n",
                 _vidmemEvictPhaseNs[VE_PH_QUIESCE] / 1000000, _vidmemEvictPhaseNs[VE_PH_SYSALLOC] / 1000000,
                 _vidmemEvictPhaseNs[VE_PH_COPY] / 1000000, _vidmemEvictPhaseNs[VE_PH_REMAP] / 1000000,
                 _vidmemEvictPhaseNs[VE_PH_PMA] / 1000000, _vidmemEvictPhaseNs[VE_PH_SYSFREE] / 1000000);
    EVICT_PRINTF("copy: %llu launches, %llu ms submitting, %llu ms waiting for the last one\n",
                 g_ceutilsCopyLaunches - ceLaunches0, (g_ceutilsCopySubmitNs - ceSubmit0) / 1000000,
                 (g_ceutilsCopyWaitNs - ceWait0) / 1000000);
    EVICT_PRINTF("total evicted now: %u allocs %llu MB\n", _vidmemEvictAllocs, _vidmemEvictBytes >> 20);

    if ((mode != 0) && ((moved != 0) || (failed != 0)) && osVidmemTraceOn(VMT_MOVE))
    {
        NvU64 a[VMT_ARGS] = { mode, _vidmemEvictWhy, movedBytes, moved, failed, numPending, pChannels->count,
                              VMT_NOW() - startNs,
                              _vidmemEvictPhaseNs[VE_PH_QUIESCE], _vidmemEvictPhaseNs[VE_PH_SYSALLOC],
                              _vidmemEvictPhaseNs[VE_PH_COPY], _vidmemEvictPhaseNs[VE_PH_REMAP],
                              _vidmemEvictPhaseNs[VE_PH_PMA], _vidmemEvictPhaseNs[VE_PH_SYSFREE] };
        osVidmemTrace(VMT_MOVE, pid, a);
    }
    if (bFrag && ((moved != 0) || (failed != 0)))
        _vidmemFragTrace(pGpu, mode, _vidmemEvictWhy, pid, &fragBefore);
    if ((mode != 0) && ((moved != 0) || (failed != 0)))
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: %s pid %u: %u allocs %llu MB (%u with the compositor), failed %u, %llu ms; total %llu MB\n",
                  VE_MODE_NAME(mode), pid, moved, movedBytes >> 20, numPending, failed,
                  (VMT_NOW() - startNs) / 1000000, _vidmemEvictBytes >> 20);
    if (pMovedBytes != NULL)
        *pMovedBytes = movedBytes;
    if ((mode == 1) && (moved != 0))
        _vidmemHotResetCounters();

free_mem:
    portMemFree(pChannels);
    portMemFree(pCompChannels);
    portMemFree(pRefs);
    portMemFree(pPending);
    return status;
}


// ---------------------------------------------------------------------------
// Hot memory (2026-11-02, the user's idea: keep what is used in vidmem, the
// rest may go). The GPU's access counters (class ACCESS_COUNTER_NOTIFY_BUFFER,
// the MIMC counters UVM uses for migrations) count GPU accesses to remote
// memory, sysmem included, per 2 MB of virtual address space and notify once
// a region reaches the threshold: entry = instance block of the channel (so
// the process and its VA space) + virtual address + count. Evicted
// allocations live in sysmem, so a notification means "this evicted
// allocation is in use": it comes back to vidmem on its own (only it, not the
// whole process), and it goes last the next time memory is taken from that
// process (cold first).
//
// One notification buffer per GPU; nvidia-uvm takes it when a CUDA/managed
// memory process registers the GPU. Then this stays off (status says so).
// Interrupts stay disabled; the buffer is polled every policy step.
// Knob "hot 0|1" (default 0 until verified on the bench).
// ---------------------------------------------------------------------------

#define VIDMEM_HOT_THRESHOLD     256
#define VIDMEM_HOT_RECENT_SEC    60      // "hot" mark lifetime for cold-first ordering
#define VIDMEM_HOT_RING          256

typedef struct
{
    NvBool          bInit, bFailed;
    NV_STATUS       failStatus;
    NvHandle        hClient, hDevice, hSubdevice, hBuffer;
    NvU8           *pBuf;
    NvU32           bufSize, entries;
    volatile NvU32 *pGet, *pPut, *pIntrEnClear;
    NvU32           intrMask;
    NvU64           notifs, sysNotifs, matched, restored;
    // Allocations that were hot lately (restored because of it): taken last.
    MEMORY_DESCRIPTOR *recent[VIDMEM_HOT_RING];
    NvU64              recentNs[VIDMEM_HOT_RING];
    NvU32              recentNext;
} VIDMEM_HOT;

static VIDMEM_HOT _vidmemHot;
static NvU32      _vidmemHotEnabled;

static void
_vidmemHotNoteRecent(MEMORY_DESCRIPTOR *pMemDesc)
{
    _vidmemHot.recent[_vidmemHot.recentNext] = pMemDesc;
    _vidmemHot.recentNs[_vidmemHot.recentNext] = osGetMonotonicTimeNs();
    _vidmemHot.recentNext = (_vidmemHot.recentNext + 1) % VIDMEM_HOT_RING;
}

// Was this allocation hot in the last VIDMEM_HOT_RECENT_SEC? (cold-first order)
static NvBool
_vidmemHotIsRecent(MEMORY_DESCRIPTOR *pMemDesc)
{
    NvU64 nowNs = osGetMonotonicTimeNs();
    NvU32 i;

    if (!_vidmemHotEnabled)
        return NV_FALSE;
    for (i = 0; i < VIDMEM_HOT_RING; i++)
    {
        if ((_vidmemHot.recent[i] == pMemDesc) &&
            (nowNs - _vidmemHot.recentNs[i] < VIDMEM_HOT_RECENT_SEC * 1000000000ULL))
            return NV_TRUE;
    }
    return NV_FALSE;
}

// Free everything (module unload, suspend, "hot 0"). Locks held.
void
vidmemEvictHotTeardown(OBJGPU *pGpu)
{
    RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);

    if (!_vidmemHot.bInit)
    {
        _vidmemHot.bFailed = NV_FALSE;
        return;
    }
    if (_vidmemHot.pBuf != NULL)
        (void)pRmApi->UnmapFromCpu(pRmApi, _vidmemHot.hClient, _vidmemHot.hSubdevice, _vidmemHot.hBuffer,
                                   _vidmemHot.pBuf, 0, osGetCurrentProcess());
    if (_vidmemHot.hBuffer != 0)
        (void)pRmApi->Free(pRmApi, _vidmemHot.hClient, _vidmemHot.hBuffer);
    rmapiutilFreeClientAndDeviceHandles(pRmApi, &_vidmemHot.hClient, &_vidmemHot.hDevice, &_vidmemHot.hSubdevice);
    portMemSet(&_vidmemHot, 0, sizeof(_vidmemHot));
}

static NV_STATUS
_vidmemHotInit(OBJGPU *pGpu)
{
    RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
    NV_ACCESS_COUNTER_NOTIFY_BUFFER_ALLOC_PARAMS                allocParams = {0};
    NVC365_CTRL_ACCESS_CNTR_BUFFER_GET_SIZE_PARAMS              sizeParams = {0};
    NVC365_CTRL_ACCESS_CNTR_BUFFER_GET_REGISTER_MAPPINGS_PARAMS regs;
    NVC365_CTRL_ACCESS_CNTR_SET_CONFIG_PARAMS                   cfg = {0};
    NVC365_CTRL_ACCESS_CNTR_BUFFER_ENABLE_PARAMS                en = {0};
    NvP64     pAddr = NvP64_NULL;
    NV_STATUS status;

    portMemSet(&regs, 0, sizeof(regs));
    _vidmemHot.bInit = NV_TRUE;

    status = rmapiutilAllocClientAndDeviceHandles(pRmApi, pGpu, &_vidmemHot.hClient,
                                                  &_vidmemHot.hDevice, &_vidmemHot.hSubdevice);
    if (status != NV_OK)
        goto fail;
    allocParams.accessCounterIndex = 0;
    status = pRmApi->Alloc(pRmApi, _vidmemHot.hClient, _vidmemHot.hSubdevice, &_vidmemHot.hBuffer,
                           ACCESS_COUNTER_NOTIFY_BUFFER, &allocParams, sizeof(allocParams));
    if (status != NV_OK)
        goto fail;     // e.g. NV_ERR_INVALID_STATE: nvidia-uvm owns it
    status = pRmApi->Control(pRmApi, _vidmemHot.hClient, _vidmemHot.hBuffer,
                             NVC365_CTRL_CMD_ACCESS_CNTR_BUFFER_GET_SIZE, &sizeParams, sizeof(sizeParams));
    if (status != NV_OK)
        goto fail;
    _vidmemHot.bufSize = sizeParams.accessCntrBufferSize;
    _vidmemHot.entries = _vidmemHot.bufSize / NVC365_NOTIFY_BUF_SIZE;
    status = pRmApi->MapToCpu(pRmApi, _vidmemHot.hClient, _vidmemHot.hSubdevice, _vidmemHot.hBuffer,
                              0, _vidmemHot.bufSize, &pAddr, 0);
    if (status != NV_OK)
        goto fail;
    _vidmemHot.pBuf = KERNEL_POINTER_FROM_NvP64(NvU8 *, pAddr);
    status = pRmApi->Control(pRmApi, _vidmemHot.hClient, _vidmemHot.hBuffer,
                             NVC365_CTRL_CMD_ACCESS_CNTR_BUFFER_GET_REGISTER_MAPPINGS, &regs, sizeof(regs));
    if (status != NV_OK)
        goto fail;
    _vidmemHot.pGet         = KERNEL_POINTER_FROM_NvP64(volatile NvU32 *, regs.pAccessCntrBufferGet);
    _vidmemHot.pPut         = KERNEL_POINTER_FROM_NvP64(volatile NvU32 *, regs.pAccessCntrBufferPut);
    _vidmemHot.pIntrEnClear = KERNEL_POINTER_FROM_NvP64(volatile NvU32 *, regs.pHubIntrEnClear);
    _vidmemHot.intrMask     = regs.accessCntrMask;

    cfg.mimcGranularity = NVC365_CTRL_ACCESS_COUNTER_GRANULARITY_2M;
    cfg.momcGranularity = NVC365_CTRL_ACCESS_COUNTER_GRANULARITY_64K;
    cfg.mimcLimit       = NVC365_CTRL_ACCESS_COUNTER_USE_LIMIT_FULL;
    cfg.momcLimit       = NVC365_CTRL_ACCESS_COUNTER_USE_LIMIT_NONE;
    cfg.threshold       = VIDMEM_HOT_THRESHOLD;
    cfg.cmd = NVC365_CTRL_ACCESS_COUNTER_SET_MIMC_GRANULARITY | NVC365_CTRL_ACCESS_COUNTER_SET_MOMC_GRANULARITY |
              NVC365_CTRL_ACCESS_COUNTER_SET_MIMC_LIMIT | NVC365_CTRL_ACCESS_COUNTER_SET_MOMC_LIMIT |
              NVC365_CTRL_ACCESS_COUNTER_SET_THRESHOLD;
    status = pRmApi->Control(pRmApi, _vidmemHot.hClient, _vidmemHot.hBuffer,
                             NVC365_CTRL_CMD_ACCESS_CNTR_SET_CONFIG, &cfg, sizeof(cfg));
    if (status != NV_OK)
        goto fail;
    en.intrOwnership = NVC365_CTRL_ACCESS_COUNTER_INTERRUPT_OWNERSHIP_NOT_RM;
    en.enable        = NV_TRUE;
    status = pRmApi->Control(pRmApi, _vidmemHot.hClient, _vidmemHot.hBuffer,
                             NVC365_CTRL_CMD_ACCESS_CNTR_BUFFER_ENABLE, &en, sizeof(en));
    if (status != NV_OK)
        goto fail;
    // Polled, no interrupt.
    if (_vidmemHot.pIntrEnClear != NULL)
        MEM_WR32(_vidmemHot.pIntrEnClear, _vidmemHot.intrMask);

    NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hot memory: access counters on (%u entries, 2 MB, threshold %u)\n",
              _vidmemHot.entries, VIDMEM_HOT_THRESHOLD);
    return NV_OK;

fail:
    NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hot memory: access counters unavailable (0x%x)%s\n", status,
              (status == NV_ERR_INVALID_STATE) ? ", in use by nvidia-uvm" : "");
    vidmemEvictHotTeardown(pGpu);
    _vidmemHot.bInit      = NV_TRUE;     // do not retry every step
    _vidmemHot.bFailed    = NV_TRUE;
    _vidmemHot.failStatus = status;
    return status;
}

// Counters start again for whatever was just moved out.
static void
_vidmemHotResetCounters(void)
{
    RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
    NVC365_CTRL_ACCESS_CNTR_BUFFER_RESET_COUNTERS_PARAMS reset = {0};

    if (!_vidmemHotEnabled || !_vidmemHot.bInit || _vidmemHot.bFailed)
        return;
    reset.counterType = NVC365_CTRL_ACCESS_COUNTER_TYPE_MIMC;
    (void)pRmApi->Control(pRmApi, _vidmemHot.hClient, _vidmemHot.hBuffer,
                          NVC365_CTRL_CMD_ACCESS_CNTR_BUFFER_RESET_COUNTERS, &reset, sizeof(reset));
}

// Which evicted allocation of this VA space covers va?
static VIDMEM_EVICT_STATE *
_vidmemHotFind(void *pVas, NvU64 va)
{
    VIDMEM_EVICT_STATE *pState;
    NvU32 i;

    for (pState = _vidmemEvictList; pState != NULL; pState = pState->pNext)
    {
        for (i = 0; i < pState->numRanges; i++)
        {
            // The notification names a 2 MB region; any overlap counts.
            if ((pState->ranges[i].pVas == pVas) &&
                (va <= pState->ranges[i].vaEnd) && (va + (2ULL << 20) > pState->ranges[i].vaStart))
                return pState;
        }
    }
    return NULL;
}

// Read the notifications that arrived since the last step. GPU lock held.
static void
_vidmemHotPoll(OBJGPU *pGpu)
{
    KernelFifo *pKernelFifo = GPU_GET_KERNEL_FIFO(pGpu);
    NvU32 get, put, n = 0;
    static NvU32 logged;

    if (!_vidmemHotEnabled)
        return;
    if (!_vidmemHot.bInit)
        (void)_vidmemHotInit(pGpu);
    if (_vidmemHot.bFailed || (_vidmemHot.pBuf == NULL) || (_vidmemHot.entries == 0))
        return;

    get = MEM_RD32(_vidmemHot.pGet);
    put = MEM_RD32(_vidmemHot.pPut);
    while ((get != put) && (n++ < _vidmemHot.entries))
    {
        NvU32 *pEntry = (NvU32 *)(_vidmemHot.pBuf + (NvU64)get * NVC365_NOTIFY_BUF_SIZE);
        NvU32  addrType, aperture, count;
        NvU64  addr, instLo, instHi;
        INST_BLOCK_DESC inst;
        KernelChannel *pKernelChannel = NULL;
        VIDMEM_EVICT_STATE *pState = NULL;

        if (!DRF_VAL_MW(C365, _NOTIFY_BUF_ENTRY, _VALID, pEntry))
            break;      // not written yet
        addrType = DRF_VAL_MW(C365, _NOTIFY_BUF_ENTRY, _ADDR_TYPE, pEntry);
        aperture = DRF_VAL_MW(C365, _NOTIFY_BUF_ENTRY, _APERTURE, pEntry);
        count    = DRF_VAL_MW(C365, _NOTIFY_BUF_ENTRY, _COUNTER_VAL, pEntry);
        addr     = (NvU64)DRF_VAL_MW(C365, _NOTIFY_BUF_ENTRY, _ADDR_LO, pEntry) |
                   ((NvU64)DRF_VAL_MW(C365, _NOTIFY_BUF_ENTRY, _ADDR_HI, pEntry) << 32);
        instLo   = DRF_VAL_MW(C365, _NOTIFY_BUF_ENTRY, _INST_LO, pEntry);
        instHi   = DRF_VAL_MW(C365, _NOTIFY_BUF_ENTRY, _INST_HI, pEntry);
        inst.address  = (instLo + (instHi << DRF_SIZE_MW(NVC365_NOTIFY_BUF_ENTRY_INST_LO))) << 12;
        inst.aperture = DRF_VAL_MW(C365, _NOTIFY_BUF_ENTRY, _INST_APERTURE, pEntry);
        inst.gfid     = GPU_GFID_PF;

        _vidmemHot.notifs++;
        //
        // A virtual address (GVA) entry: its aperture field says nothing
        // (always 0 here, BACKLOG I5, 2026-11-07: 28665 entries from a
        // process reading its evicted memory, all "aperture 0"); whether the
        // VA lies in an evicted allocation is what the lookup tells. A
        // physical one counts only when it points at sysmem.
        //
        if ((addrType == NVC365_NOTIFY_BUF_ENTRY_ADDR_TYPE_GVA) ||
            (aperture == NVC365_NOTIFY_BUF_ENTRY_APERTURE_SYS_MEM_COHERENT) ||
            (aperture == NVC365_NOTIFY_BUF_ENTRY_APERTURE_SYS_MEM_NONCOHERENT))
        {
            _vidmemHot.sysNotifs++;
            if ((addrType == NVC365_NOTIFY_BUF_ENTRY_ADDR_TYPE_GVA) &&
                (kfifoConvertInstToKernelChannel_HAL(pGpu, pKernelFifo, &inst, &pKernelChannel) == NV_OK) &&
                (pKernelChannel != NULL))
                pState = _vidmemHotFind(pKernelChannel->pVAS, addr);
            if (pState != NULL)
            {
                _vidmemHot.matched++;
                pState->hotNs = osGetMonotonicTimeNs();
                pState->hotCount++;
            }
        }
        if (logged < 32)
        {
            logged++;
            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hot memory: %s 0x%llx aperture %u count %u channel %s -> %s\n",
                      (addrType == NVC365_NOTIFY_BUF_ENTRY_ADDR_TYPE_GVA) ? "va" : "pa", addr, aperture, count,
                      (pKernelChannel != NULL) ? "found" : "-",
                      (pState != NULL) ? "evicted allocation" : "not evicted");
        }

        // Hand the entry back to the hardware.
        pEntry[7] &= ~NVBIT(31);
        get = (get + 1) % _vidmemHot.entries;
    }
    MEM_WR32(_vidmemHot.pGet, get);
}

// ---------------------------------------------------------------------------
// Automatic policy (no userspace daemon).
//
// Every VIDMEM_EVICT_TICK_SEC seconds a work item (API lock RW) runs
// _vidmemEvictPolicyTick():
//   - reads GSP's perfmon samples (the ones `nvidia-smi pmon` shows: which
//     process was on GR/NVDEC/NVENC, ~7 samples a second) and keeps per process
//     when it was last active;
//   - "avail" = free vidmem outside the reserve block (what reserve-bound
//     processes, i.e. games, can still get);
//   - avail < lowMB: evict whole processes that were not active for
//     VIDMEM_EVICT_IDLE_SEC, least recently active first, never the busiest
//     process and never the desktop itself (_vidmemEvictNeverNames); repeated
//     every tick, so what a background app allocates later goes too;
//   - otherwise restore: a recently active process as soon as it fits above
//     lowMB + VIDMEM_EVICT_HYST_MB, the others once there was no pressure for
//     VIDMEM_EVICT_QUIET_SEC and highMB stays free after the restore; one
//     process per tick.
// Registry (NVreg_RegistryDwords): RmVidmemAutoEvict (default 1 when the user
// reserve is on), RmVidmemEvictLowMB (1024), RmVidmemEvictHighMB (2560).
// Runtime: "auto 0|1" to the vidmem_evict procfs file.
// ---------------------------------------------------------------------------

#include "ctrl/ctrl2080/ctrl2080perf.h"
#include "ctrl/ctrl2080/ctrl2080gpumon.h"
#include "os/workitem.h"

void vidmemEvictNoteBlock(PMA *pPma);
NvBool vidmemGetUserReserveBlockInfo(PMA *pPma, NvU64 *pBlockBytes, NvU64 *pFreeBytes,
                                     NvU64 *pLargestFreeBytes);

static NvU32  _vidmemTickCount;
static volatile NvU32 _vidmemWorkQueued;

static void
_vidmemActivityNote(NvU32 pid, NvU32 util, NvU64 ts)
{
    VIDMEM_EVICT_ACTIVITY *pAct;

    if ((pid == 0) || (pid == NV2080_GPUMON_PID_INVALID) || (util == 0))
        return;

    pAct = _vidmemActivityFind(pid, NV_TRUE);
    pAct->lastActiveTs = NV_MAX(pAct->lastActiveTs, ts);
    pAct->recentSamples++;
}

// GPU lock held.
static void
_vidmemActivitySample(OBJGPU *pGpu)
{
    NV2080_CTRL_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2_PARAMS *pParams;
    RM_API   *pRmApi = GPU_GET_PHYSICAL_RMAPI(pGpu);
    NvU64     newest = _vidmemActivityNewestTs;
    NvU32     i;

    pParams = portMemAllocNonPaged(sizeof(*pParams));
    if (pParams == NULL)
        return;
    portMemSet(pParams, 0, sizeof(*pParams));
    pParams->type    = NV2080_CTRL_GPUMON_SAMPLE_TYPE_PERFMON_UTIL;
    pParams->bufSize = NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_BUFFER_SIZE;

    if (pRmApi->Control(pRmApi, pGpu->hInternalClient, pGpu->hInternalSubdevice,
                        NV2080_CTRL_CMD_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2,
                        pParams, sizeof(*pParams)) == NV_OK)
    {
        // Decay: samples older than ~10 s drop out of the ring anyway.
        for (i = 0; i < VIDMEM_EVICT_MAX_PROCS; i++)
            _vidmemActivity[i].recentSamples -= _vidmemActivity[i].recentSamples / 5;

        for (i = 0; i < NV2080_CTRL_PERF_GPUMON_SAMPLE_COUNT_PERFMON_UTIL; i++)
        {
            NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE *pS = &pParams->samples[i];
            NvU64 ts = pS->base.timeStamp;

            if ((ts == 0) || (ts <= _vidmemActivityNewestTs))
                continue;
            newest = NV_MAX(newest, ts);
            _vidmemActivityNote(pS->gr.procId,    pS->gr.util,    ts);
            _vidmemActivityNote(pS->nvdec.procId, pS->nvdec.util, ts);
            _vidmemActivityNote(pS->nvenc.procId, pS->nvenc.util, ts);
        }
        _vidmemActivityNewestTs = newest;
    }

    portMemFree(pParams);
}

//
// Work submission per process: read GP_PUT (USERD offset 0x8C, the same in
// every GPFIFO class since Volta) of each channel of each user client and
// note a change of the per-process signature. API lock RW, GPU lock held.
//
#define VIDMEM_EVICT_USERD_GP_PUT 0x8C

static void
_vidmemActivitySampleSubmits(OBJGPU *pGpu)
{
    MemoryManager *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    NvU32          subdev = gpumgrGetSubDeviceInstanceFromGpu(pGpu);
    NvU64          nowNs = osGetMonotonicTimeNs();
    RmClient     **ppClient;
    typedef struct { NvU32 pid; NvU64 sig; } VE_SUBMIT_ACC;
    VE_SUBMIT_ACC *acc;
    NvU32          nAcc = 0, i;

    acc = portMemAllocNonPaged(sizeof(*acc) * VIDMEM_EVICT_MAX_PROCS);
    if (acc == NULL)
        return;

    for (ppClient = serverutilGetFirstClientUnderLock();
         ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RmClient   *pRmClient = *ppClient;
        RsClient   *pRsClient = staticCast(pRmClient, RsClient);
        RS_ITERATOR iter;
        NvU64       sig = 0;
        NvBool      bAny = NV_FALSE;

        if ((pRsClient->type == CLIENT_TYPE_KERNEL) || (pRmClient->ProcID == 0))
            continue;

        iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0, RS_ITERATE_DESCENDANTS, NV_TRUE);
        while (clientRefIterNext(iter.pClient, &iter))
        {
            KernelChannel    *pKC = dynamicCast(iter.pResourceRef->pResource, KernelChannel);
            TRANSFER_SURFACE  surf = {0};
            NvU32             put = 0;

            if ((pKC == NULL) || (pKC->pUserdSubDeviceMemDesc[subdev] == NULL))
                continue;
            surf.pMemDesc = pKC->pUserdSubDeviceMemDesc[subdev];
            surf.offset   = VIDMEM_EVICT_USERD_GP_PUT;
            if (memmgrMemRead(pMemoryManager, &surf, &put, sizeof(put),
                              TRANSFER_FLAGS_PREFER_PROCESSOR | TRANSFER_FLAGS_MAP_PROTECT_READABLE) != NV_OK)
                continue;
            sig = sig * 1000003ULL + put + ((NvU64)RES_GET_HANDLE(pKC) << 32);
            bAny = NV_TRUE;
        }
        if (!bAny)
            continue;

        for (i = 0; i < nAcc; i++)
        {
            if (acc[i].pid == pRmClient->ProcID)
                break;
        }
        if (i == nAcc)
        {
            if (nAcc >= VIDMEM_EVICT_MAX_PROCS)
                continue;
            acc[nAcc].pid = pRmClient->ProcID;
            acc[nAcc].sig = 0;
            nAcc++;
        }
        acc[i].sig = acc[i].sig * 31ULL + sig;
    }

    for (i = 0; i < nAcc; i++)
    {
        VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(acc[i].pid, NV_TRUE);

        if ((pAct->submitSig != 0) && (pAct->submitSig != acc[i].sig))
            pAct->lastSubmitNs = nowNs;
        pAct->submitSig = acc[i].sig;
    }
    portMemFree(acc);
}

// Is pRef the first Memory object of process `pid` (in client scan order) holding pMemDesc?
static NvBool
_vidmemEvictIsFirstPidHolder(MEMORY_DESCRIPTOR *pMemDesc, NvU32 pid, RsResourceRef *pRef)
{
    RmClient **ppClient;

    if (_veHolders != NULL)
    {
        NvU32 first, n = _veHolderRange(pMemDesc, &first), k;

        for (k = 0; k < n; k++)
        {
            RsResourceRef *pH = _veHolders[first + k].pRef;
            RmClient      *pRmClient = dynamicCast(pH->pClient, RmClient);

            if ((pH->pClient->type == CLIENT_TYPE_KERNEL) || (pRmClient == NULL) || (pRmClient->ProcID != pid))
                continue;
            return pH == pRef;
        }
        return NV_FALSE;
    }

    for (ppClient = serverutilGetFirstClientUnderLock();
         ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RsClient   *pRsClient = staticCast(*ppClient, RsClient);
        RS_ITERATOR iter;

        if ((pRsClient->type == CLIENT_TYPE_KERNEL) || ((*ppClient)->ProcID != pid))
            continue;

        iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0, RS_ITERATE_DESCENDANTS, NV_TRUE);
        while (clientRefIterNext(iter.pClient, &iter))
        {
            Memory *pMemory = dynamicCast(iter.pResourceRef->pResource, Memory);

            if ((pMemory != NULL) && (pMemory->pMemDesc == pMemDesc))
                return iter.pResourceRef == pRef;
        }
    }
    return NV_FALSE;
}

//
// The platform's part of _vidmemEvictGatherProcs() (vidmem_evict_policy.h):
// per-process totals over every non-kernel client. API lock RW, GPU lock held.
// Returns the number of entries filled.
//
NvU32
vepPlatGatherProcs(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 maxProcs)
{
    VIDMEM_EVICT_MAPREF *pRefs;
    RmClient           **ppClient;
    NvU32                count = 0;

    pRefs = portMemAllocNonPaged(sizeof(*pRefs) * VIDMEM_EVICT_MAX_MAPPINGS);
    if (pRefs == NULL)
        return 0;
    _veHolderIndexBegin();

    for (ppClient = serverutilGetFirstClientUnderLock();
         ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RmClient          *pRmClient = *ppClient;
        RsClient          *pRsClient = staticCast(pRmClient, RsClient);
        VIDMEM_EVICT_PROC *pProc = NULL;
        RS_ITERATOR        iter;
        NvU32              i;

        if ((pRsClient->type == CLIENT_TYPE_KERNEL) || (pRmClient->ProcID == 0))
            continue;

        for (i = 0; i < count; i++)
        {
            if (pProcs[i].pid == pRmClient->ProcID)
            {
                pProc = &pProcs[i];
                break;
            }
        }
        if (pProc == NULL)
        {
            if (count >= maxProcs)
                continue;
            pProc = &pProcs[count++];
            portMemSet(pProc, 0, sizeof(*pProc));
            pProc->pid = pRmClient->ProcID;
            portStringCopy(pProc->name, sizeof(pProc->name), pRmClient->name, sizeof(pRmClient->name));
        }
        pProc->clients++;
        if (_vidmemEvictIsNeverName(pRmClient->name))
            pProc->bNever = NV_TRUE;

        iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0,
                                 RS_ITERATE_DESCENDANTS, NV_TRUE);
        while (clientRefIterNext(iter.pClient, &iter))
        {
            RsResourceRef *pRef = iter.pResourceRef;
            Memory        *pMemory = dynamicCast(pRef->pResource, Memory);
            NvU32          numRefs = 0;
            NvU64          bytes;

            const char    *reason;

            if ((pMemory == NULL) || (pMemory->pMemDesc == NULL) ||
                (pRef->externalClassId != NV01_MEMORY_LOCAL_USER) ||
                (memdescGetAddressSpace(pMemory->pMemDesc) != ADDR_FBMEM))
                continue;

            // A buffer this process holds several times counts once.
            if ((pMemory->pMemDesc->DupCount > 1) &&
                !_vidmemEvictIsFirstPidHolder(pMemory->pMemDesc, pProc->pid, pRef))
                continue;

            //
            // The desktop's views of other processes' buffers (window buffers
            // held by kwin) belong to those processes: they are evicted and
            // restored with them, not with the compositor.
            //
            if (pProc->bNever && !pMemory->isMemDescOwner)
                continue;

            bytes = memdescGetSize(pMemory->pMemDesc);
            if (pMemory->pMemDesc->pLocalEvictState != NULL)
            {
                pProc->evictedBytes += bytes;
                continue;
            }
            pProc->vidmemBytes += bytes;
            reason = _vidmemEvictCheck(pGpu, pRef, pProc->pid, pRefs, &numRefs);
            if (reason == NULL)
                pProc->eligibleBytes += bytes;
        }
    }

    _veHolderIndexEnd();
    portMemFree(pRefs);
    return count;
}


// Free vidmem outside the reserve block, in MB. GPU lock held.
NvU64
vepPlatAvailMB(OBJGPU *pGpu)
{
    Heap  *pHeap = GPU_GET_HEAP(pGpu);
    NvU64  freeBytes = 0, blockBytes = 0, blockFree = 0, blockLargest = 0;

    if ((pHeap == NULL) || (pHeap->pPmaObject == NULL))
        return 0;
    pmaGetFreeMemory(pHeap->pPmaObject, &freeBytes);
    (void)vidmemGetUserReserveBlockInfo(pHeap->pPmaObject, &blockBytes, &blockFree, &blockLargest);
    return ((freeBytes > blockFree) ? (freeBytes - blockFree) : 0) >> 20;
}

//
// Bring back evicted allocations that are in use (hot), one at a time, for
// processes that are not hidden: from sysmem they cost PCIe on every access.
// Idle hidden processes make room. GPU lock held.
//
void
vepPlatHotRestore(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count)
{
    NvU64 nowNs = osGetMonotonicTimeNs();
    NvU32 done = 0;
    VIDMEM_EVICT_STATE *pState, *pNext;

    if (!_vidmemHotEnabled)
        return;
    for (pState = _vidmemEvictList; (pState != NULL) && (done < 8); pState = pNext)
    {
        MEMORY_DESCRIPTOR *pMemDesc = pState->pMemDesc;
        NvU64 needMB = (pState->bytes >> 20) + 1, avail, keepMB;
        NvU32 hint;

        pNext = pState->pNext;
        if ((pState->hotNs == 0) || (nowNs - pState->hotNs > 4000000000ULL) || (pState->ownerPid == 0))
            continue;
        hint = VE_HINT_STATE(_vidmemHint(pState->ownerPid, NULL));
        if (_vidmemHintsActive() && (hint == VE_HINT_HIDDEN))
            continue;
        keepMB = _vidmemHintsActive() ? vepHotRestoreKeepMB(pState->ownerPid) : VIDMEM_EVICT_RESTORE_MARGIN_MB;
        if (keepMB == NV_U64_MAX)
            continue;
        _vidmemEvictWhy = VMT_WHY_ROOM;
        avail = _vidmemEvictHintFree(pGpu, pProcs, count, needMB + keepMB,
                                     pState->ownerPid, VE_LEVEL_HIDDEN, "hot allocation");
        // With 16 MB to spare; next to a focused game, leaving its headroom.
        if (avail < needMB + ((keepMB > VIDMEM_EVICT_RESTORE_MARGIN_MB) ? keepMB : 16))
            continue;
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hot memory: restoring %llu KB of pid %u (%u notifications)\n",
                  pState->bytes >> 10, pState->ownerPid, pState->hotCount);
        _vidmemEvictOnlyMemDesc = pMemDesc;
        _vidmemEvictWhy = VMT_WHY_HOT;
        (void)_vidmemEvictPidLocked(pGpu, 2, pState->ownerPid, NULL, 0, NULL, NULL);
        _vidmemEvictOnlyMemDesc = NULL;
        if (pMemDesc->pLocalEvictState == NULL)
        {
            _vidmemHot.restored++;
            _vidmemHotNoteRecent(pMemDesc);
        }
        done++;
        pNext = _vidmemEvictList;     // the list changed
    }
}

NV_STATUS
vepPlatMovePid(OBJGPU *pGpu, NvU32 mode, NvU32 pid, NvU64 *pMovedBytes)
{
    return _vidmemEvictPidLocked(pGpu, mode, pid, NULL, 0, NULL, pMovedBytes);
}

// One policy step: sample the GPU activity, then the policy. API lock RW, GPU lock held.
static void
_vidmemEvictPolicyStep(OBJGPU *pGpu)
{
    _vidmemActivitySample(pGpu);
    _vidmemActivitySampleSubmits(pGpu);
    _vidmemHotPoll(pGpu);
    if (GPU_GET_HEAP(pGpu) != NULL)
        vidmemEvictNoteBlock(GPU_GET_HEAP(pGpu)->pPmaObject);
    vepPolicyStep(pGpu);
}

//
// One policy step, traced (VMT_TICK, VMT_PROC): everything in it runs with
// the API lock held for writing and the GPU lock, i.e. every other RM call
// waits. trigger: 0 timer (queuedNs: when the work item was queued), 1 hints.
//
static NvU64 _vidmemTickQueuedNs;
static NvU32 _vidmemFragTicks;

static void
_vidmemEvictPolicyTick(OBJGPU *pGpu, NvU32 trigger)
{
    NvU64 startNs = VMT_NOW();
    NvU64 blockBytes = 0, blockFree = 0, blockLargest = 0;
    NvU32 procs = 0;
    NvU64 stepNs = osGetMonotonicTimeNs();   // the clock the policy keeps (not VMT_NOW's)

    _vidmemEvictPolicyStep(pGpu);

    if (osVidmemTraceOn(VMT_PROC))
    {
        VIDMEM_EVICT_PROC *pLast = NULL;
        NvU64 lastNs = 0;
        NvU32 lastCount = vepLastProcs(&pLast, &lastNs);
        VIDMEM_EVICT_PROC *pProcs = portMemAllocNonPaged(sizeof(*pProcs) * VIDMEM_EVICT_MAX_PROCS);
        NvU64 nowNs = osGetMonotonicTimeNs();
        NvU32 i;

        if (pProcs != NULL)
        {
            // The step just walked the clients: its totals, not a walk of our own (#1).
            if ((lastNs >= stepNs) && (lastCount != 0))
            {
                portMemCopy(pProcs, sizeof(*pProcs) * lastCount, pLast, sizeof(*pProcs) * lastCount);
                procs = lastCount;
            }
            else
            {
                procs = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);
            }
            for (i = 0; i < procs; i++)
            {
                VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pProcs[i].pid, NV_FALSE);
                NvU64 a[VMT_ARGS] = { pProcs[i].vidmemBytes, pProcs[i].evictedBytes, pProcs[i].eligibleBytes,
                                      pProcs[i].hint, _vidmemActivityIdleSec(pProcs[i].pid),
                                      (pAct != NULL) ? pAct->recentSamples : 0,
                                      ((pAct != NULL) && (pAct->lastSubmitNs != 0)) ?
                                          (nowNs - pAct->lastSubmitNs) / 1000000000ULL : NV_U32_MAX,
                                      pProcs[i].bNever };
                osVidmemTrace(VMT_PROC, pProcs[i].pid, a);
            }
            portMemFree(pProcs);
        }
    }
    if (osVidmemTraceOn(VMT_TICK))
    {
        NvU64 a[VMT_ARGS] = {0};

        if (GPU_GET_HEAP(pGpu) != NULL)
            (void)vidmemGetUserReserveBlockInfo(GPU_GET_HEAP(pGpu)->pPmaObject, &blockBytes, &blockFree,
                                                &blockLargest);
        a[0] = VMT_NOW() - startNs;
        a[1] = ((trigger == 0) && (_vidmemTickQueuedNs != 0)) ? startNs - _vidmemTickQueuedNs : 0;
        a[2] = vepPlatAvailMB(pGpu);
        a[3] = blockFree >> 20;
        a[4] = blockLargest >> 20;
        a[5] = _vidmemEvictBytes >> 20;
        a[6] = _vidmemBusiestPid;
        a[7] = _vidmemHintsActive();
        a[8] = procs;
        a[9] = trigger;
        osVidmemTrace(VMT_TICK, 0, a);
    }
    // Every 5th timer step (10 s): how fragmented free vidmem is (BACKLOG #16).
    if ((trigger == 0) && ((_vidmemFragTicks++ % 5) == 0) && osVidmemTraceOn(VMT_FRAG))
        _vidmemFragTrace(pGpu, 0, 0, 0, NULL);
}

static void
_vidmemEvictWorkItem(NvU32 gpuInstance, void *pArgs)
{
    OBJGPU *pGpu = gpumgrGetGpu(gpuInstance);

    if ((pGpu != NULL) && gpuIsGpuFullPower(pGpu) &&
        !pGpu->getProperty(pGpu, PDB_PROP_GPU_IN_PM_CODEPATH) &&
        rmapiLockIsWriteOwner() &&
        (rmGpuLocksAcquire(GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_MEM) == NV_OK))
    {
        _vidmemEvictPolicyTick(pGpu, 0);
        rmGpuLocksRelease(GPUS_LOCK_FLAGS_NONE, NULL);
    }
    _vidmemWorkQueued = 0;
}

static void
_vidmemEvict1HzCallback(OBJGPU *pGpu, void *pData)
{
    if ((++_vidmemTickCount % VIDMEM_EVICT_TICK_SEC) != 0)
        return;
    if (_vidmemWorkQueued)
        return;

    _vidmemWorkQueued = 1;
    _vidmemTickQueuedNs = VMT_NOW();
    if (osQueueWorkItem(pGpu, _vidmemEvictWorkItem, NULL,
                        (OsQueueWorkItemFlags){
                            .apiLock = WORKITEM_FLAGS_API_LOCK_READ_WRITE,
                            .bDontFreeParams = NV_TRUE,
                            .bDropOnUnloadQueueFlush = NV_TRUE}) != NV_OK)
    {
        _vidmemWorkQueued = 0;
    }
}

NvU64
vidmemEvictReclaimableFor(RmClient *pRmClient)
{
    return (pRmClient != NULL) ? vepReclaimableFor(pRmClient->ProcID) : 0;
}

//
// Step 2: the user reserve is about to deny a vidmem allocation. Should it be
// created in sysmem instead (vidmemEvictBornAllocation)? The policy decides
// for whom (vepWantBorn()); here: only what eviction supports (no
// contiguous/ISO/fixed-address requests), up to 512 MB, and only if the
// reserve block (no reserve: the free memory) can hold it for the moment it
// takes to move it.
//
#define VIDMEM_EVICT_BORN_MAX_BYTES (512ULL << 20)

// The calling client's vidmem (vepWantBorn() asks when its last walk is old).
static NvU64
_vidmemEvictClientVidmem(void *ctx)
{
    RmClient   *pRmClient = ctx;
    RsClient   *pRsClient = staticCast(pRmClient, RsClient);
    RS_ITERATOR iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0,
                                         RS_ITERATE_DESCENDANTS, NV_TRUE);
    NvU64 bytes = 0;

    while (clientRefIterNext(iter.pClient, &iter))
    {
        Memory *pMemory = dynamicCast(iter.pResourceRef->pResource, Memory);

        if ((pMemory != NULL) && (pMemory->pMemDesc != NULL) &&
            (iter.pResourceRef->externalClassId == NV01_MEMORY_LOCAL_USER) &&
            (pMemory->pMemDesc->pLocalEvictState == NULL) &&
            (memdescGetAddressSpace(pMemory->pMemDesc) == ADDR_FBMEM))
            bytes += memdescGetSize(pMemory->pMemDesc);
    }
    return bytes;
}

NvBool
vidmemEvictWantBorn(OBJGPU *pGpu, RmClient *pRmClient, NV_MEMORY_ALLOCATION_PARAMS *pAllocData)
{
    Heap  *pHeap = GPU_GET_HEAP(pGpu);
    NvU64  blockBytes = 0, blockFree = 0, blockLargest = 0;

    if ((pRmClient == NULL) || (pAllocData == NULL) ||
        !vepWantBorn(pRmClient->ProcID, pRmClient->name, _vidmemEvictClientVidmem, pRmClient))
        return NV_FALSE;
    if ((pAllocData->size == 0) || (pAllocData->size > VIDMEM_EVICT_BORN_MAX_BYTES))
        return NV_FALSE;
    if (FLD_TEST_DRF(OS32, _ATTR, _PHYSICALITY, _CONTIGUOUS, pAllocData->attr) ||
        FLD_TEST_DRF(OS32, _ATTR2, _ISO, _YES, pAllocData->attr2) ||
        (pAllocData->flags & (NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE |
                              NVOS32_ALLOC_FLAGS_USE_BEGIN_END |
                              NVOS32_ALLOC_FLAGS_VIRTUAL)))
        return NV_FALSE;
    if (pHeap == NULL)
        return NV_FALSE;
    if (!vidmemGetUserReserveBlockInfo(pHeap->pPmaObject, &blockBytes, &blockFree, &blockLargest))
    {
        // No reserve: it sits in the free memory for that moment.
        NvU64 freeBytes = 0;

        pmaGetFreeMemory(pHeap->pPmaObject, &freeBytes);
        return freeBytes >= pAllocData->size + (64ULL << 20);
    }
    return blockFree >= pAllocData->size + (128ULL << 20);
}

// See vepBackgroundCushion().
NvU64
vidmemEvictBackgroundCushion(RmClient *pRmClient)
{
    return (pRmClient != NULL) ? vepBackgroundCushion(pRmClient->ProcID, pRmClient->name) : 0;
}

//
// An allocation of a visible process is about to be denied (or would eat into
// the foreground headroom): move others out (vepMakeRoomRun()). Needs the API
// lock for writing; vidmemConstruct() calls this before it takes the device
// GPU lock (PMA allocates first, without it), so the lock is taken here for
// the moves.
//
//
// The foreground's headroom refill (vepHeadroomRun()) runs here, a piece per
// work item: the locks are free between pieces (BACKLOG #5).
//
static volatile NvU32 _vidmemHeadroomQueued;

static void _vidmemHeadroomQueue(OBJGPU *pGpu);

static void
_vidmemHeadroomWorkItem(NvU32 gpuInstance, void *pArgs)
{
    OBJGPU *pGpu = gpumgrGetGpu(gpuInstance);
    NvBool  bMore = NV_FALSE;

    _vidmemHeadroomQueued = 0;
    if ((pGpu != NULL) && gpuIsGpuFullPower(pGpu) &&
        !pGpu->getProperty(pGpu, PDB_PROP_GPU_IN_PM_CODEPATH) &&
        rmapiLockIsWriteOwner() &&
        (rmGpuLocksAcquire(GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_MEM) == NV_OK))
    {
        bMore = vepHeadroomRun(pGpu);
        rmGpuLocksRelease(GPUS_LOCK_FLAGS_NONE, NULL);
    }
    if (bMore && (pGpu != NULL))
        _vidmemHeadroomQueue(pGpu);
}

static void
_vidmemHeadroomQueue(OBJGPU *pGpu)
{
    if (_vidmemHeadroomQueued)
        return;
    _vidmemHeadroomQueued = 1;
    if (osQueueWorkItem(pGpu, _vidmemHeadroomWorkItem, NULL,
                        (OsQueueWorkItemFlags){
                            .apiLock = WORKITEM_FLAGS_API_LOCK_READ_WRITE,
                            .bDontFreeParams = NV_TRUE,
                            .bDropOnUnloadQueueFlush = NV_TRUE}) != NV_OK)
    {
        _vidmemHeadroomQueued = 0;
    }
}

static NvBool _vidmemEvictMakeRoomWant(OBJGPU *pGpu, RmClient *pRmClient, NvU64 size, NvU64 headroomMB,
                                       const char *why);

NvBool
vidmemEvictMakeRoom(OBJGPU *pGpu, RmClient *pRmClient, NvU64 size)
{
    return _vidmemEvictMakeRoomWant(pGpu, pRmClient, size, VIDMEM_EVICT_HINT_LOW_MB, "allocation");
}

void
vidmemEvictKeepFgHeadroom(OBJGPU *pGpu, RmClient *pRmClient, NvU64 size)
{
    if ((pRmClient == NULL) || _vidmemEvictIsBusyThread() ||
        !vepKeepFgHeadroomWanted(pGpu, pRmClient->ProcID, size))
        return;
    // In the background, not inside this allocation (BACKLOG #5).
    if (vepHeadroomRequest(pRmClient->ProcID))
        _vidmemHeadroomQueue(pGpu);
}

static NvBool
_vidmemEvictMakeRoomWant(OBJGPU *pGpu, RmClient *pRmClient, NvU64 size, NvU64 headroomMB, const char *why)
{
    static NvU32       lockLogged;
    NvU64              wantMB = 0, avail = 0;
    NvBool             bOk;
    NvBool             bLocked = NV_FALSE;
    NvU64              t0;

    if ((pRmClient == NULL) || _vidmemEvictIsBusyThread() || !vepMakeRoomAllowed(pRmClient->ProcID))
        return NV_FALSE;
    if (!rmapiLockIsWriteOwner())
    {
        if (lockLogged++ < 4)
            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: cannot make room for %s(%u) here: no API write lock\n",
                      pRmClient->name, pRmClient->ProcID);
        return NV_FALSE;
    }

    t0 = VMT_NOW();
    if (!rmGpuLockIsOwner() && !rmDeviceGpuLockIsOwner(pGpu->gpuInstance))
    {
        if (rmDeviceGpuLocksAcquire(pGpu, GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_MEM) != NV_OK)
            return NV_FALSE;
        bLocked = NV_TRUE;
    }
    bOk = vepMakeRoomRun(pGpu, pRmClient->ProcID, size, headroomMB, why, &wantMB, &avail);
    if (bLocked)
        rmDeviceGpuLocksRelease(pGpu, GPUS_LOCK_FLAGS_NONE, NULL);
    if (osVidmemTraceOn(VMT_MAKEROOM))
    {
        NvU64 a[VMT_ARGS] = { size, wantMB, avail, bOk, VMT_NOW() - t0,
                              (VE_HINT_STATE(_vidmemHint(pRmClient->ProcID, NULL)) == VE_HINT_FG) ?
                                  VE_LEVEL_FG_IDLE : VE_LEVEL_HIDDEN };
        osVidmemTrace(VMT_MAKEROOM, pRmClient->ProcID, a);
    }
    return bOk;
}

//
// No reserve: clear a contiguous window of `size` for the compositor (a
// scanout buffer is physically contiguous). Marks the frames of everything
// the victims could give up, picks the 2 MB-aligned window with no frame that
// cannot be freed and the least to move (the lowest of equals: scanout
// buffers grow up), and moves just the allocations in it. GPU lock held.
//
NvU64 *vidmemPmaUsedMap(PMA *pPma, NvU64 *pBase, NvU64 *pFrames, NvU64 *pLargestFree); // video_mem.c

#define VIDMEM_EVICT_CHUNK_FRAMES ((2ULL << 20) >> PMA_PAGE_SHIFT)

//
// Free 2 MB pages (physically 2 MB aligned: the region base is not) in a frame
// map from vidmemPmaUsedMap(): how many, and the longest run of them. What a
// 2 MB-page allocation can use; free bytes in 64 KB holes are no help to it
// (2026-11-04 scanout-churn: 96 MB free, the compositor's 32 MB failed).
//
static void
_vidmemEvictFreeChunks(const NvU64 *pUsed, NvU64 base, NvU64 frames, NvU64 *pFree, NvU64 *pLongest)
{
    NvU64 off = (NV_ALIGN_UP64(base, 2ULL << 20) - base) >> PMA_PAGE_SHIFT;
    NvU64 f, g, run = 0;

    *pFree = *pLongest = 0;
    for (f = off; f + VIDMEM_EVICT_CHUNK_FRAMES <= frames; f += VIDMEM_EVICT_CHUNK_FRAMES)
    {
        for (g = f; g < f + VIDMEM_EVICT_CHUNK_FRAMES; g++)
        {
            if ((pUsed[g >> 6] >> (g & 63)) & 1)
                break;
        }
        if (g < f + VIDMEM_EVICT_CHUNK_FRAMES)
        {
            run = 0;
            continue;
        }
        (*pFree)++;
        *pLongest = NV_MAX(*pLongest, ++run);
    }
}

//
// The cheapest window of `need` 2 MB chunks starting at a multiple of `align`:
// frames to move (in use and movable, pMovable), NV_U64_MAX if every window
// holds a frame that cannot be freed. *pStart (optional) gets its first chunk.
// pCost is scratch space for `chunks` entries. A frame in pHeavy (optional:
// memory of a process that is drawing in a window the user sees, BACKLOG
// #15) counts VIDMEM_EVICT_HEAVY_COST times: a window over hidden programs
// is taken before a smaller one over the game on the other monitor.
//
#define VIDMEM_EVICT_HEAVY_COST 4

static NvU64
_vidmemEvictBestWindow(const NvU64 *pUsed, const NvU64 *pMovable, const NvU64 *pHeavy, NvU32 *pCost, NvU64 base,
                       NvU64 off, NvU64 chunks, NvU64 need, NvU64 align, NvU64 *pStart)
{
    NvU64 c, f, s, best = NV_U64_MAX;

    for (c = 0; c < chunks; c++)
    {
        NvU32 n = 0;

        for (f = off + c * VIDMEM_EVICT_CHUNK_FRAMES; f < off + (c + 1) * VIDMEM_EVICT_CHUNK_FRAMES; f++)
        {
            NvBool bUsed = (pUsed[f >> 6] >> (f & 63)) & 1;
            NvBool bMov  = (pMovable[f >> 6] >> (f & 63)) & 1;

            if (bUsed && !bMov)
            {
                n = NV_U32_MAX;
                break;
            }
            if (bUsed)
                n += ((pHeavy != NULL) && ((pHeavy[f >> 6] >> (f & 63)) & 1)) ? VIDMEM_EVICT_HEAVY_COST : 1;
        }
        pCost[c] = n;
    }
    for (s = 0; s + need <= chunks; s++)
    {
        NvU64 addr = base + ((off + s * VIDMEM_EVICT_CHUNK_FRAMES) << PMA_PAGE_SHIFT);
        NvU64 cost = 0;

        if ((addr % align) != 0)
            continue;
        for (c = s; c < s + need; c++)
        {
            if (pCost[c] == NV_U32_MAX)
                break;
            cost += pCost[c];
        }
        if ((c == s + need) && (cost < best))
        {
            best = cost;
            if (pStart != NULL)
                *pStart = s;
            if (best == 0)
                break;
        }
    }
    return best;
}

// Free 2 MB pages now and their longest run, or NV_FALSE.
static NvBool
_vidmemEvictChunkInfo(PMA *pPma, NvU64 *pFree, NvU64 *pLongest)
{
    NvU64  base = 0, frames = 0, largest = 0;
    NvU64 *pUsed = vidmemPmaUsedMap(pPma, &base, &frames, &largest);

    if (pUsed == NULL)
        return NV_FALSE;
    _vidmemEvictFreeChunks(pUsed, base, frames, pFree, pLongest);
    portMemFree(pUsed);
    return NV_TRUE;
}

// Does `pid` hold a movable-looking allocation with a page in the window being cleared?
static NvBool
_vidmemPidHasInRange(NvU32 pid)
{
    RmClient **ppClient;

    for (ppClient = serverutilGetFirstClientUnderLock(); ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RsClient   *pRsClient = staticCast(*ppClient, RsClient);
        RS_ITERATOR iter;

        if ((pRsClient->type == CLIENT_TYPE_KERNEL) || ((*ppClient)->ProcID != pid))
            continue;
        iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0, RS_ITERATE_DESCENDANTS, NV_TRUE);
        while (clientRefIterNext(iter.pClient, &iter))
        {
            Memory *pMemory = dynamicCast(iter.pResourceRef->pResource, Memory);

            if ((pMemory != NULL) && (pMemory->pMemDesc != NULL) &&
                (iter.pResourceRef->externalClassId == NV01_MEMORY_LOCAL_USER) &&
                (pMemory->pMemDesc->pLocalEvictState == NULL) &&
                _vidmemMemDescInRange(pMemory->pMemDesc, _vidmemEvictRangeLo, _vidmemEvictRangeHi))
                return NV_TRUE;
        }
    }
    return NV_FALSE;
}

static NvU64 _vidmemContigInVidmemMB;   // moved inside vidmem by window clearing, in all (status)

//
// Who gives for a window: for the compositor (forPid 0) anyone but the
// desktop; for an allocation of `forPid` those who may give at its `level`
// (so a visible Chrome's buffer does not move the focused game). forPid's own
// memory is moved only inside vidmem, and only what is not contiguous: on
// the host its previous 130 MB buffer went to sysmem for each new one, up to
// 30 times a second, and came back as "the foreground's memory" at the cost
// of the game's (BACKLOG #15, 165 clearings in two hours).
//
static NvBool
_vidmemEvictMakeContig(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 size, NvU64 align,
                       NvU32 forPid, NvU32 level, NvU64 *pMovedMB)
{
    NvU64   inVidmem = 0, *pInVidmemMB = &inVidmem;
    Heap   *pHeap = GPU_GET_HEAP(pGpu);
    PMA    *pPma = (pHeap != NULL) ? pHeap->pPmaObject : NULL;
    NvU64   base = 0, frames = 0, largest = 0, words, chunks, need, off, freeChunks, longest;
    NvU64  *pUsed, *pMovable = NULL, *pHeavy = NULL;
    NvU32  *pCost = NULL;
    NvU64   bestStart = NV_U64_MAX, bestCost = NV_U64_MAX, f;
    VIDMEM_EVICT_MAPREF *pRefs = NULL;
    NvU64   nowNs = osGetMonotonicTimeNs();
    NvBool  bOk = NV_FALSE;
    NvU32   i;

    *pMovedMB = 0;
    pUsed = vidmemPmaUsedMap(pPma, &base, &frames, &largest);
    if (pUsed == NULL)
        return NV_FALSE;
    //
    // The window starts at a multiple of the allocation's alignment: an ISO
    // scanout buffer may want more than 2 MB (2026-11-04 scanout-churn: a
    // cleared 2 MB-aligned 32 MB window at 0x1ffc00000 did not do).
    //
    align  = NV_MAX(align, 2ULL << 20);
    need   = NV_DIV_AND_CEIL(size, 2ULL << 20);
    _vidmemEvictFreeChunks(pUsed, base, frames, &freeChunks, &longest);
    words  = (frames + 63) >> 6;
    off    = (NV_ALIGN_UP64(base, 2ULL << 20) - base) >> PMA_PAGE_SHIFT;
    chunks = (frames > off) ? (frames - off) / VIDMEM_EVICT_CHUNK_FRAMES : 0;
    pMovable = portMemAllocNonPaged(words * sizeof(NvU64));
    pHeavy   = portMemAllocNonPaged(words * sizeof(NvU64));
    pCost    = portMemAllocNonPaged(chunks * sizeof(NvU32));
    pRefs    = portMemAllocNonPaged(sizeof(*pRefs) * VIDMEM_EVICT_MAX_MAPPINGS);
    if ((pMovable == NULL) || (pHeavy == NULL) || (pCost == NULL) || (pRefs == NULL) || (need == 0) ||
        (need > chunks))
        goto done;
    portMemSet(pMovable, 0, words * sizeof(NvU64));
    portMemSet(pHeavy, 0, words * sizeof(NvU64));
    // Already there: an aligned window with nothing in it.
    if (_vidmemEvictBestWindow(pUsed, pMovable, NULL, pCost, base, off, chunks, need, align, NULL) == 0)
    {
        bOk = NV_TRUE;
        goto done;
    }

    // Frames the victims could give up.
    for (i = 0; i < count; i++)
    {
        RmClient **ppClient;
        NvBool     bSelf = (forPid != 0) && (pProcs[i].pid == forPid);
        // Given only because of the level (drawing in a window the user sees): costly.
        NvBool     bHeavy = !bSelf && !_vidmemEvictHintVictimOk(&pProcs[i], forPid, nowNs, VE_LEVEL_HIDDEN);

        if (!bSelf && !_vidmemEvictHintVictimOk(&pProcs[i], forPid, nowNs, level))
            continue;
        for (ppClient = serverutilGetFirstClientUnderLock(); ppClient != NULL;
             ppClient = serverutilGetNextClientUnderLock(ppClient))
        {
            RsClient   *pRsClient = staticCast(*ppClient, RsClient);
            RS_ITERATOR iter;

            if ((pRsClient->type == CLIENT_TYPE_KERNEL) || ((*ppClient)->ProcID != pProcs[i].pid))
                continue;
            iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0, RS_ITERATE_DESCENDANTS, NV_TRUE);
            while (clientRefIterNext(iter.pClient, &iter))
            {
                Memory *pMemory = dynamicCast(iter.pResourceRef->pResource, Memory);
                NvU32   numRefs = 0;

                if ((pMemory == NULL) || (pMemory->pMemDesc == NULL) ||
                    (iter.pResourceRef->externalClassId != NV01_MEMORY_LOCAL_USER) ||
                    (pMemory->pMemDesc->pLocalEvictState != NULL) ||
                    (_vidmemEvictCheck(pGpu, iter.pResourceRef, pProcs[i].pid, pRefs, &numRefs) != NULL))
                    continue;
                //
                // Its own: only what can go to free pages elsewhere in vidmem
                // (not a contiguous buffer: the window is cleared because no
                // free run of that size is left, and not what the compositor
                // maps: _vidmemRelocate() leaves that).
                //
                if (bSelf && ((pMemory->pMemDesc->pPmaAllocInfo == NULL) ||
                              pMemory->pMemDesc->pPmaAllocInfo->bContig ||
                              _vidmemEvictHasCompositor(pRefs, numRefs)))
                    continue;
                (void)_vidmemMemDescFrames(pMemory->pMemDesc, 0, 0, pMovable, base, frames);
                if (bHeavy)
                    (void)_vidmemMemDescFrames(pMemory->pMemDesc, 0, 0, pHeavy, base, frames);
            }
        }
    }

    (void)_vidmemEvictBestWindow(pUsed, pMovable, pHeavy, pCost, base, off, chunks, need, align, &bestStart);
    if (bestStart == NV_U64_MAX)
    {
        static NvU32 noneLogged;

        // A page asking 30 times a second (BACKLOG #15): the first few, then one in 64.
        if ((noneLogged++ < 8) || ((noneLogged % 64) == 0))
            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: contiguous: no %llu MB window aligned to %llu KB can be cleared for pid %u (longest free %llu MB)\n",
                      need * 2, align >> 10, forPid, longest * 2);
        goto done;
    }
    // What is in it, in MB (the cost above counts some frames several times).
    bestCost = 0;
    for (f = off + bestStart * VIDMEM_EVICT_CHUNK_FRAMES; f < off + (bestStart + need) * VIDMEM_EVICT_CHUNK_FRAMES; f++)
        bestCost += (pUsed[f >> 6] >> (f & 63)) & 1;

    NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: contiguous: clearing %llu MB at 0x%llx for %llu KB aligned to %llu KB of pid %u (longest free %llu MB, %llu MB to move)\n",
              need * 2, base + ((off + bestStart * VIDMEM_EVICT_CHUNK_FRAMES) << PMA_PAGE_SHIFT), size >> 10,
              align >> 10, forPid, longest * 2, (bestCost << PMA_PAGE_SHIFT) >> 20);
    _vidmemEvictRangeLo = base + ((off + bestStart * VIDMEM_EVICT_CHUNK_FRAMES) << PMA_PAGE_SHIFT);
    _vidmemEvictRangeHi = _vidmemEvictRangeLo + (need << 21);
    for (i = 0; i < count; i++)
    {
        NvU64  moved = 0;
        NvBool bSelf = (forPid != 0) && (pProcs[i].pid == forPid);

        if ((!bSelf && !_vidmemEvictHintVictimOk(&pProcs[i], forPid, nowNs, level)) ||
            !_vidmemPidHasInRange(pProcs[i].pid))
            continue;
        //
        // To other vidmem pages first (BACKLOG #16, #15: Chrome's contiguous
        // buffers pushed the game's memory out to sysmem, which then ran from
        // there until a restore); to sysmem what is left in the window: what
        // the compositor maps too, or when no vidmem outside it is free.
        //
        (void)_vidmemEvictPidLocked(pGpu, 3, pProcs[i].pid, NULL, 0, NULL, &moved);
        *pMovedMB += moved >> 20;
        *pInVidmemMB += moved >> 20;
        if (bSelf || !_vidmemPidHasInRange(pProcs[i].pid))
            continue;
        moved = 0;
        (void)_vidmemEvictPidLocked(pGpu, 1, pProcs[i].pid, NULL, 0, NULL, &moved);
        *pMovedMB += moved >> 20;
    }
    _vidmemEvictRangeLo = _vidmemEvictRangeHi = 0;

    portMemFree(pUsed);
    pUsed = vidmemPmaUsedMap(pPma, &base, &frames, &largest);
    portMemSet(pMovable, 0, words * sizeof(NvU64));
    bOk = (pUsed != NULL) &&
          (_vidmemEvictBestWindow(pUsed, pMovable, NULL, pCost, base, off, chunks, need, align, NULL) == 0);
    _vidmemContigInVidmemMB += inVidmem;
    NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: window %s: %llu MB moved, %llu of them inside vidmem\n",
              bOk ? "cleared" : "not cleared", *pMovedMB, inVidmem);

done:
    portMemFree(pUsed);
    portMemFree(pMovable);
    portMemFree(pHeavy);
    portMemFree(pCost);
    portMemFree(pRefs);
    return bOk;
}

//
// Background compaction (BACKLOG #16), one step: the window of 2 MB chunks
// that `budgetMB` of moving makes the longest free run, holding only memory
// that may move (_vidmemCompactVictim(): never the foreground's), is cleared
// to vidmem pages outside it. Allocations bigger than the budget count as fixed: moving one
// whole for a corner of it would hold the locks too long.
//
static NvU32 _vidmemCompactSteps;
static NvU64 _vidmemCompactMovedMB;
//
// Whose memory compaction may move. Not the rules of moving out
// (_vidmemEvictHintVictimOk()): the memory stays in vidmem, nobody renders
// from sysmem afterwards; the price is a pause of the owner's channels for
// the copy, a few ms a step. So: anyone but the window in focus (its frames
// come first) and the compositor with its X server (their pause is everyone's
// pause). With only the idle ones movable, a desktop with a game on screen
// had 31 MB movable of 3.9 GB in use and compaction stood still
// (2026-11-10: game 2186 MB "drawing", plasmashell 370 MB "never").
//
static NvBool
_vidmemCompactVictim(VIDMEM_EVICT_PROC *p)
{
    if ((VE_HINT_STATE(p->hint) == VE_HINT_FG) || (p->vidmemBytes == 0))
        return NV_FALSE;
    return (portStringCompare(p->name, VIDMEM_EVICT_COMPOSITOR, NV_PROC_NAME_MAX_LENGTH) != 0) &&
           (portStringCompare(p->name, "Xwayland", NV_PROC_NAME_MAX_LENGTH) != 0);
}

// What the last step saw in vidmem, by why it could or could not move (bytes).
enum { VC_MOVABLE, VC_DESKTOP, VC_FG, VC_SHARED, VC_BIG, VC_REFUSED, VC_COUNT };
static NvU64 _vidmemCompactSeen[VC_COUNT];
static NvU64 _vidmemCompactSeenUsed;

NvU64
vepPlatCompact(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 budgetMB, NvU64 wantMB)
{
    Heap   *pHeap = GPU_GET_HEAP(pGpu);
    PMA    *pPma = (pHeap != NULL) ? pHeap->pPmaObject : NULL;
    NvU64   base = 0, frames = 0, largest = 0, words, chunks, off, c, f;
    NvU64   runStart = 0, runLen = 0, cur = 0, freeChunks = 0;
    NvU64   budgetFrames = (budgetMB << 20) >> PMA_PAGE_SHIFT, cost, lo, hi;
    NvU64  *pUsed, *pMovable = NULL, movedMB = 0;
    NvU32  *pCost = NULL;
    VIDMEM_EVICT_MAPREF *pRefs = NULL;
    NvU32   i, victims = 0;
    const char *why = "";

    if (pPma == NULL)
        return 0;
    pUsed = vidmemPmaUsedMap(pPma, &base, &frames, &largest);
    if (pUsed == NULL)
        return 0;
    words  = (frames + 63) >> 6;
    off    = (NV_ALIGN_UP64(base, 2ULL << 20) - base) >> PMA_PAGE_SHIFT;
    chunks = (frames > off) ? (frames - off) / VIDMEM_EVICT_CHUNK_FRAMES : 0;
    pMovable = portMemAllocNonPaged(words * sizeof(NvU64));
    pCost    = portMemAllocNonPaged(NV_MAX(chunks, 1) * sizeof(NvU32));
    pRefs    = portMemAllocNonPaged(sizeof(*pRefs) * VIDMEM_EVICT_MAX_MAPPINGS);
    if ((pMovable == NULL) || (pCost == NULL) || (pRefs == NULL) || (chunks == 0))
        goto done;
    portMemSet(pMovable, 0, words * sizeof(NvU64));

    // Frames in use per chunk for now; the longest free run.
    for (c = 0; c < chunks; c++)
    {
        pCost[c] = (NvU32)_vidmemFramesUsed(pUsed, off + c * VIDMEM_EVICT_CHUNK_FRAMES, VIDMEM_EVICT_CHUNK_FRAMES);
        if (pCost[c] == 0)
        {
            freeChunks++;
            if (++cur > runLen)
            {
                runLen   = cur;
                runStart = c + 1 - cur;
            }
        }
        else
        {
            cur = 0;
        }
    }
    //
    // The run wanted: the knob, or three quarters of what is free when that
    // is more. A fixed 1 GB was "reached" on a desktop with 10.6 GB free in 88
    // runs, the longest 1.2 GB, and room for a 32 MB buffer of the compositor
    // then took moving 137 MB (2026-11-10, nvtest focus and comproom failed).
    //
    wantMB = NV_MAX(wantMB, (freeChunks << 1) * 3 / 4);
    // Long enough already, or nothing free elsewhere to gather (and to move into).
    if ((runLen << 1) >= wantMB)
        goto done;
    if ((runLen == 0) || (freeChunks < runLen + (budgetMB >> 1) + 16))
    {
        why = "too little free outside the run";
        goto done;
    }

    //
    // What could move: allocations no bigger than the budget, of everyone
    // but the foreground and the compositor (the move pauses the owner for
    // the copy, the memory stays in vidmem).
    //
    portMemSet(_vidmemCompactSeen, 0, sizeof(_vidmemCompactSeen));
    _vidmemCompactSeenUsed = _vidmemFramesUsed(pUsed, 0, frames) << PMA_PAGE_SHIFT;
    for (i = 0; i < count; i++)
    {
        RmClient **ppClient;
        NvBool     bVictim = _vidmemCompactVictim(&pProcs[i]);
        NvU32      cat = (VE_HINT_STATE(pProcs[i].hint) == VE_HINT_FG) ? VC_FG : VC_DESKTOP;

        if (bVictim)
            victims++;
        for (ppClient = serverutilGetFirstClientUnderLock(); ppClient != NULL;
             ppClient = serverutilGetNextClientUnderLock(ppClient))
        {
            RsClient   *pRsClient = staticCast(*ppClient, RsClient);
            RS_ITERATOR iter;

            if ((pRsClient->type == CLIENT_TYPE_KERNEL) || ((*ppClient)->ProcID != pProcs[i].pid))
                continue;
            iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0, RS_ITERATE_DESCENDANTS, NV_TRUE);
            while (clientRefIterNext(iter.pClient, &iter))
            {
                Memory *pMemory = dynamicCast(iter.pResourceRef->pResource, Memory);
                NvU32   numRefs = 0;

                NvU64   bytes;

                if ((pMemory == NULL) || (pMemory->pMemDesc == NULL) ||
                    (iter.pResourceRef->externalClassId != NV01_MEMORY_LOCAL_USER) ||
                    (pMemory->pMemDesc->pLocalEvictState != NULL) ||
                    (memdescGetAddressSpace(pMemory->pMemDesc) != ADDR_FBMEM) ||
                    (pProcs[i].bNever && !pMemory->isMemDescOwner))
                    continue;
                bytes = memdescGetSize(pMemory->pMemDesc);
                if (!bVictim)
                {
                    _vidmemCompactSeen[cat] += bytes;
                    continue;
                }
                if (bytes > (budgetMB << 20))
                {
                    _vidmemCompactSeen[VC_BIG] += bytes;
                    continue;
                }
                if (_vidmemEvictCheck(pGpu, iter.pResourceRef, pProcs[i].pid, pRefs, &numRefs) != NULL)
                {
                    _vidmemCompactSeen[VC_REFUSED] += bytes;
                    continue;
                }
                if (_vidmemEvictHasCompositor(pRefs, numRefs))
                {
                    _vidmemCompactSeen[VC_SHARED] += bytes;
                    continue;
                }
                _vidmemCompactSeen[VC_MOVABLE] += bytes;
                (void)_vidmemMemDescFrames(pMemory->pMemDesc, 0, 0, pMovable, base, frames);
            }
        }
    }
    // A chunk with a frame nobody can move ends the run for good.
    for (c = 0; c < chunks; c++)
    {
        if (pCost[c] == 0)
            continue;
        for (f = off + c * VIDMEM_EVICT_CHUNK_FRAMES; f < off + (c + 1) * VIDMEM_EVICT_CHUNK_FRAMES; f++)
        {
            if (((pUsed[f >> 6] >> (f & 63)) & 1) && !((pMovable[f >> 6] >> (f & 63)) & 1))
            {
                pCost[c] = NV_U32_MAX;
                break;
            }
        }
    }

    //
    // The longest window that `budgetMB` of moving turns into one free run,
    // among the stretches without a fixed frame that are longer than the run
    // there is now (shorter ones can never beat it). Step after step the same
    // window grows: what is free in it costs nothing the next time.
    //
    lo = hi = 0;
    {
        NvU64 seg = 0, best = 0, bestCost = 0;

        for (c = 0; c <= chunks; c++)
        {
            NvU64 l, r;

            if ((c < chunks) && (pCost[c] != NV_U32_MAX))
                continue;
            if (c - seg > runLen)
            {
                for (l = r = seg, cost = 0; r < c; r++)
                {
                    cost += pCost[r];
                    while (cost > budgetFrames)
                        cost -= pCost[l++];
                    if ((cost != 0) && ((r + 1 - l > best) || ((r + 1 - l == best) && (cost < bestCost))))
                    {
                        best     = r + 1 - l;
                        bestCost = cost;
                        lo       = l;
                        hi       = r + 1;
                    }
                }
            }
            seg = c + 1;
        }
    }
    if (hi == lo)
    {
        why = "no stretch longer than the run is free of memory that cannot be moved now";
        goto done;
    }

    _vidmemEvictRangeLo     = base + ((off + lo * VIDMEM_EVICT_CHUNK_FRAMES) << PMA_PAGE_SHIFT);
    _vidmemEvictRangeHi     = base + ((off + hi * VIDMEM_EVICT_CHUNK_FRAMES) << PMA_PAGE_SHIFT);
    _vidmemRelocateMaxBytes = budgetMB << 20;
    // The run there is stays free, unless the window is growing that very run.
    if ((runStart >= hi) || (runStart + runLen <= lo))
    {
        _vidmemRelocateKeepLo = base + ((off + runStart * VIDMEM_EVICT_CHUNK_FRAMES) << PMA_PAGE_SHIFT);
        _vidmemRelocateKeepHi = base + ((off + (runStart + runLen) * VIDMEM_EVICT_CHUNK_FRAMES) << PMA_PAGE_SHIFT);
    }
    for (i = 0; i < count; i++)
    {
        NvU64 moved = 0;

        if (!_vidmemCompactVictim(&pProcs[i]) || !_vidmemPidHasInRange(pProcs[i].pid))
            continue;
        (void)_vidmemEvictPidLocked(pGpu, 3, pProcs[i].pid, NULL, 0, NULL, &moved);
        movedMB += moved >> 20;
    }
    _vidmemEvictRangeLo = _vidmemEvictRangeHi = 0;
    _vidmemRelocateMaxBytes = 0;
    _vidmemRelocateKeepLo = _vidmemRelocateKeepHi = 0;
    if (movedMB != 0)
    {
        _vidmemCompactSteps++;
        _vidmemCompactMovedMB += movedMB;
    }

done:
    // Said when a step could do nothing (the policy then rests for a while).
    if (why[0] != '\0')
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: compaction: longest free run %llu MB of %llu MB wanted, %llu MB free in chunks, %u processes may be moved: %s\n",
                  runLen << 1, wantMB, freeChunks << 1, victims, why);
    portMemFree(pUsed);
    portMemFree(pMovable);
    portMemFree(pCost);
    portMemFree(pRefs);
    return movedMB;
}

static NvU32 _vidmemCompRooms, _vidmemCompRoomFails;
static NvU64 _vidmemCompRoomMovedMB, _vidmemCompRoomMaxNs;

NvBool
vepPlatChunkInfo(OBJGPU *pGpu, NvU64 *pFreeChunks, NvU64 *pLongest)
{
    Heap *pHeap = GPU_GET_HEAP(pGpu);

    return (pHeap != NULL) && _vidmemEvictChunkInfo(pHeap->pPmaObject, pFreeChunks, pLongest);
}

NvBool
vepPlatMakeContig(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 size, NvU64 align,
                  NvU32 forPid, NvU32 level, NvU64 *pMovedMB)
{
    return _vidmemEvictMakeContig(pGpu, pProcs, count, size, align, forPid, level, pMovedMB);
}

//
// No reserve: an allocation of the compositor (kwin_wayland, Xwayland, a
// kernel thread) just failed: make room (vepMakeRoomCompositorRun()), the
// caller tries again. Needs the API lock for writing (vidmemConstruct()
// holds it).
//
NvBool
vidmemEvictMakeRoomCompositor(OBJGPU *pGpu, RmClient *pRmClient, NvU64 size, NvBool bContig, NvU64 align)
{
    static NvU32       lockLogged;
    NvU64              t0 = VMT_NOW(), avail = 0, wantMB = 0, movedMB = 0, dt;
    NvBool             bOk;
    NvBool             bLocked = NV_FALSE;

    if (!_vidmemAutoActive() || _vidmemEvictIsBusyThread())
        return NV_FALSE;
    if (!rmapiLockIsWriteOwner())
    {
        if (lockLogged++ < 4)
            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: cannot make room for the compositor (%s) here: no API write lock\n",
                      (pRmClient != NULL) ? pRmClient->name : "kernel");
        return NV_FALSE;
    }
    if (!rmGpuLockIsOwner() && !rmDeviceGpuLockIsOwner(pGpu->gpuInstance))
    {
        if (rmDeviceGpuLocksAcquire(pGpu, GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_MEM) != NV_OK)
            return NV_FALSE;
        bLocked = NV_TRUE;
    }
    bOk = vepMakeRoomCompositorRun(pGpu, size, bContig, align, &wantMB, &avail, &movedMB);
    if (bLocked)
        rmDeviceGpuLocksRelease(pGpu, GPUS_LOCK_FLAGS_NONE, NULL);

    dt = VMT_NOW() - t0;
    _vidmemCompRooms++;
    _vidmemCompRoomFails += !bOk;
    _vidmemCompRoomMovedMB += movedMB;
    _vidmemCompRoomMaxNs = NV_MAX(_vidmemCompRoomMaxNs, dt);
    NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: compositor: %s %llu KB%s for %s: %s, %llu MB moved in %llu us\n",
              bOk ? "room made for" : "no room for", size >> 10, bContig ? " contiguous" : "",
              (pRmClient != NULL) ? pRmClient->name : "kernel", bOk ? "retrying" : "it fails",
              movedMB, dt / 1000);
    if (osVidmemTraceOn(VMT_MAKEROOM))
    {
        NvU64 a[VMT_ARGS] = { size, wantMB, avail, bOk, dt, VE_LEVEL_COMPOSITOR };
        osVidmemTrace(VMT_MAKEROOM, (pRmClient != NULL) ? pRmClient->ProcID : osGetCurrentProcess(), a);
    }
    return bOk;
}

//
// An allocation of an ordinary process found no place in PMA although the
// free memory looked enough (64 KB holes, 2 MB pages; BACKLOG #7: the WebGPU
// page focused next to a game got some buffers and then none). The
// foreground and visible processes get whole chunks made free, as the
// compositor; the caller tries once more. Same locking as the compositor's.
//
NvBool
vidmemEvictMakeRoomPlace(OBJGPU *pGpu, RmClient *pRmClient, NvU64 size, NvBool bContig, NvU64 align)
{
    static NvU32 failLogged;
    NvU64        t0 = VMT_NOW(), avail = 0, wantMB = 0, movedMB = 0, dt;
    NvU32        level;
    NvBool       bOk;
    NvBool       bLocked = NV_FALSE;

    if ((pRmClient == NULL) || _vidmemEvictIsBusyThread() || !rmapiLockIsWriteOwner())
        return NV_FALSE;
    level = vepMakeRoomPlaceLevel(pRmClient->ProcID);
    if (level == NV_U32_MAX)
        return NV_FALSE;
    if (!rmGpuLockIsOwner() && !rmDeviceGpuLockIsOwner(pGpu->gpuInstance))
    {
        if (rmDeviceGpuLocksAcquire(pGpu, GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_MEM) != NV_OK)
            return NV_FALSE;
        bLocked = NV_TRUE;
    }
    bOk = vepMakeRoomPlaceRun(pGpu, pRmClient->ProcID, size, bContig, align, level, &wantMB, &avail, &movedMB);
    if (bLocked)
        rmDeviceGpuLocksRelease(pGpu, GPUS_LOCK_FLAGS_NONE, NULL);
    dt = VMT_NOW() - t0;
    if (bOk || (failLogged++ < 8) || ((failLogged % 256) == 0))
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: %s %llu KB%s of %s(%u): %llu MB moved in %llu us\n",
                  bOk ? "room made to place" : "no place for", size >> 10, bContig ? " contiguous" : "",
                  pRmClient->name, pRmClient->ProcID, movedMB, dt / 1000);
    if (osVidmemTraceOn(VMT_MAKEROOM))
    {
        NvU64 a[VMT_ARGS] = { size, wantMB, avail, bOk, dt, level };
        osVidmemTrace(VMT_MAKEROOM, pRmClient->ProcID, a);
    }
    return bOk;
}

//
// Context buffers of a new channel found no room (ctxBufPoolReserve(),
// BACKLOG #13): make whole chunks free for `size` as for an allocation of
// `pid`, the process creating the channel. Needs the API lock for writing
// (channel allocations hold it); takes the GPU lock if not held.
//
NvBool
vidmemEvictMakeRoomForKernel(OBJGPU *pGpu, NvU32 pid, NvU64 size)
{
    static NvU32 logged;
    NvU64        t0 = VMT_NOW(), avail = 0, wantMB = 0, movedMB = 0;
    NvU32        level;
    NvBool       bOk;
    NvBool       bLocked = NV_FALSE;

    if (_vidmemEvictIsBusyThread() || !rmapiLockIsWriteOwner())
        return NV_FALSE;
    level = vepMakeRoomContextsLevel(pid);
    if (level == NV_U32_MAX)
        return NV_FALSE;
    if (!rmGpuLockIsOwner() && !rmDeviceGpuLockIsOwner(pGpu->gpuInstance))
    {
        if (rmDeviceGpuLocksAcquire(pGpu, GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_MEM) != NV_OK)
            return NV_FALSE;
        bLocked = NV_TRUE;
    }
    bOk = vepMakeRoomPlaceRun(pGpu, pid, size, NV_FALSE, 0, level, &wantMB, &avail, &movedMB);
    if (bLocked)
        rmDeviceGpuLocksRelease(pGpu, GPUS_LOCK_FLAGS_NONE, NULL);
    if (bOk || (logged++ < 8) || ((logged % 256) == 0))
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: %s %llu KB of channel contexts for pid %u: %llu MB moved in %llu us\n",
                  bOk ? "room made for" : "no room for", size >> 10, pid, movedMB, (VMT_NOW() - t0) / 1000);
    return bOk;
}


//
// Move an allocation that vidmemConstruct() just made to sysmem. It has no
// mappings yet, so nothing has to be paused; its content is the scrubbed
// (zeroed) vidmem. GPU lock held.
//
NV_STATUS
vidmemEvictBornAllocation(OBJGPU *pGpu, Memory *pMemory)
{
    RmClient            *pRmClient = dynamicCast(RES_GET_CLIENT(pMemory), RmClient);
    VIDMEM_EVICT_MAPREF *pRefs;
    VIDMEM_EVICT_STATE  *pState = NULL;
    const char          *reason;
    NvU32                numRefs = 0;
    NV_STATUS            status;
    NvU64                startNs = VMT_NOW();

    NV_ASSERT_OR_RETURN(rmGpuLockIsOwner() || rmDeviceGpuLockIsOwner(pGpu->gpuInstance),
                        NV_ERR_INVALID_LOCK_STATE);

    pRefs = portMemAllocNonPaged(sizeof(*pRefs) * VIDMEM_EVICT_MAX_MAPPINGS);
    if (pRefs == NULL)
        return NV_ERR_NO_MEMORY;

    reason = _vidmemEvictCheck(pGpu, RES_GET_REF(pMemory),
                               (pRmClient != NULL) ? pRmClient->ProcID : 0, pRefs, &numRefs);
    if ((reason != NULL) || (numRefs != 0))
    {
        status = NV_ERR_NOT_SUPPORTED;
    }
    else
    {
        _vidmemEvictSetBusy(NV_TRUE);
        status = _vidmemEvictBegin(pGpu, pMemory->pMemDesc, pRefs, 0, &pState);
        if (status == NV_OK)
            status = _vidmemEvictCommit(pGpu, pMemory->pMemDesc, pState, pRefs, 0);
        _vidmemEvictSetBusy(NV_FALSE);
    }

    if (status == NV_OK)
    {
        _vidmemBornAllocs++;
        _vidmemBornBytes += memdescGetSize(pMemory->pMemDesc);
        if ((_vidmemBornAllocs & (_vidmemBornAllocs - 1)) == 0)
            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: created in sysmem instead of denied: %s(%u) %llu KB (%u so far, %llu MB)\n",
                      (pRmClient != NULL) ? pRmClient->name : "?", (pRmClient != NULL) ? pRmClient->ProcID : 0,
                      memdescGetSize(pMemory->pMemDesc) >> 10, _vidmemBornAllocs, _vidmemBornBytes >> 20);
    }
    else
    {
        _vidmemBornFailed++;
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: could not move a new allocation of %s(%u) to sysmem (%s, 0x%x); it stays in vidmem\n",
                  (pRmClient != NULL) ? pRmClient->name : "?", (pRmClient != NULL) ? pRmClient->ProcID : 0,
                  (reason != NULL) ? reason : "-", status);
    }
    if (osVidmemTraceOn(VMT_MOVE))
    {
        NvU64 a[VMT_ARGS] = { 1, VMT_WHY_BORN, (status == NV_OK) ? memdescGetSize(pMemory->pMemDesc) : 0,
                              status == NV_OK, status != NV_OK, 0, 0, VMT_NOW() - startNs };
        osVidmemTrace(VMT_MOVE, (pRmClient != NULL) ? pRmClient->ProcID : 0, a);
    }

    portMemFree(pRefs);
    return status;
}

//
// Called once from video_mem.c when the user reserve is read from the
// registry (first vidmem allocation after load).
//
void
vidmemEvictAutoInit(OBJGPU *pGpu, NvU32 reserveMB)
{
    NvU32 data32;

    if (_vidmemAutoEnabled != NV_U32_MAX)
        return;

    //
    // Default: only with window hints (2026-11-02 10:06-10:10 game test with
    // the sample-based policy: it evicted apps that kept drawing, which then
    // starved the game of GR time and PCIe). Registry RmVidmemAutoEvict:
    // 0 off, 1 also without hints, 2 hints only.
    //
    _vidmemAutoEnabled = VIDMEM_AUTO_HINTS;
    if ((osReadRegistryDword(pGpu, "RmVidmemAutoEvict", &data32) == NV_OK) && (data32 <= 2))
        _vidmemAutoEnabled = data32;
    if ((osReadRegistryDword(pGpu, "RmVidmemEvictLowMB", &data32) == NV_OK) && (data32 != 0))
        _vidmemLowMB = data32;
    if ((osReadRegistryDword(pGpu, "RmVidmemEvictHighMB", &data32) == NV_OK) && (data32 > _vidmemLowMB))
        _vidmemHighMB = data32;
    if (osReadRegistryDword(pGpu, "RmVidmemFgHeadroomMB", &data32) == NV_OK)
        _vidmemFgHeadroomMB = data32;
    if (osReadRegistryDword(pGpu, "RmVidmemBgCushionMB", &data32) == NV_OK)
        _vidmemBgCushionMB = data32;
    if (_vidmemHighMB <= _vidmemLowMB + VIDMEM_EVICT_HYST_MB)
        _vidmemHighMB = _vidmemLowMB + 2 * VIDMEM_EVICT_HYST_MB;

    // Sampling also feeds "status", so the timer runs even with auto off.
    if (osSchedule1HzCallback(pGpu, _vidmemEvict1HzCallback, NULL, NV_OS_1HZ_REPEAT) != NV_OK)
        NV_PRINTF(LEVEL_ERROR, "vidmem evict: cannot schedule the 1 Hz callback\n");

    NV_PRINTF(LEVEL_ERROR, "vidmem evict: auto %s, evict below %u MB free outside the reserve, restore above %u MB\n",
              _vidmemAutoName(), _vidmemLowMB, _vidmemHighMB);
}

//
// Low-water marks of the user reserve block since load: free bytes and the
// largest contiguous free run, with who made the allocation that hit them.
// Updated after every allocation that may go into the block (exempt process
// or kernel thread) and on every policy tick.
//
static NvU64 _vidmemBlockMinFree = NV_U64_MAX;
static NvU64 _vidmemBlockMinLargest = NV_U64_MAX;
static NvU64 _vidmemBlockMinFreeNs, _vidmemBlockMinLargestNs;
static char  _vidmemBlockMinFreeWho[NV_PROC_NAME_MAX_LENGTH];
static char  _vidmemBlockMinLargestWho[NV_PROC_NAME_MAX_LENGTH];

void
vidmemEvictNoteBlock(PMA *pPma)
{
    NvU64 blockBytes, blockFree, blockLargest;
    char  who[NV_PROC_NAME_MAX_LENGTH];

    if (!vidmemGetUserReserveBlockInfo(pPma, &blockBytes, &blockFree, &blockLargest) || (blockBytes == 0))
        return;

    if ((blockFree >= _vidmemBlockMinFree) && (blockLargest >= _vidmemBlockMinLargest))
        return;

    portMemSet(who, 0, sizeof(who));
    if ((osGetCurrentProcessFlags() & OS_CURRENT_PROCESS_FLAG_KERNEL_THREAD) != 0)
        portStringCopy(who, sizeof(who), "(kernel)", sizeof("(kernel)"));
    else
        osGetCurrentProcessName(who, sizeof(who));

    if (blockFree < _vidmemBlockMinFree)
    {
        _vidmemBlockMinFree = blockFree;
        _vidmemBlockMinFreeNs = osGetMonotonicTimeNs();
        portMemCopy(_vidmemBlockMinFreeWho, sizeof(who), who, sizeof(who));
    }
    if (blockLargest < _vidmemBlockMinLargest)
    {
        _vidmemBlockMinLargest = blockLargest;
        _vidmemBlockMinLargestNs = osGetMonotonicTimeNs();
        portMemCopy(_vidmemBlockMinLargestWho, sizeof(who), who, sizeof(who));
    }
}

static void
_vidmemEvictPrintBlock(OBJGPU *pGpu, char *buf, NvU32 bufSize, NvU32 *pLen)
{
    Heap  *pHeap = GPU_GET_HEAP(pGpu);
    NvU64  blockBytes = 0, blockFree = 0, blockLargest = 0;
    NvU64  nowNs = osGetMonotonicTimeNs();

    if (pHeap == NULL)
        return;
    if (!vidmemGetUserReserveBlockInfo(pHeap->pPmaObject, &blockBytes, &blockFree, &blockLargest))
    {
        NvU64  base = 0, frames = 0, largest = 0;
        NvU64 *pUsed = vidmemPmaUsedMap(pHeap->pPmaObject, &base, &frames, &largest);

        VIDMEM_FRAG fr;

        portMemFree(pUsed);
        EVICT_PRINTF("no reserve: largest contiguous free %llu MB; background cushion %u MB; "
                     "room made for the compositor %u times (%u failed), %llu MB moved, longest %llu us\n",
                     largest >> 20, _vidmemBgCushionMB, _vidmemCompRooms, _vidmemCompRoomFails,
                     _vidmemCompRoomMovedMB, _vidmemCompRoomMaxNs / 1000);
        EVICT_PRINTF("windows cleared for contiguous allocations: %llu MB moved inside vidmem; "
                     "background compaction: keeps a free run of %u MB, %u steps, %llu MB moved\n",
                     _vidmemContigInVidmemMB, _vidmemCompactRunMB, _vidmemCompactSteps, _vidmemCompactMovedMB);
        EVICT_PRINTF("compaction saw (MB): in use %llu; movable %llu; compositor %llu, foreground %llu, "
                     "shared with the compositor %llu, bigger than a step %llu, refused %llu\n",
                     _vidmemCompactSeenUsed >> 20, _vidmemCompactSeen[VC_MOVABLE] >> 20,
                     _vidmemCompactSeen[VC_DESKTOP] >> 20, _vidmemCompactSeen[VC_FG] >> 20,
                     _vidmemCompactSeen[VC_SHARED] >> 20, _vidmemCompactSeen[VC_BIG] >> 20,
                     _vidmemCompactSeen[VC_REFUSED] >> 20);
        if (_vidmemFragStats(pGpu, &fr))
            EVICT_PRINTF("fragmentation: free %llu MB of %llu; in whole 2 MB chunks %llu MB in %u runs "
                         "(%u of 32 MB or more, %u of 128 MB or more, longest %llu MB); in holes %llu MB "
                         "in %u partly used chunks (%llu MB in use there)\n",
                         fr.freeBytes >> 20, fr.totalBytes >> 20, fr.chunkBytes >> 20, fr.runs, fr.runs32,
                         fr.runs128, fr.longestBytes >> 20, (fr.freeBytes - fr.chunkBytes) >> 20, fr.partial,
                         fr.partialUsedBytes >> 20);
        return;
    }
    vidmemEvictNoteBlock(pHeap->pPmaObject);

    EVICT_PRINTF("reserve block %llu MB: free %llu MB (largest %llu MB); lowest since load: free %llu MB (%llu s ago, %s), largest %llu MB (%llu s ago, %s)\n",
                 blockBytes >> 20, blockFree >> 20, blockLargest >> 20,
                 _vidmemBlockMinFree >> 20, (nowNs - _vidmemBlockMinFreeNs) / 1000000000ULL,
                 _vidmemBlockMinFreeWho,
                 _vidmemBlockMinLargest >> 20, (nowNs - _vidmemBlockMinLargestNs) / 1000000000ULL,
                 _vidmemBlockMinLargestWho);
}

//
// Research report for shared vidmem: for every allocation of `pid` that is
// duplicated (DupCount > 1) or imported (not the memdesc owner), list every
// Memory object in any client (kernel clients too) that points at the same
// memdesc, with its owner and GPU/CPU mapping counts. API lock RW, GPU lock.
//
typedef struct
{
    char  sig[160];
    NvU32 count;
    NvU64 bytes;
} VIDMEM_EVICT_SIG;

static void
_vidmemEvictShared(OBJGPU *pGpu, NvU32 pid, char *buf, NvU32 bufSize, NvU32 *pLen)
{
    VIDMEM_EVICT_SIG *pSigs;
    RmClient        **ppClient;
    NvU32             numSigs = 0, shown = 0, i;
    NvU64             total = 0;

    pSigs = portMemAllocNonPaged(sizeof(*pSigs) * 64);
    if (pSigs == NULL)
        return;

    EVICT_PRINTF("shared pid %u\n# handle size_kb kind contig ref child: holders name(pid)[o=owner,k=kernel client,gN=gpu maps,cN=cpu maps]\n", pid);

    for (ppClient = serverutilGetFirstClientUnderLock();
         ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RsClient   *pRsClient = staticCast(*ppClient, RsClient);
        RS_ITERATOR iter;

        if ((pRsClient->type == CLIENT_TYPE_KERNEL) || ((*ppClient)->ProcID != pid))
            continue;

        iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0, RS_ITERATE_DESCENDANTS, NV_TRUE);
        while (clientRefIterNext(iter.pClient, &iter))
        {
            RsResourceRef     *pRef = iter.pResourceRef;
            Memory            *pMemory = dynamicCast(pRef->pResource, Memory);
            MEMORY_DESCRIPTOR *pMD;
            RmClient         **ppOther;
            char               sig[160];
            char               line[512];
            NvU32              sigLen = 0, lineLen = 0;

            if ((pMemory == NULL) || (pMemory->pMemDesc == NULL) ||
                (pRef->externalClassId != NV01_MEMORY_LOCAL_USER) ||
                (memdescGetAddressSpace(pMemory->pMemDesc) != ADDR_FBMEM))
                continue;
            pMD = pMemory->pMemDesc;
            if ((pMD->DupCount <= 1) && pMemory->isMemDescOwner)
                continue;

            // Only report each memdesc once per process (first Memory object seen).
            sig[0] = line[0] = '\0';

            for (ppOther = serverutilGetFirstClientUnderLock();
                 ppOther != NULL;
                 ppOther = serverutilGetNextClientUnderLock(ppOther))
            {
                RsClient   *pOtherRs = staticCast(*ppOther, RsClient);
                RS_ITERATOR it2 = serverutilRefIter(pOtherRs->hClient, NV01_NULL_OBJECT, 0,
                                                    RS_ITERATE_DESCENDANTS, NV_TRUE);

                while (clientRefIterNext(it2.pClient, &it2))
                {
                    Memory *pOther = dynamicCast(it2.pResourceRef->pResource, Memory);
                    int     n;

                    if ((pOther == NULL) || (pOther->pMemDesc != pMD))
                        continue;

                    n = nvDbgSnprintf(line + lineLen, sizeof(line) - lineLen, " %s(%u)[%s%s g%u c%u]",
                                      (*ppOther)->name, (*ppOther)->ProcID,
                                      pOther->isMemDescOwner ? "o" : "",
                                      (pOtherRs->type == CLIENT_TYPE_KERNEL) ? "k" : "",
                                      listCount(&it2.pResourceRef->interBackRefsMappable),
                                      listCount(&it2.pResourceRef->cpuMappings));
                    if (n > 0)
                        lineLen = NV_MIN(lineLen + (NvU32)n, sizeof(line) - 1);

                    if ((*ppOther)->ProcID != pid)
                    {
                        n = nvDbgSnprintf(sig + sigLen, sizeof(sig) - sigLen, "%s%s%s",
                                          (sigLen != 0) ? "+" : "",
                                          (pOtherRs->type == CLIENT_TYPE_KERNEL) ? "k:" : "",
                                          (*ppOther)->name);
                        if (n > 0)
                            sigLen = NV_MIN(sigLen + (NvU32)n, sizeof(sig) - 1);
                    }
                }
            }
            if (sigLen == 0)
                portStringCopy(sig, sizeof(sig), "(same process only)", sizeof("(same process only)"));

            for (i = 0; i < numSigs; i++)
            {
                if (portStringCompare(pSigs[i].sig, sig, sizeof(sig)) == 0)
                    break;
            }
            if ((i == numSigs) && (numSigs < 64))
            {
                portStringCopy(pSigs[i].sig, sizeof(pSigs[i].sig), sig, sizeof(sig));
                pSigs[i].count = 0;
                pSigs[i].bytes = 0;
                numSigs++;
            }
            if (i < numSigs)
            {
                pSigs[i].count++;
                pSigs[i].bytes += memdescGetSize(pMD);
            }
            total += memdescGetSize(pMD);

            if (*pLen + 1024 < bufSize - 8192)
            {
                EVICT_PRINTF("0x%x %llu 0x%x %u %u %u:%s\n", RES_GET_HANDLE(pMemory),
                             memdescGetSize(pMD) >> 10, memdescGetPteKind(pMD),
                             memdescGetContiguity(pMD, AT_GPU) ? 1 : 0, pMD->RefCount,
                             pMD->childDescriptorCnt, line);
                shown++;
            }
        }
    }

    EVICT_PRINTF("summary by other holders (%llu MB total):\n", total >> 20);
    for (i = 0; i < numSigs; i++)
        EVICT_PRINTF("  %6llu MB %4u allocs  %s\n", pSigs[i].bytes >> 20, pSigs[i].count, pSigs[i].sig);

    portMemFree(pSigs);
}

// GPU lock held.
static void
_vidmemEvictStatus(OBJGPU *pGpu, char *buf, NvU32 bufSize, NvU32 *pLen)
{
    VIDMEM_EVICT_PROC *pProcs;
    NvU32 count, busiest, i;

    pProcs = portMemAllocNonPaged(sizeof(*pProcs) * VIDMEM_EVICT_MAX_PROCS);
    if (pProcs == NULL)
        return;

    _vidmemActivitySample(pGpu);
    _vidmemActivitySampleSubmits(pGpu);
    count   = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);
    busiest = _vidmemEvictBusiestPid(pProcs, count);
    (void)busiest;

    EVICT_PRINTF("auto %s, evict below %u MB, restore above %u MB; free outside reserve %llu MB; in sysmem %u allocs %llu MB\n",
                 _vidmemAutoName(), _vidmemLowMB, _vidmemHighMB,
                 vepPlatAvailMB(pGpu), _vidmemEvictAllocs, _vidmemEvictBytes >> 20);
    EVICT_PRINTF("created in sysmem instead of denied: %u allocs %llu MB (%u failed); busiest pid %u; window buffers %s; quiesce %u\n",
                 _vidmemBornAllocs, _vidmemBornBytes >> 20, _vidmemBornFailed, _vidmemBusiestPid,
                 _vidmemEvictWindows ? "evicted too" : "kept in vidmem", _vidmemEvictQuiesce);
    _vidmemEvictPrintBlock(pGpu, buf, bufSize, pLen);
    EVICT_PRINTF("hot memory: %s; notifications %llu (sysmem %llu, on evicted allocations %llu), hot allocations restored %llu\n",
                 !_vidmemHotEnabled ? "off" :
                 (!_vidmemHot.bInit ? "on, not started yet" : (_vidmemHot.bFailed ? "on, access counters unavailable" : "on")),
                 _vidmemHot.notifs, _vidmemHot.sysNotifs, _vidmemHot.matched, _vidmemHot.restored);
    EVICT_PRINTF("window hints: %s; could give up %llu MB to visible apps, %llu MB to the foreground (added to their budgets)\n",
                 _vidmemHintsActive() ? "on (policy follows window visibility)" : "off (GPU samples)",
                 _vidmemReclaimableBytes >> 20, _vidmemReclaimableFgBytes >> 20);
    EVICT_PRINTF("%-8s %-16s %7s %7s %8s %7s %7s %-7s %s\n",
                 "pid", "name", "vram_mb", "ram_mb", "evict_mb", "idle_s", "samples", "window", "flags");

    for (i = 0; i < count; i++)
    {
        NvU32 idle = _vidmemActivityIdleSec(pProcs[i].pid);
        char  idleStr[16];

        if ((pProcs[i].vidmemBytes == 0) && (pProcs[i].evictedBytes == 0))
            continue;
        if (idle == NV_U32_MAX)
            portStringCopy(idleStr, sizeof(idleStr), "-", 2);
        else
            nvDbgSnprintf(idleStr, sizeof(idleStr), "%u", idle);

        static const char * const hintNames[] = { "-", "hidden", "visible", "fg" };
        char hintStr[16];

        nvDbgSnprintf(hintStr, sizeof(hintStr), "%s%s", hintNames[VE_HINT_STATE(pProcs[i].hint) & 3],
                      (pProcs[i].hint & VE_HINT_INHERITED) ? "^" : "");
        EVICT_PRINTF("%-8u %-16.16s %7llu %7llu %8llu %7s %7u %-7s %s%s\n",
                     pProcs[i].pid, pProcs[i].name,
                     pProcs[i].vidmemBytes >> 20, pProcs[i].evictedBytes >> 20,
                     pProcs[i].eligibleBytes >> 20, idleStr, _vidmemActivityRecent(pProcs[i].pid), hintStr,
                     pProcs[i].bNever ? "never " : "",
                     (pProcs[i].pid == _vidmemBusiestPid) ? "busiest" : "");
    }

    portMemFree(pProcs);
}

//
// procfs entry point. Caller holds the API lock for writing.
//   mode 0: dry run, 1: evict, 2: restore (arg = pid)
//   mode 3: status (arg ignored)
//   mode 4: automatic policy off/on (arg = 0/1)
//   mode 5: shared-memory report for a process (arg = pid)
//   mode 6: log every remap off/on (arg = 0/1)
//   mode 7: evict window buffers (shared with the compositor) off/on (arg = 0/1)
//
NV_STATUS
vidmemEvictProcess
(
    OBJGPU *pGpu,
    NvU32   mode,
    NvU32   arg,
    char   *buf,
    NvU32   bufSize,
    NvU32  *pLen
)
{
    NV_STATUS status;

    NV_ASSERT_OR_RETURN(rmapiLockIsWriteOwner(), NV_ERR_INVALID_LOCK_STATE);

    status = rmGpuLocksAcquire(GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_MEM);
    if (status != NV_OK)
        return status;

    // 12: move inside vidmem (BACKLOG #16), mode 3 of _vidmemEvictPidLocked().
    if (mode == 13)
    {
        _vidmemCompactRunMB = arg;
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: background compaction keeps a free run of %u MB\n", arg);
    }
    else if (mode == 12)
        status = _vidmemEvictPidLocked(pGpu, 3, arg, buf, bufSize, pLen, NULL);
    else if (mode <= 2)
    {
        _vidmemEvictWhy = VMT_WHY_MANUAL;
        status = _vidmemEvictPidLocked(pGpu, mode, arg, buf, bufSize, pLen, NULL);
    }
    else if (mode == 3)
    {
        _vidmemEvictStatus(pGpu, buf, bufSize, pLen);
    }
    else if (mode == 5)
    {
        _vidmemEvictShared(pGpu, arg, buf, bufSize, pLen);
    }
    else if (mode == 6)
    {
        _vidmemEvictTrace = (arg != 0);
        EVICT_PRINTF("trace %s\n", _vidmemEvictTrace ? "on" : "off");
    }
    else if (mode == 7)
    {
        _vidmemEvictWindows = (arg != 0);
        EVICT_PRINTF("window buffers %s\n", _vidmemEvictWindows ? "evicted too" : "kept in vidmem");
    }
    else if (mode == 10)
    {
        _vidmemHotEnabled = (arg != 0);
        if (!_vidmemHotEnabled)
            vidmemEvictHotTeardown(pGpu);
        EVICT_PRINTF("hot memory tracking %s\n", _vidmemHotEnabled ? "on (next policy step)" : "off");
    }
    else if (mode == 9)
    {
        // Window hints changed (procfs vidmem_hints): run the policy now.
        _vidmemEvictPolicyTick(pGpu, 1);
        EVICT_PRINTF("policy run\n");
    }
    else if (mode == 11)
    {
        vidmemTraceLockNs = (NvU64)arg * 1000;
        EVICT_PRINTF("lock waits/holds traced from %u us\n", arg);
    }
    else if (mode == 8)
    {
        if (arg <= VIDMEM_EVICT_QUIESCE_MAX)
            _vidmemEvictQuiesce = arg;
        EVICT_PRINTF("quiesce %u\n", _vidmemEvictQuiesce);
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: quiesce mode %u\n", _vidmemEvictQuiesce);
    }
    else
    {
        _vidmemAutoEnabled = NV_MIN(arg, 2);
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: auto %s\n", _vidmemAutoName());
        EVICT_PRINTF("auto %s\n", _vidmemAutoName());
    }

    rmGpuLocksRelease(GPUS_LOCK_FLAGS_NONE, NULL);
    return status;
}
