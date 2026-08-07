#pragma once

#ifdef __cplusplus
#include "Fat/ff.h"
#include "GbaHeader.h"

#define EXTERNAL_PATCH_MAX_BLOCK_COUNT 8192

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

// Block index entry
struct RomBlockEntry
{
    u32 block_patch_address;            // Offset of the IPS patches for this block
    u32 block_patch_count;              // Count of patch records
    u32 block_patch_size;               // Total size of the IPS patches
    u32 block_index;                    // ROM block index (0~8191)
};

// Compact in-memory index entry (8 bytes per block, max 64KB total).
// block_patch_size is bounded by 64KB which is far above the worst case
// IPS data for a single 4KB block (~24KB).
struct RomBlockIndexEntry
{
    u32 block_patch_address;
    u16 block_patch_size;
    u16 block_index;
};
#pragma pack(pop)

class ExternalPatch
{
public:
    bool TryLoad(const GbaHeader& header);
    bool ApplyLinearPatches();
    bool ApplyRomBlockPatches(u32 romBlock, void* cacheBlock);
    bool IsBlockPatched(u32 romBlock) const;
    bool IsLoaded() const { return mLoaded; }
    u32 GetFailedBlockCount() const { return mFailedBlockCount; }

private:
    int FindBlockIndex(u32 romBlock) const;
    bool ApplyIPSPatches(u32 patchOffset, u32 patchSize, u8* targetBuf);
    bool LoadBlockIndex();

    bool            mLoaded = false;
    u32             mIndexCount = 0;
    u32             mFailedBlockCount = 0;
    PatchFileHeader mHeader;
};

extern ExternalPatch gExternalPatch;
#endif // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif

void externalPatch_applyRomBlock(u32 romBlock, void* cacheBlock);
bool externalPatch_isBlockPatched(u32 romBlock);
#ifdef __cplusplus
}
#endif
