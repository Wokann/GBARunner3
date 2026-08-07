#pragma once

#ifdef __cplusplus
#include "Fat/ff.h"
#include "GbaHeader.h"

// 4096 blocks = 16MB of patched ROM, far beyond any realistic patch. The
// in-memory index is 8 bytes per block, so this keeps EWRAM usage at 32KB.
#define EXTERNAL_PATCH_MAX_BLOCK_COUNT 4096

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

// ===========================================================================
// Pre-patched block cache ("bake") sidecar
// ---------------------------------------------------------------------------
// Path: /_gba/prepatch/XXXX00.pre (XXXX00 = same gamecode+version as the
// .patch file, so the filename alone identifies the patch it belongs to).
//
// Stores ONLY the 4KB blocks modified by the patch, each pre-patched once at
// boot. At runtime the SD cache reads those blocks straight from this file
// via raw sector reads: no FatFS, no IPS parsing, no IRQ masking in the hot
// path. Unpatched blocks keep reading from the original source (ROM file /
// slot2 cart).
//
// On-disk layout (little-endian, packed):
//   [PrepatchHeader 16B]
//   [PrepatchBlockEntry x N]      one per patched block, in index order
//   [padding to a cluster-size multiple]  keeps every baked block aligned to
//                                         an SD cluster boundary
//   [4096-byte baked block x N]   entry i lives at dataBase + i * 4096
//
// Boot validation: valid==1, formatVersion, CRC32 of the whole .patch file,
// ROM/cart size, exact file size, and index consistency. Any mismatch (or a
// missing file) triggers a re-bake. `valid` is written 0 first and promoted
// to 1 only after everything is on disk + f_sync, so a power loss mid-bake
// is detected and rebuilt instead of being trusted.
// ===========================================================================
#define PREPATCH_FORMAT_VERSION 1

#pragma pack(push, 1)
struct PrepatchHeader
{
    u8    valid;              // 1 = committed, 0 = incomplete bake (power loss)
    u8    formatVersion;      // PREPATCH_FORMAT_VERSION
    u8    reserved[2];
    u32   patchHash;          // CRC32 of the whole .patch file
    u32   patchedBlockCount;
    u32   romSize;            // ROM/cart size used for the out-of-bounds base
};

struct PrepatchBlockEntry
{
    u16   blockIndex;
    u16   reserved;
    u32   dataOffset;         // byte offset of the 4096-byte baked block
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
    u32 GetBlockCount() const { return mIndexCount; }
    int FindBlockIndex(u32 romBlock) const;
    /// @brief Sets the ROM size explicitly (slot2 cart mode). 0 = f_size(&gFile).
    void SetRomSize(u32 romSize) { mRomSize = romSize; }

private:
    bool ApplyIPSPatches(u32 patchOffset, u32 patchSize, u8* targetBuf);
    bool LoadBlockIndex();
    bool TryLoadPrepatch(u32 patchHash);
    bool BakePrepatch(u32 patchHash);
    bool ReadRomBlock(u32 romBlock, u8* dst);

    bool            mLoaded = false;
    u32             mIndexCount = 0;
    u32             mFailedBlockCount = 0;
    u32             mRomSize = 0;
    PatchFileHeader mHeader;
};

extern ExternalPatch gExternalPatch;
#endif // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif

void externalPatch_applyRomBlock(u32 romBlock, void* cacheBlock);
bool externalPatch_isBlockPatched(u32 romBlock);
/// @brief Returns the physical SD sector of the baked 4KB block in the .pre
///        sidecar, or 0 if the block is not patched / no cache is active.
u32 externalPatch_getBakedBlockSector(u32 romBlock);
#ifdef __cplusplus
}
#endif
