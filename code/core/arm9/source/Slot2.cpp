#include "common.h"
#include "GbaHeader.h"
#include "MemCopy.h"
#include <libtwl/mem/memExtern.h>
#include "Slot2.h"

// In EWRAM BSS: the default .bss (vrama) is full and has no headroom.
[[gnu::section(".ewram.bss")]]
bool gSlot2Active = false;
[[gnu::section(".ewram.bss")]]
u32 gSlot2RomSize = 0;

extern GbaHeader gRomHeader;

/// @brief Detects the physical cart size by binary searching for the highest
///        4KB block whose first word still mirrors the start of the cart.
///        GBA carts mirror their ROM content beyond their physical size
///        because the upper address lines are not connected.
[[gnu::section(".ewram")]]
static u32 detectSlot2RomSize()
{
    // Header capacity code at ROM offset 0x80 (most carts write 0x00-0x06,
    // some write 0x96 for 32MB).
    u32 headerSize = 0;
    const u8* cartHeader = (const u8*)0x08000000;
    u8 sizeCode = cartHeader[0x80];
    if (sizeCode <= 0x06)
    {
        headerSize = 512 * 1024u << sizeCode; // 512KB .. 32MB
    }
    else if (sizeCode == 0x96)
    {
        headerSize = 32 * 1024 * 1024u;
    }

    // Mirror probe. This can under-estimate on carts that return garbage
    // instead of mirroring beyond their physical size.
    u32 firstWord;
    mem_copy32((void*)0x08000000u, &firstWord, 4);

    u32 lo = 0x00080000u / 4096; // 512KB
    u32 hi = 0x02000000u / 4096; // 32MB
    while (lo < hi)
    {
        u32 mid = (lo + hi + 1) / 2;
        u32 testWord;
        mem_copy32((void*)(0x08000000u + mid * 4096), &testWord, 4);
        if (testWord == firstWord)
        {
            lo = mid;
        }
        else
        {
            hi = mid - 1;
        }
    }
    u32 probeSize = lo * 4096;

    // Take the larger estimate: a too-large size only exposes mirrored
    // garbage (like the real GBA), while a too-small one would clobber real
    // cart data with the out-of-bounds fill.
    return probeSize > headerSize ? probeSize : headerSize;
}

// Checks if SLOT2 holds a game cart.
extern "C" [[gnu::section(".ewram")]] bool checkSlot2()
{
    if (gSlot2Active)
        return gSlot2Active;
    mem_setGbaCartridgeRamWait(EXMEMCNT_SLOT2_RAM_WAIT_10);
    mem_setGbaCartridgeRomWaits(EXMEMCNT_SLOT2_ROM_WAIT1_10, EXMEMCNT_SLOT2_ROM_WAIT2_6);
    mem_setGbaCartridgePhi(EXMEMCNT_SLOT2_PHI_LOW);
    mem_setGbaCartridgeCpu(EXMEMCNT_SLOT2_CPU_ARM9);

    mem_copy32((GbaHeader*)0x08000000u, &gRomHeader, sizeof(GbaHeader));

    gSlot2Active = (gRomHeader.gameCode != 0xFFFFFFFF);
    if (gSlot2Active)
    {
        gSlot2RomSize = detectSlot2RomSize();
    }

    return gSlot2Active;
}
