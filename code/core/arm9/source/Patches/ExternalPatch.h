#pragma once

#ifdef __cplusplus
#include "Fat/ff.h"
#include "GbaHeader.h"

#pragma pack(push, 1)
struct PatchFileHeader
{
    char  magic[8];                     // Magic number: "PATCHGR3"
    u32   filesize;                     // Total size of the patch file
    u32   gamecode;                  // Game code of the target ROM, like "AXVE"
    u8    version;                      // Version of the target ROM
    u8    pad[3];                       // Alignment padding
    u32   rom_block_index_address;      // Block index address in patch file (0 means no patch)
    u32   rom_block_count;              // Count of blocks need to be patched
    u32   rom_block_index_size;         // Total size of the block index
    u16   rom_block_min;                // Min block id (0~8191)
    u16   rom_block_max;                // Max block id (0~8191)
};

// Block index entry, 16-byte aligned, compatible with original hicode entry
struct RomBlockEntry
{
    u32 block_patch_address;            // Offset of the IPS patches for this block
    u32 block_patch_count;              // Count of patch records
    u32 block_patch_size;               // Total size of the IPS patches
    u32 block_index;                    // ROM block index (0~8191)
};
#pragma pack(pop)

class ExternalPatch
{
public:
    bool TryLoad(const GbaHeader& header);
    //void ApplyLinearPatches();
    void ApplyRomBlockPatches(u32 romBlock, void* cacheBlock);
    bool IsLoaded() const { return mLoaded; }

private:
    bool            mLoaded = false;
    PatchFileHeader mHeader;
    void ApplyIPSPatches(u32 patch_offset, u32 patch_size, u8* target_buf);//, u32 max_target_offset);
};

extern ExternalPatch gExternalPatch;
#endif // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif

void externalPatch_applyRomBlock(u32 romBlock, void* cacheBlock);
#ifdef __cplusplus
}
#endif
