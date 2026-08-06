#pragma once
#include "SaveTypeInfo.h"

// Slot2 GBA cartridge save support, kept independent from the original save
// code. When g_useSlot2Save is true the save is read/written directly on the
// GBA cartridge through these functions; when false the original SD card file
// logic is used instead.

extern bool g_useSlot2Save;
extern SaveType g_slot2SaveType;
extern bool g_slot2SyncFLASHEraseFinish;

enum Slot2WriteType {
    SLOT2_WRITE_NONE,
    SLOT2_WRITE_ERASE_FLASH_CHIP,
    SLOT2_WRITE_ERASE_FLASH_SECTOR,
    SLOT2_WRITE_PROGRAM_FLASH_SECTOR,
    SLOT2_WRITE_PROGRAM_FLASH_BYTE,
    SLOT2_WRITE_PROGRAM_EEPROM_UNIT
};

#ifdef __cplusplus
extern "C" {
#endif

void slot2FlashSetBank(u8 bank);
u32 slot2FlashEraseChip(void);
u32 slot2FlashEraseSector(u32 sectorAddr);
void slot2FlashReadByte(u32 saveAddress, u8* buffer);
u32 slot2FlashProgramByte(u32 saveAddress, u8 data);

// EEPROM serial access on the DS (0x09FFFF00). The protocol is the same as
// the GBA SDK / GodMode9i, but bits are transferred with plain CPU accesses
// instead of DMA3 (DMA3 is used by the GBA emulator).
void slot2EepromRead8Bytes(u8* out, u16 addr, bool shortAddr);
void slot2EepromWrite8Bytes(const u8* in, u16 addr, bool shortAddr);

// Upper-layer integration points (called from the original save code).
void slot2InitializeSave(const SaveTypeInfo* saveTypeInfo, u32 saveSize);
void sav_syncSlot2Save(void);
void sav_performSaveWrite(void);

// SRAM slot2 access. Return true when g_useSlot2Save handled the operation,
// false to fall back to the original logic. src/dst use AGB addresses
// (0x0E000000 + offset), matching the SRAM SDK.
bool slot2SramRead(const u8* src, u8* dst, u32 size);
bool slot2SramWrite(const u8* src, u8* dst, u32 size);
bool slot2SramVerify(const u8* src, const u8* tgt, u32 size);

void slot2Log(const char* msg, u32 v0, u32 v1);
void slot2LogFlush(void);

#ifdef __cplusplus
}
#endif
