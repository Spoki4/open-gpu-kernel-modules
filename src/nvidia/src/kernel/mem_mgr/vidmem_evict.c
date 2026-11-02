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
#include "rmapi/client.h"
#include "rmapi/mapping_list.h"
#include "rmapi/rs_utils.h"
#include "vgpu/rpc.h"
#include "class/cl0040.h" // NV01_MEMORY_LOCAL_USER
#include "ctrl/ctrl2080/ctrl2080fifo.h"
#include "ctrl/ctrla06c.h"
#include "kernel/gpu/fifo/kernel_channel_group_api.h"
#include "kernel/gpu/fifo/kernel_channel_group.h"
#include "gpu/device/device.h"

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

typedef struct VIDMEM_EVICT_STATE
{
    MEMORY_DESCRIPTOR    *pSysMemDesc;
    NvU64                 bytes;
    NvU64                 restoreAlign;   // contiguous: alignment of the new block
    NvU32                 staticBar1Refs; // static BAR1 references dropped at evict
    NvU32                 numMappings;
    VIDMEM_EVICT_MAPPING  mappings[VIDMEM_EVICT_MAX_MAPPINGS];
} VIDMEM_EVICT_STATE;

typedef struct
{
    NvU32    count;
    NvHandle hClient[VIDMEM_EVICT_MAX_CHANNELS];
    NvHandle hChannel[VIDMEM_EVICT_MAX_CHANNELS];
    NvHandle hTsg[VIDMEM_EVICT_MAX_CHANNELS];     // channel group of each channel (0: none)
    NvHandle hDevice[VIDMEM_EVICT_MAX_CHANNELS];  // device the channel belongs to (IDLE_CHANNELS)
    NvU8     bImplicitTsg[VIDMEM_EVICT_MAX_CHANNELS]; // TSG allocated by RM around a bare channel
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
#define VE_PHASE_BEGIN() NvU64 _vePhaseT0 = osGetMonotonicTimeNs()
#define VE_PHASE_END(ph)                                                      \
    do {                                                                    \
        NvU64 _vePhaseT1 = osGetMonotonicTimeNs();                          \
        _vidmemEvictPhaseNs[ph] += _vePhaseT1 - _vePhaseT0;                 \
        _vePhaseT0 = _vePhaseT1;                                            \
    } while (0)
#define VE_PHASE_SKIP() (_vePhaseT0 = osGetMonotonicTimeNs())

//
// Partial eviction: when non-zero, an evict call stops once it has moved this
// many bytes (set around one call by _vidmemEvictHintFree()). Making room
// for 300 MB must not move a hidden game's 10 GB: bringing it back would
// cost ~0.5 s instead of ~20 ms (nvtest.sh switch, 2026-11-02 12:05).
//
static NvU64 _vidmemEvictBudgetBytes;

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
    NvU64                      startNs = osGetMonotonicTimeNs(), t;
    NV_STATUS                  status = NV_OK;
    VE_PHASE_BEGIN();

    if (!bDisable)
    {
        status = _vidmemEvictSetChannelsN(pGpu, pChannels, pChannels->count, NV_FALSE, NV_FALSE);
        VE_PHASE_END(VE_PH_QUIESCE);
        return status;
    }
    if (pChannels->count == 0)
        return NV_OK;

    stats.mode     = _vidmemEvictQuiesce;
    stats.channels = pChannels->count;
    stats.idleStatus = NV_OK;

    switch (stats.mode)
    {
        case 1:
            status = _vidmemEvictSetChannelsN(pGpu, pChannels, pChannels->count, NV_TRUE, NV_TRUE);
            stats.disableNs = osGetMonotonicTimeNs() - startNs;
            break;

        case 2:
        case 3:
            status = _vidmemEvictSetChannelsN(pGpu, pChannels, pChannels->count, NV_TRUE, NV_FALSE);
            stats.disableNs = osGetMonotonicTimeNs() - startNs;
            if (status != NV_OK)
                break;
            t = osGetMonotonicTimeNs();
            stats.idleStatus = _vidmemEvictIdleChannels(pGpu, pChannels, stats.mode == 3);
            stats.idleNs = osGetMonotonicTimeNs() - t;
            if (stats.idleStatus != NV_OK)
                osDelayUs(VIDMEM_EVICT_DRAIN_US);
            break;

        case 4:
            t = osGetMonotonicTimeNs();
            stats.idleStatus = _vidmemEvictIdleChannels(pGpu, pChannels, NV_TRUE);
            stats.idleNs = osGetMonotonicTimeNs() - t;
            t = osGetMonotonicTimeNs();
            status = _vidmemEvictSetChannelsN(pGpu, pChannels, pChannels->count, NV_TRUE, NV_FALSE);
            stats.disableNs = osGetMonotonicTimeNs() - t;
            if ((status == NV_OK) && (stats.idleStatus != NV_OK))
                osDelayUs(VIDMEM_EVICT_DRAIN_US);
            break;

        default:
            status = _vidmemEvictSetChannelsN(pGpu, pChannels, pChannels->count, NV_TRUE, NV_FALSE);
            stats.disableNs = osGetMonotonicTimeNs() - startNs;
            if (status != NV_OK)
                break;
            _vidmemEvictPreemptTsgs(pGpu, pChannels, &stats);
            if (stats.preemptRefused != 0)
                osDelayUs(VIDMEM_EVICT_DRAIN_US);
            break;
    }

    stats.totalNs = osGetMonotonicTimeNs() - startNs;
    _vidmemEvictLastStop = stats;
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
            _vidmemEvictCollectChannels(pRsClient, pChannels);
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
    }
    pState->numMappings = numRefs;

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

    allocOptions.flags     = PMA_ALLOCATE_FORCE_ALIGNMENT;
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
                                            PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE);
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

    if (_vidmemEvictCollectHolders(pGpu, pMemDesc, 0, RES_GET_REF(pMemory), NV_FALSE,
                                   pRefs, &numRefs, &numHolders) != NULL)
    {
        status = NV_ERR_INVALID_STATE;
        goto done;
    }
    _vidmemEvictCollectChannels(RES_GET_CLIENT(pMemory), pChannels);
    for (i = 0; i < numRefs; i++)
        _vidmemEvictCollectChannels(RES_GET_CLIENT(pRefs[i].pHolder), pChannels);

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
    NvU64                  startNs = osGetMonotonicTimeNs();
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
    portMemSet(_vidmemEvictPhaseNs, 0, sizeof(_vidmemEvictPhaseNs));

    EVICT_PRINTF("%s pid %u\n", (mode == 0) ? "dry" : ((mode == 1) ? "evict" : "restore"), pid);

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
                 skipped, skippedBytes >> 20, (osGetMonotonicTimeNs() - startNs) / 1000000);
    EVICT_PRINTF("phases ms: quiesce %llu, sysmem alloc %llu, copy %llu, remap %llu, pma %llu, sysmem free %llu\n",
                 _vidmemEvictPhaseNs[VE_PH_QUIESCE] / 1000000, _vidmemEvictPhaseNs[VE_PH_SYSALLOC] / 1000000,
                 _vidmemEvictPhaseNs[VE_PH_COPY] / 1000000, _vidmemEvictPhaseNs[VE_PH_REMAP] / 1000000,
                 _vidmemEvictPhaseNs[VE_PH_PMA] / 1000000, _vidmemEvictPhaseNs[VE_PH_SYSFREE] / 1000000);
    EVICT_PRINTF("total evicted now: %u allocs %llu MB\n", _vidmemEvictAllocs, _vidmemEvictBytes >> 20);

    if ((mode != 0) && ((moved != 0) || (failed != 0)))
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: %s pid %u: %u allocs %llu MB (%u with the compositor), failed %u, %llu ms; total %llu MB\n",
                  (mode == 1) ? "evict" : "restore", pid, moved, movedBytes >> 20, numPending, failed,
                  (osGetMonotonicTimeNs() - startNs) / 1000000, _vidmemEvictBytes >> 20);
    if (pMovedBytes != NULL)
        *pMovedBytes = movedBytes;

free_mem:
    portMemFree(pChannels);
    portMemFree(pCompChannels);
    portMemFree(pRefs);
    portMemFree(pPending);
    return status;
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

// Seconds since `pid` was last seen on an engine (NV_U32_MAX: never).
static NvU32
_vidmemActivityIdleSec(NvU32 pid)
{
    VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(pid, NV_FALSE);
    NvU64 since;

    if (pAct == NULL)
        return NV_U32_MAX;
    since = NV_MAX(pAct->lastActiveTs, pAct->firstSeenTs);
    if ((since == 0) || (_vidmemActivityNewestTs < since))
        return (pAct->lastActiveTs == 0) ? NV_U32_MAX : 0;
    return (NvU32)NV_MIN((_vidmemActivityNewestTs - since) / 1000000000ULL, NV_U32_MAX - 1);
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
// May `p` be moved out to make room, by the window hints? Hidden (own
// windows, or inherited after the grace period), or no window anywhere in
// its ancestry and idle on the GPU. Never the caller (`exceptPid`), the
// desktop, or a process that needed a buffer back a moment ago.
//
static NvBool
_vidmemEvictHintVictimOk(VIDMEM_EVICT_PROC *p, NvU32 exceptPid, NvU64 nowNs)
{
    VIDMEM_EVICT_ACTIVITY *pAct = _vidmemActivityFind(p->pid, NV_TRUE);
    NvU32 st = VE_HINT_STATE(p->hint);

    if (p->bNever || (p->pid == exceptPid) || (p->eligibleBytes < (4ULL << 20)))
        return NV_FALSE;
    if (st >= VE_HINT_VISIBLE)
        return NV_FALSE;
    if ((pAct->lastRestoreNs != 0) &&
        (nowNs - pAct->lastRestoreNs < VIDMEM_EVICT_AFTER_ACCESS_SEC * 1000000000ULL))
        return NV_FALSE;
    if (st == VE_HINT_HIDDEN)
    {
        if ((p->hint & VE_HINT_OTHER_APP) &&
            (nowNs - pAct->firstSeenNs < VIDMEM_EVICT_INHERIT_GRACE_SEC * 1000000000ULL))
            return NV_FALSE;
        return NV_TRUE;
    }
    // Unknown: a background process without windows; only when idle.
    return (_vidmemActivityIdleSec(p->pid) >= VIDMEM_EVICT_IDLE_SEC) &&
           !_vidmemEvictIsProtected(p->pid, nowNs);
}

//
// Order: hidden and idle on the GPU first, then hidden ones still drawing
// (an X11 app such as Steam keeps rendering behind a fullscreen game; moved
// out it draws from sysmem over PCIe), then windowless idle ones; within a
// class the one hidden longest (never visible = longest), then the biggest.
//
static VIDMEM_EVICT_PROC *
_vidmemEvictHintPickVictim(VIDMEM_EVICT_PROC *pProcs, NvU32 count, NvU32 exceptPid, NvU64 nowNs)
{
    VIDMEM_EVICT_PROC *pPick = NULL;
    NvU64 pickVisible = 0;
    NvU32 pickClass = 0;
    NvU32 i;

    for (i = 0; i < count; i++)
    {
        NvU64 vis;
        NvU32 cls;

        if (!_vidmemEvictHintVictimOk(&pProcs[i], exceptPid, nowNs))
            continue;
        vis = _vidmemActivityFind(pProcs[i].pid, NV_TRUE)->lastVisibleNs;
        if (VE_HINT_STATE(pProcs[i].hint) == VE_HINT_UNKNOWN)
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
                     NvU32 exceptPid, const char *why)
{
    NvU64 nowNs = osGetMonotonicTimeNs();
    NvU64 avail = _vidmemEvictAvailMB(pGpu);

    while (avail < wantMB)
    {
        VIDMEM_EVICT_PROC *pPick = _vidmemEvictHintPickVictim(pProcs, count, exceptPid, nowNs);
        NvU64 moved = 0;

        if (pPick == NULL)
            break;
        NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: %s: free %llu MB, want %llu MB, evicting up to %llu MB of %s (pid %u, %s%s, %llu MB movable)\n",
                  why, avail, wantMB, (wantMB - avail) + 64, pPick->name, pPick->pid,
                  (VE_HINT_STATE(pPick->hint) == VE_HINT_HIDDEN) ? "hidden" : "no window",
                  (pPick->hint & VE_HINT_INHERITED) ? " (parent)" : "", pPick->eligibleBytes >> 20);
        // Only what is missing (+64 MB), not the whole process.
        _vidmemEvictBudgetBytes = ((wantMB - avail) + 64) << 20;
        (void)_vidmemEvictPidLocked(pGpu, 1, pPick->pid, NULL, 0, NULL, &moved);
        _vidmemEvictBudgetBytes = 0;
        pPick->eligibleBytes = (moved != 0 && pPick->eligibleBytes > moved) ? pPick->eligibleBytes - moved : 0;
        pPick->evictedBytes += moved;
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
static NvU64 _vidmemReclaimableBytes;

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
    NvU64 sum = 0;
    NvU32 i;

    for (i = 0; i < count; i++)
    {
        if (_vidmemEvictHintVictimOk(&pProcs[i], 0, nowNs))
            sum += pProcs[i].eligibleBytes;
    }
    _vidmemReclaimableBytes = (sum > VIDMEM_EVICT_RECLAIM_MARGIN) ? (sum - VIDMEM_EVICT_RECLAIM_MARGIN) : 0;
}

NvU64
vidmemEvictReclaimableFor(RmClient *pRmClient)
{
    if (!_vidmemAutoActive() || !_vidmemHintsActive() || (pRmClient == NULL) || (pRmClient->ProcID == 0))
        return 0;
    if (VE_HINT_STATE(_vidmemHint(pRmClient->ProcID, NULL)) < VE_HINT_VISIBLE)
        return 0;
    return _vidmemReclaimableBytes;
}

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
            avail = _vidmemEvictHintFree(pGpu, pProcs, count, needMB + VIDMEM_EVICT_HINT_LOW_MB, pProcs[i].pid,
                                         "visible again");
            if (avail < needMB + 16)
            {
                NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: %s (pid %u) is visible, %llu MB in sysmem, only %llu MB free\n",
                          pProcs[i].name, pProcs[i].pid, needMB, avail);
                continue;
            }
            NV_PRINTF(LEVEL_ERROR, "NVRM: vidmem evict: hints: restoring %s (pid %u, %s, %llu MB)\n",
                      pProcs[i].name, pProcs[i].pid, (want == VE_HINT_FG) ? "foreground" : "visible", needMB);
            (void)_vidmemEvictPidLocked(pGpu, 2, pProcs[i].pid, NULL, 0, NULL, NULL);
            pProcs[i].evictedBytes = 0;
        }
    }

    //
    // 2. Pressure: hidden processes out until lowMB + hysteresis is free.
    //
    avail = _vidmemEvictAvailMB(pGpu);
    if (avail < VIDMEM_EVICT_HINT_LOW_MB)
    {
        _vidmemLastPressureNs = nowNs;
        (void)_vidmemEvictHintFree(pGpu, pProcs, count, VIDMEM_EVICT_HINT_LOW_MB + VIDMEM_EVICT_HINT_HYST_MB, 0,
                                   "pressure");
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
            (void)_vidmemEvictPidLocked(pGpu, 2, pProcs[i].pid, NULL, 0, NULL, NULL);
            break;
        }
    }
}

// One policy step. API lock RW, GPU lock held.
static void
_vidmemEvictPolicyTick(OBJGPU *pGpu)
{
    VIDMEM_EVICT_PROC *pProcs;
    NvU64 nowNs = osGetMonotonicTimeNs();
    NvU64 avail;
    NvU32 count, busiest, done = 0, i;

    _vidmemActivitySample(pGpu);
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
            (void)_vidmemEvictPidLocked(pGpu, 2, pPick->pid, NULL, 0, NULL, NULL);
        }
    }

    portMemFree(pProcs);
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
        _vidmemEvictPolicyTick(pGpu);
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
    if ((pHeap == NULL) ||
        !vidmemGetUserReserveBlockInfo(pHeap->pPmaObject, &blockBytes, &blockFree, &blockLargest) ||
        (blockFree < pAllocData->size + (128ULL << 20)))
        return NV_FALSE;
    return NV_TRUE;
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
NvBool
vidmemEvictMakeRoom(OBJGPU *pGpu, RmClient *pRmClient, NvU64 size)
{
    static NvU64       lastFruitlessNs;
    static NvU32       lockLogged;
    VIDMEM_EVICT_PROC *pProcs;
    NvU64              nowNs = osGetMonotonicTimeNs();
    NvU64              wantMB, avail;
    NvU32              count;
    NvBool             bOk;
    NvBool             bLocked = NV_FALSE;

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
    wantMB = (size >> 20) + 1 + VIDMEM_EVICT_HINT_LOW_MB;
    avail  = _vidmemEvictHintFree(pGpu, pProcs, count, wantMB, pRmClient->ProcID, "allocation");
    bOk    = (avail > (size >> 20));
    _vidmemEvictUpdateReclaimable(pProcs, count, nowNs);
    lastFruitlessNs = bOk ? 0 : nowNs;
    if (bOk)
        _vidmemLastPressureNs = nowNs;

    portMemFree(pProcs);
    if (bLocked)
        rmDeviceGpuLocksRelease(pGpu, GPUS_LOCK_FLAGS_NONE, NULL);
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

    if ((pHeap == NULL) ||
        !vidmemGetUserReserveBlockInfo(pHeap->pPmaObject, &blockBytes, &blockFree, &blockLargest))
        return;
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
    EVICT_PRINTF("window hints: %s; hidden processes could give up %llu MB (added to the budget of visible ones)\n",
                 _vidmemHintsActive() ? "on (policy follows window visibility)" : "off (GPU samples)",
                 _vidmemReclaimableBytes >> 20);
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
    else if (mode == 9)
    {
        // Window hints changed (procfs vidmem_hints): run the policy now.
        _vidmemEvictPolicyTick(pGpu);
        EVICT_PRINTF("policy run\n");
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
