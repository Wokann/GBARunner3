#pragma once
#include "Fat/ff.h"
#include "SaveTypeInfo.h"
#include "GbaSaveShared.h"

#define SAVE_DATA_FILL              0xFF
#define SAVE_DATA_SIZE              (32 * 1024)
#define ISNITRO_SAVE_BUFFER         ((vu8*)0x02480000)
#define ISNITRO_SAVE_BUFFER_SIZE    (128 * 1024)

extern u8 gSaveData[SAVE_DATA_SIZE];
extern FIL gSaveFile;
extern gba_save_shared_t gGbaSaveShared;

extern u32 emu_vblankIrqSkipSaveCheckInstruction;

extern bool g_useSlot2Save;
enum Slot2WriteType {
    SLOT2_WRITE_NONE,
    SLOT2_WRITE_ERASE_FLASH_CHIP, 
    SLOT2_WRITE_ERASE_FLASH_SECTOR, 
    SLOT2_WRITE_PROGRAM_FLASH_SECTOR, 
    SLOT2_WRITE_PROGRAM_FLASH_BYTE,
    SLOT2_WRITE_PROGRAM_EEPROM_UNIT
};

extern SaveType g_slot2SaveType;
extern bool g_slot2SyncFLASHEraseFinish;

bool sav_tryPatchFunction(const u32* signature, u32 saveSwiNumber, void* patchFunction);
void sav_initializeSave(const SaveTypeInfo* saveTypeInfo, const char* savePath);

#ifdef __cplusplus
extern "C" {
#endif

u8 sav_readSaveByteFromFile(u32 offset);
void sav_writeSaveByteToFile(u32 offset, u8 data);
void sav_flushSaveFile(void);
void sav_writeSaveToFile(void);

void slot2FlashSetBank(u8 bank);
void slot2FlashEraseChip(void);
void slot2FlashEraseSector(u32 sectorAddr);
void slot2FlashReadByte(u32 saveAddress, u8* buffer);
void slot2FlashProgramByte(u32 saveAddress, u8 data);

#ifdef __cplusplus
}
#endif
