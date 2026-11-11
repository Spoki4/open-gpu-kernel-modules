/*
 * SPDX-FileCopyrightText: Copyright (c) 2020-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "mem_mgr/video_mem.h"
#include "gpu/mem_mgr/mem_desc.h"
#include "gpu/mem_mgr/heap.h"
#include "gpu/mem_mgr/phys_mem_allocator/phys_mem_allocator.h"
#include "gpu/mem_mgr/phys_mem_allocator/phys_mem_allocator_private.h" // local patch: region list
#include "gpu/mem_mgr/phys_mem_allocator/regmap.h"                    // local patch: block scan
#include "gpu/mem_mgr/mem_mgr.h"
#include "gpu/mem_mgr/mem_utils.h"
#include "gpu/mem_sys/kern_mem_sys.h"
#include "kernel/gpu/mig_mgr/kernel_mig_manager.h"
#include "vgpu/rpc.h"
#include "core/locks.h"
#include "kernel/gpu/rc/kernel_rc.h"
#include "diagnostics/gpu_acct.h"
#include "gpu/device/device.h"
#include "gpu/bus/third_party_p2p.h"
#include "gpu/bus/kern_bus.h"

#include "class/cl0040.h" // NV01_MEMORY_LOCAL_USER
#include "mem_mgr/vidmem_trace.h" // local patch

/*!
 * _vidmemQueryAlignment
 *
 * @brief
 *     Returns the size and alignment for this allocation.
 *
 * @param[in]  pRVHCP      Pointer to RmVidHeapControlParams data
 * @param[in]  pAllocData  Pointer to VIDHEAP_ALLOC_DATA
 * @param[out] pSize       The size aligned to the HW/requested alignment
 * @param[out] pAlign      The alignment required for this allocation.

 * @returns
 *      NV_OK Operation is successful.
 */
static NV_STATUS
_vidmemQueryAlignment
(
    MEMORY_ALLOCATION_REQUEST *pAllocRequest,
    NvU64                     *pSize,
    NvU64                     *pAlign
)
{
    NV_MEMORY_ALLOCATION_PARAMS *pAllocData     = pAllocRequest->pUserParams;
    OBJGPU                      *pGpu           = pAllocRequest->pGpu;
    MemoryManager               *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    NV_STATUS                    rmStatus       = NV_OK;
    NvU64                        size           = pAllocData->size;
    NvU64                        pageSize       = 0;
    NvU64                        align          = 0;
    NvU32                        retAttr        = pAllocData->attr;
    NvU32                        retAttr2       = pAllocData->attr2;

    NV_ASSERT_OR_RETURN((NULL != pSize) && (NULL != pAlign),
                      NV_ERR_INVALID_ARGUMENT);

    if ((pAllocData->flags & NVOS32_ALLOC_FLAGS_ALIGNMENT_HINT) ||
        (pAllocData->flags & NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE))
        align = pAllocData->alignment;
    else
        align = RM_PAGE_SIZE;

    // Fetch RM page size
    pageSize = memmgrDeterminePageSize(pMemoryManager, pAllocRequest->hClient, size, pAllocData->format,
                                       pAllocData->flags, &retAttr, &retAttr2);

    if (pageSize == 0)
    {
        rmStatus = NV_ERR_INVALID_STATE;
        NV_ASSERT_OK_FAILED("memmgrDeterminePageSize", rmStatus);
        return rmStatus;
    }

    // Fetch memory alignment
    NV_ASSERT_OK_OR_RETURN(memmgrAllocDetermineAlignment_HAL(pGpu, pMemoryManager,
                    &size, &align, 0, pAllocData->flags, retAttr, retAttr2, 0));

    *pSize = size;
    *pAlign = align + 1;

    return rmStatus;
}

//
// Local patch: keep a slice of vidmem free for the desktop compositor.
//
// When a game fills vidmem, the Wayland compositor can no longer allocate its
// scanout buffers (nvidia-drm only falls back to sysmem for NO_SCANOUT GEM
// objects) and the whole desktop freezes. With RmVidmemUserReserveMB set
// (NVreg_RegistryDwords="RmVidmemUserReserveMB=768"), the bottom that many MB
// of vidmem belong to the compositor: allocations made by ordinary processes
// get a PMA address range above it (PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE) and
// fail with NV_ERR_NO_MEMORY once the free memory outside the block is used
// up. A block rather than a byte count, because scanout buffers must be
// physically contiguous: with only a byte reserve, a game freeing and
// re-allocating at the limit left 776 MB free but 66 MB (often less)
// contiguous (see local/README.md). The compositor and kernel threads keep
// PMA's bottom-up placement for contiguous allocations, and their
// non-contiguous ones (textures, pixmaps) go top-down, so they use the block
// only once everything above is taken. Reserve-bound processes are told the
// free memory outside the block as HEAP_FREE, which their Vulkan/DXGI memory
// budget is built from.
//
// Exempt: the compositor processes listed below, matched by the RM client's
// name (process name recorded when the client was created) or by the current
// thread's name, and kernel clients when they allocate from a kernel thread.
// nvkms also allocates with kernel privilege inside the ioctl of whichever
// process asked for a GEM or dumb buffer (GBM, DRM_IOCTL_MODE_CREATE_DUMB);
// those allocations count against that process, and nvidia-drm then falls
// back to sysmem for NO_SCANOUT buffers.
//
// Without the reserve (0 or unset, the default since 2026-11-04) nothing is
// held back; the desktop is protected by priority instead:
//   - placement: every non-contiguous PMA allocation goes top-down
//     (vidmemPmaTopDown()), so the bottom of vidmem keeps the contiguous
//     ones, the compositor's scanout buffers among them;
//   - the compositor is never turned away: when its allocation fails, others
//     are moved to sysmem until it fits, for a scanout buffer a contiguous
//     window (vidmemEvictMakeRoomCompositor());
//   - background processes (hidden, or windowless and idle) do not get the
//     last free vidmem: below vidmemEvictBackgroundCushion() their new
//     allocations are created in sysmem (vidmemEvictWantBorn());
//   - visible ones get room made by moving others out, as with the reserve.
//
#define NV_REG_STR_RM_VIDMEM_USER_RESERVE_MB "RmVidmemUserReserveMB"

static const char * const _vidmemReserveExemptProcs[] =
{
    "kwin_wayland",
    "Xwayland",
};

static NvBool
_vidmemIsReserveExempt(const char *name)
{
    NvU32 i;

    for (i = 0; i < NV_ARRAY_ELEMENTS(_vidmemReserveExemptProcs); i++)
    {
        if (portStringCompare(name, _vidmemReserveExemptProcs[i], NV_PROC_NAME_MAX_LENGTH) == 0)
            return NV_TRUE;
    }
    return NV_FALSE;
}

static NvU32 _vidmemUserReserveMB = NV_U32_MAX; // NV_U32_MAX: registry not read yet

// Local patch: for mem_mgr/vidmem_evict.c
NvBool vidmemIsReserveExemptName(const char *name);
NvBool vidmemUserReserveBoundRangeAny(PMA *pPma, NvU64 pageSize, NvU64 *pBegin, NvU64 *pEnd);
void vidmemEvictAutoInit(OBJGPU *pGpu, NvU32 reserveMB);
void vidmemEvictNoteBlock(PMA *pPma);
NV_STATUS vidmemEvictNoteDupBar1(MEMORY_DESCRIPTOR *pMemDesc);
NvBool vidmemEvictWantBorn(OBJGPU *pGpu, RmClient *pRmClient, NV_MEMORY_ALLOCATION_PARAMS *pAllocData);
NvBool vidmemEvictMakeRoom(OBJGPU *pGpu, RmClient *pRmClient, NvU64 size);
NvBool vidmemEvictMakeRoomCompositor(OBJGPU *pGpu, RmClient *pRmClient, NvU64 size, NvBool bContig, NvU64 align);
NvBool vidmemEvictMakeRoomPlace(OBJGPU *pGpu, RmClient *pRmClient, NvU64 size, NvBool bContig, NvU64 align);
// Alignment PMA was last asked for (_vidmemPmaAllocate()), for the compositor's retry.
static NvU64 _vidmemLastPmaAlign;
//
// Whether that request was contiguous and how many bytes it asked for: an
// allocation with PHYSICALITY_DEFAULT is contiguous too (bContig below), and
// the size is rounded up to its pages. Making room by the attribute alone
// counted free 2 MB chunks for a contiguous 6 MB WebGL buffer, saw enough and
// moved nothing (nvtest fgprio-idle after a scanout stress run: 35 free chunks,
// longest free run 5 MB), and the same for kwin's 32 MB buffers (scanout
// churn-8x16).
//
static NvBool _vidmemLastPmaContig;
static NvU64  _vidmemLastPmaBytes;
NvU64 vidmemEvictBackgroundCushion(RmClient *pRmClient);
void vidmemEvictKeepFgHeadroom(OBJGPU *pGpu, RmClient *pRmClient, NvU64 size);
NvU64 vidmemEvictReclaimableFor(RmClient *pRmClient);
NV_STATUS vidmemEvictBornAllocation(OBJGPU *pGpu, Memory *pMemory);

NvBool
vidmemIsReserveExemptName(const char *name)
{
    return _vidmemIsReserveExempt(name);
}

void osGetCurrentProcessLeaderName(char *ProcName, NvU32 Length); // arch/nvalloc/unix/src/os.c

// Used by osapi.c (vidmem_pma), kern_mem_sys_ctrl.c (HEAP_FREE) and
// phys_mem_allocator.c (pmaAllocatePages).
NvBool vidmemGetUserReserveBlockInfo(PMA *pPma, NvU64 *pBlockBytes, NvU64 *pFreeBytes,
                                     NvU64 *pLargestFreeBytes);
NvU64 vidmemUserReserveReportedFree(PMA *pPma, RmClient *pRmClient, NvU64 freeBytes);
NvBool vidmemUserReservePmaRange(PMA *pPma, NvU64 pageSize, NvU64 *pBegin, NvU64 *pEnd);

//
// The physical range reserve-bound allocations may use: everything above the
// bottom _vidmemUserReserveMB of the lowest PMA region, aligned to pageSize.
//
static NvBool
_vidmemReserveBoundRange
(
    PMA   *pPma,
    NvU64  pageSize,
    NvU64 *pBegin,
    NvU64 *pEnd
)
{
    static NvBool bLogged;
    NvU64         base = NV_U64_MAX;
    NvU64         limit = 0;
    NvU32         i;

    if ((_vidmemUserReserveMB == 0) || (_vidmemUserReserveMB == NV_U32_MAX))
        return NV_FALSE;

    for (i = 0; i < pPma->regSize; i++)
    {
        PMA_REGION_DESCRIPTOR *pDesc = pPma->pRegDescriptors[i];

        if ((pDesc == NULL) || pDesc->bProtected)
            continue;
        if (!bLogged)
            NV_PRINTF(LEVEL_ERROR, "vidmem user reserve: PMA region %u 0x%llx-0x%llx%s\n",
                      i, pDesc->base, pDesc->limit, pDesc->bSupportISO ? " ISO" : "");
        base  = NV_MIN(base, pDesc->base);
        limit = NV_MAX(limit, pDesc->limit);
    }
    bLogged = NV_TRUE;

    if (base == NV_U64_MAX)
        return NV_FALSE;

    *pBegin = NV_ALIGN_UP(base + ((NvU64)_vidmemUserReserveMB << 20), pageSize);
    *pEnd   = NV_ALIGN_DOWN(limit + 1, pageSize);
    if (*pEnd <= *pBegin)
        return NV_FALSE;
    *pEnd -= 1;
    return NV_TRUE;
}

// The reserve-bound range regardless of the calling thread (the eviction
// worker is a kernel thread but restores memory of ordinary processes).
NvBool
vidmemUserReserveBoundRangeAny(PMA *pPma, NvU64 pageSize, NvU64 *pBegin, NvU64 *pEnd)
{
    return (pPma != NULL) && _vidmemReserveBoundRange(pPma, pageSize, pBegin, pEnd);
}

//
// Without the reserve, non-contiguous PMA allocations without an address range
// go top-down (pmaAllocatePages()): textures and game memory fill vidmem from
// the top, contiguous allocations (scanout buffers, cursors) keep the bottom.
// Nothing is held back; it only keeps the compositor's contiguous buffers from
// having to wait for a window to be cleared.
//
NvBool
vidmemPmaTopDown(PMA *pPma)
{
    return (pPma != NULL) && (_vidmemUserReserveMB == 0);
}

// The frame map of the lowest unprotected PMA region, which holds the block.
static PMA_REGMAP *
_vidmemReserveBlockRegmap(PMA *pPma)
{
    PMA_REGMAP *pRegmap = NULL;
    NvU64       lowest = NV_U64_MAX;
    NvU32       i;

    for (i = 0; i < pPma->regSize; i++)
    {
        PMA_REGION_DESCRIPTOR *pDesc = pPma->pRegDescriptors[i];

        if ((pDesc != NULL) && !pDesc->bProtected && (pDesc->base < lowest))
        {
            lowest  = pDesc->base;
            pRegmap = (PMA_REGMAP *)pPma->pRegions[i];
        }
    }
    return pRegmap;
}

// Free bytes inside the bottom block (popcount over its ~200 map words).
static NvU64
_vidmemReserveBlockFreeBytes(PMA *pPma)
{
    PMA_REGMAP *pRegmap = _vidmemReserveBlockRegmap(pPma);
    NvU64       frames, word, used = 0, idx;

    if (pRegmap == NULL)
        return 0;

    frames = NV_MIN(((NvU64)_vidmemUserReserveMB << 20) >> PMA_PAGE_SHIFT, pRegmap->totalFrames);

    portSyncSpinlockAcquire(pPma->pPmaLock);
    for (idx = 0; idx < (frames >> 6); idx++)
    {
        word = pRegmap->map[MAP_IDX_ALLOC_UNPIN][idx] | pRegmap->map[MAP_IDX_ALLOC_PIN][idx];
        used += nvPopCount64(word);
    }
    if ((frames & 63) != 0)
    {
        word = pRegmap->map[MAP_IDX_ALLOC_UNPIN][idx] | pRegmap->map[MAP_IDX_ALLOC_PIN][idx];
        used += nvPopCount64(word & (NVBIT64(frames & 63) - 1));
    }
    portSyncSpinlockRelease(pPma->pPmaLock);

    return (frames - used) << PMA_PAGE_SHIFT;
}

//
// Local patch, for /proc/driver/nvidia/gpus/<bdf>/vidmem_pma: size of the
// bottom reserve block and how much of it is free / largest contiguous free,
// read from the PMA frame bitmap. Returns NV_FALSE if the reserve is off.
//
NvBool
vidmemGetUserReserveBlockInfo
(
    PMA   *pPma,
    NvU64 *pBlockBytes,
    NvU64 *pFreeBytes,
    NvU64 *pLargestFreeBytes
)
{
    PMA_REGMAP *pRegmap;
    NvU64       frames, frame, run = 0, freeFrames = 0, largest = 0;

    *pBlockBytes = *pFreeBytes = *pLargestFreeBytes = 0;

    if ((pPma == NULL) || (_vidmemUserReserveMB == 0) || (_vidmemUserReserveMB == NV_U32_MAX))
        return NV_FALSE;

    pRegmap = _vidmemReserveBlockRegmap(pPma);

    portSyncSpinlockAcquire(pPma->pPmaLock);

    if (pRegmap != NULL)
    {
        frames = NV_MIN(((NvU64)_vidmemUserReserveMB << 20) >> PMA_PAGE_SHIFT, pRegmap->totalFrames);
        for (frame = 0; frame < frames; frame++)
        {
            NvU64 word = pRegmap->map[MAP_IDX_ALLOC_UNPIN][frame >> 6] |
                         pRegmap->map[MAP_IDX_ALLOC_PIN][frame >> 6];

            if ((word >> (frame & 63)) & 1)
            {
                run = 0;
            }
            else
            {
                freeFrames++;
                run++;
                largest = NV_MAX(largest, run);
            }
        }
        *pBlockBytes       = frames << PMA_PAGE_SHIFT;
        *pFreeBytes        = freeFrames << PMA_PAGE_SHIFT;
        *pLargestFreeBytes = largest << PMA_PAGE_SHIFT;
    }

    portSyncSpinlockRelease(pPma->pPmaLock);

    return (pRegmap != NULL);
}

//
// Local patch, for vidmemEvictMakeRoomCompositor() (no reserve): a copy of the
// allocated-frame map of the lowest unprotected PMA region (bit set: frame
// taken, pinned or not, or retired), with the region's base address, its
// frame count (PMA_PAGE_SHIFT frames) and the largest contiguous free run in
// bytes. NULL if there is no such region or no memory. portMemFree() it.
//
NvU64 *
vidmemPmaUsedMap
(
    PMA   *pPma,
    NvU64 *pBase,
    NvU64 *pFrames,
    NvU64 *pLargestFree
)
{
    PMA_REGMAP *pRegmap = NULL;
    NvU64       lowest = NV_U64_MAX, words, w, frame, run = 0, largest = 0;
    NvU64      *pMap;
    NvU32       i;

    if (pPma == NULL)
        return NULL;
    for (i = 0; i < pPma->regSize; i++)
    {
        PMA_REGION_DESCRIPTOR *pDesc = pPma->pRegDescriptors[i];

        if ((pDesc != NULL) && !pDesc->bProtected && (pDesc->base < lowest))
        {
            lowest  = pDesc->base;
            pRegmap = (PMA_REGMAP *)pPma->pRegions[i];
        }
    }
    if (pRegmap == NULL)
        return NULL;

    words = (pRegmap->totalFrames + 63) >> 6;
    pMap = portMemAllocNonPaged(words * sizeof(NvU64));
    if (pMap == NULL)
        return NULL;

    portSyncSpinlockAcquire(pPma->pPmaLock);
    for (w = 0; w < words; w++)
        pMap[w] = pRegmap->map[MAP_IDX_ALLOC_UNPIN][w] | pRegmap->map[MAP_IDX_ALLOC_PIN][w] |
                  pRegmap->map[MAP_IDX_BLACKLIST][w];
    portSyncSpinlockRelease(pPma->pPmaLock);

    for (frame = 0; frame < pRegmap->totalFrames; )
    {
        NvU64 word = pMap[frame >> 6];

        // Whole words at once where they are all free or all taken (problem #16:
        // this runs before and after every move batch while tracing).
        if (((frame & 63) == 0) && (frame + 64 <= pRegmap->totalFrames) && ((word == 0) || (word == ~0ULL)))
        {
            run = (word == 0) ? run + 64 : 0;
            largest = NV_MAX(largest, run);
            frame += 64;
            continue;
        }
        if ((word >> (frame & 63)) & 1)
        {
            run = 0;
        }
        else
        {
            run++;
            largest = NV_MAX(largest, run);
        }
        frame++;
    }
    // Frames past the end count as taken.
    for (frame = pRegmap->totalFrames; frame < (words << 6); frame++)
        pMap[frame >> 6] |= NVBIT64(frame & 63);

    *pBase        = lowest;
    *pFrames      = pRegmap->totalFrames;
    *pLargestFree = largest << PMA_PAGE_SHIFT;
    return pMap;
}

static NvBool
_vidmemIsReserveExemptContext
(
    CALL_CONTEXT *pCallContext,
    RmClient     *pRmClient,
    char         *procName,
    NvU32         procNameSize
)
{
    portMemSet(procName, 0, procNameSize);

    if ((pCallContext->secInfo.privLevel >= RS_PRIV_LEVEL_KERNEL) &&
        ((osGetCurrentProcessFlags() & OS_CURRENT_PROCESS_FLAG_KERNEL_THREAD) != 0))
        return NV_TRUE;

    if (pRmClient != NULL && _vidmemIsReserveExempt(pRmClient->name))
        return NV_TRUE;

    osGetCurrentProcessName(procName, procNameSize);
    if (_vidmemIsReserveExempt(procName))
        return NV_TRUE;

    // A renamed thread of an exempt process (procName keeps the thread name for the log).
    {
        char leader[NV_PROC_NAME_MAX_LENGTH];

        portMemSet(leader, 0, sizeof(leader));
        osGetCurrentProcessLeaderName(leader, sizeof(leader));
        return _vidmemIsReserveExempt(leader);
    }
}

//
// Local patch, called by pmaAllocatePages() for every PMA allocation that does
// not already carry an address range. Allocations RM makes internally on behalf
// of a process (GR context buffers, page tables, channel memory...) do not go
// through vidmemConstruct, and PMA places them bottom-up, i.e. into the
// reserve block: every new GPU process (a game, a restarted browser) ate into
// it and left holes behind (block 359 -> 97 MB free, 34 MB contiguous). Unless
// the calling thread belongs to an exempt process (by thread or process name)
// or is a kernel thread, those allocations get the range above the block too.
//
NvBool
vidmemUserReservePmaRange
(
    PMA   *pPma,
    NvU64  pageSize,
    NvU64 *pBegin,
    NvU64 *pEnd
)
{
    char name[NV_PROC_NAME_MAX_LENGTH];

    if ((_vidmemUserReserveMB == 0) || (_vidmemUserReserveMB == NV_U32_MAX) || (pPma == NULL))
        return NV_FALSE;

    if ((osGetCurrentProcessFlags() & OS_CURRENT_PROCESS_FLAG_KERNEL_THREAD) != 0)
        return NV_FALSE;

    portMemSet(name, 0, sizeof(name));
    osGetCurrentProcessName(name, sizeof(name));
    if (_vidmemIsReserveExempt(name))
        return NV_FALSE;

    portMemSet(name, 0, sizeof(name));
    osGetCurrentProcessLeaderName(name, sizeof(name));
    if (_vidmemIsReserveExempt(name))
        return NV_FALSE;

    return _vidmemReserveBoundRange(pPma, pageSize, pBegin, pEnd);
}

//
// Local patch: free vidmem as reported to a client through
// NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE. The userspace driver builds the Vulkan
// memory budget (VK_EXT_memory_budget, DXGI's budget under vkd3d-proton) as
// "own usage + free", so without this a game at the limit still counts the
// free part of the block as its budget, plans its texture streaming for it
// and runs into denials. Reserve-bound callers get the free memory outside
// the block; exempt callers and kernel threads the real value.
//
NvU64
vidmemUserReserveReportedFree
(
    PMA      *pPma,
    RmClient *pRmClient,
    NvU64     freeBytes
)
{
    CALL_CONTEXT *pCallContext = resservGetTlsCallContext();
    char          procName[NV_PROC_NAME_MAX_LENGTH];
    NvU64         blockFree;

    if ((_vidmemUserReserveMB == NV_U32_MAX) || (pCallContext == NULL) || (pPma == NULL))
        return freeBytes;

    if (_vidmemIsReserveExemptContext(pCallContext, pRmClient, procName, sizeof(procName)))
        return freeBytes;

    if (_vidmemUserReserveMB == 0)
    {
        //
        // No reserve: the free memory, less the cushion a background process
        // may not take, plus what others could give up for a visible one.
        //
        NvU64 cushion = vidmemEvictBackgroundCushion(pRmClient);
        NvU64 reclaim = vidmemEvictReclaimableFor(pRmClient);
        NvU64 reported = ((freeBytes > cushion) ? (freeBytes - cushion) : 0) + reclaim;

        if (osVidmemTraceOn(VMT_BUDGET))
        {
            NvU64 a[VMT_ARGS] = { freeBytes, cushion, reclaim, reported };
            osVidmemTrace(VMT_BUDGET, (pRmClient != NULL) ? pRmClient->ProcID : osGetCurrentProcess(), a);
        }
        return reported;
    }

    blockFree = _vidmemReserveBlockFreeBytes(pPma);
    //
    // A visible process (window hints) is also told about the vidmem hidden
    // processes could give up: allocating it moves them out
    // (vidmemEvictMakeRoom()).
    //
    {
        NvU64 reclaim = vidmemEvictReclaimableFor(pRmClient);
        NvU64 reported = ((freeBytes > blockFree) ? (freeBytes - blockFree) : 0) + reclaim;

        if (osVidmemTraceOn(VMT_BUDGET))
        {
            NvU64 a[VMT_ARGS] = { freeBytes, blockFree, reclaim, reported };
            osVidmemTrace(VMT_BUDGET, (pRmClient != NULL) ? pRmClient->ProcID : osGetCurrentProcess(), a);
        }
        return reported;
    }
}

//
// Returns NV_ERR_NO_MEMORY if the free memory outside the block cannot hold
// the allocation (PMA would fail it anyway because of the address range; this
// logs it and skips the scan).
// *pbReserveBound is set when the allocation counts against the reserve and
// so must stay out of the bottom block (see _vidmemReserveBoundRange);
// *pbExempt when the reserve is on and the caller is exempt from it.
//
static NV_STATUS
_vidmemCheckUserReserve
(
    OBJGPU       *pGpu,
    Heap         *pHeap,
    CALL_CONTEXT *pCallContext,
    RmClient     *pRmClient,
    NvU64         size,
    NvBool       *pbReserveBound,
    NvBool       *pbExempt
)
{
    static NvU32 denyCount;
    NvU32        reserveMB;
    char         procName[NV_PROC_NAME_MAX_LENGTH];
    NvU64        freeBytes = 0;
    NvU64        blockFree;
    NvU64        outsideFree;

    *pbReserveBound = NV_FALSE;
    *pbExempt = NV_FALSE;

    if (_vidmemUserReserveMB == NV_U32_MAX)
    {
        NvU32 data32 = 0;

        if (osReadRegistryDword(pGpu, NV_REG_STR_RM_VIDMEM_USER_RESERVE_MB, &data32) != NV_OK)
            data32 = 0;
        _vidmemUserReserveMB = data32;
        if (data32 != 0)
            NV_PRINTF(LEVEL_ERROR, "vidmem user reserve: %u MB, kept at the bottom of vidmem\n",
                      data32);
        vidmemEvictAutoInit(pGpu, data32);
    }
    reserveMB = _vidmemUserReserveMB;

    if ((pHeap == NULL) || (pHeap->pPmaObject == NULL))
        return NV_OK;

    if (_vidmemIsReserveExemptContext(pCallContext, pRmClient, procName, sizeof(procName)))
    {
        *pbExempt = NV_TRUE;
        return NV_OK;
    }

    //
    // Without the reserve *pbReserveBound only means "an ordinary process":
    // no address range, but the same room making / sysmem fallback, with a
    // background process kept off the last vidmemEvictBackgroundCushion().
    //
    *pbReserveBound = NV_TRUE;

    pmaGetFreeMemory(pHeap->pPmaObject, &freeBytes);
    if (reserveMB == 0)
    {
        blockFree   = vidmemEvictBackgroundCushion(pRmClient);
        outsideFree = (freeBytes > blockFree) ? (freeBytes - blockFree) : 0;
        if (outsideFree >= size)
            return NV_OK;
        // Not worth a log line: a background process gets sysmem instead.
        if (blockFree != 0)
            return NV_ERR_NO_MEMORY;
    }
    else
    {
        blockFree   = _vidmemReserveBlockFreeBytes(pHeap->pPmaObject);
        outsideFree = (freeBytes > blockFree) ? (freeBytes - blockFree) : 0;
        if (outsideFree >= size)
            return NV_OK;
    }

    denyCount++;
    if (denyCount <= 8 || (denyCount % 1024) == 0)
    {
        NV_PRINTF(LEVEL_ERROR,
                  "vidmem user reserve: denied %llu KB to %s/%s (pid %u%s), free %llu MB, "
                  "%llu MB of it in the reserve block, denials %u\n",
                  size >> 10, (pRmClient != NULL) ? pRmClient->name : "?", procName,
                  osGetCurrentProcess(),
                  (pCallContext->secInfo.privLevel >= RS_PRIV_LEVEL_KERNEL) ? ", kernel client" : "",
                  freeBytes >> 20, blockFree >> 20, denyCount);
    }

    return NV_ERR_NO_MEMORY;
}

/*!
 * _vidmemPmaAllocate
 *
 * @brief
 *     Allocates memory on vidmem through PMA.
 *
 * @param[in] pHeap         Pointer to Heap object
 * @param[in] pAllocRequest Pointer to the MEMORY_ALLOCATION_REQUEST.
 * @param[in] bReserveBound Local patch: keep out of the bottom reserve block
 *                          (see _vidmemCheckUserReserve).
 * @param[in] bExempt       Local patch: caller is exempt from the reserve;
 *                          non-contiguous allocations go top-down.
 *
 * @returns
 *      NV_OK    Operation is successful
 *      NV_ERR_* Error code in case of errors.
 */
static NV_STATUS
_vidmemPmaAllocate
(
    Heap                      *pHeap,
    MEMORY_ALLOCATION_REQUEST *pAllocRequest,
    NvBool                     bReserveBound,
    NvBool                     bExempt,
    NvBool                     bAnywhere
)
{
    NV_MEMORY_ALLOCATION_PARAMS *pAllocData     = pAllocRequest->pUserParams;
    OBJGPU                      *pGpu           = pAllocRequest->pGpu;
    MemoryManager               *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    PMA                         *pPma           = pHeap->pPmaObject;
    NvU64                        size           = 0;
    NvU32                        pageCount      = 0;
    NvU32                        pmaInfoSize;
    NvU64                        pageSize;
    NV_STATUS                    status;
    NvU64                        sizeAlign    = 0;
    PMA_ALLOCATION_OPTIONS       allocOptions = {0};
    NvBool                       bContig;
    NvU32                        pmaConfig    = PMA_QUERY_NUMA_ENABLED;

    // Local patch: nothing asked of PMA yet (a failure before it is not its).
    _vidmemLastPmaContig = FLD_TEST_DRF(OS32, _ATTR, _PHYSICALITY, _CONTIGUOUS, pAllocData->attr);
    _vidmemLastPmaBytes  = 0;

    status = pmaQueryConfigs(pPma, &pmaConfig);
    NV_ASSERT(status == NV_OK);

    //
    // In NUMA platforms, contig memory is allocated using page order from
    // kernel and that could lead to memory wastage when the size is not
    // naturally aligned to page order. Prefer non-contig when clients
    // are okay with NON_CONTIG.
    //
    if ((status == NV_OK) && (pmaConfig & PMA_QUERY_NUMA_ENABLED))
    {
        bContig =
            !FLD_TEST_DRF(OS32, _ATTR, _PHYSICALITY,
                                _ALLOW_NONCONTIGUOUS, pAllocData->attr) &&
            !FLD_TEST_DRF(OS32, _ATTR, _PHYSICALITY,
                          _NONCONTIGUOUS, pAllocData->attr);
    }
    else
    {
        bContig = !FLD_TEST_DRF(OS32, _ATTR,
                                _PHYSICALITY, _NONCONTIGUOUS,
                                pAllocData->attr);
    }

    NV_PRINTF(LEVEL_INFO, "PMA input\n");
    NV_PRINTF(LEVEL_INFO, "          Owner: 0x%x\n", pAllocData->owner);
    NV_PRINTF(LEVEL_INFO, "        hMemory: 0x%x\n", pAllocRequest->hMemory);
    NV_PRINTF(LEVEL_INFO, "           Type: 0x%x\n", pAllocData->type);
    NV_PRINTF(LEVEL_INFO, "          Flags: 0x%x\n", pAllocData->flags);
    NV_PRINTF(LEVEL_INFO, "          Begin: 0x%08llx\n", pAllocData->rangeLo);
    NV_PRINTF(LEVEL_INFO, "            End: 0x%08llx\n", pAllocData->rangeHi);
    NV_PRINTF(LEVEL_INFO, "         Height: 0x%x\n", pAllocData->height);
    NV_PRINTF(LEVEL_INFO, "          Width: 0x%x\n", pAllocData->width);
    NV_PRINTF(LEVEL_INFO, "          Pitch: 0x%x\n", pAllocData->pitch);
    NV_PRINTF(LEVEL_INFO, "           Size: 0x%08llx\n", pAllocData->size);
    NV_PRINTF(LEVEL_INFO, "      Alignment: 0x%08llx\n",
              pAllocData->alignment);
    NV_PRINTF(LEVEL_INFO, "         Offset: 0x%08llx\n", pAllocData->offset);
    NV_PRINTF(LEVEL_INFO, "           Attr: 0x%x\n", pAllocData->attr);
    NV_PRINTF(LEVEL_INFO, "          Attr2: 0x%x\n", pAllocData->attr2);
    NV_PRINTF(LEVEL_INFO, "         Format: 0x%x\n", pAllocData->format);
    NV_PRINTF(LEVEL_INFO, "      ComprCovg: 0x%x\n", pAllocData->comprCovg);
    NV_PRINTF(LEVEL_INFO, "      ZCullCovg: 0x%x\n", pAllocData->zcullCovg);
    NV_PRINTF(LEVEL_INFO, "     CtagOffset: 0x%x\n", pAllocData->ctagOffset);
    NV_PRINTF(LEVEL_INFO, "       hVASpace: 0x%x\n", pAllocData->hVASpace);

    // Get the page size returned by RM.
    pageSize = stdmemQueryPageSize(pMemoryManager, pAllocRequest->hClient, pAllocData);
    NV_ASSERT_OR_RETURN(pageSize != 0, NV_ERR_INVALID_STATE);

    if (pageSize == RM_PAGE_SIZE)
    {
        //
        // TODO Remove this after the suballocator is in place
        // Minimum granularity of PMA is 64K.
        //
        pageSize = RM_PAGE_SIZE_64K;
    }

    // Get the alignment returned by RM without actually allocating.
    status = _vidmemQueryAlignment(pAllocRequest, &size, &sizeAlign);

    //
    // Pass the turn blacklist off flag to PMA Allocation API
    // No need for FB check since PMA only manages FB
    // Bug:2451834, gpuCheckPageRetirementSupport should not be called outside
    // RM lock.
    //
    if (FLD_TEST_DRF(OS32, _ATTR2, _BLACKLIST, _OFF, pAllocData->attr2))
    {
        allocOptions.flags |= PMA_ALLOCATE_TURN_BLACKLIST_OFF;
    }

    NV_ASSERT_OR_RETURN(NV_OK == status, status);

    // RM allocations are always pinned.
    allocOptions.flags |= PMA_ALLOCATE_PINNED;

    if (pAllocData->flags & NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM)
    {
        allocOptions.flags |= PMA_ALLOCATE_PERSISTENT;
    }

    // Check for VPR region.
    if (pAllocData->flags & NVOS32_ALLOC_FLAGS_PROTECTED)
    {
        allocOptions.flags |= PMA_ALLOCATE_PROTECTED_REGION;
    }

    // Check memory alloc direction.
    if (pAllocData->flags & NVOS32_ALLOC_FLAGS_FORCE_REVERSE_ALLOC)
    {
        allocOptions.flags |= PMA_ALLOCATE_REVERSE_ALLOC;
    }

    // Local patch: keep the compositor's non-contiguous allocations out of the
    // bottom reserve block while there is room above (PMA: no reverse on NUMA).
    if (bExempt && !bContig && !(pmaConfig & PMA_QUERY_NUMA_ENABLED))
    {
        allocOptions.flags |= PMA_ALLOCATE_REVERSE_ALLOC;
    }

    // Fixed address allocations.
    if (pAllocData->flags & NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE)
    {
        NvU64 offsetAlign = NV_MAX(sizeAlign, pageSize);

        allocOptions.flags |= PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE;
        allocOptions.physBegin = NV_ALIGN_DOWN(pAllocData->offset, offsetAlign);
        allocOptions.physEnd   = NV_ALIGN_UP(allocOptions.physBegin + size, offsetAlign) - 1;
        size = allocOptions.physEnd - allocOptions.physBegin + 1;
    }

    // Range based allocations.
    if (pAllocData->flags & NVOS32_ALLOC_FLAGS_USE_BEGIN_END)
    {
        allocOptions.flags |= PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE;
        allocOptions.physBegin = NV_ALIGN_UP(pAllocData->rangeLo, ((NvU64)pageSize));
        allocOptions.physEnd   = NV_ALIGN_DOWN(pAllocData->rangeHi, ((NvU64)pageSize));
        allocOptions.physEnd = (allocOptions.physEnd > 0) ?
                                          allocOptions.physEnd - 1 : 0;
        NV_ASSERT_OR_RETURN(allocOptions.physBegin <= allocOptions.physEnd,
                         NV_ERR_INVALID_ARGUMENT);
    }

    // Local patch: reserve-bound allocations stay above the bottom reserve block.
    if (bReserveBound && !(allocOptions.flags & PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE))
    {
        NvU64 physBegin, physEnd;

        if (_vidmemReserveBoundRange(pPma, pageSize, &physBegin, &physEnd))
        {
            allocOptions.flags |= PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE;
            allocOptions.physBegin = physBegin;
            allocOptions.physEnd   = physEnd;
        }
    }

    //
    // Local patch: an allocation that goes to sysmem right after this
    // (vidmemEvictBornAllocation) may sit anywhere for that moment, the
    // reserve block included. An explicit whole range, so pmaAllocatePages()
    // does not add the above-the-block one.
    //
    if (bAnywhere && !(allocOptions.flags & PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE))
    {
        NvU64 align = NV_MAX(sizeAlign, pageSize);

        allocOptions.flags    |= PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE;
        allocOptions.physBegin = 0;
        allocOptions.physEnd   = NV_ALIGN_DOWN64(NV_U64_MAX, align) - 1;
    }

    // Skip scrubber, used only by scrubber construction path
    if (pAllocData->internalflags & NVOS32_ALLOC_INTERNAL_FLAGS_SKIP_SCRUB)
    {
        allocOptions.flags |= PMA_ALLOCATE_NO_ZERO;
    }

    // Pass along client requested alignment
    allocOptions.flags |= PMA_ALLOCATE_FORCE_ALIGNMENT;
    allocOptions.alignment = NV_MAX(sizeAlign, pageSize);

    if (FLD_TEST_DRF(OS32, _ATTR2, _ENABLE_LOCALIZED_MEMORY, _UGPU0, pAllocData->attr2))
    {
        allocOptions.flags |= PMA_ALLOCATE_LOCALIZED_UGPU0;
    }
    else if (FLD_TEST_DRF(OS32, _ATTR2, _ENABLE_LOCALIZED_MEMORY, _UGPU1, pAllocData->attr2))
    {
        allocOptions.flags |= PMA_ALLOCATE_LOCALIZED_UGPU1;
    }

    // Get the number of pages to be allocated by PMA
    NV_CHECK_OR_RETURN(LEVEL_ERROR,
        (NV_DIV_AND_CEIL(size, pageSize) <= NV_U32_MAX),
        NV_ERR_NO_MEMORY);
    pageCount = (NvU32) NV_DIV_AND_CEIL(size, pageSize);

retry_alloc:
    // Evaluate the size of the PMA_ALLOC_INFO struct.
    if (bContig)
    {
        allocOptions.flags |= PMA_ALLOCATE_CONTIGUOUS;
        pmaInfoSize = sizeof(PMA_ALLOC_INFO);
    }
    else
    {
        NV_CHECK_OR_RETURN(LEVEL_ERROR,
            portSafeMulU32((pageCount - 1), (sizeof(NvU64)), &pmaInfoSize),
            NV_ERR_NO_MEMORY);
        NV_CHECK_OR_RETURN(LEVEL_ERROR,
            portSafeAddU32(pmaInfoSize, (sizeof(PMA_ALLOC_INFO)), &pmaInfoSize),
            NV_ERR_NO_MEMORY);
    }

    // Alloc the tracking structure and store the values in it.
    pAllocRequest->pPmaAllocInfo = portMemAllocNonPaged(pmaInfoSize);
    NV_ASSERT_OR_RETURN(NULL != pAllocRequest->pPmaAllocInfo, NV_ERR_NO_MEMORY);
    portMemSet(pAllocRequest->pPmaAllocInfo, 0, pmaInfoSize);

    pAllocRequest->pPmaAllocInfo->pageSize  = pageSize;
    pAllocRequest->pPmaAllocInfo->pageCount = pageCount;
    pAllocRequest->pPmaAllocInfo->allocSize = ((NvU64) pageCount) * pageSize;
    pAllocRequest->pPmaAllocInfo->bContig   = bContig;
    pAllocRequest->pPmaAllocInfo->refCount  = 1;
    pAllocRequest->pPmaAllocInfo->flags     = allocOptions.flags;

    NV_PRINTF(LEVEL_INFO, "\nNVRM:  Size requested: 0x%llx bytes\n", size);
    NV_PRINTF(LEVEL_INFO, "       PageSize: 0x%llx bytes\n", pageSize);
    NV_PRINTF(LEVEL_INFO, "      PageCount: 0x%x\n", pageCount);
    NV_PRINTF(LEVEL_INFO, "    Actual Size: 0x%llx\n",
              pAllocRequest->pPmaAllocInfo->allocSize);
    NV_PRINTF(LEVEL_INFO, "     Contiguous:  %s\n", bContig ? "YES" : "NO");

    // Get the allocation from PMA.
    _vidmemLastPmaAlign  = allocOptions.alignment;   // local patch
    _vidmemLastPmaContig = bContig;
    _vidmemLastPmaBytes  = (NvU64)pageCount * pageSize;
    status = pmaAllocatePages(pPma, pageCount, pageSize, &allocOptions,
                              pAllocRequest->pPmaAllocInfo->pageArray);
    if (NV_OK != status)
    {
        portMemFree(pAllocRequest->pPmaAllocInfo);
        pAllocRequest->pPmaAllocInfo = NULL;

        if (bContig)
        {
            if (FLD_TEST_DRF(OS32, _ATTR, _PHYSICALITY, _ALLOW_NONCONTIGUOUS, pAllocData->attr) ||
                (FLD_TEST_DRF(OS32, _ATTR, _PHYSICALITY, _DEFAULT, pAllocData->attr) &&
                   pHeap->getProperty(pHeap, PDB_PROP_HEAP_NONCONTIG_ALLOC_BY_DEFAULT)))
            {
                bContig = NV_FALSE;
                allocOptions.flags &= ~PMA_ALLOCATE_CONTIGUOUS;
                NV_PRINTF(LEVEL_INFO,
                          "pmaAllocatePages failed -- retrying as noncontiguous\n");
                goto retry_alloc;
            }
        }

        NV_PRINTF(LEVEL_WARNING, "pmaAllocatePages failed (%x)\n", status);
    }
    else
    {
        pAllocData->attr = (bContig ?
            FLD_SET_DRF(OS32, _ATTR, _PHYSICALITY, _CONTIGUOUS,    pAllocData->attr) :
            FLD_SET_DRF(OS32, _ATTR, _PHYSICALITY, _NONCONTIGUOUS, pAllocData->attr));
    }

    return status;
}

/*!
 * vidmemPmaFree
 *
 * @brief
 *     Frees the memory allocated by PMA
 *
 * @param[in] pGpu           Pointer to OBJGPU
 * @param[in] pHeap          Pointer to Heap object
 * @param[in] pPmaAllocInfo  Pointer to the PMA allocation tracking structure
 * @param[in] flag           Flags to modify PMA behavior
 *
 * @returns NONE
 */
void
vidmemPmaFree
(
    OBJGPU         *pGpu,
    Heap           *pHeap,
    PMA_ALLOC_INFO *pPmaAllocInfo,
    NvU32           flags
)
{
    PMA   *pPma  = pHeap->pPmaObject;
    NvU32 pmaFreeFlags = flags;

    NV_ASSERT_OR_RETURN_VOID(NULL != pPmaAllocInfo);

    // Decrement the refcount and free only in case of zero references.
    pPmaAllocInfo->refCount--;
    if (pPmaAllocInfo->refCount != 0)
    {
        return;
    }

    //
    // Skip the scrubber if the memory is allocated with scrubber skipped.
    // The only use case is scrubber internal allocations.
    //
    if (pPmaAllocInfo->flags & PMA_ALLOCATE_NO_ZERO)
    {
        pmaFreeFlags |= PMA_FREE_SKIP_SCRUB;
    }

    if (pPmaAllocInfo->bContig)
    {
        pmaFreePages(pPma, pPmaAllocInfo->pageArray, 1,
                     pPmaAllocInfo->allocSize, pmaFreeFlags);
    }
    else
    {
        pmaFreePages(pPma, pPmaAllocInfo->pageArray,
                     pPmaAllocInfo->pageCount,
                     pPmaAllocInfo->pageSize, pmaFreeFlags);
    }
    portMemFree(pPmaAllocInfo);
    pPmaAllocInfo = NULL;
}

Heap*
vidmemGetHeap
(
    OBJGPU  *pGpu,
    Device  *pDevice,
    NvBool   bSubheap,
    NvBool   bForceGlobalHeap
)
{
    MemoryManager *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
    NV_STATUS      status         = NV_OK;

    if (bSubheap)
    {
        Heap *pHeap = memmgrGetDeviceSuballocator(pMemoryManager, bSubheap);

        NV_CHECK_OR_RETURN(LEVEL_ERROR, pHeap != NULL && pHeap->heapType == HEAP_TYPE_PHYS_MEM_SUBALLOCATOR, NULL);
        return pHeap;
    }

    if (IS_MIG_IN_USE(pGpu) && !bForceGlobalHeap)
    {
        KernelMIGManager *pKernelMIGManager = GPU_GET_KERNEL_MIG_MANAGER(pGpu);
        Heap *pMemoryPartitionHeap = NULL;

        status = kmigmgrGetMemoryPartitionHeapFromDevice(pGpu, pKernelMIGManager, pDevice,
                                                         &pMemoryPartitionHeap);
        if (status == NV_OK)
        {
            if (pMemoryPartitionHeap != NULL)
                return pMemoryPartitionHeap;
        }
        else
        {
            NV_PRINTF(LEVEL_ERROR,
                "failed to get memory partition heap for hClient = 0x%x, hDevice = 0x%x\n",
                RES_GET_CLIENT_HANDLE(pDevice), RES_GET_HANDLE(pDevice));
            return NULL;
        }
    }

    return MEMORY_MANAGER_GET_HEAP(pMemoryManager);
}

static NV_STATUS
vidmemCopyConstruct
(
    VideoMemory                     *pVideoMemory,
    CALL_CONTEXT                    *pCallContext,
    RS_RES_ALLOC_PARAMS_INTERNAL    *pParams
)
{
    Memory            *pMemorySrc   = dynamicCast(pParams->pSrcRef->pResource, Memory);
    OBJGPU            *pGpu         = pMemorySrc->pMemDesc->pGpu;
    MEMORY_DESCRIPTOR *pMemDesc     = pMemorySrc->pMemDesc;
    NV_STATUS          status       = NV_ERR_INVALID_ARGUMENT;
    KernelBus         *pKernelBus    = GPU_GET_KERNEL_BUS(pGpu);
    NvBool             bSrcLockAcquired = NV_FALSE;

    if (!rmDeviceGpuLockIsOwner(pGpu->gpuInstance) && !rmGpuLockIsOwner())
    {
        NV_ASSERT_OK_OR_RETURN(rmDeviceGpuLocksAcquire(pGpu, GPUS_LOCK_FLAGS_NONE, RM_LOCK_MODULES_MEM));

        bSrcLockAcquired = NV_TRUE;
    }

    if (kbusShouldRefcountConstruct_HAL(pGpu, pKernelBus, pMemDesc))
    {
        // No flags specified on the initial static BAR1 mapping
        // (local patch: not for an allocation moved to sysmem: its vidmem pages
        // belong to someone else now; the reference is taken on restore)
        if (pMemDesc->pLocalEvictState != NULL)
            status = vidmemEvictNoteDupBar1(pMemDesc);
        else
            status = kbusIncreaseStaticBar1Refcount_HAL(pGpu, pKernelBus,
                           pMemDesc, BUS_MAP_FB_FLAGS_NONE);

        if (status == NV_OK)
        {
            // nothing
        }
        else if (status == NV_ERR_NOT_SUPPORTED)
        {
            status = NV_OK;
        }
        else
        {
            return status;
        }
    }

    switch (memdescGetCustomHeap(pMemDesc))
    {
        case MEMDESC_CUSTOM_HEAP_SCANOUT_CARVEOUT:
            status = memmgrDuplicateFromScanoutCarveoutRegion(pGpu,
                        GPU_GET_MEMORY_MANAGER(pGpu), pMemDesc);
            break;
        case MEMDESC_CUSTOM_HEAP_NONE:
            {
                MEMORY_DESCRIPTOR *pSrcMemDesc = pMemorySrc->pMemDesc;
                status = heapReference(pGpu, pSrcMemDesc->pHeap, pMemorySrc->HeapOwner,
                                       pSrcMemDesc);
                NV_ASSERT_OK_OR_RETURN(status);
            }
            break;
        default:
            NV_ASSERT_OR_RETURN(0, NV_ERR_INVALID_ARGUMENT);
            break;
    }

    // copy and reference any memory accounting charges
    if (status == NV_OK)
    {
        Memory *pMemory = staticCast(pVideoMemory, Memory);
        pMemory->pCharge = pMemorySrc->pCharge;
        memacctIncrementChargeRefCount(pMemory->pCharge);
    }

    if (bSrcLockAcquired)
    {
        // UNLOCK: release GPUs lock
        rmDeviceGpuLocksRelease(pGpu, GPUS_LOCK_FLAGS_NONE, NULL);
    }

    return status;
}

/*!
 * vidmemConstruct
 *
 * @brief
 *     This routine provides common allocation services used by the
 *     following heap allocation functions:
 *       NVOS32_FUNCTION_ALLOC_SIZE
 *       NVOS32_FUNCTION_ALLOC_SIZE_RANGE
 *       NVOS32_FUNCTION_ALLOC_TILED_PITCH_HEIGHT
 *
 * @param[in]  pVideoMemory      Pointer to VideoMemory object
 * @param[in]  pCallContext      Pointer to the current CALL_CONTEXT.
 * @param[in]  pParams           Pointer to the alloc params
 *
 * @return 'NV_OK'
 *     Operation completed successfully.
 * @return 'NV_ERR_NO_MEMORY'
 *     There is not enough available memory to satisfy allocation request.
 * @return 'NV_ERR_NOT_SUPPORTED'
 *     Operation not supported on broken FB.
 * @return 'NV_ERR_INSUFFICIENT_RESOURCES'
 *     Not enough available resources to satisfy allocation request.
 */
NV_STATUS
vidmemConstruct_IMPL
(
    VideoMemory                  *pVideoMemory,
    CALL_CONTEXT                 *pCallContext,
    RS_RES_ALLOC_PARAMS_INTERNAL *pParams
)
{
    Memory                      *pMemory               = staticCast(pVideoMemory, Memory);
    NV_MEMORY_ALLOCATION_PARAMS *pAllocData            = pParams->pAllocParams;
    NvHandle                     hClient               = pCallContext->pClient->hClient;
    NvHandle                     hParent               = pCallContext->pResourceRef->pParentRef->hResource;
    MEMORY_ALLOCATION_REQUEST    allocRequest          = {0};
    MEMORY_ALLOCATION_REQUEST   *pAllocRequest         = &allocRequest;
    OBJGPU                      *pGpu                  = pMemory->pGpu;
    MemoryManager               *pMemoryManager        = GPU_GET_MEMORY_MANAGER(pGpu);
    Heap                        *pHeap                 = NULL;
    NvBool                       bSubheap              = NV_FALSE;
    NvBool                       bRsvdHeap             = NV_FALSE;
    MEMORY_DESCRIPTOR           *pTopLevelMemDesc      = NULL;
    MEMORY_DESCRIPTOR           *pTempMemDesc          = NULL;
    HWRESOURCE_INFO              hwResource            = {0};
    RsClient                    *pRsClient             = pCallContext->pClient;
    RmClient                    *pRmClient             = dynamicCast(pRsClient, RmClient);
    RsResourceRef               *pResourceRef          = pCallContext->pResourceRef;
    RsResourceRef               *pDeviceRef;
    Device                      *pDevice;
    NvU32                        gpuCacheAttrib;
    NvBool                       bIsPmaAlloc           = NV_FALSE;
    NvU64                        sizeOut;
    NvU64                        offsetOut;
    NvBool                       bLockAcquired         = NV_FALSE;
    NvBool                       bBornEvicted          = NV_FALSE; // local patch
    NvBool                       bReserveDenied        = NV_FALSE; // local patch: trace
    NvU32                        attr                  = 0;
    NvU32                        attr2                 = 0;
    FB_ALLOC_INFO               *pFbAllocInfo          = NULL;
    FB_ALLOC_PAGE_FORMAT        *pFbAllocPageFormat    = NULL;
    NV_STATUS                    rmStatus              = NV_OK;
    KernelBus                   *pKernelBus            = GPU_GET_KERNEL_BUS(pGpu);
    NvBool                       bCprMemEnabled         = gpuIsCCFeatureEnabled(pGpu);

    bCprMemEnabled = bCprMemEnabled || gpuIsTdispFeatureEnabled(pGpu);

    NV_ASSERT_OR_RETURN(pRmClient != NULL, NV_ERR_INVALID_CLIENT);

    NV_ASSERT_OK_OR_RETURN(
        refFindAncestorOfType(pResourceRef, classId(Device), &pDeviceRef));

    pDevice = dynamicCast(pDeviceRef->pResource, Device);

    if (RS_IS_COPY_CTOR(pParams))
    {
        // No need to lock this GPU
        rmStatus = vidmemCopyConstruct(pVideoMemory, pCallContext, pParams);
        goto done;
    }

    if (FLD_TEST_DRF(OS32, _ATTR, _PHYSICALITY, _DEFAULT, pAllocData->attr))
    {
        pAllocData->attr =
            FLD_SET_DRF_NUM(OS32, _ATTR, _PHYSICALITY,
                            pDevice->defaultVidmemPhysicalityOverride,
                            pAllocData->attr);
    }

    NV_CHECK_OK_OR_RETURN(LEVEL_WARNING, stdmemValidateParams(pGpu, pRmClient, pAllocData));
    NV_CHECK_OR_RETURN(LEVEL_WARNING,
                       DRF_VAL(OS32, _ATTR, _LOCATION, pAllocData->attr) == NVOS32_ATTR_LOCATION_VIDMEM &&
                           !(pAllocData->flags & NVOS32_ALLOC_FLAGS_VIRTUAL),
                       NV_ERR_INVALID_ARGUMENT);

    stdmemDumpInputAllocParams(pAllocData, pCallContext);

    if (!IS_MIG_ENABLED(pGpu))
    {
        NV_CHECK_OK_OR_RETURN(LEVEL_ERROR,
            memacctTryCharge(pRmClient, pGpu->gpuId, pAllocData->size, &pMemory->pCharge));
    }

    if (pCallContext->secInfo.privLevel >= RS_PRIV_LEVEL_KERNEL)
    {
        bRsvdHeap = FLD_TEST_DRF(OS32, _ATTR, _ALLOCATE_FROM_RESERVED_HEAP, _YES, pAllocData->attr);
    }

    bSubheap = FLD_TEST_DRF(OS32, _ATTR2, _ALLOCATE_FROM_SUBHEAP, _YES, pAllocData->attr2);
    pHeap = vidmemGetHeap(pGpu, pDevice, bSubheap, bRsvdHeap);
    NV_CHECK_OR_ELSE(LEVEL_INFO,
        pHeap != NULL,
        rmStatus = NV_ERR_INVALID_STATE;
        goto done);

    attr  = pAllocData->attr;
    attr2 = pAllocData->attr2;

    if (bCprMemEnabled &&
        !FLD_TEST_DRF(OS32, _ATTR2, _MEMORY_PROTECTION, _UNPROTECTED, pAllocData->attr2))
    {
        pAllocData->flags |= NVOS32_ALLOC_FLAGS_PROTECTED;
    }
    else if (bCprMemEnabled &&
             FLD_TEST_DRF(OS32, _ATTR2, _MEMORY_PROTECTION, _UNPROTECTED, pAllocData->attr2) &&
             (pMemoryManager->unprotectedVidmemSize == 0))
    {
        // CC-TODO: Remove this once non-CPR regions are created
        NV_PRINTF(LEVEL_ERROR, "Non-CPR region not yet created\n");
        NV_ASSERT_OR_ELSE(0,
            rmStatus = NV_ERR_INVALID_ARGUMENT;
            goto done);
    }
    else if (!bCprMemEnabled &&
             FLD_TEST_DRF(OS32, _ATTR2, _MEMORY_PROTECTION, _PROTECTED, pAllocData->attr2))
    {
        NV_PRINTF(LEVEL_ERROR, "Protected memory not enabled but PROTECTED flag is set by client");
        rmStatus = NV_ERR_INVALID_ARGUMENT;
        goto done;
    }

    pAllocRequest->classNum = NV01_MEMORY_LOCAL_USER;
    pAllocRequest->pUserParams = pAllocData;
    pAllocRequest->hMemory = pResourceRef->hResource;
    pAllocRequest->hClient = hClient;
    pAllocRequest->hParent = hParent;
    pAllocRequest->pGpu = pGpu;
    pAllocRequest->internalflags = NVOS32_ALLOC_INTERNAL_FLAGS_CLIENTALLOC;
    pAllocRequest->pHwResource = &hwResource;

    // Unsure if we need to keep separate copies, but keeping old behavior for now.
    sizeOut = pAllocData->size;
    offsetOut = pAllocData->offset;

    bIsPmaAlloc = memmgrIsPmaInitialized(pMemoryManager) &&
                  !bSubheap &&
                  !bRsvdHeap &&
                  !FLD_TEST_DRF(OS32, _ATTR2, _USE_SCANOUT_CARVEOUT, _TRUE, attr2) &&
                  !(pAllocData->flags & NVOS32_ALLOC_FLAGS_WPR1) &&
                  !(pAllocData->flags & NVOS32_ALLOC_FLAGS_WPR2) &&
                  (!(pAllocData->flags & NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE) ||
                      heapIsPmaManaged(pGpu, pHeap, pAllocData->offset, pAllocData->offset+pAllocData->size-1));

    // Scrub-on-free is not supported by heap. Make sure clients don't get unscrubbed allocations
    NV_CHECK_OR_ELSE(LEVEL_WARNING,
        !memmgrIsScrubOnFreeEnabled(pMemoryManager) || bIsPmaAlloc || bSubheap || bRsvdHeap,
            rmStatus = NV_ERR_INVALID_STATE;
            goto done);

    if (!FLD_TEST_DRF(OS32, _ATTR2, _ENABLE_LOCALIZED_MEMORY, _DEFAULT, pAllocData->attr2))
    {
        if (!bIsPmaAlloc)
        {
            // heap does not support localized allocations
            rmStatus = NV_ERR_NOT_SUPPORTED;
            goto done;
        }
    }

    // Get the allocation from PMA if enabled.
    if (bIsPmaAlloc)
    {
        NvBool bReserveBound = NV_FALSE;
        NvBool bExempt = NV_FALSE;

        rmStatus = _vidmemCheckUserReserve(pGpu, vidmemGetHeap(pGpu, pDevice, NV_FALSE, NV_FALSE),
                                           pCallContext, pRmClient, pAllocData->size,
                                           &bReserveBound, &bExempt);
        bReserveDenied = (rmStatus == NV_ERR_NO_MEMORY) && bReserveBound;

        //
        // Local patch: a visible process the reserve would turn away gets
        // room made by moving hidden processes to sysmem (window hints).
        //
        if ((rmStatus == NV_ERR_NO_MEMORY) && bReserveBound &&
            vidmemEvictMakeRoom(pGpu, pRmClient, pAllocData->size))
        {
            rmStatus = _vidmemCheckUserReserve(pGpu, vidmemGetHeap(pGpu, pDevice, NV_FALSE, NV_FALSE),
                                               pCallContext, pRmClient, pAllocData->size,
                                               &bReserveBound, &bExempt);
        }

        //
        // Local patch: a background process the reserve would turn away gets
        // the allocation in sysmem instead (allocated, then evicted at once;
        // it comes back to vidmem with the process when there is room).
        //
        if ((rmStatus == NV_ERR_NO_MEMORY) && bReserveBound &&
            vidmemEvictWantBorn(pGpu, pRmClient, pAllocData))
        {
            rmStatus      = NV_OK;
            bReserveBound = NV_FALSE;
            bBornEvicted  = NV_TRUE;
        }
        //
        // Local patch: no reserve, and only the background cushion said no,
        // but the allocation cannot go to sysmem (contiguous, too big, a
        // process drawing): vidmem then, as without the cushion.
        //
        if ((rmStatus == NV_ERR_NO_MEMORY) && bReserveBound && (_vidmemUserReserveMB == 0))
        {
            NvU64 freeBytes = 0;

            pmaGetFreeMemory(vidmemGetHeap(pGpu, pDevice, NV_FALSE, NV_FALSE)->pPmaObject, &freeBytes);
            if (freeBytes >= pAllocData->size)
                rmStatus = NV_OK;
        }
        //
        // Local patch: keep real free vidmem ahead of the foreground game's
        // allocations (its GL driver puts textures in sysmem when vidmem
        // looks full, budget or not; vidmemEvictKeepFgHeadroom()).
        //
        if ((rmStatus == NV_OK) && bReserveBound && !bBornEvicted)
            vidmemEvictKeepFgHeadroom(pGpu, pRmClient, pAllocData->size);
        if (bReserveDenied && osVidmemTraceOn(VMT_RESERVE))
        {
            PMA   *pPma = vidmemGetHeap(pGpu, pDevice, NV_FALSE, NV_FALSE)->pPmaObject;
            NvU64  freeBytes = 0;
            NvU64  a[VMT_ARGS] = {0};

            pmaGetFreeMemory(pPma, &freeBytes);
            a[0] = pAllocData->size;
            a[1] = freeBytes;
            a[2] = _vidmemReserveBlockFreeBytes(pPma);
            a[3] = bBornEvicted ? VMT_RES_SYSMEM : ((rmStatus == NV_OK) ? VMT_RES_ROOM : VMT_RES_DENIED);
            a[4] = (pCallContext != NULL) && (pCallContext->secInfo.privLevel >= RS_PRIV_LEVEL_KERNEL);
            osVidmemTrace(VMT_RESERVE, (pRmClient != NULL) ? pRmClient->ProcID : osGetCurrentProcess(), a);
        }
        if (NV_OK != rmStatus)
            goto done;

        pAllocRequest->pGpu = pGpu;
        rmStatus = _vidmemPmaAllocate(vidmemGetHeap(pGpu, pDevice, NV_FALSE, NV_FALSE), pAllocRequest,
                                      bReserveBound, bExempt, bBornEvicted);
        //
        // Local patch: the compositor is never turned away. Without the
        // reserve, others move to sysmem until its allocation fits (a
        // contiguous window for a scanout buffer), then it is tried again.
        //
        if ((rmStatus == NV_ERR_NO_MEMORY) && bExempt && (_vidmemUserReserveMB == 0) &&
            vidmemEvictMakeRoomCompositor(pGpu, pRmClient, NV_MAX(pAllocData->size, _vidmemLastPmaBytes),
                                          _vidmemLastPmaContig,
                                          _vidmemLastPmaAlign))
        {
            rmStatus = _vidmemPmaAllocate(vidmemGetHeap(pGpu, pDevice, NV_FALSE, NV_FALSE), pAllocRequest,
                                          bReserveBound, bExempt, bBornEvicted);
        }
        //
        // Local patch: an ordinary process passed the free-memory check but
        // PMA found no place (free memory in 64 KB holes, a 2 MB-page
        // allocation). The foreground and visible processes get whole chunks
        // made free, as the compositor, then it is tried again
        // (vidmemEvictMakeRoomPlace(), problem #7).
        //
        if ((rmStatus == NV_ERR_NO_MEMORY) && bReserveBound && !bExempt && !bBornEvicted &&
            (_vidmemUserReserveMB == 0) &&
            vidmemEvictMakeRoomPlace(pGpu, pRmClient, NV_MAX(pAllocData->size, _vidmemLastPmaBytes),
                                     _vidmemLastPmaContig,
                                     _vidmemLastPmaAlign))
        {
            rmStatus = _vidmemPmaAllocate(vidmemGetHeap(pGpu, pDevice, NV_FALSE, NV_FALSE), pAllocRequest,
                                          bReserveBound, bExempt, bBornEvicted);
        }
        if (NV_OK != rmStatus)
            goto done;

        // Local patch: track how low the reserve block gets (vidmem_evict.c status).
        if (bExempt)
            vidmemEvictNoteBlock(vidmemGetHeap(pGpu, pDevice, NV_FALSE, NV_FALSE)->pPmaObject);
    }

    if (RMCFG_FEATURE_RM_BASIC_LOCK_MODEL)
    {
        //
        // Can't move locking up as PMA locks need to be taken first.
        // Acquire the lock *only after* PMA is done allocating.
        //
        if (!rmDeviceGpuLockIsOwner(pGpu->gpuInstance) && !rmGpuLockIsOwner())
        {
            rmStatus = rmDeviceGpuLocksAcquire(pGpu, GPUS_LOCK_FLAGS_NONE,
                                               RM_LOCK_MODULES_MEM);
            NV_ASSERT_OR_GOTO(NV_OK == rmStatus, done);

            bLockAcquired = NV_TRUE;
        }
        else
        {
            NV_ASSERT(0);
        }
    }

    {
        //
        // If using thwap to generate an allocation failure here, fail the
        // alloc right away.
        //
        KernelWatchdog *pKernelWatchdog = gpuGetKernelWatchdogFromDevice(pGpu, pDevice);
        if (pKernelWatchdog != NULL &&
            !kwdtTestAllowAlloc(pGpu, pKernelWatchdog,
                                NV_ROBUST_CHANNEL_ALLOCFAIL_HEAP))
        {
            rmStatus = NV_ERR_INSUFFICIENT_RESOURCES;
            goto done;
        }
    }

    // Don't allow FB allocations if FB is broken unless it is a virtual allocation or running in L2 cache only mode
    if (pGpu->getProperty(pGpu, PDB_PROP_GPU_BROKEN_FB) &&
        !FLD_TEST_DRF(OS32, _ATTR2, _USE_SCANOUT_CARVEOUT, _TRUE, pAllocData->attr2) &&
        !gpuIsCacheOnlyModeEnabled(pGpu))
    {
        NV_ASSERT_FAILED("Video memory requested despite BROKEN FB");
        rmStatus = NV_ERR_NOT_SUPPORTED;
        goto done;
    }

    if (gpuIsDebuggerActive_HAL(pGpu))
    {
        // Bug 643431 - WAR for GR WFI timeouts when debugger is active
        rmStatus = NV_ERR_BUSY_RETRY;
        goto done;
    }

    pFbAllocInfo = portMemAllocNonPaged(sizeof(FB_ALLOC_INFO));
    NV_ASSERT_TRUE_OR_GOTO(rmStatus, pFbAllocInfo != NULL, NV_ERR_NO_MEMORY, done);

    pFbAllocPageFormat = portMemAllocNonPaged(sizeof(FB_ALLOC_PAGE_FORMAT));
    NV_ASSERT_TRUE_OR_GOTO(rmStatus, pFbAllocPageFormat != NULL, NV_ERR_NO_MEMORY, done);

    // Call heapAlloc to get memory.
    {

        portMemSet(pFbAllocInfo, 0, sizeof(FB_ALLOC_INFO));
        portMemSet(pFbAllocPageFormat, 0, sizeof(FB_ALLOC_PAGE_FORMAT));
        pFbAllocInfo->pageFormat = pFbAllocPageFormat;

        memUtilsInitFBAllocInfo(pAllocRequest->pUserParams, pFbAllocInfo, hClient, pDeviceRef->hResource);

        rmStatus = memmgrAllocResources(pGpu, pMemoryManager, pAllocRequest, pFbAllocInfo);
        if (rmStatus != NV_OK)
            goto done;

        rmStatus = vidmemAllocResources(pGpu, pMemoryManager, pAllocRequest, pFbAllocInfo, pHeap);
        if (rmStatus != NV_OK)
            goto done;

        NV_ASSERT(pAllocRequest->pMemDesc);

        pTempMemDesc = pTopLevelMemDesc = pAllocRequest->pMemDesc;
        offsetOut = memdescGetPhysAddr(pTempMemDesc, AT_GPU, 0);
        sizeOut   = pTempMemDesc->Size;

        if (bSubheap)
            memdescSetFlag(pTempMemDesc, MEMDESC_FLAGS_OWNED_BY_CURRENT_DEVICE, NV_TRUE);
    }

    pAllocData->limit = sizeOut - 1;

    if (bIsPmaAlloc)
    {
        // Cache the PMA_ALLOC_INFO structure.
        pTopLevelMemDesc->pPmaAllocInfo = pAllocRequest->pPmaAllocInfo;
    }

    //
    // Set the unprotected flag in memdesc. Some control calls will use
    // this flag to determine if this memory lies in the protected or
    // unprotected region and use that to gather statistics like total
    // protected and unprotected memory usage by different clients, etc
    //
    if (bCprMemEnabled &&
        FLD_TEST_DRF(OS32, _ATTR2, _MEMORY_PROTECTION, _UNPROTECTED, pAllocData->attr2))
    {
        memdescSetFlag(pTopLevelMemDesc, MEMDESC_FLAGS_ALLOC_IN_UNPROTECTED_MEMORY,
                       NV_TRUE);
    }

    if (!memdescGetFlag(pTempMemDesc, MEMDESC_FLAGS_ALLOC_FROM_SCANOUT_CARVEOUT))
    {
        //
        // Video memory is always locally transparently cached.  It does not require
        // any cache management.  Marked cached unconditionally.  Non-coherent peer
        // caching is handled with an override at mapping time.
        //
        if (DRF_VAL(OS32, _ATTR2, _GPU_CACHEABLE, pAllocData->attr2) ==
            NVOS32_ATTR2_GPU_CACHEABLE_DEFAULT)
        {
            pAllocData->attr2 = FLD_SET_DRF(OS32, _ATTR2, _GPU_CACHEABLE, _YES,
                                            pAllocData->attr2);
        }
        gpuCacheAttrib = NV_MEMORY_CACHED;

        // ClientDB can set the pagesize for memdesc.
        // With GPU SMMU mapping, this needs to be set on the SMMU memdesc.
        // So SMMU allocation should happen before memConstructCommon()
        // Eventually SMMU allocation will be part of memdescAlloc().

        //
        // There are a few cases where the heap will return an existing
        // memdesc.  Only update attributes if it is new.
        //
        // @todo attr tracking should move into heapAlloc
        //
        if (pTempMemDesc->RefCount == 1)
        {
            memdescSetGpuCacheAttrib(pTopLevelMemDesc, gpuCacheAttrib);

            // An SMMU mapping will be added to FB allocations in the following cases:
            // 1. RM clients forcing SMMU mapping via flags
            //    GPU Arch verification with VPR is one such usecase.

            if (FLD_TEST_DRF(OS32, _ATTR2, _SMMU_ON_GPU, _ENABLE, pAllocData->attr2))
            {
                NV_ASSERT_FAILED("SMMU mapping allocation is not supported for ARMv7");
                rmStatus = NV_ERR_NOT_SUPPORTED;

                memdescFree(pTopLevelMemDesc);
                memdescDestroy(pTopLevelMemDesc);
                goto done;
            }
        }
    }

    rmStatus = memConstructCommon(pMemory, pAllocRequest->classNum, pAllocData->flags,
                                  pTopLevelMemDesc, pAllocData->owner, pHeap, pAllocData->attr,
                                  pAllocData->attr2, 0, pAllocData->type,
                                  pAllocData->tag, &hwResource);
    if (rmStatus != NV_OK)
    {
        memdescFree(pTopLevelMemDesc);
        memdescDestroy(pTopLevelMemDesc);

        goto done;
    }
    NV_ASSERT(pMemory->pMemDesc);
    NV_ASSERT(pMemory->pHeap);

    //
    // vGPU:
    //
    // Since vGPU does all real hardware management in the
    // host, if we are in guest OS (where IS_VIRTUAL(pGpu) is true),
    // do an RPC to the host to do the hardware update.
    //
    // XXX: This is a hack for now. No Hw resources are assumed to be used in the call.
    // The host is only requested to make an alias to the allocated heap.

    //
    // Heap alloc may allocate non-contiguous pages when it is not able to
    // find contiguous pages. Replace this field before passing to RPC.
    //
    attr = (pAllocData->attr &  DRF_SHIFTMASK(NVOS32_ATTR_PHYSICALITY)) |
           (attr             & ~DRF_SHIFTMASK(NVOS32_ATTR_PHYSICALITY));

    if (memNeedRpcAlloc(pMemory))
    {
        NV_RM_RPC_ALLOC_VIDMEM(pGpu,
                               hClient,
                               hParent,
                               pAllocRequest->hMemory,
                               pTopLevelMemDesc,
                               sizeOut,
                               attr,
                               attr2,
                               pAllocData->type,
                               pAllocData->flags,
                               pAllocData->height,
                               pAllocData->width,
                               pAllocData->format,
                               pAllocData->comprCovg,
                               pAllocData->zcullCovg,
                               pAllocData->alignment,
                               pAllocData->pitch,
                               pAllocData->ctagOffset,
                               rmStatus);

        if (rmStatus != NV_OK)
        {
            goto done;
        }

        pMemory->bRpcAlloc = NV_TRUE;
    }

    if (RMCFG_MODULE_GPUACCT)
    {
        OBJGPU *pGpu = pMemory->pGpu;
        OBJSYS *pSys = SYS_GET_INSTANCE();
        GpuAccounting *pGpuAcct = SYS_GET_GPUACCT(pSys);
        RmClient *pClient = dynamicCast(pRsClient, RmClient);

        if (pGpu->getProperty(pGpu, PDB_PROP_GPU_ACCOUNTING_ON) &&
            (pMemory->pSubDevice == NULL) &&  // Skipping for subdevice memory allocations. Was this intentional?
            (pCallContext->secInfo.privLevel < RS_PRIV_LEVEL_KERNEL))
        {
            //
            // Update the process's running FB usage incrementally by this
            // allocation's size instead of re-walking every resource of
            // every client to recompute total usage from scratch.
            //
            {
                NvU64 sessionId = 0U;
                if (gpuacctIncrementProcFbUsage(pGpuAcct, pGpu->gpuInstance,
                        pClient->ProcID, pClient->SubProcessID,
                        pMemory->Length, &sessionId) == NV_OK)
                {
                    pMemory->gpuAcctSessionId = sessionId;
            }
        }
    }
    }

    if (kbusShouldRefcountConstruct_HAL(pGpu, pKernelBus, pMemory->pMemDesc))
    {
        // No flags specified on the initial static BAR1 mapping 
        rmStatus = kbusIncreaseStaticBar1Refcount_HAL(pGpu, pKernelBus,
                       pMemory->pMemDesc, BUS_MAP_FB_FLAGS_NONE);

        if (rmStatus == NV_OK)
        {
            // nothing
        }
        else if (rmStatus == NV_ERR_NOT_SUPPORTED)
        {
            rmStatus = NV_OK;
        }
        else
        {
            goto done;
        }
    }

    pAllocData->size = sizeOut;
    pAllocData->offset = offsetOut;

    stdmemDumpOutputAllocParams(pAllocData);

    // Local patch: move it to sysmem now (if that fails it just stays in vidmem).
    if (bBornEvicted)
        (void)vidmemEvictBornAllocation(pGpu, pMemory);

done:
    if (pMemory->pCharge != NULL && rmStatus != NV_OK)
        memacctReleaseCharge(pMemory->pCharge);

    //
    // If the allocation failed after curFbUsage was already incremented,
    // roll it back so the counter stays consistent. gpuAcctSessionId is
    // non-zero only if gpuacctIncrementProcFbUsage succeeded above.
    //
    if (RMCFG_MODULE_GPUACCT && rmStatus != NV_OK &&
        pMemory->gpuAcctSessionId != 0U)
    {
        OBJSYS        *pSys     = SYS_GET_INSTANCE();
        GpuAccounting *pGpuAcct = SYS_GET_GPUACCT(pSys);

        gpuacctDecrementProcFbUsage(pGpuAcct, pGpu->gpuInstance,
            pRmClient->ProcID, pRmClient->SubProcessID,
            pMemory->Length, pMemory->gpuAcctSessionId);
        pMemory->gpuAcctSessionId = 0U;
    }

    if (bSubheap && pTempMemDesc != NULL && rmStatus != NV_OK)
        heapRemoveRef(pHeap);

    portMemFree(pFbAllocPageFormat);
    portMemFree(pFbAllocInfo);

    if (bLockAcquired)
    {
        // UNLOCK: release GPUs lock
        rmDeviceGpuLocksRelease(pGpu, GPUS_LOCK_FLAGS_NONE, NULL);
    }

    if (bIsPmaAlloc && NV_OK != rmStatus)
    {
        if (pAllocRequest->pPmaAllocInfo)
            vidmemPmaFree(pGpu, vidmemGetHeap(pGpu, pDevice, NV_FALSE, NV_FALSE),
                          pAllocRequest->pPmaAllocInfo, 0);
    }

    return rmStatus;
}

void
vidmemDestruct_IMPL
(
    VideoMemory        *pVideoMemory
)
{
    Memory             *pMemory        = staticCast(pVideoMemory, Memory);
    OBJGPU             *pGpu           = pMemory->pGpu;
    MEMORY_DESCRIPTOR  *pMemDesc       = pMemory->pMemDesc;
    MEMDESC_CUSTOM_HEAP customHeap;
    KernelBus          *pKernelBus      = GPU_GET_KERNEL_BUS(pGpu);

    // Free any association of the memory with existing third-party p2p object
    CliUnregisterMemoryFromThirdPartyP2P(pMemory);

    if (RMCFG_MODULE_GPUACCT && pMemory->gpuAcctSessionId != 0U)
    {
        OBJSYS *pSys = SYS_GET_INSTANCE();
        GpuAccounting *pGpuAcct = SYS_GET_GPUACCT(pSys);
        RmClient *pClient = dynamicCast(RES_GET_CLIENT(pVideoMemory), RmClient);

        if (pMemDesc->DupCount > 1U)
        {
            //
            // Surviving dups share the same physical pages; transfer accounting
            // responsibility to the next dup (which memDestructCommon will
            // promote to isMemDescOwner) so curFbUsage only decrements when
            // the last counted object is freed.
            //
            Memory   *pNextDup    = pMemory->dupListItem.pNext;
            RmClient *pNextClient = dynamicCast(RES_GET_CLIENT(
                                        dynamicCast(pNextDup, VideoMemory)), RmClient);

            if (pNextClient != NULL &&
                (pNextClient->ProcID != pClient->ProcID ||
                 pNextClient->SubProcessID != pClient->SubProcessID))
            {
                //
                // Cross-process dup: decrement from the current process (it is
                // being freed) using the existing session ID, then try to
                // increment for the new owner.  Only stamp the dup with the
                // new session ID if the increment succeeded; otherwise leave
                // gpuAcctSessionId == 0 so the next seed walk picks it up.
                //
                NvU64 newSessionId = 0U;
                gpuacctDecrementProcFbUsage(pGpuAcct, pGpu->gpuInstance,
                    pClient->ProcID, pClient->SubProcessID,
                    pMemory->Length, pMemory->gpuAcctSessionId);
                if (gpuacctIncrementProcFbUsage(pGpuAcct, pGpu->gpuInstance,
                        pNextClient->ProcID, pNextClient->SubProcessID,
                        pMemory->Length, &newSessionId) == NV_OK)
                {
                    pNextDup->gpuAcctSessionId = newSessionId;
                }
            }
            else
            {
                // Same-process dup: no net change to curFbUsage; pass session ID.
                pNextDup->gpuAcctSessionId = pMemory->gpuAcctSessionId;
            }
        }
        else
        {
            // Last holder of these physical pages: decrement using the session ID.
            gpuacctDecrementProcFbUsage(pGpuAcct, pGpu->gpuInstance,
                pClient->ProcID, pClient->SubProcessID,
                pMemory->Length, pMemory->gpuAcctSessionId);
        }
    }

    memacctReleaseCharge(pMemory->pCharge);
    memDestructCommon(pMemory);

    if (kbusShouldRefcountConstruct_HAL(pGpu, pKernelBus, pMemDesc))
    {
        //
        // static BAR1: memory must be released from the static BAR1 mapping
        // to restore the static BAR1 mapping to a default state since it can
        // immediately be allocated by UVM after PMA free
        //
        (void)kbusDecreaseStaticBar1Refcount_HAL(pGpu,
                    pKernelBus, pMemDesc,
                    NULL);
    }

    // free the video memory based on how it was alloced ... a non-zero
    // heapOwner indicates it was heapAlloc-ed.
    customHeap = memdescGetCustomHeap(pMemDesc);
    if (customHeap == MEMDESC_CUSTOM_HEAP_NONE)
    {
        MemoryManager      *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
        NvHandle            hClient        = RES_GET_CLIENT_HANDLE(pVideoMemory);
        NvHandle            hParent        = RES_GET_PARENT_HANDLE(pVideoMemory);
        NvU32               heapOwner      = pMemory->HeapOwner;
        NV_STATUS           status;

        NV_PRINTF(LEVEL_INFO, "Function: FREE\n");
        NV_PRINTF(LEVEL_INFO, "   Owner: 0x%x\n", heapOwner);
        NV_PRINTF(LEVEL_INFO, " hMemory: 0x%x\n", RES_GET_HANDLE(pVideoMemory));

        //
        // memHandle (and the block's size/type) is returned, but not
        // needed ... the caller already has the correct handle to pass
        // to memDestructCommon
        //
        status = memmgrFree(pGpu,
                            pMemoryManager,
                            pMemDesc->pHeap,
                            hClient,
                            hParent, // device or subdevice
                            0,
                            heapOwner,
                            pMemDesc);
        NV_ASSERT(status == NV_OK);
    }
    else if (customHeap == MEMDESC_CUSTOM_HEAP_SCANOUT_CARVEOUT)
    {
        MemoryManager      *pMemoryManager = GPU_GET_MEMORY_MANAGER(pGpu);
        memmgrFreeFromScanoutCarveoutRegion(pGpu, pMemoryManager, pMemDesc);
    }
}

NV_STATUS
vidmemAllocResources
(
    OBJGPU                      *pGpu,
    MemoryManager               *pMemoryManager,
    MEMORY_ALLOCATION_REQUEST   *pAllocRequest,
    FB_ALLOC_INFO               *pFbAllocInfo,
    Heap                        *pHeap
)
{
    NV_STATUS                    status               = NV_OK;
    KernelMemorySystem          *pKernelMemorySystem  = GPU_GET_KERNEL_MEMORY_SYSTEM(pGpu);
    NV_MEMORY_ALLOCATION_PARAMS *pVidHeapAlloc        = pAllocRequest->pUserParams;
    NvU64                        requestedSize        = pVidHeapAlloc->size;
    HWRESOURCE_INFO             *pHwResource          = NULL;
    MEMORY_DESCRIPTOR           *pMemDesc             = NULL;
    NvBool                       bAllocedMemDesc      = NV_FALSE;
    NvBool                       bAllocedMemory       = NV_FALSE;
    NvBool                       bNoncontigAllowed    = NV_FALSE;
    NvBool                       bNoncontigAllocation = NV_FALSE;
    NvHandle                     hVASpace             = pVidHeapAlloc->hVASpace;
    NvBool                       bIsPmaOwned          = NV_FALSE;
    NvBool                       bContig              = NV_TRUE;

    const MEMORY_SYSTEM_STATIC_CONFIG *pMemorySystemConfig =
        kmemsysGetStaticConfig(pGpu, pKernelMemorySystem);

    // Most vidmem allocations external to RM get routed to PMA.
    bIsPmaOwned = (pAllocRequest->pPmaAllocInfo != NULL);

    // check if pMemorySystemConfig is not yet initialized on Offload client.
    if (pMemorySystemConfig == NULL)
    {
        status = NV_ERR_INVALID_STATE;
        NV_ASSERT(0);
        goto failed;
    }

    if (pVidHeapAlloc->flags & NVOS32_ALLOC_FLAGS_VIRTUAL_ONLY)
    {
        NV_PRINTF(LEVEL_WARNING,
                  "Virtual-only flag used with physical allocation\n");
        status = NV_ERR_INVALID_ARGUMENT;
        goto failed;
    }
    //
    // In NUMA systems, the memory allocation comes from kernel
    // and kernel doesn't support fixed address allocation.
    //
    if ((pVidHeapAlloc->flags & NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE) &&
        bIsPmaOwned &&
        osNumaOnliningEnabled(pGpu->pOsGpuInfo))
    {
        NV_PRINTF(LEVEL_WARNING,
                  "NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE for PMA cannot be "
                  "accommodated for NUMA systems\n");
        status = NV_ERR_INVALID_ARGUMENT;
        goto failed;
    }
    if (FLD_TEST_DRF(OS32, _ATTR2, _32BIT_POINTER, _ENABLE, pVidHeapAlloc->attr2))
    {
        NV_PRINTF(LEVEL_WARNING,
                  "Virtual-only 32-bit pointer attr used with physical allocation\n");
        status = NV_ERR_INVALID_ARGUMENT;
        goto failed;
    }
    if (hVASpace != 0)
    {
        NV_PRINTF(LEVEL_WARNING,
                  "VA space handle used with physical allocation\n");
        status = NV_ERR_INVALID_ARGUMENT;
        goto failed;
    }

    if (pGpu->getProperty(pGpu, PDB_PROP_GPU_IS_SOC_SDM))
    {
        if (FLD_TEST_DRF(OS32, _ATTR2, _USE_SCANOUT_CARVEOUT, _TRUE, pVidHeapAlloc->attr2))
        {
            NvU32 heapFlag  = NVOS32_ALLOC_FLAGS_FORCE_MEM_GROWS_DOWN;

            status = memmgrAllocFromScanoutCarveoutRegion(pGpu,
                    pMemoryManager,
                    pFbAllocInfo->hClient,
                    pVidHeapAlloc,
                    &heapFlag,
                    &pAllocRequest->pMemDesc);

            if (status == NV_OK)
            {
                pMemDesc = pAllocRequest->pMemDesc;
                // Force the format to whatever is required by client.
                memdescSetPteKind(pMemDesc, pFbAllocInfo->format);
                NV_PRINTF(LEVEL_INFO, "Allocated surface in scanout region\n");
            }
            else
            {
                NV_PRINTF(LEVEL_WARNING,
                        "Failed to allocate surface in scanout region\n");
                goto failed;
            }

            goto pre_alloc_done;
        }
    }

    // Prior to this change, heap was silently ignoring non-contig Vidmem allocation requests.
    // With this change to allow non-contig vidmem allocation, I was getting a DVS Extended Sanity failures & regression on Windows.
    // It seems Windows is making some allocations with non-contig flag, but was expecting contig allocation.
    // So enable the non-contig path only for verif platforms.
    //
    bContig = FLD_TEST_DRF(OS32, _ATTR, _PHYSICALITY, _DEFAULT, pVidHeapAlloc->attr) ?
        !pHeap->getProperty(pHeap, PDB_PROP_HEAP_NONCONTIG_ALLOC_BY_DEFAULT) :
        !FLD_TEST_DRF(OS32, _ATTR, _PHYSICALITY, _NONCONTIGUOUS, pVidHeapAlloc->attr);

    NV_CHECK_OK_OR_GOTO(status, LEVEL_WARNING, memUtilsAllocMemDesc(pGpu, pAllocRequest, pFbAllocInfo, &pMemDesc, pHeap,
                                                                  ADDR_FBMEM, bContig, &bAllocedMemDesc), failed);

#ifndef NV_DISABLE_NONCONTIG_ALLOC
    bNoncontigAllowed =
        (!bContig || FLD_TEST_DRF(OS32, _ATTR, _PHYSICALITY, _ALLOW_NONCONTIGUOUS, pVidHeapAlloc->attr)) &&
        pMemoryManager->bAllowNoncontiguousAllocation &&
        !FLD_TEST_DRF(OS32, _ATTR, _FORMAT, _SWIZZLED, pVidHeapAlloc->attr);
#endif

    if (bIsPmaOwned)
    {
        pFbAllocInfo->offset = pMemDesc->_pteArray[0];

        if (SYS_GET_INSTANCE()->bEnableDynamicGranularityPageArrays == NV_TRUE && !memdescGetContiguity(pMemDesc, AT_CPU))
        {
            //
            // set pagearray granularity if dynamic memdesc pagesize is enabled
            // this ensures consistency in calculation of page count
            //
            pMemDesc->pageArrayGranularity = pAllocRequest->pPmaAllocInfo->pageSize;
        }

        if (bContig)
        {
                NV_PRINTF(LEVEL_INFO, "---> PMA Path taken contiguous\n");
                pVidHeapAlloc->attr = FLD_SET_DRF(OS32, _ATTR, _PHYSICALITY,
                                                  _CONTIGUOUS,
                                                  pVidHeapAlloc->attr);

                memdescDescribe(pAllocRequest->pMemDesc, ADDR_FBMEM,
                                pAllocRequest->pPmaAllocInfo->pageArray[0],
                                pFbAllocInfo->adjustedSize);
        }
        else
        {
            NV_PRINTF(LEVEL_INFO, "---> PMA Path taken discontiguous\n");
            NV_ASSERT(!bContig && bNoncontigAllowed);
            pVidHeapAlloc->attr = FLD_SET_DRF(OS32, _ATTR, _PHYSICALITY,
                                          _NONCONTIGUOUS,
                                          pVidHeapAlloc->attr);
            memdescFillPages(pAllocRequest->pMemDesc, 0,
                         pAllocRequest->pPmaAllocInfo->pageArray,
                         pAllocRequest->pPmaAllocInfo->pageCount,
                         pAllocRequest->pPmaAllocInfo->pageSize);
        }
        if (!FLD_TEST_DRF(OS32, _ATTR2, _ENABLE_LOCALIZED_MEMORY, _DEFAULT, pVidHeapAlloc->attr2))
        {
            if (pMemoryManager->bLocalizedMemorySupported &&
                !IS_MIG_ENABLED(pGpu))
            {
                memdescSetFlag(pAllocRequest->pMemDesc, MEMDESC_FLAGS_ALLOC_AS_LOCALIZED, NV_TRUE);
                pAllocRequest->pMemDesc->localizedMask = pMemoryManager->localizedMask;
            }
            else
            {
                NV_PRINTF(LEVEL_ERROR, "Localized memory requested when localized memory not enabled\n");
                status = NV_ERR_INVALID_ARGUMENT;
                goto failed;
            }
        }
    }
    else
    {
        OBJHEAP_ALLOC_DATA allocData = { 0 };

        bNoncontigAllocation = !bContig;

        allocData.alignment = pVidHeapAlloc->alignment;
        allocData.allocSize = pFbAllocInfo->size + pFbAllocInfo->pad;

        status = heapAlloc(pGpu,
                           pFbAllocInfo->hClient,
                           pHeap,
                           pAllocRequest,
                           pAllocRequest->hMemory,
                           &allocData,
                           pFbAllocInfo,
                           &pHwResource,
                           &bNoncontigAllocation,
                           bNoncontigAllowed,
                           bAllocedMemDesc);

        // heapAlloc might create a new memdesc for compbit/discontig case
        pMemDesc = pAllocRequest->pMemDesc;

        if (status != NV_OK)
        {
            goto failed;
        }

        bAllocedMemory = NV_TRUE;

        if (pVidHeapAlloc->flags & NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM)
        {
            memdescSetFlag(pMemDesc, MEMDESC_FLAGS_PRESERVE_CONTENT_ON_SUSPEND, NV_TRUE);
        }
    }

    if (!bIsPmaOwned && (pVidHeapAlloc->type != NVOS32_TYPE_PMA))
    {
        NvU64 size, numBlocks;
        NvU32 i;

        //
        // Pre-fill cache to prevent FB read accesses if in cache only mode and not doing one time pre-fill
        // Also, only need to fill the *requested* size and not the actual allocation size
        // These might not work with noncontig allocation since they assume
        // physically contig memory
        //
        if (!bNoncontigAllocation &&
            gpuIsCacheOnlyModeEnabled(pGpu) &&
            !pMemorySystemConfig->bL2PreFill)
        {
            NV_STATUS preFillStatus;

            requestedSize = (requestedSize == 0) ? pVidHeapAlloc->size : requestedSize;
            preFillStatus = kmemsysPreFillCacheOnlyMemory_HAL(pGpu, pKernelMemorySystem,
                    pFbAllocInfo->offset, requestedSize);
            NV_ASSERT(preFillStatus == NV_OK);
        }

        if (memdescGetContiguity(pMemDesc, AT_GPU))
        {
            size = pFbAllocInfo->adjustedSize;
            numBlocks = 1; // One contiguous page
        }
        else
        {
            // Only 4k-sized noncontig pages supported currently
            size = RM_PAGE_SIZE;
            numBlocks = pMemDesc->PageCount;
        }

        for (i = 0; i < numBlocks; i++)
        {
            // Ensures memory is fully initialized
            memmgrScrubMemory_HAL(pGpu, pMemoryManager, memdescGetPte(pMemDesc, AT_GPU, i), size);
        }
    }

    // get possibly updated surface attributes
    pVidHeapAlloc->attr = pFbAllocInfo->retAttr;
    pVidHeapAlloc->attr2 = pFbAllocInfo->retAttr2;

    // update contiguity attribute to reflect memdesc
    if (memdescGetContiguity(pAllocRequest->pMemDesc, AT_GPU))
    {
        pVidHeapAlloc->attr = FLD_SET_DRF(OS32, _ATTR, _PHYSICALITY,
                                          _CONTIGUOUS,
                                          pVidHeapAlloc->attr);
    }
    else
    {
        pVidHeapAlloc->attr = FLD_SET_DRF(OS32, _ATTR, _PHYSICALITY,
                                          _NONCONTIGUOUS,
                                          pVidHeapAlloc->attr);
    }

    // TODO remove once we don't have hwResource in MEM_BLOCK
    if (pHwResource != NULL)
    {
        pHwResource->attr       = pFbAllocInfo->retAttr;
        pHwResource->attr2      = pFbAllocInfo->retAttr2;
        pHwResource->hwResId    = pFbAllocInfo->hwResId;
        pHwResource->comprCovg  = pFbAllocInfo->comprCovg;
        pHwResource->ctagOffset = pFbAllocInfo->ctagOffset;
    }

pre_alloc_done:

    pVidHeapAlloc->offset = pFbAllocInfo->offset;

    if (pAllocRequest->pHwResource != NULL)
    {
        pAllocRequest->pHwResource->attr       = pFbAllocInfo->retAttr;
        pAllocRequest->pHwResource->attr2      = pFbAllocInfo->retAttr2;
        pAllocRequest->pHwResource->hwResId    = pFbAllocInfo->hwResId;
        pAllocRequest->pHwResource->comprCovg  = pFbAllocInfo->comprCovg;
        pAllocRequest->pHwResource->ctagOffset = pFbAllocInfo->ctagOffset;
        pAllocRequest->pHwResource->hwResId    = pFbAllocInfo->hwResId;
    }

    return NV_OK;

failed:
    if (bAllocedMemory)
    {
        memmgrFree(pGpu, pMemoryManager, pHeap,
                   pFbAllocInfo->hClient, pFbAllocInfo->hDevice, 0,
                   pVidHeapAlloc->owner,
                   pMemDesc);
    }
    else
    {
        memmgrFreeHwResources(pGpu, pMemoryManager, pFbAllocInfo);
    }

    if (bAllocedMemDesc)
    {
        memdescDestroy(pAllocRequest->pMemDesc);
        pAllocRequest->pMemDesc = NULL;
    }

    return status;
}

NV_STATUS
vidmemCheckCopyPermissions_IMPL
(
    VideoMemory        *pVideoMemory,
    OBJGPU             *pDstGpu,
    Device             *pDstDevice
)
{
    Memory           *pMemory               = staticCast(pVideoMemory, Memory);
    OBJGPU           *pSrcGpu               = pMemory->pGpu;
    RsClient         *pSrcClient            = RES_GET_CLIENT(pVideoMemory);
    RsClient         *pDstClient            = RES_GET_CLIENT(pDstDevice);
    KernelMIGManager *pSrcKernelMIGManager  = GPU_GET_KERNEL_MIG_MANAGER(pSrcGpu);
    KernelMIGManager *pDstKernelMIGManager  = GPU_GET_KERNEL_MIG_MANAGER(pDstGpu);
    NvBool            bSrcClientKernel      =
        (rmclientGetCachedPrivilege(dynamicCast(pSrcClient, RmClient)) >=
         RS_PRIV_LEVEL_KERNEL);
    NvBool            bDstClientKernel      =
        (rmclientGetCachedPrivilege(dynamicCast(pDstClient, RmClient)) >=
         RS_PRIV_LEVEL_KERNEL);

    //
    // XXX: In case of MIG memory, duping across GPU instances is not allowed
    // Bug 2815350 - Due to this bug, allow kernel clients to bypass this check
    //
    if (!bDstClientKernel && (IS_MIG_IN_USE(pSrcGpu) || IS_MIG_IN_USE(pDstGpu)))
    {
        //
        // Due to Bug 2815350 we have to take an exception for kernel clients,
        // hence we can't use a direct instanceRef check.
        // Rather than defaulting to heap based checks, keeping the
        // instanceRef check in else as that's what we should only have
        // when Bug 2815350 is fixed.
        // Both clients are kernel - Force subscription check. No exception needed.
        // Only SrcClientKernel - Enforce subscription check in dstClient
        //      DstClientKernel - Ignore any enforcement as kernel clients are
        //                        allowed to dup without any enforcement
        //

        if (bSrcClientKernel)
        {
            // Get memory partition heap from both clients and compare
            Heap *pDstClientHeap = NULL;
            NV_CHECK_OK_OR_RETURN(LEVEL_WARNING,
                                  kmigmgrGetMemoryPartitionHeapFromDevice(pDstGpu, pDstKernelMIGManager,
                                                                          pDstDevice, &pDstClientHeap));

            // Make sure memory is coming from same heaps
            if (pDstClientHeap != pMemory->pHeap)
            {
                 NV_PRINTF(LEVEL_WARNING,
                          "Duping outside of GPU instance not allowed with MIG\n");
                return NV_ERR_NOT_SUPPORTED;
            }
        }
        else
        {
            //
            // Due to Bug 2815350 we have to take an exception for kernel clients,
            // hence we can't use a direct instanceRef check
            //
            MIG_INSTANCE_REF srcInstRef;
            MIG_INSTANCE_REF dstInstRef;
            RsResourceRef *pSrcDeviceRef;
            Device *pSrcDevice;

            NV_ASSERT_OK_OR_RETURN(
                refFindAncestorOfType(RES_GET_REF(pMemory), classId(Device), &pSrcDeviceRef));

            pSrcDevice = dynamicCast(pSrcDeviceRef->pResource, Device);

            // Check instance subscription of source and destination clients
            NV_CHECK_OK_OR_RETURN(LEVEL_WARNING,
                                  kmigmgrGetInstanceRefFromDevice(pSrcGpu, pSrcKernelMIGManager,
                                                                  pSrcDevice, &srcInstRef));
            NV_CHECK_OK_OR_RETURN(LEVEL_WARNING,
                                  kmigmgrGetInstanceRefFromDevice(pDstGpu, pDstKernelMIGManager,
                                                                  pDstDevice, &dstInstRef));

            //
            // Memory duping is allowed accross compute instances. so ignore
            // compute instance differences
            //
            srcInstRef = kmigmgrMakeGIReference(srcInstRef.pKernelMIGGpuInstance);
            dstInstRef = kmigmgrMakeGIReference(dstInstRef.pKernelMIGGpuInstance);
            if (!kmigmgrAreMIGReferencesSame(&srcInstRef, &dstInstRef))
            {
                NV_PRINTF(LEVEL_WARNING,
                          "GPU instance subscription differ between Source and Destination clients\n");
                return NV_ERR_NOT_SUPPORTED;
            }
        }
    }

    return NV_OK;
}
