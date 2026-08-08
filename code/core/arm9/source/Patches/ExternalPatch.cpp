#include "common.h"
#include <string.h>
#include "mini-printf.h"
#include "Fat/ff.h"
#include "Fat/FsIpc.h"
#include "Fat/diskio.h"
#include "GbaHeader.h"
#include "MemoryEmulator/RomDefs.h"
#include "SdCache/SdCache.h"
#include "MemCopy.h"
#include "ExternalPatch.h"
#include "cp15.h"

extern FIL gExternalPatchFile;
extern FIL gFile; // ROM file (SD mode); slot2 cart mode uses SetRomSize() instead.
extern bool gSlot2Active;

[[gnu::section(".ewram.bss"), gnu::aligned(4)]]
static RomBlockIndexEntry sBlockIndex[EXTERNAL_PATCH_MAX_BLOCK_COUNT];

[[gnu::section(".ewram.bss"), gnu::aligned(32)]]
static FIL gPrepatchFile;

[[gnu::section(".ewram.bss"), gnu::aligned(32)]]
static DWORD sPrepatchClusterTable[256];

[[gnu::section(".ewram.bss")]]
static bool sPrepatchTableValid;
[[gnu::section(".ewram.bss")]]
static u32 sPrepatchFileSize;

// Bake scratch: the SD cache is unused during boot-time baking, so reuse
// cache block 0 instead of reserving another 4KB in EWRAM.
#define BAKE_BUF (&sdc_cache[0][0])

// In EWRAM BSS: the default .bss (vrama) is full and has no headroom.
[[gnu::section(".ewram.bss")]]
ExternalPatch gExternalPatch;

#define GAME_PATCH_FILE_PATH_FORMAT     "/_gba/titles/%c%c%c%c%02X.patch"
#define GAME_PREPATCH_DIR_PATH          "/_gba/prepatch"
#define GAME_PREPATCH_FILE_PATH_FORMAT "/_gba/prepatch/%c%c%c%c%02X.pre"

[[gnu::section(".ewram")]]
static u32 crc32Update(u32 crc, const u8* data, u32 len)
{
    for (u32 i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    return crc;
}

[[gnu::section(".ewram")]]
static u32 computePatchFileHash()
{
    if (f_lseek(&gExternalPatchFile, 0) != FR_OK)
        return 0;

    u32 crc = 0xFFFFFFFF;
    while (true)
    {
        UINT br;
        if (f_read(&gExternalPatchFile, BAKE_BUF, SDC_BLOCK_SIZE, &br) != FR_OK)
            return 0;
        if (br == 0)
            break;
        crc = crc32Update(crc, BAKE_BUF, br);
    }
    return crc ^ 0xFFFFFFFF;
}

[[gnu::section(".ewram")]]
static void getPrepatchPath(char* out, u32 size, u32 gameCode, u8 version)
{
    mini_snprintf(out, size, GAME_PREPATCH_FILE_PATH_FORMAT,
        gameCode & 0xFF,
        (gameCode >> 8) & 0xFF,
        (gameCode >> 16) & 0xFF,
        gameCode >> 24,
        version);
}

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
bool ExternalPatch::ReadRomBlock(u32 romBlock, u8* dst)
{
    // SD mode: read from the open ROM file. For slot2 cart mode this must be
    // replaced by a mem_copy32 from 0x08000000 + romBlock * SDC_BLOCK_SIZE
    // (see the cache-hicode/slot2patch branch).
    if (mRomSize == 0)
    {
        mRomSize = (u32)f_size(&gFile);
    }

    if ((u64)romBlock * SDC_BLOCK_SIZE >= mRomSize)
    {
        // Block beyond the ROM/cart size: 0xFF base, same as ApplyRomBlockPatches.
        memset(dst, 0xFF, SDC_BLOCK_SIZE);
        return true;
    }

    if (gSlot2Active)
    {
        // Slot2 cart: the original block is read straight from the cart.
        mem_copy32((void*)(0x08000000 + (u32)romBlock * SDC_BLOCK_SIZE), dst, SDC_BLOCK_SIZE);
        return true;
    }

    UINT br;
    if (f_lseek(&gFile, (FSIZE_t)romBlock * SDC_BLOCK_SIZE) != FR_OK)
        return false;
    if (f_read(&gFile, dst, SDC_BLOCK_SIZE, &br) != FR_OK || br != SDC_BLOCK_SIZE)
        return false;
    return true;
}

[[gnu::section(".ewram")]]
u32 ExternalPatch::ComputeRomSourceHash()
{
    // Hash a small sample of the ROM source (the slot2 cart in cart mode, or
    // the SD ROM file in SD mode) so a .pre baked from a different source is
    // detected and rebuilt.
    u32 romSize = mRomSize != 0 ? mRomSize : (u32)f_size(&gFile);
    u32 blocks[6];
    blocks[0] = 0;
    blocks[1] = 1;
    blocks[2] = 2;
    blocks[3] = 3;
    blocks[4] = romSize > 0 ? (romSize / 2) / SDC_BLOCK_SIZE : 0;
    blocks[5] = romSize > 0 ? (romSize - 1) / SDC_BLOCK_SIZE : 0;

    u32 crc = 0xFFFFFFFF;
    for (u32 i = 0; i < 6; i++)
    {
        u32 block = blocks[i];
        if ((u64)block * SDC_BLOCK_SIZE >= romSize)
            continue;
        if (gSlot2Active)
        {
            mem_copy32((void*)(0x08000000 + block * SDC_BLOCK_SIZE), BAKE_BUF, SDC_BLOCK_SIZE);
        }
        else
        {
            UINT br;
            if (f_lseek(&gFile, (FSIZE_t)block * SDC_BLOCK_SIZE) != FR_OK)
                return 0;
            if (f_read(&gFile, BAKE_BUF, SDC_BLOCK_SIZE, &br) != FR_OK || br != SDC_BLOCK_SIZE)
                return 0;
        }
        crc = crc32Update(crc, BAKE_BUF, SDC_BLOCK_SIZE);
    }
    return crc ^ 0xFFFFFFFF;
}

[[gnu::section(".ewram")]]
bool ExternalPatch::TryLoadPrepatch(u32 patchHash)
{
    if (mIndexCount == 0)
        return false;

    char prePath[64];
    getPrepatchPath(prePath, sizeof(prePath), mHeader.gamecode, mHeader.version);

    if (f_open(&gPrepatchFile, prePath, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return false;

    auto reject = [&]() -> bool {
        f_close(&gPrepatchFile);
        return false;
    };

    UINT br;
    PrepatchHeader header;
    if (f_read(&gPrepatchFile, &header, sizeof(header), &br) != FR_OK || br != sizeof(header))
        return reject();
    if (header.valid != 1 || header.formatVersion != PREPATCH_FORMAT_VERSION)
        return reject();
    if (header.patchHash != patchHash)
        return reject();
    if (header.patchedBlockCount != mIndexCount)
        return reject();

    u32 romSize = mRomSize != 0 ? mRomSize : (u32)f_size(&gFile);
    if (header.romSize != romSize)
        return reject();
    u32 romHash = ComputeRomSourceHash();
    if (romHash == 0 || header.romHash != romHash)
        return reject();

    u32 clusterSize = gFile.obj.fs->csize * 512;
    u32 indexBytes = mIndexCount * sizeof(PrepatchBlockEntry);
    u32 dataBase = (sizeof(PrepatchHeader) + indexBytes + clusterSize - 1) & ~(clusterSize - 1);
    FSIZE_t expectedSize = (FSIZE_t)dataBase + (FSIZE_t)mIndexCount * SDC_BLOCK_SIZE;
    if (f_size(&gPrepatchFile) != expectedSize)
        return reject();

    // Entries must match the in-memory index exactly (order, ids, offsets).
    for (u32 i = 0; i < mIndexCount; i++)
    {
        PrepatchBlockEntry entry;
        if (f_read(&gPrepatchFile, &entry, sizeof(entry), &br) != FR_OK || br != sizeof(entry))
            return reject();
        if (entry.blockIndex != sBlockIndex[i].block_index
            || entry.dataOffset != dataBase + i * SDC_BLOCK_SIZE)
            return reject();
    }

    // Build the cluster map for runtime raw-sector reads (no FatFS in the
    // hot path). The file handle stays open so obj.fs/cltbl remain valid.
    sPrepatchClusterTable[0] = sizeof(sPrepatchClusterTable) / sizeof(DWORD);
    gPrepatchFile.cltbl = sPrepatchClusterTable;
    if (f_lseek(&gPrepatchFile, CREATE_LINKMAP) != FR_OK)
        return reject();

    sPrepatchFileSize = (u32)f_size(&gPrepatchFile);
    sPrepatchTableValid = true;
    gLogger->Log(LogLevel::Debug, "External patch: prepatch cache valid (%u blocks)\n", mIndexCount);
    return true;
}

[[gnu::section(".ewram")]]
bool ExternalPatch::BakePrepatch(u32 patchHash)
{
    // TryLoad guarantees the patch is loaded before calling this. The
    // mLoaded flag is intentionally NOT checked: a corrupted flag must not
    // hide the real bake failure behind a silent early return.
    if (mIndexCount == 0)
    {
        return true;
    }

    char prePath[64];
    getPrepatchPath(prePath, sizeof(prePath), mHeader.gamecode, mHeader.version);

    FRESULT openRes = f_open(&gPrepatchFile, prePath, FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
    if (openRes != FR_OK)
    {
        gLogger->Log(LogLevel::Error, "External patch: cannot create %s (res=%d)\n", prePath, (int)openRes);
        return false;
    }

    auto abortBake = [&](u32 step) -> bool {
        gLogger->Log(LogLevel::Error, "External patch: bake aborted at step %u\n", step);
        f_sync(&gPrepatchFile);
        f_close(&gPrepatchFile);
        f_unlink(prePath);
        sPrepatchTableValid = false;
        return false;
    };

    u32 clusterSize = gFile.obj.fs->csize * 512;
    u32 indexBytes = mIndexCount * sizeof(PrepatchBlockEntry);
    u32 dataBase = (sizeof(PrepatchHeader) + indexBytes + clusterSize - 1) & ~(clusterSize - 1);

    PrepatchHeader header;
    memset(&header, 0, sizeof(header));
    header.valid = 0; // placeholder; promoted to 1 only after everything is written
    header.formatVersion = PREPATCH_FORMAT_VERSION;
    header.patchHash = patchHash;
    header.patchedBlockCount = mIndexCount;
    header.romSize = mRomSize != 0 ? mRomSize : (u32)f_size(&gFile);
    header.romHash = ComputeRomSourceHash();
    if (header.romHash == 0)
    {
        return abortBake(1);
    }

    UINT bw;
    if (f_write(&gPrepatchFile, &header, sizeof(header), &bw) != FR_OK || bw != sizeof(header))
        return abortBake(1);

    // Index entries, then padding up to the 512/cluster-aligned data base.
    u32 pos = sizeof(PrepatchHeader);
    for (u32 i = 0; i < mIndexCount; i++)
    {
        PrepatchBlockEntry entry;
        entry.blockIndex = sBlockIndex[i].block_index;
        entry.reserved = 0;
        entry.dataOffset = dataBase + i * SDC_BLOCK_SIZE;
        if (f_write(&gPrepatchFile, &entry, sizeof(entry), &bw) != FR_OK || bw != sizeof(entry))
            return abortBake(2);
        pos += sizeof(entry);
    }
    if (pos < dataBase)
    {
        memset(BAKE_BUF, 0, SDC_BLOCK_SIZE);
        u32 pad = dataBase - pos;
        while (pad > 0)
        {
            u32 chunk = pad > SDC_BLOCK_SIZE ? SDC_BLOCK_SIZE : pad;
            if (f_write(&gPrepatchFile, BAKE_BUF, chunk, &bw) != FR_OK || bw != chunk)
                return abortBake(3);
            pad -= chunk;
        }
    }

    // Bake every patched block: original block + IPS records -> 4KB result.
    u32 expectedFirstDword = 0;
    for (u32 i = 0; i < mIndexCount; i++)
    {
        const RomBlockIndexEntry& entry = sBlockIndex[i];
        if (!ReadRomBlock(entry.block_index, BAKE_BUF))
        {
            gLogger->Log(LogLevel::Error, "External patch: bake read failed for block %u\n", entry.block_index);
            return abortBake(4);
        }
        if (!ApplyIPSPatches(entry.block_patch_address, entry.block_patch_size, BAKE_BUF))
        {
            gLogger->Log(LogLevel::Error, "External patch: bake apply failed for block %u\n", entry.block_index);
            return abortBake(5);
        }
        if (i == 0)
        {
            // Capture the FIRST baked block's head for the readback check;
            // BAKE_BUF is reused and will hold the last block afterwards.
            expectedFirstDword = *(u32*)BAKE_BUF;
        }
        if (f_write(&gPrepatchFile, BAKE_BUF, SDC_BLOCK_SIZE, &bw) != FR_OK || bw != SDC_BLOCK_SIZE)
            return abortBake(6);
    }

    // Commit: promote the placeholder to a valid cache.
    header.valid = 1;
    if (f_lseek(&gPrepatchFile, 0) != FR_OK)
        return abortBake(7);
    if (f_write(&gPrepatchFile, &header, sizeof(header), &bw) != FR_OK || bw != sizeof(header))
        return abortBake(8);

    // Build the cluster map for runtime raw-sector reads.
    sPrepatchClusterTable[0] = sizeof(sPrepatchClusterTable) / sizeof(DWORD);
    gPrepatchFile.cltbl = sPrepatchClusterTable;
    if (f_lseek(&gPrepatchFile, CREATE_LINKMAP) != FR_OK)
        return abortBake(9);
    if (f_sync(&gPrepatchFile) != FR_OK)
        return abortBake(10);

    sPrepatchFileSize = (u32)f_size(&gPrepatchFile);
    sPrepatchTableValid = true;

    gLogger->Log(LogLevel::Debug,
        "External patch: baked %u blocks (clusterSize=%u) to %s\n",
        mIndexCount, clusterSize, prePath);

    // Self-check: read the first baked block back through the exact raw-sector
    // path used at runtime. Catches cluster-map/geometry mistakes at boot
    // instead of white-screening in-game.
    u32 checkSector = externalPatch_getBakedBlockSector(sBlockIndex[0].block_index);
    if (checkSector == 0)
    {
        gLogger->Log(LogLevel::Error, "External patch: readback sector lookup failed\n");
        return abortBake(11);
    }
    fs_readSectors(gFile.obj.fs->pdrv == DEV_FAT ? FS_DEVICE_DLDI : FS_DEVICE_DSI_SD,
                   BAKE_BUF, checkSector, SDC_BLOCK_SIZE / 512);
    if (memcmp(BAKE_BUF, &expectedFirstDword, 4) != 0)
    {
        gLogger->Log(LogLevel::Error,
            "External patch: readback mismatch (expected 0x%08X, got 0x%08X)\n",
            expectedFirstDword, *(u32*)BAKE_BUF);
        return abortBake(12);
    }
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
    if (sPrepatchTableValid)
    {
        f_close(&gPrepatchFile);
        sPrepatchTableValid = false;
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

    // Pre-patched cache: reuse a valid .pre, or bake a new one. The patch
    // hash recorded inside detects any change of the .patch file.
    if (mIndexCount == 0)
    {
        return true;
    }

    u32 patchHash = computePatchFileHash();
    if (patchHash == 0)
    {
        gLogger->Log(LogLevel::Error, "External patch: failed to hash patch file\n");
        f_close(&gExternalPatchFile);
        mLoaded = false;
        return false;
    }

    f_mkdir(GAME_PREPATCH_DIR_PATH); // ok if it already exists

    if (TryLoadPrepatch(patchHash))
    {
        return true;
    }

    if (!BakePrepatch(patchHash))
    {
        // The .pre cache is an optimization, not a requirement: fall back to
        // on-demand patching (linear at boot + per-cache-block at runtime).
        gLogger->Log(LogLevel::Warning, "External patch: bake failed, falling back to on-demand patching\n");
        return true;
    }
    return true;
}

[[gnu::section(".ewram")]]
bool ExternalPatch::ApplyIPSPatches(u32 patchOffset, u32 patchSize, u8* targetBuf)
{
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
    // No mLoaded check here on purpose: TryLoad() already guarantees the
    // patch is loaded, and a corrupted flag must not silently disable the
    // linear patching (see BakePrepatch for the same decision).
    mFailedBlockCount = 0;
    // Slot2 cart mode: the cart size is the boundary, not the placeholder ROM.
    u32 romFileSize = mRomSize != 0 ? mRomSize : (u32)f_size(&gFile);
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
    if (mIndexCount == 0)
    {
        return false;
    }

    int index = FindBlockIndex(romBlock);
    if (index < 0)
    {
        return false;
    }

    const RomBlockIndexEntry& entry = sBlockIndex[index];
    // Slot2 cart mode: the cart size (set via SetRomSize) is the boundary,
    // not the placeholder ROM file's size.
    u32 romFileSize = mRomSize != 0 ? mRomSize : (u32)f_size(&gFile);
    if ((u64)romBlock * SDC_BLOCK_SIZE >= romFileSize)
    {
        memset(cacheBlock, 0xFF, SDC_BLOCK_SIZE);
    }
    return ApplyIPSPatches(entry.block_patch_address, entry.block_patch_size, (u8*)cacheBlock);
}

int ExternalPatch::FindBlockIndex(u32 romBlock) const
{
    if (mIndexCount == 0)
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

extern "C" [[gnu::section(".ewram")]] u32 externalPatch_getBakedBlockSector(u32 romBlock)
{
    if (!sPrepatchTableValid)
        return 0;

    int index = gExternalPatch.FindBlockIndex(romBlock);
    if (index < 0)
        return 0;

    u32 clusterSize = gPrepatchFile.obj.fs->csize * 512;
    u32 indexBytes = gExternalPatch.GetBlockCount() * sizeof(PrepatchBlockEntry);
    u32 dataBase = (sizeof(PrepatchHeader) + indexBytes + clusterSize - 1) & ~(clusterSize - 1);
    u32 dataOffset = dataBase + (u32)index * SDC_BLOCK_SIZE;
    if (dataOffset >= sPrepatchFileSize)
        return 0;

    FATFS* fs = gPrepatchFile.obj.fs;
    u32* tbl = gPrepatchFile.cltbl + 1;
    u32* tableEnd = gPrepatchFile.cltbl + sPrepatchClusterTable[0];
    u32 csect = (UINT)(dataOffset / 512 & (fs->csize - 1));
    u32 cshift = __builtin_ctz(fs->csize) + 9;
    u32 cl = (DWORD)(dataOffset >> cshift);
    while (true)
    {
        if (tbl + 2 > tableEnd)
            return 0; // walked off the cluster map: never serve a garbage sector
        u32 ncl = *tbl++;
        if (cl < ncl)
            break;
        cl -= ncl;
        tbl++;
    }
    u32 cluster = cl + *tbl - 2;
    return fs->database + fs->csize * cluster + csect;
}
