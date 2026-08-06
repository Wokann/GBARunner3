#include "common.h"
#include <string.h>
#include "SaveSlot2.h"
#include "Save.h"
#include "Core/Environment.h"
#include <libtwl/mem/memExtern.h>
#include "VirtualMachine/VMNestedIrq.h"
#include "cp15.h"

// Slot2 debug log, off by default. Set to 1 to record slot2 operations to
// /_gba/slot2_debug.log (useful when debugging save hangs).
#define SLOT2_LOG_ENABLED 0

bool g_useSlot2Save = true;
SaveType g_slot2SaveType = SAVE_TYPE_NONE;
bool g_slot2SyncFLASHEraseFinish;
[[gnu::section(".ewram.bss")]] static u8 sSlot2CurrentBank = 0xFF;

void slot2InitializeSave(const SaveTypeInfo* saveTypeInfo, u32 saveSize)
{
    if (!g_useSlot2Save)
        return;
    if (!saveTypeInfo)
    {
        // No save type detected: fall back to the original file logic.
        g_useSlot2Save = false;
        return;
    }
    g_slot2SaveType = saveTypeInfo->type;
    if (g_slot2SaveType & SAVE_TYPE_SRAM)
    {
        mem_setGbaCartridgeCpu(EXMEMCNT_SLOT2_CPU_ARM9);
        const vu8* slot2Sram = (const vu8*)0x0A000000;
        for (u32 i = 0; i < saveSize && i < SAVE_DATA_SIZE; i++)
            gSaveData[i] = slot2Sram[i];
    }
    else if (g_slot2SaveType & SAVE_TYPE_FLASH)
    {
        mem_setGbaCartridgeCpu(EXMEMCNT_SLOT2_CPU_ARM9);
        mem_setGbaCartridgeRamWait(EXMEMCNT_SLOT2_RAM_WAIT_18);
    }
    else if (g_slot2SaveType & SAVE_TYPE_EEPROM)
    {
        // EEPROM lives at 0x0DFFFF00 (DS: 0x09FFFF00, ROM window tail).
        mem_setGbaCartridgeCpu(EXMEMCNT_SLOT2_CPU_ARM9);
    }
    else
    {
        g_useSlot2Save = false;
    }
}

// SRAM slot2 access: read/write directly on the cartridge (0x0A) when
// g_useSlot2Save is set, keeping the gSaveData mirror in sync on writes.
bool slot2SramRead(const u8* src, u8* dst, u32 size)
{
    if (!g_useSlot2Save)
        return false;
    u32 offset = (u32)src & 0xFFFF;
    const vu8* sram = (const vu8*)(0x0A000000 + offset);
    for (u32 i = 0; i < size; i++)
        *dst++ = sram[i];
    return true;
}

bool slot2SramWrite(const u8* src, u8* dst, u32 size)
{
    if (!g_useSlot2Save)
        return false;
    u32 offset = (u32)dst & 0xFFFF;
    vu8* sram = (vu8*)(0x0A000000 + offset);
    for (u32 i = 0; i < size; i++)
    {
        sram[i] = src[i];
        // Keep the gSaveData mirror in sync so the vblank sync won't
        // overwrite the cartridge with stale data.
        gSaveData[offset + i] = src[i];
    }
    return true;
}

bool slot2SramVerify(const u8* src, const u8* tgt, u32 size)
{
    if (!g_useSlot2Save)
        return false;
    u32 offset = (u32)tgt & 0xFFFF;
    const vu8* sram = (const vu8*)(0x0A000000 + offset);
    for (u32 i = 0; i < size; ++i)
    {
        if (sram[i] != *src++)
            return false;
    }
    return true;
}

// Upper-layer slot2 save sync, called by the vblank save check before the
// file write. SRAM is the only type that goes through the gSaveData buffer,
// so its cartridge write lives here; flash/EEPROM slot2 writes are handled in
// their own driver functions.
extern "C" void sav_syncSlot2Save(void)
{
    if (gGbaSaveShared.saveDataSize == 0 || Environment::IsIsNitroEmulator())
        return;
    if (g_slot2SaveType & SAVE_TYPE_SRAM)
    {
        mem_setGbaCartridgeCpu(EXMEMCNT_SLOT2_CPU_ARM9);
        vu8* slot2Sram = (vu8*)0x0A000000;
        for (u32 i = 0; i < gGbaSaveShared.saveDataSize; i++)
            slot2Sram[i] = gSaveData[i];
    }
}

// Slot2 flash helpers. Command bytes are written back-to-back (matching
// Nintendo's ctrdg and the GBA flash SDK); polling loops have timeouts to
// avoid hangs. swiDelay is not usable here because SWIs are intercepted by
// the GBA VM, so a small delay loop is used for polling.

[[gnu::section(".ewram")]] static void slot2Delay(void)
{
    // Short poll interval (~2.5us). Command bytes themselves need no delay:
    // Nintendo's ctrdg (Platinum SDK) writes them back-to-back with only the
    // EXMEMCNT bus timing, exactly like the GBA flash SDK on real hardware.
    for (volatile u32 i = 0; i < 50; ++i);
}

[[gnu::section(".ewram.bss")]] static FIL sSlot2LogFile;
[[gnu::section(".ewram.bss")]] static bool sSlot2LogOpen;
[[gnu::section(".ewram.bss")]] static char sSlot2LogBuf[256];
[[gnu::section(".ewram.bss")]] static u32 sSlot2LogLen;

// Debug log written straight to the SD card (/_gba/slot2_debug.log) and synced
// in batches. Every f_sync stalls the ARM7 SD service long enough to starve
// the sound thread (audible pops), so entries are buffered in EWRAM and
// flushed at operation boundaries. The normal gLogger is a NullLogger on real
// hardware and would not persist anything.
extern "C" __attribute__((section(".ewram"))) void slot2LogFlush(void)
{
#if SLOT2_LOG_ENABLED
    if (sSlot2LogLen == 0)
        return;
    // Do the FAT/SD work with interrupts enabled, so the game keeps running
    // while the log is flushed to the SD card.
    vm_enableNestedIrqs();
    if (!sSlot2LogOpen)
    {
        if (f_open(&sSlot2LogFile, "/_gba/slot2_debug.log", FA_OPEN_APPEND | FA_WRITE) != FR_OK)
        {
            sSlot2LogLen = 0;
            vm_disableNestedIrqs();
            return;
        }
        sSlot2LogOpen = true;
    }
    UINT bw;
    if (f_write(&sSlot2LogFile, sSlot2LogBuf, sSlot2LogLen, &bw) != FR_OK)
    {
        sSlot2LogLen = 0;
        vm_disableNestedIrqs();
        return;
    }
    f_sync(&sSlot2LogFile);
    sSlot2LogLen = 0;
    vm_disableNestedIrqs();
#else
    (void)0;
#endif
}

extern "C" __attribute__((section(".ewram"))) void slot2Log(const char* msg, u32 v0, u32 v1)
{
#if SLOT2_LOG_ENABLED
    // Hand-rolled formatting to avoid pulling in extra library code.
    char buf[64];
    char* p = buf;
    while (*msg && p < buf + sizeof(buf) - 19)
        *p++ = *msg++;
    *p++ = ' ';
    for (int i = 7; i >= 0; i--)
    {
        u32 nib = (v0 >> (i * 4)) & 0xF;
        *p++ = nib < 10 ? '0' + nib : 'A' + nib - 10;
    }
    *p++ = ' ';
    for (int i = 7; i >= 0; i--)
    {
        u32 nib = (v1 >> (i * 4)) & 0xF;
        *p++ = nib < 10 ? '0' + nib : 'A' + nib - 10;
    }
    *p++ = '\n';
    int len = (int)(p - buf);
    if (sSlot2LogLen + (u32)len >= sizeof(sSlot2LogBuf))
        slot2LogFlush();
    memcpy(&sSlot2LogBuf[sSlot2LogLen], buf, (u32)len);
    sSlot2LogLen += (u32)len;
#else
    (void)msg; (void)v0; (void)v1;
#endif
}

extern "C" __attribute__((section(".ewram"))) void slot2FlashSetBank(u8 bank)
{
    if (bank == sSlot2CurrentBank)
        return;
    *(vu8*)0x0A005555 = 0xAA;
    *(vu8*)0x0A002AAA = 0x55;
    *(vu8*)0x0A005555 = 0xB0;
    *(vu8*)0x0A000000 = bank;
    sSlot2CurrentBank = bank;
}

extern "C" __attribute__((section(".ewram"))) u32 slot2FlashEraseChip(void)
{
    *(vu8*)0x0A005555 = 0xAA;
    *(vu8*)0x0A002AAA = 0x55;
    *(vu8*)0x0A005555 = 0x80;
    *(vu8*)0x0A005555 = 0xAA;
    *(vu8*)0x0A002AAA = 0x55;
    *(vu8*)0x0A005555 = 0x10;
    // Erase chip can take up to ~2s. Interrupts stay masked during the wait:
    // toggling them around slot2 accesses causes audible pops.
    u32 timeout = 800000;
    while (*(vu8*)0x0A000000 != 0xFF && timeout > 0) { --timeout; slot2Delay(); }
    if (*(vu8*)0x0A000000 != 0xFF)
    {
        // Reset the chip back to read mode, like the official SDK does on timeout.
        *(vu8*)0x0A005555 = 0xF0; slot2Delay();
        return 0x8000;
    }
    return 0;
}

extern "C" __attribute__((section(".ewram"))) u32 slot2FlashEraseSector(u32 sectorAddr)
{
    bool needBank1 = sectorAddr >= 0x10000;
    slot2FlashSetBank(needBank1);
    u32 bankAddr = sectorAddr & 0xFFFF;
    vu8* sector = (vu8*)(0x0A000000 + bankAddr);
    *(vu8*)0x0A005555 = 0xAA;
    *(vu8*)0x0A002AAA = 0x55;
    *(vu8*)0x0A005555 = 0x80;
    *(vu8*)0x0A005555 = 0xAA;
    *(vu8*)0x0A002AAA = 0x55;
    *sector = 0x30;
    // Sector erase typically 0.5-1s. Interrupts stay masked during the wait.
    u32 timeout = 400000;
    while (*sector != 0xFF && timeout > 0) { --timeout; slot2Delay(); }
    if (*sector != 0xFF)
    {
        // Reset the chip back to read mode, like the official SDK does on timeout.
        *(vu8*)0x0A005555 = 0xF0; slot2Delay();
        return 0x8000;
    }
    return 0;
}

extern "C" __attribute__((section(".ewram"))) void slot2FlashReadByte(u32 saveAddress, u8* buffer)
{
    bool needBank1 = saveAddress >= 0x10000;
    slot2FlashSetBank(needBank1);
    u32 addrInBank = saveAddress & 0xFFFF;
    *buffer = *(vu8*)(0x0A000000 + addrInBank);
}

extern "C" __attribute__((section(".ewram"))) u32 slot2FlashProgramByte(u32 saveAddress, u8 data)
{
    bool needBank1 = (saveAddress >= 0x10000);
    slot2FlashSetBank(needBank1);
    u32 addrInBank = saveAddress & 0xFFFF;
    vu8* ptr = (vu8*)(0x0A000000 + addrInBank);
    *(vu8*)0x0A005555 = 0xAA;
    *(vu8*)0x0A002AAA = 0x55;
    *(vu8*)0x0A005555 = 0xA0;
    *ptr = data;
    // Byte programming is ~10us. Interrupts stay masked (no per-byte toggling).
    u32 timeout = 20000;
    while (*ptr != data && timeout > 0) { --timeout; slot2Delay(); }
    if (*ptr != data)
    {
        // Reset the chip back to read mode, like the official SDK does on timeout.
        *(vu8*)0x0A005555 = 0xF0; slot2Delay();
        return 0x8000;
    }
    return 0;
}

// EEPROM serial port on the DS: the GBA's 0x0DFFFF00 maps to 0x09FFFF00.
// The bitstream is transferred with the real DS DMA3, like GodMode9i does -
// GBARunner3's GBA DMA emulation uses CPU copies and does not touch the real
// DMA3 hardware. Each 16-bit DMA unit transfers one bit (bit0).
#define SLOT2_EEPROM_ADDR ((vu16*)0x09FFFF00)
#define SLOT2_DMA3SAD (*(vu32*)0x040000D4)
#define SLOT2_DMA3DAD (*(vu32*)0x040000D8)
#define SLOT2_DMA3CNT (*(vu32*)0x040000DC)

[[gnu::section(".ewram.bss")]] static u16 sEepromPacket[81];

static void slot2EepromSendPacket(const u16* packet, u32 size)
{
    // EWRAM is write-back cached: flush so DMA3 reads the actual command bits.
    dc_flushRange(packet, size * sizeof(u16));
    SLOT2_DMA3SAD = (u32)packet;
    SLOT2_DMA3DAD = 0x09FFFF00;
    SLOT2_DMA3CNT = 0x80000000u | size;
    while (SLOT2_DMA3CNT & 0x80000000u);
}

static void slot2EepromReceivePacket(u16* packet, u32 size)
{
    SLOT2_DMA3SAD = 0x09FFFF00;
    SLOT2_DMA3DAD = (u32)packet;
    SLOT2_DMA3CNT = 0x80000000u | size;
    while (SLOT2_DMA3CNT & 0x80000000u);
    // Invalidate so the CPU reads the data DMA3 wrote, not stale cache.
    dc_invalidateRange(packet, size * sizeof(u16));
}

extern "C" __attribute__((section(".ewram"))) void slot2EepromRead8Bytes(u8* out, u16 addr, bool shortAddr)
{
    u16* packet = sEepromPacket;
    memset(packet, 0, 68 * sizeof(u16));

    slot2Log("eeprom read", addr, shortAddr);

    // Read request: start bit + read command + address (MSB first).
    packet[0] = 1;
    packet[1] = 1;
    for (int i = 2, shift = shortAddr ? 5 : 13; i < (shortAddr ? 8 : 16); i++, shift--)
        packet[i] = (addr >> shift) & 1;
    packet[shortAddr ? 8 : 16] = 0;

    slot2EepromSendPacket(packet, shortAddr ? 9 : 17);
    slot2EepromReceivePacket(packet, 68);

    // Extract 64 data bits (4 dummy bits first), MSB byte first.
    const u16* inPos = &packet[4];
    for (int byte = 7; byte >= 0; --byte)
    {
        u8 outByte = 0;
        for (int bit = 7; bit >= 0; --bit)
            outByte |= ((*inPos++) & 1) << bit;
        *out++ = outByte;
    }
    slot2Log("eeprom read data",
        ((u32)out[-8] << 24) | ((u32)out[-7] << 16) | ((u32)out[-6] << 8) | out[-5],
        ((u32)out[-4] << 24) | ((u32)out[-3] << 16) | ((u32)out[-2] << 8) | out[-1]);
    slot2LogFlush();
}

extern "C" __attribute__((section(".ewram"))) void slot2EepromWrite8Bytes(const u8* in, u16 addr, bool shortAddr)
{
    u32 packetLen = shortAddr ? 73 : 81;
    u16* packet = sEepromPacket;
    memset(packet, 0, packetLen * sizeof(u16));

    // Write request: start bit + write command + address (MSB first) + data.
    packet[0] = 1;
    packet[1] = 0;
    slot2Log("eeprom write", addr, shortAddr);
    for (int i = 2, shift = shortAddr ? 5 : 13; i < (shortAddr ? 8 : 16); i++, shift--)
        packet[i] = (addr >> shift) & 1;

    u16* outPos = &packet[shortAddr ? 8 : 16];
    for (int byte = 7; byte >= 0; --byte)
    {
        u8 inByte = *in++;
        for (int bit = 7; bit >= 0; --bit)
            *outPos++ = (inByte >> bit) & 1;
    }
    packet[packetLen - 1] = 0;

    slot2EepromSendPacket(packet, packetLen);

    // Wait for the EEPROM to finish the write (bit0 set), with a timeout.
    // Chip write is ~10ms; 20000 * ~2.5us = 50ms cap is generous.
    u32 timeout = 20000;
    while ((*SLOT2_EEPROM_ADDR & 1) == 0 && timeout > 0) { --timeout; slot2Delay(); }
    slot2Log("eeprom write wait", timeout, (*SLOT2_EEPROM_ADDR & 1));
    slot2LogFlush();
}
