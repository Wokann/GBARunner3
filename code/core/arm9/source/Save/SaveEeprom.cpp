#include "common.h"
#include <string.h>
#include "Save.h"
#include "SaveSlot2.h"
#include "SaveSwi.h"
#include "MemFastSearch.h"
#include "MemoryEmulator/RomDefs.h"
#include "cp15.h"
#include "SaveTypeInfo.h"
#include "SaveEeprom.h"

//todo: Moero!! Jaleco Collection (Japan) reports EEPROM_V124, but the signatures below don't work!

static const u32 sReadEepromDwordV111Sig[] = { 0xB0AAB5B0u, 0x6079466Fu, 0x80081C39u, 0x88011C38u };
static const u32 sReadEepromDwordV120Sig[] = { 0xB0A2B570u, 0x04001C0Du, 0x48030C03u, 0x88806800u };
static const u32 sProgramEepromDwordV111Sig[] = { 0xB0AAB580u, 0x6079466Fu, 0x80081C39u, 0x88011C38u };

//changed in EEPROM_V124
static const u32 sProgramEepromDwordV120Sig[] = { 0xB0A9B530u, 0x04001C0Du, 0x48030C04u, 0x88806800u };

//changed in EEPROM_V126
static const u32 sProgramEepromDwordV124Sig[] = { 0xB0ACB5F0u, 0x04001C0Du, 0x06120C01u, 0x48030E17u };

static const u32 sProgramEepromDwordV126Sig[] = { 0x4647B5F0u, 0xB0ACB480u, 0x04001C0Eu, 0x06120C05u };

// EEPROMConfigure signature (identical across V122/V124 and games):
//   push {lr}; lsl r0,r0,#0x10; lsr r0,r0,#0x10; mov r2,#0; cmp r0,#4; ...
static const u32 sEepromConfigureSig[] = { 0x0400B500u, 0x22000C00u, 0xD1072804u, 0x48024901u };

static bool sEepromShortAddr;
static u32 sEepromConfigVarAddr;   // gEEPROMConfig variable (EWRAM/IWRAM)
static u32 sEepromConfig512Addr;   // config512 struct (ROM)
static u32 sEepromConfig8kAddr;    // config8k struct (ROM)
static bool sEepromConfigAddrsParsed;

// Parse gEEPROMConfig/config512/config8k addresses from EEPROMConfigure's
// literal pool. Must run at patch time (ROM still clean); at read time the
// JIT may have rewritten the code and the signature no longer matches.
static void parseEepromConfigAddrs()
{
    if (sEepromConfigAddrsParsed)
        return;
    u32* cfgFunc = (u32*)mem_fastSearch16((const u32*)ROM_LINEAR_DS_ADDRESS, ROM_LINEAR_SIZE, sEepromConfigureSig);
    if (!cfgFunc)
        return;
    const u16* code = (const u16*)cfgFunc;
    for (int i = 0; i < 40; i++)
    {
        u16 inst = code[i];
        if ((inst & 0xF800) == 0x4800)
        {
            u32 base = (((u32)&code[i] + 4) & ~3);
            u32 poolAddr = base + ((inst & 0xFF) << 2);
            u32 value = *(u32*)poolAddr;
            if (value >= 0x02000000 && value < 0x04000000 && !sEepromConfigVarAddr)
                sEepromConfigVarAddr = value;
            else if (value >= 0x08000000 && value < 0x0A000000)
            {
                if (!sEepromConfig512Addr)
                    sEepromConfig512Addr = value;
                else if (value != sEepromConfig512Addr)
                    sEepromConfig8kAddr = value;
            }
        }
    }
    sEepromConfigAddrsParsed = true;
    slot2Log("eeprom cfgaddrs", sEepromConfigVarAddr, sEepromConfig512Addr);
    slot2Log("eeprom cfg8k", sEepromConfig8kAddr, 0);
}

static u16 eepromConfigure(u16 type)
{
    slot2Log("eeprom configure", type, 0);
    u16 ret = 0;
    u32 configAddr = sEepromConfig512Addr;
    if (type == 4)
    {
        sEepromShortAddr = true; // 6-bit
    }
    else if (type == 0x40)
    {
        sEepromShortAddr = false; // 14-bit
        configAddr = sEepromConfig8kAddr;
    }
    else
    {
        sEepromShortAddr = true; // invalid arg defaults to 512B, like the SDK
        ret = 1;                 // SDK returns 1 on invalid argument
    }
    slot2Log("eeprom cfgset", sEepromShortAddr ? 6 : 14, ret);
    // Mimic the SDK: store the selected config pointer so the game's own
    // gEEPROMConfig stays valid (otherwise patching configure breaks it).
    if (sEepromConfigVarAddr && configAddr)
    {
        *(u32*)sEepromConfigVarAddr = configAddr;
        dc_drainWriteBuffer();
    }
    return ret;
}

static bool eepromUseShortAddr(u16 epAdr)
{
    // Width comes straight from EEPROMConfigure's argument (4 -> 6-bit,
    // 0x40 -> 14-bit). V111 (512B only) defaults to 6-bit.
    slot2Log("eeprom useshort", sEepromShortAddr ? 6 : 14, 0);
    return sEepromShortAddr;
}
static u16 readEepromDword(u16 epAdr, u16* dst)
{
    if (g_useSlot2Save)
    {
        // Slot2 mode: read 8 bytes from the cartridge EEPROM (0x09FFFF00).
        bool shortAddr = eepromUseShortAddr(epAdr);
        slot2Log("eeprom cfg", sEepromShortAddr ? 6 : 14, 0);
        u8 buf[8];
        slot2EepromRead8Bytes(buf, epAdr, shortAddr);
        for (int i = 0; i < 8; ++i)
            ((u8*)dst)[7 - i] = buf[i];
        slot2Log("eeprom read dword", epAdr, ((u8*)dst)[0] | (((u8*)dst)[1] << 8) | (((u8*)dst)[2] << 16) | (((u8*)dst)[3] << 24));
        return 0;
    }
    for (int i = 0; i < 8; ++i)
    {
        ((u8*)dst)[7 - i] = sav_readSaveByteFromFileFromUserMode((epAdr << 3) + i);
    }
    return 0;
}

static u16 programEepromDword(u16 epAdr, const u16* src)
{
    if (g_useSlot2Save)
    {
        // Slot2 mode: write 8 bytes to the cartridge EEPROM.
        bool shortAddr = eepromUseShortAddr(epAdr);
        u8 buf[8];
        for (int i = 0; i < 8; ++i)
            buf[i] = ((u8*)src)[7 - i];
        slot2Log("eeprom write data", epAdr,
            ((u32)buf[0] << 24) | ((u32)buf[1] << 16) | ((u32)buf[2] << 8) | buf[3]);
        slot2EepromWrite8Bytes(buf, epAdr, shortAddr);
        // Read back right away to verify the write actually landed.
        u8 readback[8];
        slot2EepromRead8Bytes(readback, epAdr, shortAddr);
        bool match = memcmp(buf, readback, 8) == 0;
        slot2Log("eeprom write verify", epAdr, match);
        return 0;
    }
    for (int i = 0; i < 8; ++i)
    {
        sav_writeSaveByteToFileFromUserMode((epAdr << 3) + i, ((u8*)src)[7 - i]);
    }
    sav_flushSaveFileFromUserMode();
    return 0;
}

bool eeprom_patchV111(const SaveTypeInfo* saveTypeInfo, FIL* romFile, u32 tagRomAddress, u8* tempBuffer)
{
    sEepromShortAddr = true; // V111 is 512B only
    sEepromConfigAddrsParsed = false;
    sEepromConfigVarAddr = 0;
    sEepromConfig512Addr = 0;
    sEepromConfig8kAddr = 0;
    parseEepromConfigAddrs();
    sav_tryPatchFunction(sEepromConfigureSig, 2, (void*)eepromConfigure); // may not exist in V111
    return sav_tryPatchFunction(sReadEepromDwordV111Sig, 0, (void*)readEepromDword)
        && sav_tryPatchFunction(sProgramEepromDwordV111Sig, 1, (void*)programEepromDword);
}

bool eeprom_patchV120(const SaveTypeInfo* saveTypeInfo, FIL* romFile, u32 tagRomAddress, u8* tempBuffer)
{
    sEepromShortAddr = false; // default 8KB; EEPROMConfigure overrides
    sEepromConfigAddrsParsed = false;
    sEepromConfigVarAddr = 0;
    sEepromConfig512Addr = 0;
    sEepromConfig8kAddr = 0;
    parseEepromConfigAddrs();
    sav_tryPatchFunction(sEepromConfigureSig, 2, (void*)eepromConfigure);
    return sav_tryPatchFunction(sReadEepromDwordV120Sig, 0, (void*)readEepromDword)
        && sav_tryPatchFunction(sProgramEepromDwordV120Sig, 1, (void*)programEepromDword);
}

bool eeprom_patchV124(const SaveTypeInfo* saveTypeInfo, FIL* romFile, u32 tagRomAddress, u8* tempBuffer)
{
    sEepromShortAddr = false; // default 8KB; EEPROMConfigure overrides
    sEepromConfigAddrsParsed = false;
    sEepromConfigVarAddr = 0;
    sEepromConfig512Addr = 0;
    sEepromConfig8kAddr = 0;
    parseEepromConfigAddrs();
    sav_tryPatchFunction(sEepromConfigureSig, 2, (void*)eepromConfigure);
    return sav_tryPatchFunction(sReadEepromDwordV120Sig, 0, (void*)readEepromDword)
        && sav_tryPatchFunction(sProgramEepromDwordV124Sig, 1, (void*)programEepromDword);
}

bool eeprom_patchV126(const SaveTypeInfo* saveTypeInfo, FIL* romFile, u32 tagRomAddress, u8* tempBuffer)
{
    sEepromShortAddr = false; // default 8KB; EEPROMConfigure overrides
    sEepromConfigAddrsParsed = false;
    sEepromConfigVarAddr = 0;
    sEepromConfig512Addr = 0;
    sEepromConfig8kAddr = 0;
    parseEepromConfigAddrs();
    sav_tryPatchFunction(sEepromConfigureSig, 2, (void*)eepromConfigure);
    return sav_tryPatchFunction(sReadEepromDwordV120Sig, 0, (void*)readEepromDword)
        && sav_tryPatchFunction(sProgramEepromDwordV126Sig, 1, (void*)programEepromDword);
}
