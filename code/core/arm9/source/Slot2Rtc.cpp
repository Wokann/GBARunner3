#include "common.h"
#include "cp15.h"
#include "Slot2Rtc.h"
#include "Save/SaveSlot2.h"
#include "SdCache/SdCache.h"
#include <libtwl/mem/memExtern.h>

// The GBA cart's GPIO/RTC registers (SII S-3511A) live at 0x080000C4-C8.
// On the DS these are the same bus addresses the fork already uses for slot2
// ROM/save access. The emulated GBA writes are passed straight through to the
// physical cartridge, and the register state is mirrored into the ROM cache
// block (the one that serves GBA loads) so reads work without a load hook.
#define SLOT2_GPIO_BASE 0x080000C4u
// Keeps bits 0-24 of the address (same as the memu load/store filters).
#define RIO_ADDRESS_MASK 0xFE000000u

enum class Slot2RtcMode
{
    CartPassthrough,
    DsTimeFallback, // TODO(point 2): implemented later
};

[[gnu::section(".ewram.bss")]] static bool sSlot2RtcBusConfigured;
[[gnu::section(".ewram.bss")]] static Slot2RtcMode sRtcMode = Slot2RtcMode::CartPassthrough;
[[gnu::section(".ewram.bss")]] static u8* sRtcOverlayBlock;

static void slot2RtcCheckCartHealth();
static void slot2RtcStoreDsFallback(u32 offset, u16 value);

static void slot2RtcConfigureBus()
{
    if (sSlot2RtcBusConfigured)
        return;

    // Match the bus setup used by SaveSlot2: the ARM9 must own the GBA slot
    // and the ROM wait states must be relaxed enough for cart chips to
    // respond. The SRAM wait-state bits are intentionally left untouched,
    // since the save code may have configured them for the cart's save chip.
    mem_setGbaCartridgeRomWaits(EXMEMCNT_SLOT2_ROM_WAIT1_10, EXMEMCNT_SLOT2_ROM_WAIT2_6);
    mem_setGbaCartridgePhi(EXMEMCNT_SLOT2_PHI_LOW);
    mem_setGbaCartridgeCpu(EXMEMCNT_SLOT2_CPU_ARM9);
    sSlot2RtcBusConfigured = true;

    slot2RtcCheckCartHealth();
}

static void slot2RtcEnsureOverlay()
{
    if (sRtcOverlayBlock)
        return;
    // Permanently pin the ROM block that contains 0x080000C4, so the register
    // mirror below can never be evicted mid-protocol. Returns the pointer to
    // the GPIO registers inside the block.
    sRtcOverlayBlock = (u8*)sdc_loadRomBlockForPatching(SLOT2_GPIO_BASE);
}

// ---------------------------------------------------------------------------
// DS-time fallback (point 2, placeholder)
//
// Future work: bit-bang the cart's S-3511 status register once over the slot2
// bus (same 3-wire protocol as rtcread-ds) and switch sRtcMode to
// DsTimeFallback when the POWER flag is set (dead battery) or the chip does
// not answer. The upstream GBARunner3 feature/rom-gpio branch already contains
// a complete virtual S-3511 implementation backed by the DS host clock
// (RomGpio + RomGpioRtc on ARM9 + RtcIpcService on ARM7) that can be plugged
// into the DsTimeFallback branches below.
static void slot2RtcCheckCartHealth()
{
    // TODO(point 2): probe the cart RTC and set sRtcMode when it is dead.
}

static void slot2RtcStoreDsFallback(u32 offset, u16 value)
{
    // TODO(point 2): feed the virtual S-3511 state machine with the
    // SCK/SIO/CS pin changes instead of touching the cartridge bus.
    (void)offset;
    (void)value;
}

extern "C" [[gnu::section(".ewram")]] void slot2RtcStore16(u32 address, u16 value)
{
    if (!g_useSlot2Save)
        return; // No matching cart in slot2: keep the original behavior.

    u32 offset = (address & ~RIO_ADDRESS_MASK) - 0xC4;
    if (offset > 4 || (offset & 1))
        return; // Not a GPIO register (0xC4/0xC6/0xC8).

    if (sRtcMode == Slot2RtcMode::DsTimeFallback)
    {
        slot2RtcStoreDsFallback(offset, value);
        return;
    }

    slot2RtcConfigureBus();
    slot2RtcEnsureOverlay();

    volatile u16* reg = (volatile u16*)(SLOT2_GPIO_BASE + offset);
    *reg = value;
    // Make sure the SCK/CS/SIO level reaches the cartridge before the game
    // samples the bus again, so the bit-banged protocol stays intact.
    dc_drainWriteBuffer();

    // Mirror the register state into the ROM cache so the game's loads from
    // 0x080000C4-C8 see the physical values.
    u16 mirrored = value;
    if (offset == 0)
    {
        // The data register read-back includes the SIO pin driven by the RTC
        // chip, so sample the actual bus state.
        dc_invalidateLine((void*)SLOT2_GPIO_BASE);
        mirrored = *(const volatile u16*)SLOT2_GPIO_BASE;
    }
    *(u16*)(sRtcOverlayBlock + offset) = mirrored;
    dc_drainWriteBuffer();
}
