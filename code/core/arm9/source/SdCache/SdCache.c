#include "common.h"
#include <string.h>
#include "Fat/ff.h"
#include "Fat/diskio.h"
#include "Fat/FsIpc.h"
#include "VirtualMachine/VMNestedIrq.h"
#include "cp15.h"
#include "Cpsr.h"
#include "SdCache.h"
#include "Patches/ExternalPatch.h"
#include "JitPatcher/JitCommon.h"

typedef struct
{
    vu16 cacheBlock;
    vu32 romBlock;
} SdcFetch;

static SdcFetch sCurrentFetch;

[[gnu::section(".vramhi.bss")]]
void* sdc_romBlockToCacheBlock[SDC_ROM_BLOCK_COUNT];

/// @brief Random generator state for random cache replacement.
static u32 sRandomState;

/// @brief Maps sd cache blocks to rom blocks.
// In EWRAM BSS: the default .bss (vrama) is full and has no headroom.
[[gnu::section(".ewram.bss")]]
static u16 sCacheBlockToRomBlock[SDC_BLOCK_COUNT];

/// @brief Second-chance (clock) use bit per cache block. A block whose bit is
///        set was recently loaded or hit and is skipped once during
///        replacement (the bit is cleared). This keeps hot blocks resident
///        while letting streamed data (e.g. large patched fonts) age out
///        naturally instead of occupying the whole cache forever.
// In EWRAM BSS: the default .bss (vrama) has no headroom left.
[[gnu::section(".ewram.bss")]]
static u8 sCacheBlockUsed[SDC_BLOCK_COUNT];

/// @brief The number of usable blocks in the cache. This can be less than the
///        total number of cache blocks when some blocks are permanently loaded.
static u32 sBlockCount;

static u32 sTabuLevel;
static u32 sTabuBlocks[2];
vu32 gSdCacheIrqForbiddenRomBlockReplacementRange;

// In EWRAM BSS: the default .bss (vrama) is full and has no headroom.
[[gnu::section(".ewram.bss"), gnu::aligned(32)]]
static DWORD sClusterTable[512];

// temporarily
extern FIL gFile;

/// @brief Returns a cache block to replace.
/// @return The index of the cache block to replace.
static u32 getBlockToReplace(void)
{
    u32 block = 0;
    // Second-chance: skip recently used blocks (and clear their bit) so they
    // survive at least one full replacement cycle.
    for (u32 attempt = 0; attempt < 8; attempt++)
    {
        sRandomState = sRandomState * 1566083941u + 2531011u;
        u32 maxPlusOne = sBlockCount;
        if (sTabuBlocks[0] != SDC_BLOCK_INVALID)
        {
            maxPlusOne--;
        }
        if (sTabuLevel > 0 && sTabuBlocks[1] != SDC_BLOCK_INVALID)
        {
            maxPlusOne--;
        }
        block = ((sRandomState >> 16) * maxPlusOne) >> 16;
        if (block == sTabuBlocks[0])
        {
            block = maxPlusOne;
        }
        else if (sTabuLevel > 0 && block == sTabuBlocks[1])
        {
            block = maxPlusOne + 1;
        }
        if (!sCacheBlockUsed[block])
        {
            break;
        }
        sCacheBlockUsed[block] = 0;
    }
    return block;
}

static bool isCurrentlyFetching(void)
{
    return sCurrentFetch.cacheBlock != SDC_BLOCK_INVALID;
}

static void finishFetch()
{
    sCacheBlockToRomBlock[sCurrentFetch.cacheBlock] = sCurrentFetch.romBlock;
    sdc_romBlockToCacheBlock[sCurrentFetch.romBlock] = &sdc_cache[sCurrentFetch.cacheBlock][0];
    sCurrentFetch.romBlock = SDC_ROM_BLOCK_INVALID;
    sCurrentFetch.cacheBlock = SDC_BLOCK_INVALID;
    dc_drainWriteBuffer();
}

static u32 getSdSectorOfRomBlock(u32 romBlock)
{
    u32 romOffset = romBlock * SDC_BLOCK_SIZE;
    if (romOffset >= f_size(&gFile))
    {
        return 0;
    }

    FATFS* fs = gFile.obj.fs;
    u32* tbl = gFile.cltbl + 1;
    u32 csect = (UINT)(romOffset / 512 & (fs->csize - 1));
    u32 cshift = __builtin_ctz(fs->csize) + 9;
    u32 cl = (DWORD)(romOffset >> cshift);
    while (true)
    {
        u32 ncl = *tbl++;
        if (cl < ncl)
        {
            break;
        }
        cl -= ncl;
        tbl++;
    }
    u32 cluster = cl + *tbl - 2;
    u32 sector = fs->database + fs->csize * cluster;
    sector += csect;
    return sector;
}

static void fillOutOfBoundsCacheBlock(u32 romBlock, u32 cacheBlock)
{
    u32 romSize = f_size(&gFile);
    u32 powerOf2RomSize = romSize < 0x100000 ? 0x100000 : (1 << (32 - __builtin_clz(romSize - 1)));
    if (romBlock * SDC_BLOCK_SIZE < powerOf2RomSize)
    {
        memset(&sdc_cache[cacheBlock][0], 0xFF, SDC_BLOCK_SIZE);
    }
    else
    {
        for (u32 i = 0; i < SDC_BLOCK_SIZE; i += 4)
        {
            u32 address = romBlock * SDC_BLOCK_SIZE + i;
            u32 oobValue = (address >> 1) & 0xFFFF;
            *(u32 *)&sdc_cache[cacheBlock][i] = oobValue | ((oobValue + 1) << 16);
        }
    }

    dc_drainWriteBuffer();
}

/// @brief Loads a rom block to the given buffer.
/// @param romBlock Rom block index to load.
/// @param dst The destination buffer.
static void* loadRomBlock(u32 romBlock, u32 cacheBlock)
{
    u32 sector = getSdSectorOfRomBlock(romBlock);
    // Patched blocks are served from the pre-patched .pre sidecar: one raw
    // sector read, no FatFS, no on-demand patch application in the hot path.
    u32 bakedSector = externalPatch_getBakedBlockSector(romBlock);
    if (bakedSector != 0)
    {
        sector = bakedSector;
    }

    u32 irqs = fs_waitForCompletionOfCurrentTransaction(true);
    if (isCurrentlyFetching())
    {
        finishFetch();
    }

    void* currentCacheBlock = sdc_romBlockToCacheBlock[romBlock];
    if (currentCacheBlock)
    {
        sCacheBlockUsed[((u32)currentCacheBlock - (u32)sdc_cache) / SDC_BLOCK_SIZE] = 1;
        arm_restoreIrqs(irqs);
        return currentCacheBlock;
    }

    if (cacheBlock == SDC_BLOCK_INVALID)
    {
        cacheBlock = getBlockToReplace();
        if ((arm_getCpsr() & 0x1F) == 0x12)
        {
            u32 forbiddenReplacementRange = gSdCacheIrqForbiddenRomBlockReplacementRange;
            if (forbiddenReplacementRange != 0)
            {
                u32 forbiddenReplacementRangeStart = forbiddenReplacementRange & 0xFFFF;
                u32 forbiddenReplacementRangeEnd = forbiddenReplacementRange >> 16;
                while (true)
                {
                    u32 oldRomBlock = sCacheBlockToRomBlock[cacheBlock];
                    if (oldRomBlock == SDC_ROM_BLOCK_INVALID ||
                        !(forbiddenReplacementRangeStart <= oldRomBlock && oldRomBlock < forbiddenReplacementRangeEnd))
                    {
                        break;
                    }
                    cacheBlock = getBlockToReplace();
                }
            }
        }
    }
    u32 oldRomBlock = sCacheBlockToRomBlock[cacheBlock];
    if (oldRomBlock != SDC_ROM_BLOCK_INVALID)
    {
        sdc_romBlockToCacheBlock[oldRomBlock] = NULL;
        sCacheBlockToRomBlock[cacheBlock] = SDC_ROM_BLOCK_INVALID;
        sCacheBlockUsed[cacheBlock] = 0;
    }

    FsWaitToken waitToken;
    if (sector != 0)
    {
        fs_readCacheAlignedSectorsAsync(
            gFile.obj.fs->pdrv == DEV_FAT ? FS_DEVICE_DLDI : FS_DEVICE_DSI_SD,
            &sdc_cache[cacheBlock][0], sector,
            SDC_BLOCK_SIZE / 512, &waitToken);
        sCurrentFetch.romBlock = romBlock;
        sCurrentFetch.cacheBlock = cacheBlock;
    }

    bool decreaseTabuLevel = false;
    if ((arm_getCpsr() & 0x1F) != 0x12 && sTabuLevel < 2)
    {
        sTabuBlocks[sTabuLevel++] = cacheBlock;
        decreaseTabuLevel = true;
    }

    arm_restoreIrqs(irqs);
    if (sector != 0)
    {
        irqs = fs_waitForCompletion(&waitToken, true);
    }
    else
    {
        fillOutOfBoundsCacheBlock(romBlock, cacheBlock);
        irqs = arm_disableIrqs();
    }

    if (bakedSector == 0)
    {
        // No pre-patched cache: fall back to on-demand patching. IRQs stay
        // disabled to avoid re-entering FatFs from interrupt context.
        externalPatch_applyRomBlock(romBlock, &sdc_cache[cacheBlock][0]);
    }
    sCacheBlockUsed[cacheBlock] = 1;

    // The block content has changed (rom data and/or external patches).
    // Clear stale JIT bits so the new content is processed on first execution.
    jit_clearBlockJitBits(&sdc_cache[cacheBlock][0]);

    if (sector != 0 && sCurrentFetch.romBlock == romBlock)
    {
        finishFetch();
    }
    else if (sector == 0)
    {
        // Publish only after patching so no one observes half-patched content.
        sCacheBlockToRomBlock[cacheBlock] = romBlock;
        sdc_romBlockToCacheBlock[romBlock] = &sdc_cache[cacheBlock][0];
        dc_drainWriteBuffer();
    }

    arm_restoreIrqs(irqs);

    if (decreaseTabuLevel)
    {
        sTabuLevel--;
    }

    return &sdc_cache[cacheBlock][0];
}

extern void logAddress(u32 address);

const void* sdc_loadRomBlockDirect(u32 romAddress)
{
    vm_enableNestedIrqs();
    // logAddress(romAddress);
    u32 romBlock = ((romAddress << 7) >> 7) / SDC_BLOCK_SIZE;
    void* cacheBlock = loadRomBlock(romBlock, SDC_BLOCK_INVALID);
    vm_disableNestedIrqs();
    return cacheBlock;
}

void* sdc_loadRomBlockForPatching(u32 romAddress)
{
    u32 romBlock = ((romAddress << 7) >> 7) / SDC_BLOCK_SIZE;
    void* data = sdc_romBlockToCacheBlock[romBlock];
    // if not loaded at all yet, or not permanent
    if (!data || (u32)data < (u32)&sdc_cache[sBlockCount][0])
    {
        if (data)
        {
            // if already loaded, but not permanent, invalidate block
            sdc_romBlockToCacheBlock[romBlock] = NULL;
            sCacheBlockToRomBlock[((u32)data - (u32)&sdc_cache[0][0]) / SDC_BLOCK_SIZE] = SDC_ROM_BLOCK_INVALID;
        }

        data = loadRomBlock(romBlock, --sBlockCount);
    }
    return (void*)((u32)data + (romAddress & SDC_BLOCK_MASK));
}

void sdc_init(void)
{
    sRandomState = 0xA512ED48; // initial random seed
    sBlockCount = SDC_BLOCK_COUNT;
    for (u32 i = 0; i < SDC_ROM_BLOCK_COUNT; i++)
    {
        sdc_romBlockToCacheBlock[i] = NULL;
    }
    for (u32 i = 0; i < SDC_BLOCK_COUNT; i++)
    {
        sCacheBlockToRomBlock[i] = SDC_ROM_BLOCK_INVALID;
        sCacheBlockUsed[i] = 0;
    }

    sCurrentFetch.cacheBlock = SDC_BLOCK_INVALID;
    sCurrentFetch.romBlock = SDC_ROM_BLOCK_INVALID;
    gSdCacheIrqForbiddenRomBlockReplacementRange = 0;
    sTabuLevel = 0;
    sTabuBlocks[0] = SDC_BLOCK_INVALID;
    sTabuBlocks[1] = SDC_BLOCK_INVALID;
    gIrqYieldingEnabled = true;

    sClusterTable[0] = sizeof(sClusterTable) / sizeof(DWORD);
    gFile.cltbl = sClusterTable;
    u32 result = f_lseek(&gFile, CREATE_LINKMAP);
    if (result != FR_OK)
    {
        logAddress(0xDEADBEEF);
        logAddress(result);
    }
}
