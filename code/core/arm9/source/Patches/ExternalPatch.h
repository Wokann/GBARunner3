#pragma once

#ifdef __cplusplus
#include "Fat/ff.h"
#include "GbaHeader.h"

#define PATCH_HEADER_SIZE    32 
#pragma pack(push, 1)
struct PatchFileHeader
{
    char  magic[5];              // "PATCH"
    u8    pad0;
    u8    pad1;
    u8    pad2;
    u32   filesize;             // total size of the patch file, including the header
    u32   gamecode;             // game code of the target ROM
    u8    version;              // version of the target ROM
    u8    pad3;
    u8    pad4;
    u8    pad5;
    u32   linear_address;       // offset in patch file where the LinearPatchEntry starts
    u32   hicode_address;       // offset in patch file where the HicodeBlockEntry starts
    u16   linear_patch_count;   // number of LinearPatchEntry(s)
    u16   hicode_block_count;   // number of HicodeBlockEntry(s)
};

struct LinearPatchEntry
{
    u32 rom_offset;             // offset within the linear ROM (2MB) where the patch should be applied
    u32 length;                 // length of the patch data
    u32 data_offset;            // offset in patch file where the patch data starts
};

struct HicodeBlockEntry
{
    u16 block_index;                // ROM block（512~8191）
    u16 block_patch_count;          // how many patche(s) need to be applied in this block
    u32 block_patches_index_offset; // offset in patch file where the HicodePatchEntry of this block starts.
};

struct HicodePatchEntry
{
    u16 block_offset;           // offset within the block where the patch should be applied
    u16 length;                 // length of the patch data
    u32 data_offset;            // offset in patch file where the patch data starts
};
#pragma pack(pop)

class ExternalPatch
{
public:
    bool TryLoad(const GbaHeader& header);
    void ApplyLinearPatches();
    void ApplyHicodeBlockPatches(u32 romBlock, void* cacheBlock);
    bool IsLoaded() const { return mLoaded; }

private:
    bool            mLoaded = false;
    PatchFileHeader mHeader;
};

extern ExternalPatch gExternalPatch;
#endif // __cplusplus


#ifdef __cplusplus
extern "C" {
#endif

void externalPatch_applyHicodeBlock(u32 romBlock, void* cacheBlock);

#ifdef __cplusplus
}
#endif
