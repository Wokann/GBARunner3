#include "common.h"
#include <string.h>
#include "mini-printf.h"
#include "Fat/ff.h"
#include "GbaHeader.h"
#include "MemoryEmulator/RomDefs.h"
#include "ExternalPatch.h"
#include "cp15.h"

extern FIL gExternalPatchFile; 
//[[gnu::section(".ewram.bss")]] 
ExternalPatch gExternalPatch;
#define GAME_PATCH_FILE_PATH_FORMAT     "/_gba/titles/%c%c%c%c%02X.patch"

//[[gnu::section(".ewram")]]  
bool ExternalPatch::TryLoad(const GbaHeader& header)
{
    mLoaded = false;

    char ExternalPatchPath[64];
    mini_snprintf(ExternalPatchPath, sizeof(ExternalPatchPath), GAME_PATCH_FILE_PATH_FORMAT,
        header.gameCode & 0xFF,
        (header.gameCode >> 8) & 0xFF,
        (header.gameCode >> 16) & 0xFF,
        header.gameCode >> 24,
        header.softwareVersion);

    memset(&gExternalPatchFile, 0, sizeof(gExternalPatchFile));
    if (f_open(&gExternalPatchFile, ExternalPatchPath, FA_OPEN_EXISTING | FA_READ) != FR_OK)
        return false;

    UINT br;
    if (f_read(&gExternalPatchFile, &mHeader, PATCH_HEADER_SIZE, &br) != FR_OK
        || br != PATCH_HEADER_SIZE
        || memcmp(mHeader.magic, "PATCH", 5) != 0
        || mHeader.gamecode != header.gameCode
        || mHeader.version != header.softwareVersion)
    {
        f_close(&gExternalPatchFile);
        return false;
    }
    mLoaded = true;
    return true;
}

//[[gnu::section(".ewram")]]  
void ExternalPatch::ApplyLinearPatches()
{
    if (!mLoaded || mHeader.linear_address == 0)
        return;

    UINT br;
    f_lseek(&gExternalPatchFile, mHeader.linear_address);

    for (u16 i = 0; i < mHeader.linear_patch_count; i++)
    {
        LinearPatchEntry e;
        if (f_read(&gExternalPatchFile, &e, sizeof(LinearPatchEntry), &br) != FR_OK
            || br != sizeof(LinearPatchEntry))
            break;

        u32 romOffset = e.rom_offset - ROM_LINEAR_GBA_ADDRESS;
        if (romOffset >= ROM_LINEAR_SIZE)
            continue;

        FSIZE_t nextEntryPos = f_tell(&gExternalPatchFile);
        f_lseek(&gExternalPatchFile, e.data_offset);
        f_read(&gExternalPatchFile, (u8*)ROM_LINEAR_DS_ADDRESS + romOffset, e.length, &br);
        f_lseek(&gExternalPatchFile, nextEntryPos);
    }
}

//[[gnu::section(".ewram")]]  
void ExternalPatch::ApplyHicodeBlockPatches(u32 romBlock, void* cacheBlock)
{
    if (!mLoaded || mHeader.hicode_address == 0 || romBlock < 512)
        return;

    UINT br;
    HicodeBlockEntry be;
    bool found = false;

    int left = 0, right = (int)mHeader.hicode_block_count - 1;
    while (left <= right)
    {
        int mid = (left + right) / 2;
        f_lseek(&gExternalPatchFile,
            mHeader.hicode_address + (u32)mid * sizeof(HicodeBlockEntry));
        if (f_read(&gExternalPatchFile, &be, sizeof(HicodeBlockEntry), &br) != FR_OK
            || br != sizeof(HicodeBlockEntry))
            return;

        if (be.block_index == (u16)romBlock)
        {
            found = true;
            break;
        }
        else if (be.block_index < (u16)romBlock)
            left = mid + 1;
        else
            right = mid - 1;
    }

    if (!found)
        return;

    f_lseek(&gExternalPatchFile, be.block_patches_index_offset);
    for (u16 j = 0; j < be.block_patch_count; j++)
    {
        HicodePatchEntry pe;
        if (f_read(&gExternalPatchFile, &pe, sizeof(HicodePatchEntry), &br) != FR_OK
            || br != sizeof(HicodePatchEntry))
            break;

        FSIZE_t nextEntryPos = f_tell(&gExternalPatchFile);
        f_lseek(&gExternalPatchFile, pe.data_offset);
        f_read(&gExternalPatchFile, (u8*)cacheBlock + pe.block_offset, pe.length, &br);
        f_lseek(&gExternalPatchFile, nextEntryPos);
    }
}

//extern "C" [[gnu::section(".ewram")]]  
extern "C" void externalPatch_applyHicodeBlock(u32 romBlock, void* cacheBlock)
{
    gExternalPatch.ApplyHicodeBlockPatches(romBlock, cacheBlock);
}
