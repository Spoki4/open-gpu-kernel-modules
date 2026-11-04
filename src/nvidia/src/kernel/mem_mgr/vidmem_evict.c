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
static NvU64 _vidmemEvictBytes;
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
static NvU64 _vidmemEvictBudgetBytes;

// Process the current evict/restore call works on (recorded in the state).
static NvU32 _vidmemEvictCurPid;

// Who asked for the next evict/restore call (VMT_WHY_*), set by each caller.
static NvU32 _vidmemEvictWhy;

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

        pInfo = intermapGetDmaMapping(pVirtualMemory, pMapping->dmaOffset, NV_U32_MAX);
        if (pInfo == NULL)
            return "map-info";

        if (bCheck)
        {
            if (vaspaceGetFlags(pVas) & (VASPACE_FLAGS_BAR | VASPACE_FLAGS_FLA))
                return "map-bar-fla";
            if (pInfo->pNext != NULL)
                return "map-info";
            if ((pInfo->pMemDesc == NULL) || (pInfo->pMemDesc->_pParentDescriptor != pMemDesc) ||
                (pInfo->pMemDesc->childDescriptorCnt != 0) || (pInfo->pMemDesc->RefCount != 1))
                return "map-memdesc";
            if ((pInfo->KernelVAddr[0] != NULL) || (pInfo->FbApertureLen[0] != 0) ||
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

    for (ppClient = serverutilGetFirstClientUnderLock();
         ppClient != NULL;
         ppClient = serverutilGetNextClientUnderLock(ppClient))
    {
        RmClient   *pRmClient = *ppClient;
        RsClient   *pRsClient = staticCast(pRmClient, RsClient);
        RS_ITERATOR iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0,
                                             RS_ITERATE_DESCENDANTS, NV_TRUE);

        while (clientRefIterNext(iter.pClient, &iter))
        {
            RsResourceRef *pRef = iter.pResourceRef;
            Memory        *pMemory = dynamicCast(pRef->pResource, Memory);
            NvBool         bCompositor = NV_FALSE;

            if ((pMemory == NULL) || (pMemory->pMemDesc != pMemDesc))
                continue;

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
                    continue;
                }
                if (pRmClient->ProcID == pid)
                {
                    if (!bSeenPid && (pRef != pPrimary))
                        return "secondary-holder";
                    bSeenPid = NV_TRUE;
                }
                else if (_vidmemEvictIsCompositor(pRmClient))
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
                bCompositor = (pRsClient->type != CLIENT_TYPE_KERNEL) && _vidmemEvictIsCompositor(pRmClient);
            }

            reason = _vidmemEvictAddHolderMaps(pGpu, pMemDesc, pRef, bCompositor, bCheck, pRefs, pNumRefs);
            if (reason != NULL)
                return reason;
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

static void _vidmemEvictNoteRestoreOnAccess(NvU32 pid);

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

    EVICT_PRINTF("%s pid %u\n", (mode == 0) ? "dry" : ((mode == 1) ? "evict" : "restore"), pid);
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

                // Partial eviction: enough moved (counting deferred ones).
                if (_vidmemEvictBudgetBytes != 0)
                {
                    NvU64 sofar = movedBytes;

                    for (i = 0; i < numPending; i++)
                        sofar += memdescGetSize(pPending[i].pMemDesc);
                    if (sofar >= _vidmemEvictBudgetBytes)
                        continue;
                }

                status = _vidmemEvictBegin(pGpu, pMemDesc, pRefs, numRefs, &pState);
                if ((status == NV_OK) && _vidmemEvictHasCompositor(pRefs, numRefs))
                    bPending = NV_TRUE;
                else if (status == NV_OK)
                    status = _vidmemEvictCommit(pGpu, pMemDesc, pState, pRefs, numRefs);
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
                             (mode == 1) ? "evict" : "restore", status);
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
                             bytes >> 10, (mode == 1) ? "evict" : "restore", st);
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
                 (mode == 0) ? "eligible" : ((mode == 1) ? "evicted" : "restored"),
                 moved, movedBytes >> 20, shared, sharedBytes >> 20, failed, failedBytes >> 20,
                 skipped, skippedBytes >> 20, (VMT_NOW() - startNs) / 1000000);
    EVICT_PRINTF("phases ms: quiesce %llu, sysmem alloc %llu, copy %llu, remap %llu, pma %llu, sysmem free %llu\n",
                 _vidmemEvictPhaseNs[VE_PH_QUIESCE] / 1000000, _vidmemEvictPhaseNs[VE_PH_SYSALLOC] / 1000000,
                 _vidmemEvictPhaseNs[VE_PH_COPY] / 1000000, _vidmemEvictPhaseNs[VE_PH_REMAP] / 1000000,
                 _vidmemEvictPhaseNs[VE_PH_PMA] / 1000000, _vidmemEvictPhaseNs[VE_PH_SYSFREE] / 1000000);
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
    if ((mode != 0) && ((moved != 0) || (failed != 0)))
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: %s pid %u: %u allocs %llu MB (%u with the compositor), failed %u, %llu ms; total %llu MB\n",
                  (mode == 1) ? "evict" : "restore", pid, moved, movedBytes >> 20, numPending, failed,
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
        if ((aperture == NVC365_NOTIFY_BUF_ENTRY_APERTURE_SYS_MEM_COHERENT) ||
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

NvBool vidmemIsReserveExemptName(const char *name);        // video_mem.c
void vidmemEvictNoteBlock(PMA *pPma);
NvBool vidmemGetUserReserveBlockInfo(PMA *pPma, NvU64 *pBlockBytes, NvU64 *pFreeBytes,
                                     NvU64 *pLargestFreeBytes);

#define VIDMEM_EVICT_TICK_SEC       2
#define VIDMEM_EVICT_IDLE_SEC       10
#define VIDMEM_EVICT_QUIET_SEC      30
#define VIDMEM_EVICT_HYST_MB        512
#define VIDMEM_EVICT_MAX_PROCS      128
#define VIDMEM_EVICT_MAX_PER_TICK   2

// Never evicted automatically: the desktop itself.
static const char * const _vidmemEvictNeverNames[] =
{
    "plasmashell",
    // The login screen: its windows belong to the greeter's own compositor,
    // which reports no hints, so it would look like a windowless process.
    "sddm-greeter-qt",
    "sddm-greeter",
};

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
} VIDMEM_EVICT_ACTIVITY;

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

// A process is not evicted in its first seconds, nor right after it needed a
// buffer back (it is still working with them: no evict/restore ping-pong).
#define VIDMEM_EVICT_NEW_PROC_SEC     30
#define VIDMEM_EVICT_AFTER_ACCESS_SEC 60

typedef struct
{
    NvU32 pid;
    char  name[NV_PROC_NAME_MAX_LENGTH];
    NvU32 clients;
    NvU64 vidmemBytes;    // owned, in vidmem
    NvU64 evictedBytes;   // owned, moved to sysmem
    NvU64 eligibleBytes;  // could be moved now
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
// A process that inherits "hidden" from an ancestor of another name (no
// window of its own) is left alone in its first two minutes: a game started
// from Steam has no window while it loads. Helper processes of the same app
// (Chrome's GPU process under the browser) get no such grace.
#define VIDMEM_EVICT_INHERIT_GRACE_SEC 120

NvU32 osGetVidmemHint(NvU32 pid, NvU32 *pGeneration);   // arch/nvalloc/unix/src/os.c

static NvU32 _vidmemHint(NvU32 pid, NvBool *pbActive)
{
    NvU32 gen = 0;
    NvU32 hint = osGetVidmemHint(pid, &gen);

    if (pbActive != NULL)
        *pbActive = (gen != 0);
    return hint;
}

static NvBool _vidmemHintsActive(void)
{
    NvU32 gen = 0;

    (void)osGetVidmemHint(0, &gen);
    return gen != 0;
}

static VIDMEM_EVICT_ACTIVITY _vidmemActivity[VIDMEM_EVICT_MAX_PROCS];
static NvU64  _vidmemActivityNewestTs;
//
// 0 off, 1 on (window hints when present, GPU samples otherwise), 2 (default)
// only while window hints are present: the sample-based policy alone
// mistakes visible, lightly drawing apps for idle ones.
//
#define VIDMEM_AUTO_HINTS 2
static NvU32  _vidmemAutoEnabled = NV_U32_MAX;  // NV_U32_MAX: not initialised

static const char *_vidmemAutoName(void)
{
    return (_vidmemAutoEnabled == 0) ? "off" :
           ((_vidmemAutoEnabled == 1) ? "on" : "with window hints only");
}

// Is the automatic policy running right now?
static NvBool _vidmemAutoActive(void)
{
    if ((_vidmemAutoEnabled == 0) || (_vidmemAutoEnabled == NV_U32_MAX))
        return NV_FALSE;
    return (_vidmemAutoEnabled == 1) || _vidmemHintsActive();
}
static NvU32  _vidmemLowMB = 1024;
static NvU32  _vidmemHighMB = 2560;
static NvU64  _vidmemLastPressureNs;
static NvU32  _vidmemBusiestPid;          // updated every tick
static NvU64  _vidmemBusiestSinceNs;      // since when it is the busiest without a break
static NvU64  _vidmemBornBytes;           // allocations created in sysmem
static NvU32  _vidmemBornAllocs, _vidmemBornFailed;
static NvU32  _vidmemTickCount;
static volatile NvU32 _vidmemWorkQueued;

static VIDMEM_EVICT_ACTIVITY *
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
// Seconds since `pid` last did GPU work (NV_U32_MAX: never seen doing any):
// the shorter of "last GR/NVDEC/NVENC sample" and "last change of its
// channels' GP_PUT". The samples name only the busiest process per sample,
// so a light renderer next to a game never shows up in them (nvtest.sh
// fgprio-anim: a 60 fps Chrome looked idle and was moved out); a moved
// GP_PUT means work was submitted, however light.
//
static NvU32
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

static NvU32
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

static void
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
        (nowNs - _vidmemBusiestSinceNs >= VIDMEM_EVICT_BUSY_TAKEOVER_SEC * 1000000000ULL))
        return NV_FALSE;
    return (pAct != NULL) && (pAct->lastBusiestNs != 0) &&
           (nowNs - pAct->lastBusiestNs < VIDMEM_EVICT_BUSY_STICKY_SEC * 1000000000ULL);
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
        (nowNs - pAct->lastRestoreNs < VIDMEM_EVICT_AFTER_ACCESS_SEC * 1000000000ULL))
        return NV_TRUE;
    if ((pAct->firstSeenTs != 0) && (_vidmemActivityNewestTs >= pAct->firstSeenTs) &&
        (_vidmemActivityNewestTs - pAct->firstSeenTs < VIDMEM_EVICT_NEW_PROC_SEC * 1000000000ULL))
        return NV_TRUE;
    return NV_FALSE;
}

static NvBool
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

// Is pRef the first Memory object of process `pid` (in client scan order) holding pMemDesc?
static NvBool
_vidmemEvictIsFirstPidHolder(MEMORY_DESCRIPTOR *pMemDesc, NvU32 pid, RsResourceRef *pRef)
{
    RmClient **ppClient;

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
// Per-process totals over every non-kernel client. API lock RW, GPU lock held.
// Returns the number of entries filled.
//
static NvU32
_vidmemEvictGatherProcs(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 maxProcs)
{
    VIDMEM_EVICT_MAPREF *pRefs;
    RmClient           **ppClient;
    NvU32                count = 0;

    pRefs = portMemAllocNonPaged(sizeof(*pRefs) * VIDMEM_EVICT_MAX_MAPPINGS);
    if (pRefs == NULL)
        return 0;

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
            (void)_vidmemActivityFind(pProc->pid, NV_TRUE);
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

    {
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
                pAct->lastFgNs = pAct->vidmemAtNs;
            if (VE_HINT_STATE(pProcs[k].hint) != VE_HINT_HIDDEN)
                pAct->hiddenSinceNs = 0;
            else if (pAct->hiddenSinceNs == 0)
                pAct->hiddenSinceNs = pAct->vidmemAtNs;
        }
    }

    portMemFree(pRefs);
    return count;
}

// Free vidmem outside the reserve block, in MB. GPU lock held.
static NvU64
_vidmemEvictAvailMB(OBJGPU *pGpu)
{
    Heap  *pHeap = GPU_GET_HEAP(pGpu);
    NvU64  freeBytes = 0, blockBytes = 0, blockFree = 0, blockLargest = 0;

    if ((pHeap == NULL) || (pHeap->pPmaObject == NULL))
        return 0;
    pmaGetFreeMemory(pHeap->pPmaObject, &freeBytes);
    (void)vidmemGetUserReserveBlockInfo(pHeap->pPmaObject, &blockBytes, &blockFree, &blockLargest);
    return ((freeBytes > blockFree) ? (freeBytes - blockFree) : 0) >> 20;
}

// The process with the most recent engine samples: the one to keep.
static NvU32
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
// "Drawing now": GPU work within the last two policy steps.
#define VIDMEM_EVICT_DRAWING_SEC      4
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
#define VE_LEVEL_HIDDEN     0
#define VE_LEVEL_FG_IDLE    1
//
// RESTORE: + visible ones still drawing, only to bring back the foreground's
// own memory (bounded; the user just switched to it, e.g. clicked Chrome on
// the second monitor while the game keeps drawing on the first: the game
// draws partly from sysmem until it is the foreground again, ~0.1 s). Never
// for allocations or pressure (13:15: that took a drawing Chrome for the
// game's streaming, 1 fps).
//
#define VE_LEVEL_FG_RESTORE 2
//
// COMPOSITOR: anyone but the desktop itself, the foreground included (last):
// the compositor's allocation must not fail (no reserve), a game moved out
// in part only draws slower for a moment (vidmemEvictMakeRoomCompositor()).
//
#define VE_LEVEL_COMPOSITOR 3

//
// A process that was the foreground a moment ago (the game the user alt-tabbed
// away from, or is alt-tabbing back to: the hints say hidden for a moment) is
// not moved out for anybody but the foreground and the compositor (2026-11-04
// death scenario: 241 MB of the game moved out to bring back a minimized
// launcher's 21 MB at the switch back, a 215 ms frame).
//
#define VIDMEM_EVICT_RECENT_FG_SEC 30
//
// "Drawing again" brings a moved-out process back only when it has drawn at
// every policy step for this long: a minimized launcher drawing a frame now
// and then was brought back and moved out again six times (ping-pong).
//
#define VIDMEM_EVICT_STEADY_SEC    6
// Margin on top of what a restore of a non-foreground process needs.
#define VIDMEM_EVICT_RESTORE_MARGIN_MB 64

//
// May `p` be moved out to make room, by the window hints? Hidden (own
// windows, or inherited after the grace period), or no window anywhere in
// its ancestry and idle on the GPU; for the foreground process also visible
// ones (see VE_LEVEL_*). Never the foreground, the caller (`exceptPid`), the
// desktop, or a process that needed a buffer back a moment ago.
//
static NvBool
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
    if ((level == VE_LEVEL_HIDDEN) && (pAct->lastFgNs != 0) &&
        (nowNs - pAct->lastFgNs < VIDMEM_EVICT_RECENT_FG_SEC * 1000000000ULL))
        return NV_FALSE;
    if ((pAct->lastRestoreNs != 0) &&
        (nowNs - pAct->lastRestoreNs < VIDMEM_EVICT_AFTER_ACCESS_SEC * 1000000000ULL))
        return NV_FALSE;

    switch (st)
    {
        case VE_HINT_VISIBLE:
            // Drawing: only to bring back the foreground's own memory.
            if (level == VE_LEVEL_FG_RESTORE)
                return NV_TRUE;
            return (level == VE_LEVEL_FG_IDLE) && (idle >= VIDMEM_EVICT_VISIBLE_IDLE_SEC);

        case VE_HINT_HIDDEN:
            //
            // Drawing now while hidden (work in the last two steps, after it
            // was hidden): no. Work only before it was hidden (alt-tab) or a
            // short burst now and then (a check every 20 s): yes.
            //
            if ((idle < VIDMEM_EVICT_DRAWING_SEC) && (pAct->hiddenSinceNs != 0) &&
                ((nowNs - pAct->hiddenSinceNs) / 1000000000ULL > (NvU64)idle + 1))
                return NV_FALSE;
            if ((p->hint & VE_HINT_OTHER_APP) &&
                (nowNs - pAct->firstSeenNs < VIDMEM_EVICT_INHERIT_GRACE_SEC * 1000000000ULL))
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
// out it draws from sysmem over PCIe), then windowless idle ones; within a
// class the one hidden longest (never visible = longest), then the biggest.
//
static VIDMEM_EVICT_PROC *
_vidmemEvictHintPickVictim(VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU32 exceptPid, NvU64 nowNs, NvU32 level)
{
    VIDMEM_EVICT_PROC *pPick = NULL;
    NvU64 pickVisible = 0;
    NvU32 pickClass = 0;
    NvU32 i;

    for (i = 0; i < count; i++)
    {
        NvU64 vis;
        NvU32 cls;

        if (!_vidmemEvictHintVictimOk(&pProcs[i], exceptPid, nowNs, level))
            continue;
        vis = _vidmemActivityFind(pProcs[i].pid, NV_TRUE)->lastVisibleNs;
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
            ((cls == pickClass) && ((vis < pickVisible) ||
                                    ((vis == pickVisible) && (pProcs[i].eligibleBytes > pPick->eligibleBytes)))))
        {
            pPick = &pProcs[i];
            pickVisible = vis;
            pickClass = cls;
        }
    }
    return pPick;
}

//
// Move hidden processes out until `wantMB` is free outside the reserve block
// (or nothing is left to move). Returns the free MB afterwards.
//
static NvU64
_vidmemEvictHintFree(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 wantMB,
                     NvU32 exceptPid, NvU32 level, const char *why)
{
    NvU64 nowNs = osGetMonotonicTimeNs();
    NvU64 avail = _vidmemEvictAvailMB(pGpu);

    while (avail < wantMB)
    {
        VIDMEM_EVICT_PROC *pPick = _vidmemEvictHintPickVictim(pProcs, count, exceptPid, nowNs, level);
        NvU64 moved = 0;

        if (pPick == NULL)
            break;
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: %s: free %llu MB, want %llu MB, evicting up to %llu MB of %s (pid %u, %s%s, %llu MB movable)\n",
                  why, avail, wantMB, (wantMB - avail) + 64, pPick->name, pPick->pid,
                  (VE_HINT_STATE(pPick->hint) == VE_HINT_HIDDEN) ? "hidden" :
                  ((VE_HINT_STATE(pPick->hint) == VE_HINT_VISIBLE) ? "visible, for the foreground" :
                   ((VE_HINT_STATE(pPick->hint) == VE_HINT_FG) ? "foreground, for the compositor" : "no window")),
                  (pPick->hint & VE_HINT_INHERITED) ? " (parent)" : "", pPick->eligibleBytes >> 20);
        // Only what is missing (+64 MB), not the whole process.
        _vidmemEvictBudgetBytes = ((wantMB - avail) + 64) << 20;
        (void)_vidmemEvictPidLocked(pGpu, 1, pPick->pid, NULL, 0, NULL, &moved);
        _vidmemEvictBudgetBytes = 0;
        pPick->eligibleBytes = (moved != 0 && pPick->eligibleBytes > moved) ? pPick->eligibleBytes - moved : 0;
        pPick->evictedBytes += moved;
        if (VE_HINT_STATE(pPick->hint) == VE_HINT_VISIBLE)
            _vidmemActivityFind(pPick->pid, NV_TRUE)->evictedForFgNs = osGetMonotonicTimeNs();
        avail = _vidmemEvictAvailMB(pGpu);
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
static NvU64 _vidmemReclaimableBytes;      // for visible processes: hidden/windowless ones
static NvU64 _vidmemReclaimableFgBytes;    // for the foreground: + visible idle ones

//
// Free-memory targets with hints. Smaller than lowMB/hysteresis of the
// sample policy: a visible process's allocation makes room on demand
// (vidmemEvictMakeRoom), so a big free cushion only makes every switch move
// more (2026-11-02 12:10: 1.4 GB of the game moved to bring back Chrome's
// 345 MB).
//
#define VIDMEM_EVICT_HINT_LOW_MB  256
#define VIDMEM_EVICT_HINT_HYST_MB 256
#define VIDMEM_EVICT_RECLAIM_MARGIN (256ULL << 20)

static void
_vidmemEvictUpdateReclaimable(VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 nowNs)
{
    NvU64 sum = 0, sumFg = 0;
    NvU32 i;

    for (i = 0; i < count; i++)
    {
        if (_vidmemEvictHintVictimOk(&pProcs[i], 0, nowNs, VE_LEVEL_HIDDEN))
            sum += pProcs[i].eligibleBytes;
        if (_vidmemEvictHintVictimOk(&pProcs[i], 0, nowNs, VE_LEVEL_FG_IDLE))
            sumFg += pProcs[i].eligibleBytes;
    }
    _vidmemReclaimableBytes = (sum > VIDMEM_EVICT_RECLAIM_MARGIN) ? (sum - VIDMEM_EVICT_RECLAIM_MARGIN) : 0;
    _vidmemReclaimableFgBytes = (sumFg > VIDMEM_EVICT_RECLAIM_MARGIN) ? (sumFg - VIDMEM_EVICT_RECLAIM_MARGIN) : 0;
}

NvU64
vidmemEvictReclaimableFor(RmClient *pRmClient)
{
    if (!_vidmemAutoActive() || !_vidmemHintsActive() || (pRmClient == NULL) || (pRmClient->ProcID == 0))
        return 0;
    switch (VE_HINT_STATE(_vidmemHint(pRmClient->ProcID, NULL)))
    {
        case VE_HINT_FG:      return _vidmemReclaimableFgBytes;
        case VE_HINT_VISIBLE: return _vidmemReclaimableBytes;
        default:              return 0;
    }
}

static void _vidmemHotRestore(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count);

// The policy step when window hints are on. API lock RW, GPU lock held.
static void
_vidmemEvictPolicyHints(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 nowNs)
{
    NvU64 avail;
    NvU32 pass, i;

    //
    // 1. Visible processes get their memory back now: the foreground first,
    // then other visible ones; hidden processes make room.
    //
    for (pass = 0; pass < 2; pass++)
    {
        NvU32 want = (pass == 0) ? VE_HINT_FG : VE_HINT_VISIBLE;

        for (i = 0; i < count; i++)
        {
            NvU64 needMB = pProcs[i].evictedBytes >> 20;

            if ((pProcs[i].evictedBytes == 0) || (VE_HINT_STATE(pProcs[i].hint) != want))
                continue;
            //
            // A visible app moved out for the foreground stays out for a while
            // (no ping-pong with the game's next allocation) unless there is
            // room without moving anything.
            //
            if (want == VE_HINT_VISIBLE)
            {
                VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pProcs[i].pid, NV_TRUE);

                if ((pAct->evictedForFgNs != 0) &&
                    (nowNs - pAct->evictedForFgNs < VIDMEM_EVICT_FG_HOLD_SEC * 1000000000ULL))
                    continue;
                if (_vidmemEvictAvailMB(pGpu) < needMB + VIDMEM_EVICT_HINT_LOW_MB + VIDMEM_EVICT_HINT_HYST_MB)
                {
                    // Only hidden ones make room for a visible (not foreground) app.
                    _vidmemEvictWhy = VMT_WHY_ROOM;
                    (void)_vidmemEvictHintFree(pGpu, pProcs, count, needMB + VIDMEM_EVICT_RESTORE_MARGIN_MB,
                                               pProcs[i].pid, VE_LEVEL_HIDDEN, "visible again");
                }
                avail = _vidmemEvictAvailMB(pGpu);
            }
            else
            {
                _vidmemEvictWhy = VMT_WHY_ROOM;
                avail = _vidmemEvictHintFree(pGpu, pProcs, count, needMB + VIDMEM_EVICT_HINT_LOW_MB, pProcs[i].pid,
                                             VE_LEVEL_FG_RESTORE, "foreground again");
            }
            if (avail < needMB + 16)
            {
                NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: %s (pid %u) is visible, %llu MB in sysmem, only %llu MB free\n",
                          pProcs[i].name, pProcs[i].pid, needMB, avail);
                continue;
            }
            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: restoring %s (pid %u, %s, %llu MB)\n",
                      pProcs[i].name, pProcs[i].pid, (want == VE_HINT_FG) ? "foreground" : "visible", needMB);
            _vidmemEvictWhy = VMT_WHY_RESTORE;
            (void)_vidmemEvictPidLocked(pGpu, 2, pProcs[i].pid, NULL, 0, NULL, NULL);
            pProcs[i].evictedBytes = 0;
            _vidmemActivityFind(pProcs[i].pid, NV_TRUE)->evictedForFgNs = 0;
        }
    }

    //
    // 1b. A process moved out that draws steadily again comes back: from
    // sysmem it would load PCIe and GR for everyone. After the foreground,
    // which the user is working with. A visible one gets room made by idle
    // hidden processes; a hidden one (minimized, behind the game) only takes
    // memory that is free anyway: it is slowed down, not the others (a
    // minimized launcher drawing a frame now and then ping-ponged six times
    // and took 241 MB of the game at a switch, 2026-11-04).
    //
    for (i = 0; i < count; i++)
    {
        VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pProcs[i].pid, NV_TRUE);
        NvU64 needMB = pProcs[i].evictedBytes >> 20;
        NvBool bVisible = (VE_HINT_STATE(pProcs[i].hint) == VE_HINT_VISIBLE);

        if ((pProcs[i].evictedBytes == 0) || pProcs[i].bNever ||
            (VE_HINT_STATE(pProcs[i].hint) == VE_HINT_FG) ||
            (pAct->drawingSinceNs == 0) ||
            (nowNs - pAct->drawingSinceNs < VIDMEM_EVICT_STEADY_SEC * 1000000000ULL))
            continue;
        if (bVisible)
        {
            _vidmemEvictWhy = VMT_WHY_ROOM;
            avail = _vidmemEvictHintFree(pGpu, pProcs, count, needMB + VIDMEM_EVICT_RESTORE_MARGIN_MB,
                                         pProcs[i].pid, VE_LEVEL_HIDDEN, "drawing again");
            if (avail < needMB + 16)
                continue;
        }
        else if (_vidmemEvictAvailMB(pGpu) < needMB + VIDMEM_EVICT_HINT_LOW_MB)
        {
            continue;
        }
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: restoring %s (pid %u, drawing again, %s, %llu MB)\n",
                  pProcs[i].name, pProcs[i].pid, bVisible ? "visible" : "not visible", needMB);
        _vidmemEvictWhy = VMT_WHY_RESTORE;
        (void)_vidmemEvictPidLocked(pGpu, 2, pProcs[i].pid, NULL, 0, NULL, NULL);
        pProcs[i].evictedBytes = 0;
        pAct->evictedForFgNs = 0;
    }

    // 1c. Evicted allocations in use (access counters) come back one by one.
    _vidmemHotRestore(pGpu, pProcs, count);

    //
    // 2. Pressure: hidden processes out until lowMB + hysteresis is free.
    //
    avail = _vidmemEvictAvailMB(pGpu);
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
            if ((VE_HINT_STATE(pProcs[i].hint) == VE_HINT_FG) && (pProcs[i].vidmemBytes >= VIDMEM_EVICT_GAME_BYTES))
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
    // are ready when shown (one per step, as without hints).
    //
    if ((_vidmemEvictBytes != 0) &&
        ((nowNs - _vidmemLastPressureNs) >= (VIDMEM_EVICT_QUIET_SEC * 1000000000ULL)))
    {
        for (i = 0; i < count; i++)
        {
            NvU64 needMB = pProcs[i].evictedBytes >> 20;

            if ((pProcs[i].evictedBytes == 0) || pProcs[i].bNever || (avail < needMB + _vidmemHighMB))
                continue;
            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: plenty free (%llu MB), restoring %s (pid %u, %llu MB)\n",
                      avail, pProcs[i].name, pProcs[i].pid, needMB);
            _vidmemEvictWhy = VMT_WHY_IDLE_BACK;
            (void)_vidmemEvictPidLocked(pGpu, 2, pProcs[i].pid, NULL, 0, NULL, NULL);
            break;
        }
    }
}

//
// Bring back evicted allocations that are in use (hot), one at a time, for
// processes that are not hidden: from sysmem they cost PCIe on every access.
// Idle hidden processes make room. GPU lock held.
//
static void
_vidmemHotRestore(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count)
{
    NvU64 nowNs = osGetMonotonicTimeNs();
    NvU32 done = 0;
    VIDMEM_EVICT_STATE *pState, *pNext;

    if (!_vidmemHotEnabled)
        return;
    for (pState = _vidmemEvictList; (pState != NULL) && (done < 8); pState = pNext)
    {
        MEMORY_DESCRIPTOR *pMemDesc = pState->pMemDesc;
        NvU64 needMB = (pState->bytes >> 20) + 1, avail;
        NvU32 hint;

        pNext = pState->pNext;
        if ((pState->hotNs == 0) || (nowNs - pState->hotNs > 4000000000ULL) || (pState->ownerPid == 0))
            continue;
        hint = VE_HINT_STATE(_vidmemHint(pState->ownerPid, NULL));
        if (_vidmemHintsActive() && (hint == VE_HINT_HIDDEN))
            continue;
        _vidmemEvictWhy = VMT_WHY_ROOM;
        avail = _vidmemEvictHintFree(pGpu, pProcs, count, needMB + VIDMEM_EVICT_RESTORE_MARGIN_MB,
                                     pState->ownerPid, VE_LEVEL_HIDDEN, "hot allocation");
        if (avail < needMB + 16)
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

// One policy step. API lock RW, GPU lock held.
static void
_vidmemEvictPolicyStep(OBJGPU *pGpu)
{
    VIDMEM_EVICT_PROC *pProcs;
    NvU64 nowNs = osGetMonotonicTimeNs();
    NvU64 avail;
    NvU32 count, busiest, done = 0, i;

    _vidmemActivitySample(pGpu);
    _vidmemActivitySampleSubmits(pGpu);
    _vidmemHotPoll(pGpu);
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
    if (GPU_GET_HEAP(pGpu) != NULL)
        vidmemEvictNoteBlock(GPU_GET_HEAP(pGpu)->pPmaObject);

    if (!_vidmemAutoActive())
        return;

    // Common case without hints: no pressure, nothing to restore; skip the
    // client walk. With hints every step walks them: the reclaimable total
    // (budget of visible processes) must stay current.
    avail = _vidmemEvictAvailMB(pGpu);
    if (!_vidmemHintsActive() && (avail >= _vidmemLowMB) && (_vidmemEvictBytes == 0))
        return;

    pProcs = portMemAllocNonPaged(sizeof(*pProcs) * VIDMEM_EVICT_MAX_PROCS);
    if (pProcs == NULL)
        return;

    count   = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);
    if (_vidmemHintsActive())
    {
        _vidmemEvictUpdateReclaimable(pProcs, count, nowNs);
        _vidmemEvictPolicyHints(pGpu, pProcs, count, nowNs);
        // Moves change what is left to reclaim.
        count = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);
        _vidmemEvictUpdateReclaimable(pProcs, count, osGetMonotonicTimeNs());
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
            (void)_vidmemEvictPidLocked(pGpu, 1, pPick->pid, NULL, 0, NULL, &moved);
            pPick->eligibleBytes = 0;
            avail += moved >> 20;
            done++;
        }
    }
    else if (_vidmemEvictBytes != 0)
    {
        NvBool bQuiet = (nowNs - _vidmemLastPressureNs) >= (VIDMEM_EVICT_QUIET_SEC * 1000000000ULL);
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
            (void)_vidmemEvictPidLocked(pGpu, 2, pPick->pid, NULL, 0, NULL, NULL);
        }
    }

    portMemFree(pProcs);
}

//
// One policy step, traced (VMT_TICK, VMT_PROC): everything in it runs with
// the API lock held for writing and the GPU lock, i.e. every other RM call
// waits. trigger: 0 timer (queuedNs: when the work item was queued), 1 hints.
//
static NvU64 _vidmemTickQueuedNs;

static void
_vidmemEvictPolicyTick(OBJGPU *pGpu, NvU32 trigger)
{
    NvU64 startNs = VMT_NOW();
    NvU64 blockBytes = 0, blockFree = 0, blockLargest = 0;
    NvU32 procs = 0;

    _vidmemEvictPolicyStep(pGpu);

    if (osVidmemTraceOn(VMT_PROC))
    {
        VIDMEM_EVICT_PROC *pProcs = portMemAllocNonPaged(sizeof(*pProcs) * VIDMEM_EVICT_MAX_PROCS);
        NvU64 nowNs = osGetMonotonicTimeNs();
        NvU32 i;

        if (pProcs != NULL)
        {
            procs = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);
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
        a[2] = _vidmemEvictAvailMB(pGpu);
        a[3] = blockFree >> 20;
        a[4] = blockLargest >> 20;
        a[5] = _vidmemEvictBytes >> 20;
        a[6] = _vidmemBusiestPid;
        a[7] = _vidmemHintsActive();
        a[8] = procs;
        a[9] = trigger;
        osVidmemTrace(VMT_TICK, 0, a);
    }
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

//
// Step 2: the user reserve is about to deny a vidmem allocation. Should it be
// created in sysmem instead (vidmemEvictBornAllocation)? Only for idle
// processes, never the game or the desktop, only for what eviction
// supports (no contiguous/ISO/fixed-address requests), up to 512 MB, and only
// if the reserve block can hold it for the moment it takes to move it.
//
#define VIDMEM_EVICT_BORN_MAX_BYTES (512ULL << 20)

NvBool
vidmemEvictWantBorn(OBJGPU *pGpu, RmClient *pRmClient, NV_MEMORY_ALLOCATION_PARAMS *pAllocData)
{
    Heap  *pHeap = GPU_GET_HEAP(pGpu);
    NvU64  blockBytes = 0, blockFree = 0, blockLargest = 0;

    if (!_vidmemAutoActive() || (pRmClient == NULL) || (pAllocData == NULL))
        return NV_FALSE;
    //
    // With window hints: only for hidden processes (and idle ones without
    // windows). A visible one gets room made instead (vidmemEvictMakeRoom).
    //
    if (_vidmemHintsActive())
    {
        NvU32 st = VE_HINT_STATE(_vidmemHint(pRmClient->ProcID, NULL));

        if (st >= VE_HINT_VISIBLE)
            return NV_FALSE;
        if ((st == VE_HINT_UNKNOWN) && (_vidmemActivityIdleSec(pRmClient->ProcID) < VIDMEM_EVICT_IDLE_SEC))
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
    if ((pRmClient->ProcID == 0) || _vidmemEvictIsNeverName(pRmClient->name) ||
        _vidmemEvictWasBusiest(pRmClient->ProcID, osGetMonotonicTimeNs()) ||
        (_vidmemActivityIdleSec(pRmClient->ProcID) < VIDMEM_EVICT_IDLE_SEC))
        return NV_FALSE;
    {
        //
        // Never for a process holding >= 1 GB of vidmem (a game, or a busy
        // browser): measured right here from the calling client (cached for
        // 2 s), since the policy tick may not have walked it yet (02:49: the
        // game got 128 MB in sysmem 40 s after it started).
        //
        VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pRmClient->ProcID, NV_TRUE);
        NvU64 nowNs = osGetMonotonicTimeNs();

        if ((pAct->vidmemAtNs == 0) || (nowNs - pAct->vidmemAtNs > 2000000000ULL))
        {
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
            pAct->vidmemBytes = NV_MAX(bytes, (pAct->vidmemAtNs != 0) ? pAct->vidmemBytes : 0);
            pAct->vidmemAtNs  = nowNs;
        }
        if (pAct->vidmemBytes >= VIDMEM_EVICT_GAME_BYTES)
            return NV_FALSE;
    }
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
static NvU32 _vidmemBgCushionMB = 256;

NvU64
vidmemEvictBackgroundCushion(RmClient *pRmClient)
{
    VIDMEM_EVICT_ACTIVITY *pAct;
    NvU64 nowNs;
    NvU32 st;

    if ((_vidmemBgCushionMB == 0) || !_vidmemAutoActive() || !_vidmemHintsActive() ||
        (pRmClient == NULL) || (pRmClient->ProcID == 0) || _vidmemEvictIsNeverName(pRmClient->name))
        return 0;
    st = VE_HINT_STATE(_vidmemHint(pRmClient->ProcID, NULL));
    if (st >= VE_HINT_VISIBLE)
        return 0;
    pAct = _vidmemActivityFind(pRmClient->ProcID, NV_FALSE);
    if (pAct == NULL)
        return 0;
    nowNs = osGetMonotonicTimeNs();
    if (_vidmemEvictIsProtected(pRmClient->ProcID, nowNs) ||
        (_vidmemActivityIdleSec(pRmClient->ProcID) < VIDMEM_EVICT_DRAWING_SEC) ||
        ((pAct->lastFgNs != 0) && (nowNs - pAct->lastFgNs < VIDMEM_EVICT_RECENT_FG_SEC * 1000000000ULL)))
        return 0;
    return (NvU64)_vidmemBgCushionMB << 20;
}

//
// The user reserve is about to deny an allocation of `pRmClient`. If it is a
// visible process (window hints), move hidden processes out right now and
// let the caller check again: the foreground app gets the memory, not a
// denial (a game would drop textures or spill into sysmem) and not sysmem.
// Moving 1 GB takes ~100 ms (2026-11-02, CeUtils page merging). Needs the
// API lock for writing; vidmemConstruct() calls this before it takes the
// device GPU lock (PMA allocates first, without it), so the lock is taken
// here for the moves.
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
static NvU32 _vidmemFgHeadroomMB = 2048;

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
    if ((_vidmemFgHeadroomMB == 0) || !_vidmemAutoActive() || !_vidmemHintsActive() ||
        (pRmClient == NULL) || (pRmClient->ProcID == 0) || _vidmemEvictIsBusyThread() ||
        (VE_HINT_STATE(_vidmemHint(pRmClient->ProcID, NULL)) != VE_HINT_FG))
        return;
    if (_vidmemEvictAvailMB(pGpu) >= (size >> 20) + _vidmemFgHeadroomMB)
        return;
    (void)_vidmemEvictMakeRoomWant(pGpu, pRmClient, size, _vidmemFgHeadroomMB, "foreground headroom");
}

static NvBool
_vidmemEvictMakeRoomWant(OBJGPU *pGpu, RmClient *pRmClient, NvU64 size, NvU64 headroomMB, const char *why)
{
    static NvU64       lastFruitlessNs;
    static NvU32       lockLogged;
    VIDMEM_EVICT_PROC *pProcs;
    NvU64              nowNs = osGetMonotonicTimeNs();
    NvU64              wantMB, avail;
    NvU32              count;
    NvBool             bOk;
    NvBool             bLocked = NV_FALSE;
    NvU64              t0;

    if (!_vidmemAutoActive() || (pRmClient == NULL) || (pRmClient->ProcID == 0) || !_vidmemHintsActive() ||
        _vidmemEvictIsBusyThread())
        return NV_FALSE;
    if (VE_HINT_STATE(_vidmemHint(pRmClient->ProcID, NULL)) < VE_HINT_VISIBLE)
        return NV_FALSE;
    if (!rmapiLockIsWriteOwner())
    {
        if (lockLogged++ < 4)
            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: cannot make room for %s(%u) here: no API write lock\n",
                      pRmClient->name, pRmClient->ProcID);
        return NV_FALSE;
    }
    // Nothing to move a moment ago: do not walk every client per allocation.
    if ((lastFruitlessNs != 0) && (nowNs - lastFruitlessNs < 1000000000ULL))
        return NV_FALSE;

    t0 = VMT_NOW();
    if (!rmGpuLockIsOwner() && !rmDeviceGpuLockIsOwner(pGpu->gpuInstance))
    {
        if (rmDeviceGpuLocksAcquire(pGpu, GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_MEM) != NV_OK)
            return NV_FALSE;
        bLocked = NV_TRUE;
    }

    pProcs = portMemAllocNonPaged(sizeof(*pProcs) * VIDMEM_EVICT_MAX_PROCS);
    if (pProcs == NULL)
    {
        if (bLocked)
            rmDeviceGpuLocksRelease(pGpu, GPUS_LOCK_FLAGS_NONE, NULL);
        return NV_FALSE;
    }
    count = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);

    // Some headroom, so the next allocations do not each come here.
    wantMB = (size >> 20) + 1 + headroomMB;
    _vidmemEvictWhy = VMT_WHY_ALLOC;
    avail  = _vidmemEvictHintFree(pGpu, pProcs, count, wantMB, pRmClient->ProcID,
                                  (VE_HINT_STATE(_vidmemHint(pRmClient->ProcID, NULL)) == VE_HINT_FG) ?
                                      VE_LEVEL_FG_IDLE : VE_LEVEL_HIDDEN, why);
    bOk    = (avail > (size >> 20));
    _vidmemEvictUpdateReclaimable(pProcs, count, nowNs);
    lastFruitlessNs = bOk ? 0 : nowNs;
    if (bOk)
        _vidmemLastPressureNs = nowNs;

    portMemFree(pProcs);
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
// pCost is scratch space for `chunks` entries.
//
static NvU64
_vidmemEvictBestWindow(const NvU64 *pUsed, const NvU64 *pMovable, NvU32 *pCost, NvU64 base, NvU64 off,
                       NvU64 chunks, NvU64 need, NvU64 align, NvU64 *pStart)
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
            n += bUsed;
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

static NvBool
_vidmemEvictMakeContig(OBJGPU *pGpu, VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU64 size, NvU64 align,
                       NvU64 *pMovedMB)
{
    Heap   *pHeap = GPU_GET_HEAP(pGpu);
    PMA    *pPma = (pHeap != NULL) ? pHeap->pPmaObject : NULL;
    NvU64   base = 0, frames = 0, largest = 0, words, chunks, need, off, freeChunks, longest;
    NvU64  *pUsed, *pMovable = NULL;
    NvU32  *pCost = NULL;
    NvU64   bestStart = NV_U64_MAX, bestCost = NV_U64_MAX;
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
    pCost    = portMemAllocNonPaged(chunks * sizeof(NvU32));
    pRefs    = portMemAllocNonPaged(sizeof(*pRefs) * VIDMEM_EVICT_MAX_MAPPINGS);
    if ((pMovable == NULL) || (pCost == NULL) || (pRefs == NULL) || (need == 0) || (need > chunks))
        goto done;
    portMemSet(pMovable, 0, words * sizeof(NvU64));
    // Already there: an aligned window with nothing in it.
    if (_vidmemEvictBestWindow(pUsed, pMovable, pCost, base, off, chunks, need, align, NULL) == 0)
    {
        bOk = NV_TRUE;
        goto done;
    }

    // Frames the victims could give up.
    for (i = 0; i < count; i++)
    {
        RmClient **ppClient;

        if (!_vidmemEvictHintVictimOk(&pProcs[i], 0, nowNs, VE_LEVEL_COMPOSITOR))
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
                (void)_vidmemMemDescFrames(pMemory->pMemDesc, 0, 0, pMovable, base, frames);
            }
        }
    }

    bestCost = _vidmemEvictBestWindow(pUsed, pMovable, pCost, base, off, chunks, need, align, &bestStart);
    if (bestStart == NV_U64_MAX)
    {
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: compositor: no %llu MB window aligned to %llu KB can be cleared (longest free %llu MB)\n",
                  need * 2, align >> 10, longest * 2);
        goto done;
    }

    NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: compositor: clearing %llu MB at 0x%llx for a contiguous %llu KB aligned to %llu KB (longest free %llu MB, %llu MB to move)\n",
              need * 2, base + ((off + bestStart * VIDMEM_EVICT_CHUNK_FRAMES) << PMA_PAGE_SHIFT), size >> 10,
              align >> 10, longest * 2, (bestCost << PMA_PAGE_SHIFT) >> 20);
    _vidmemEvictRangeLo = base + ((off + bestStart * VIDMEM_EVICT_CHUNK_FRAMES) << PMA_PAGE_SHIFT);
    _vidmemEvictRangeHi = _vidmemEvictRangeLo + (need << 21);
    for (i = 0; i < count; i++)
    {
        RmClient **ppClient;
        NvBool     bHas = NV_FALSE;
        NvU64      moved = 0;

        if (!_vidmemEvictHintVictimOk(&pProcs[i], 0, nowNs, VE_LEVEL_COMPOSITOR))
            continue;
        for (ppClient = serverutilGetFirstClientUnderLock(); (ppClient != NULL) && !bHas;
             ppClient = serverutilGetNextClientUnderLock(ppClient))
        {
            RsClient   *pRsClient = staticCast(*ppClient, RsClient);
            RS_ITERATOR iter;

            if ((pRsClient->type == CLIENT_TYPE_KERNEL) || ((*ppClient)->ProcID != pProcs[i].pid))
                continue;
            iter = serverutilRefIter(pRsClient->hClient, NV01_NULL_OBJECT, 0, RS_ITERATE_DESCENDANTS, NV_TRUE);
            while (!bHas && clientRefIterNext(iter.pClient, &iter))
            {
                Memory *pMemory = dynamicCast(iter.pResourceRef->pResource, Memory);

                bHas = (pMemory != NULL) && (pMemory->pMemDesc != NULL) &&
                       (iter.pResourceRef->externalClassId == NV01_MEMORY_LOCAL_USER) &&
                       (pMemory->pMemDesc->pLocalEvictState == NULL) &&
                       _vidmemMemDescInRange(pMemory->pMemDesc, _vidmemEvictRangeLo, _vidmemEvictRangeHi);
            }
        }
        if (!bHas)
            continue;
        (void)_vidmemEvictPidLocked(pGpu, 1, pProcs[i].pid, NULL, 0, NULL, &moved);
        *pMovedMB += moved >> 20;
    }
    _vidmemEvictRangeLo = _vidmemEvictRangeHi = 0;

    portMemFree(pUsed);
    pUsed = vidmemPmaUsedMap(pPma, &base, &frames, &largest);
    portMemSet(pMovable, 0, words * sizeof(NvU64));
    bOk = (pUsed != NULL) &&
          (_vidmemEvictBestWindow(pUsed, pMovable, pCost, base, off, chunks, need, align, NULL) == 0);

done:
    portMemFree(pUsed);
    portMemFree(pMovable);
    portMemFree(pCost);
    portMemFree(pRefs);
    return bOk;
}

static NvU32 _vidmemCompRooms, _vidmemCompRoomFails;
static NvU64 _vidmemCompRoomMovedMB, _vidmemCompRoomMaxNs;

//
// No reserve: an allocation of the compositor (kwin_wayland, Xwayland, a
// kernel thread) just failed. Move others to sysmem until it fits, hidden
// and idle ones first and the foreground last (VE_LEVEL_COMPOSITOR), and for
// a contiguous one clear a window (_vidmemEvictMakeContig()). The caller
// tries again. Needs the API lock for writing (vidmemConstruct() holds it).
//
NvBool
vidmemEvictMakeRoomCompositor(OBJGPU *pGpu, RmClient *pRmClient, NvU64 size, NvBool bContig, NvU64 align)
{
    static NvU32       lockLogged;
    VIDMEM_EVICT_PROC *pProcs;
    NvU64              t0 = VMT_NOW(), avail, wantMB, movedMB = 0, dt;
    NvU32              count;
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
    pProcs = portMemAllocNonPaged(sizeof(*pProcs) * VIDMEM_EVICT_MAX_PROCS);
    if (pProcs == NULL)
    {
        if (bLocked)
            rmDeviceGpuLocksRelease(pGpu, GPUS_LOCK_FLAGS_NONE, NULL);
        return NV_FALSE;
    }
    count = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);

    //
    // Free 2 MB pages, not free bytes: a 2 MB-page allocation cannot use
    // 64 KB holes. Each round asks for what is still missing (whole
    // allocations move, and some of them may sit in holes).
    //
    _vidmemEvictWhy = VMT_WHY_ALLOC;
    wantMB = 2 * NV_DIV_AND_CEIL(size, 2ULL << 20) + VIDMEM_EVICT_RESTORE_MARGIN_MB;
    avail  = _vidmemEvictAvailMB(pGpu);
    {
        Heap  *pHeap = GPU_GET_HEAP(pGpu);
        NvU64  freeChunks = 0, longest = 0, round;

        for (round = 0; round < 4; round++)
        {
            NvU64 before = avail;

            if (!_vidmemEvictChunkInfo(pHeap->pPmaObject, &freeChunks, &longest) || (2 * freeChunks >= wantMB))
                break;
            avail = _vidmemEvictHintFree(pGpu, pProcs, count, avail + (wantMB - 2 * freeChunks), 0,
                                         VE_LEVEL_COMPOSITOR, "compositor");
            movedMB += (avail > before) ? (avail - before) : 0;
            if (avail <= before)
                break;      // nothing left to move
        }
        bOk = _vidmemEvictChunkInfo(pHeap->pPmaObject, &freeChunks, &longest) &&
              (2 * freeChunks > (size >> 20));
    }
    if (bOk && bContig)
    {
        NvU64 contigMB = 0;

        // What moved changed what the victims hold.
        count = _vidmemEvictGatherProcs(pGpu, pProcs, VIDMEM_EVICT_MAX_PROCS);
        bOk = _vidmemEvictMakeContig(pGpu, pProcs, count, size, align, &contigMB);
        movedMB += contigMB;
    }
    _vidmemEvictUpdateReclaimable(pProcs, count, osGetMonotonicTimeNs());
    _vidmemLastPressureNs = osGetMonotonicTimeNs();

    portMemFree(pProcs);
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

        portMemFree(pUsed);
        EVICT_PRINTF("no reserve: largest contiguous free %llu MB; background cushion %u MB; "
                     "room made for the compositor %u times (%u failed), %llu MB moved, longest %llu us\n",
                     largest >> 20, _vidmemBgCushionMB, _vidmemCompRooms, _vidmemCompRoomFails,
                     _vidmemCompRoomMovedMB, _vidmemCompRoomMaxNs / 1000);
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
                 _vidmemEvictAvailMB(pGpu), _vidmemEvictAllocs, _vidmemEvictBytes >> 20);
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

    if (mode <= 2)
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
