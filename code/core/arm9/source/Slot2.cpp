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

// Log format strings land in .rodata (vrama), like the rest of the codebase.
static const char kProbeFmt[] = "slot2probe %uK %08X m%u\n";
static const char kProbeSizeFmt[] = "slot2probe size=%u h=%02X\n";

// Boundary probe signature bits (logged with kProbeFmt).
#define SLOT2_MATCH_ECHO (1u << 0) // each u16 equals its halfword index (GodMode9i)
#define SLOT2_MATCH_SENT (1u << 1) // fixed 0xFFFE0000 sentinel (older GodMode9i)
#define SLOT2_MATCH_MIR  (1u << 2) // start of the ROM mirrored at the boundary

/// @brief Detects the physical cart size by scanning power-of-two boundaries.
///        A GBA cart is a power of two between 512KB and 32MB. Reading past
///        the end of a cart returns one of several bus signatures depending
///        on the cart and DS revision. GodMode9i (arm9/source/driveMenu.cpp)
///        checks whether each u16 equals its own halfword index over an 8KB
///        region at each boundary; older GodMode9i dumps looked for the fixed
///        0xFFFE0000 sentinel. Classic GBA hardware mirrors by cart size, so
///        the start of the ROM also repeats at the exact size boundary. Note
///        that an 8KB run of 0xFFFF is NOT used as a signature: real retail
///        ROMs (e.g. Pokemon Sapphire) contain large 0xFF padding regions at
///        power-of-two offsets, which would false-positive. The first boundary
///        with any signature is the cart size; if none matches, fall back to
///        32MB (the whole GBA window).
[[gnu::section(".ewram")]]
static u32 detectSlot2RomSize()
{
    const u32 base = 0x08000000u;

    // Start-of-ROM reference for the mirror check.
    u32 firstWords[4];
    for (u32 i = 0; i < 4; i++)
        firstWords[i] = *(const volatile u32*)(base + i * 4);

    u32 size = 512 * 1024;
    while (size <= 16 * 1024 * 1024)
    {
        const u32 addr = base + size;
        const volatile u16* p16 = (const volatile u16*)addr;
        u32 match = 0;

        // Address echo: the whole 8KB region must equal its halfword index.
        bool echo = true;
        for (u32 j = 0; j < 0x1000; j++)
        {
            if (p16[j] != (u16)j)
            {
                echo = false;
                break;
            }
        }
        if (echo)
            match |= SLOT2_MATCH_ECHO;

        // Fixed sentinel used by the original GodMode9i GBA dump code.
        if (*(const volatile u32*)addr == 0xFFFE0000u)
            match |= SLOT2_MATCH_SENT;

        // Classic mirror: the cart repeats from its start at the boundary.
        bool mirror = true;
        for (u32 i = 0; i < 4; i++)
        {
            if (*(const volatile u32*)(addr + i * 4) != firstWords[i])
            {
                mirror = false;
                break;
            }
        }
        if (mirror)
            match |= SLOT2_MATCH_MIR;

        gLogger->Log(LogLevel::Debug, kProbeFmt, size / 1024, *(const volatile u32*)addr, match);

        if (match != 0)
            return size;

        size <<= 1;
    }

    return 32 * 1024 * 1024;
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
        gLogger->Log(LogLevel::Debug, kProbeSizeFmt, gSlot2RomSize, ((const u8*)0x08000000)[0x80]);
    }

    return gSlot2Active;
}
