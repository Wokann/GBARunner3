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
//[[gnu::section(".ewram.bss")]] 
ExternalPatch gExternalPatch;
#define GAME_PATCH_FILE_PATH_FORMAT     "/_gba/titles/%c%c%c%c%02X.patch"

//[[gnu::section(".ewram")]]  
// Generic IPS patch application function, fully on-demand reading, no extra large memory usage
void ExternalPatch::ApplyIPSPatches(u32 patch_offset, u32 patch_size, u8* target_buf)//, u32 max_target_offset)
{
    if (!mLoaded)
        return;
        
    UINT br;
    // Seek to the start of the patch data
    f_lseek(&gExternalPatchFile, patch_offset + 5);
    
    u32 end_pos = patch_offset + patch_size;
    while (f_tell(&gExternalPatchFile) < end_pos)
    {
        // Read 3 bytes of IPS offset (big-endian)
        u8 offset_buf[3];
        if (f_read(&gExternalPatchFile, offset_buf, 3, &br) != FR_OK || br != 3)
            break;
            
        u32 offset = ((u32)offset_buf[0] << 16) | 
                     ((u32)offset_buf[1] << 8) | 
                     offset_buf[2];
                     
        // Check if this is the EOF marker of IPS
        if (offset == 0x454F46) // "EOF"
            break;
            
        // Read 2 bytes of length
        u8 len_buf[2];
        if (f_read(&gExternalPatchFile, len_buf, 2, &br) != FR_OK || br != 2)
            break;
            
        u16 len = ((u16)len_buf[0] << 8) | len_buf[1];
        
        if (len == 0)
        {
            // RLE record: 3 bytes offset + 2 bytes zero marker + 2 bytes length + 1 byte value
            u8 rle_len_buf[2];
            if (f_read(&gExternalPatchFile, rle_len_buf, 2, &br) != FR_OK || br != 2)
                break;
                
            u16 rle_len = ((u16)rle_len_buf[0] << 8) | rle_len_buf[1];
            u8 val;
            if (f_read(&gExternalPatchFile, &val, 1, &br) != FR_OK || br != 1)
                break;
                
            // Apply RLE, check for out of bounds
            if (offset + rle_len <= SDC_BLOCK_SIZE) // Max 4KB per block
                memset(target_buf + offset, val, rle_len);
        }
        else
        {
            // Standard record: read data directly to target buffer, no intermediate buffer needed
            if (offset + len <= SDC_BLOCK_SIZE)
            {
                // Read directly to the target address, same logic as old version
                f_read(&gExternalPatchFile, target_buf + offset, len, &br);
            }
            else
            {
                // Out of bounds, skip this data
                f_lseek(&gExternalPatchFile, f_tell(&gExternalPatchFile) + len);
            }
        }
    }
    dc_flushRange(target_buf, SDC_BLOCK_SIZE);
    dc_drainWriteBuffer();
    ic_invalidateAll();
}

bool ExternalPatch::TryLoad(const GbaHeader& header)
{
    if (mLoaded) f_close(&gExternalPatchFile);
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
    if (f_read(&gExternalPatchFile, &mHeader, sizeof(PatchFileHeader), &br) != FR_OK
        || br != sizeof(PatchFileHeader)
        || memcmp(mHeader.magic, "PATCHGR3", 8) != 0
        || mHeader.gamecode != header.gameCode
        || mHeader.version != header.softwareVersion)
    {
        f_close(&gExternalPatchFile);
        return false;
    }
    mLoaded = true;
    return true;
}
/*
//[[gnu::section(".ewram")]]  
// Apply all patches for 0~511 blocks to linear memory at once on load
// Index is sorted, iterate entries directly without binary search
void ExternalPatch::ApplyLinearPatches()
{
    if (!mLoaded || mHeader.rom_block_index_address == 0)
        return;
        
    // Iterate all index entries in order, they are sorted by block_id
    for (u32 i=0; i<mHeader.rom_block_count; i++)
    {
        UINT br;
        RomBlockEntry be;
        f_lseek(&gExternalPatchFile,
            mHeader.rom_block_index_address + i * sizeof(RomBlockEntry));
        if (f_read(&gExternalPatchFile, &be, sizeof(RomBlockEntry), &br) != FR_OK
            || br != sizeof(RomBlockEntry))
            return;
            
        // Since index is sorted, once we hit block >=512, we can break early
        if (be.block_index >= 512)
            break;
            
        // Apply patch to linear memory
        u8* linear_buf = (u8*)ROM_LINEAR_DS_ADDRESS + be.block_index * SDC_BLOCK_SIZE;
        ApplyIPSPatches(be.block_patch_address, be.block_patch_size, linear_buf);//, SDC_BLOCK_SIZE);
    }
}*/

//[[gnu::section(".ewram")]]  
// Apply patch for single block, sync to linear memory if it's in first 2MB
void ExternalPatch::ApplyRomBlockPatches(u32 romBlock, void* cacheBlock)
{
    if (!mLoaded || mHeader.rom_block_index_address == 0)
        return;
    if (romBlock < mHeader.rom_block_min || romBlock > mHeader.rom_block_max)
        return;

    UINT br;
    RomBlockEntry be;
    bool found = false;

    int left = 0, right = (int)mHeader.rom_block_count - 1;
    while (left <= right)
    {
        int mid = (left + right) / 2;
        f_lseek(&gExternalPatchFile,
            mHeader.rom_block_index_address + (u32)mid * sizeof(RomBlockEntry));
        if (f_read(&gExternalPatchFile, &be, sizeof(RomBlockEntry), &br) != FR_OK
            || br != sizeof(RomBlockEntry))
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

    // All blocks are 4KB, use SDC_BLOCK_SIZE as max offset
    // First apply patch to SD cache block
    ApplyIPSPatches(be.block_patch_address, be.block_patch_size, (u8*)cacheBlock);//, SDC_BLOCK_SIZE);
    
    // If this block is in first 2MB, also sync patch to linear memory
    if (romBlock < 512)
    {
        u8* linear_buf = (u8*)ROM_LINEAR_DS_ADDRESS + romBlock * SDC_BLOCK_SIZE;
        ApplyIPSPatches(be.block_patch_address, be.block_patch_size, linear_buf);//, SDC_BLOCK_SIZE);
    }
}

extern "C" void externalPatch_applyRomBlock(u32 romBlock, void* cacheBlock)
{
    gExternalPatch.ApplyRomBlockPatches(romBlock, cacheBlock);
}
