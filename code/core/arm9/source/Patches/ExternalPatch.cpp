#include "common.h"
#include <string.h>
#include "mini-printf.h"
#include "Fat/ff.h"
#include "GbaHeader.h"
#include "MemoryEmulator/RomDefs.h"
#include "SdCache/SdCache.h"
#include "ExternalPatch.h"
#include "cp15.h"

extern FIL gExternalPatchFile;
extern FIL gFile; // ROM file (SD mode). For slot2 this must be replaced by the cart size.

[[gnu::section(".ewram.bss"), gnu::aligned(4)]]
static RomBlockIndexEntry sBlockIndex[EXTERNAL_PATCH_MAX_BLOCK_COUNT];

ExternalPatch gExternalPatch;

#define GAME_PATCH_FILE_PATH_FORMAT "/_gba/titles/%c%c%c%c%02X.patch"

[[gnu::section(".ewram")]]
bool ExternalPatch::LoadBlockIndex()
{
    mIndexCount = 0;

    u32 indexAddress = mHeader.rom_block_index_address;
    u32 blockCount = mHeader.rom_block_count;
    u32 indexSize = mHeader.rom_block_index_size;

    if (indexAddress == 0)
    {
        return blockCount == 0;
    }

    if (blockCount == 0 || blockCount > EXTERNAL_PATCH_MAX_BLOCK_COUNT)
    {
        return false;
    }

    if (indexSize != blockCount * sizeof(RomBlockEntry))
    {
        return false;
    }

    FSIZE_t fileSize = f_size(&gExternalPatchFile);
    if ((u64)indexAddress + indexSize > fileSize)
    {
        return false;
    }

    // Verify footer at the end of the file
    UINT br;
    u8 footer[6];
    if (fileSize < 6 || f_lseek(&gExternalPatchFile, fileSize - 6) != FR_OK)
    {
        return false;
    }
    if (f_read(&gExternalPatchFile, footer, 6, &br) != FR_OK || br != 6)
    {
        return false;
    }
    if (memcmp(footer, "EOFGR3", 6) != 0)
    {
        return false;
    }

    if (f_lseek(&gExternalPatchFile, indexAddress) != FR_OK)
    {
        return false;
    }

    // Validate: block ids must be unique and strictly increasing (binary search
    // depends on it), within the 32MB address space, and each patch section
    // must lie inside the file.
    u32 prevBlock = 0;
    for (u32 i = 0; i < blockCount; i++)
    {
        RomBlockEntry fileEntry;
        if (f_read(&gExternalPatchFile, &fileEntry, sizeof(RomBlockEntry), &br) != FR_OK
            || br != sizeof(RomBlockEntry))
        {
            return false;
        }

        if (fileEntry.block_index > 8191)
        {
            return false;
        }
        if (i > 0 && fileEntry.block_index <= prevBlock)
        {
            return false;
        }
        if (fileEntry.block_patch_size > 0xFFFF
            || (u64)fileEntry.block_patch_address + fileEntry.block_patch_size > fileSize)
        {
            return false;
        }

        sBlockIndex[i].block_patch_address = fileEntry.block_patch_address;
        sBlockIndex[i].block_patch_size = (u16)fileEntry.block_patch_size;
        sBlockIndex[i].block_index = (u16)fileEntry.block_index;

        prevBlock = fileEntry.block_index;
    }

    mIndexCount = blockCount;
    return true;
}

[[gnu::section(".ewram")]]
bool ExternalPatch::TryLoad(const GbaHeader& header)
{
    if (mLoaded)
    {
        f_close(&gExternalPatchFile);
        mLoaded = false;
    }
    mIndexCount = 0;
    mFailedBlockCount = 0;

    char patchPath[64];
    mini_snprintf(patchPath, sizeof(patchPath), GAME_PATCH_FILE_PATH_FORMAT,
        header.gameCode & 0xFF,
        (header.gameCode >> 8) & 0xFF,
        (header.gameCode >> 16) & 0xFF,
        header.gameCode >> 24,
        header.softwareVersion);

    memset(&gExternalPatchFile, 0, sizeof(gExternalPatchFile));
    if (f_open(&gExternalPatchFile, patchPath, FA_OPEN_EXISTING | FA_READ) != FR_OK)
    {
        gLogger->Log(LogLevel::Debug, "No external patch found: %s\n", patchPath);
        return false;
    }

    UINT br;
    if (f_read(&gExternalPatchFile, &mHeader, sizeof(PatchFileHeader), &br) != FR_OK
        || br != sizeof(PatchFileHeader))
    {
        gLogger->Log(LogLevel::Error, "External patch: failed to read header\n");
        f_close(&gExternalPatchFile);
        return false;
    }

    if (memcmp(mHeader.magic, "PATCHGR3", 8) != 0)
    {
        gLogger->Log(LogLevel::Error, "External patch: invalid magic\n");
        f_close(&gExternalPatchFile);
        return false;
    }

    // Deliberately strict: never apply a patch to the wrong ROM.
    if (mHeader.gamecode != header.gameCode)
    {
        gLogger->Log(LogLevel::Error, "External patch: gamecode mismatch\n");
        f_close(&gExternalPatchFile);
        return false;
    }
    if (mHeader.version != header.softwareVersion)
    {
        gLogger->Log(LogLevel::Error, "External patch: version mismatch\n");
        f_close(&gExternalPatchFile);
        return false;
    }

    if (!LoadBlockIndex())
    {
        gLogger->Log(LogLevel::Error, "External patch: invalid block index\n");
        f_close(&gExternalPatchFile);
        return false;
    }

    mLoaded = true;
    gLogger->Log(LogLevel::Debug, "External patch loaded: %u blocks\n", mIndexCount);
    return true;
}

[[gnu::section(".ewram")]]
bool ExternalPatch::ApplyIPSPatches(u32 patchOffset, u32 patchSize, u8* targetBuf)
{
    if (!mLoaded)
    {
        return false;
    }

    UINT br;
    if (f_lseek(&gExternalPatchFile, patchOffset + 5) != FR_OK)
    {
        return false;
    }

    u32 endPos = patchOffset + patchSize;
    bool sawEof = false;

    while (f_tell(&gExternalPatchFile) < endPos)
    {
        u8 offsetBuf[3];
        if (f_read(&gExternalPatchFile, offsetBuf, 3, &br) != FR_OK || br != 3)
        {
            return false;
        }

        u32 offset = ((u32)offsetBuf[0] << 16) | ((u32)offsetBuf[1] << 8) | offsetBuf[2];

        // IPS EOF marker
        if (offset == 0x454F46)
        {
            sawEof = true;
            break;
        }

        u8 lenBuf[2];
        if (f_read(&gExternalPatchFile, lenBuf, 2, &br) != FR_OK || br != 2)
        {
            return false;
        }

        u16 len = ((u16)lenBuf[0] << 8) | lenBuf[1];

        if (len == 0)
        {
            // RLE record
            u8 rleLenBuf[2];
            if (f_read(&gExternalPatchFile, rleLenBuf, 2, &br) != FR_OK || br != 2)
            {
                return false;
            }
            u16 rleLen = ((u16)rleLenBuf[0] << 8) | rleLenBuf[1];
            u8 val;
            if (f_read(&gExternalPatchFile, &val, 1, &br) != FR_OK || br != 1)
            {
                return false;
            }

            if (offset + rleLen > SDC_BLOCK_SIZE)
            {
                return false;
            }
            memset(targetBuf + offset, val, rleLen);
        }
        else
        {
            // Standard record
            if (offset + len > SDC_BLOCK_SIZE)
            {
                return false;
            }
            if (f_read(&gExternalPatchFile, targetBuf + offset, len, &br) != FR_OK || br != len)
            {
                return false;
            }
        }
    }

    if (!sawEof)
    {
        return false;
    }

    dc_flushRange(targetBuf, SDC_BLOCK_SIZE);
    dc_drainWriteBuffer();
    return true;
}

[[gnu::section(".ewram")]]
bool ExternalPatch::ApplyLinearPatches()
{
    if (!mLoaded)
    {
        return false;
    }

    mFailedBlockCount = 0;
    u32 romFileSize = f_size(&gFile);
    bool allOk = true;

    // The index is sorted, so we can stop as soon as a block >= 512 is hit.
    for (u32 i = 0; i < mIndexCount; i++)
    {
        const RomBlockIndexEntry& entry = sBlockIndex[i];
        if (entry.block_index >= 512)
        {
            break;
        }

        u8* linearBuf = (u8*)ROM_LINEAR_DS_ADDRESS + (u32)entry.block_index * SDC_BLOCK_SIZE;

        // Blocks beyond the ROM file have an implicit 0xFF base.
        if ((u64)entry.block_index * SDC_BLOCK_SIZE >= romFileSize)
        {
            memset(linearBuf, 0xFF, SDC_BLOCK_SIZE);
        }

        if (!ApplyIPSPatches(entry.block_patch_address, entry.block_patch_size, linearBuf))
        {
            allOk = false;
            mFailedBlockCount++;
            gLogger->Log(LogLevel::Error, "External patch: failed to apply block %u\n", entry.block_index);
        }
    }

    return allOk;
}

[[gnu::section(".ewram")]]
bool ExternalPatch::ApplyRomBlockPatches(u32 romBlock, void* cacheBlock)
{
    if (!mLoaded || mIndexCount == 0)
    {
        return false;
    }

    int index = FindBlockIndex(romBlock);
    if (index < 0)
    {
        return false;
    }

    const RomBlockIndexEntry& entry = sBlockIndex[index];
    u32 romFileSize = f_size(&gFile);
    if ((u64)romBlock * SDC_BLOCK_SIZE >= romFileSize)
    {
        memset(cacheBlock, 0xFF, SDC_BLOCK_SIZE);
    }
    return ApplyIPSPatches(entry.block_patch_address, entry.block_patch_size, (u8*)cacheBlock);
}

int ExternalPatch::FindBlockIndex(u32 romBlock) const
{
    if (!mLoaded || mIndexCount == 0)
    {
        return -1;
    }
    if (romBlock < mHeader.rom_block_min || romBlock > mHeader.rom_block_max)
    {
        return -1;
    }

    int left = 0, right = (int)mIndexCount - 1;
    while (left <= right)
    {
        int mid = (left + right) / 2;
        const RomBlockIndexEntry& entry = sBlockIndex[mid];
        if (entry.block_index == romBlock)
        {
            return mid;
        }
        else if (entry.block_index < romBlock)
        {
            left = mid + 1;
        }
        else
        {
            right = mid - 1;
        }
    }

    return -1;
}

bool ExternalPatch::IsBlockPatched(u32 romBlock) const
{
    return FindBlockIndex(romBlock) >= 0;
}

extern "C" [[gnu::section(".ewram")]] void externalPatch_applyRomBlock(u32 romBlock, void* cacheBlock)
{
    // Runtime failures are silent on purpose: the raw block stays in place.
    // Startup failures are reported by ApplyLinearPatches' caller.
    gExternalPatch.ApplyRomBlockPatches(romBlock, cacheBlock);
}

extern "C" [[gnu::section(".ewram")]] bool externalPatch_isBlockPatched(u32 romBlock)
{
    return gExternalPatch.IsBlockPatched(romBlock);
}
