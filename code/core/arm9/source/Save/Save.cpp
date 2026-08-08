#include "common.h"
#include <libtwl/ipc/ipcFifo.h>
#include <libtwl/ipc/ipcFifoSystem.h>
#include <string.h>
#include "Fat/ff.h"
#include "Core/Environment.h"
#include "MemFastSearch.h"
#include "SaveSwi.h"
#include "SaveTypeInfo.h"
#include "VirtualMachine/VMNestedIrq.h"
#include "MemoryEmulator/RomDefs.h"
#include "SdCache/SdCache.h"
#include "IpcChannels.h"
#include "GbaSaveIpcCommand.h"
#include <nds/bios.h>
#include <nds/memory.h>
#include "Save.h"
#include "SaveSlot2.h"
#include <libtwl/mem/memExtern.h>
#include "mini-printf.h"

#define DEFAULT_SAVE_SIZE   (32 * 1024)

[[gnu::section(".ewram.bss")]]
u8 gSaveData[SAVE_DATA_SIZE] alignas(32);

[[gnu::section(".ewram.bss")]]
FIL gSaveFile alignas(32);

[[gnu::section(".ewram.bss"), gnu::aligned(32)]]
gba_save_shared_t gGbaSaveShared;

// In EWRAM BSS: the default .bss (vrama) is full and has no headroom.
[[gnu::section(".ewram.bss")]]
static DWORD sClusterTable[64];
// In EWRAM BSS: the default .bss (vrama) is full and has no headroom.
[[gnu::section(".ewram.bss")]]
static u32 sSkipSaveCheckInstruction;

// Slot2 GBA cart save support
// temporarily
extern FIL gFile;
extern bool gSlot2Active;
extern u32 gSlot2RomSize;

#ifdef GBAR3_HICODE_CACHE_MAPPING

static u32* searchHiCode(const u32* signature, u32 romStart, u32 romEnd)
{
    // todo: this doesn't work if the function lies on a cache block boundary
    for (u32 i = romStart; i < romEnd; i += SDC_BLOCK_SIZE)
    {
        const void* block = sdc_getRomBlock(i);
        u32* function = (u32*)mem_fastSearch16((const u32*)block, SDC_BLOCK_SIZE, signature);
        if (function)
        {
            return (u32*)sdc_loadRomBlockForPatching(i + (u32)function - (u32)block);
        }
    }

    return nullptr;
}

#endif

[[gnu::section(".ewram")]]
bool sav_tryPatchFunction(const u32* signature, u32 saveSwiNumber, void* patchFunction)
{
    u32* function = (u32*)mem_fastSearch16((const u32*)ROM_LINEAR_DS_ADDRESS, ROM_LINEAR_SIZE, signature);
#ifdef GBAR3_HICODE_CACHE_MAPPING
    if (!function && ROM_LINEAR_GBA_ADDRESS > 0x08000000)
    {
        function = searchHiCode(signature, 0x08000000, ROM_LINEAR_GBA_ADDRESS);
    }
    if (!function)
    {
        u32 romSize = gSlot2Active ? gSlot2RomSize : (u32)f_size(&gFile);
        function = searchHiCode(signature, ROM_LINEAR_END_GBA_ADDRESS, 0x08000000 + romSize);
    }
#endif
    if (!function)
    {
        return false;
    }

    sav_swiTable[saveSwiNumber] = patchFunction;
    *(u16*)function = SAVE_THUMB_SWI(saveSwiNumber);
    return true;
}

static void loadSaveClusterMap(void)
{
    sClusterTable[0] = sizeof(sClusterTable) / sizeof(DWORD);
    gSaveFile.cltbl = sClusterTable;
    f_lseek(&gSaveFile, CREATE_LINKMAP);
}

static void fillSaveFile(u32 start, u32 end)
{
    const u8 saveFill = SAVE_DATA_FILL;
    f_lseek(&gSaveFile, start);
    for (u32 i = start; i < end; ++i)
    {
        UINT written = 0;
        f_write(&gSaveFile, &saveFill, 1, &written);
    }
    f_sync(&gSaveFile);
}

[[gnu::section(".ewram")]]
void sav_initializeSave(const SaveTypeInfo* saveTypeInfo, const char* savePath)
{
    u32 saveSize = saveTypeInfo ? saveTypeInfo->size : DEFAULT_SAVE_SIZE;
    memset(gSaveData, SAVE_DATA_FILL, SAVE_DATA_SIZE);
    if (Environment::IsIsNitroEmulator())
    {
        memset((void*)ISNITRO_SAVE_BUFFER, SAVE_DATA_FILL, ISNITRO_SAVE_BUFFER_SIZE);
    }
    memset(&gSaveFile, 0, sizeof(gSaveFile));
    if (!g_useSlot2Save && savePath && f_open(&gSaveFile, savePath, FA_OPEN_EXISTING | FA_READ | FA_WRITE) == FR_OK)
    {
        bool clusterMapLoaded = false;
        u32 initialSize = f_size(&gSaveFile);
        if (initialSize < saveSize)
        {
            if (f_lseek(&gSaveFile, saveSize) == FR_OK)
            {
                f_rewind(&gSaveFile);
                loadSaveClusterMap();
                clusterMapLoaded = true;
                fillSaveFile(initialSize, saveSize);
            }
        }

        if (!clusterMapLoaded)
        {
            loadSaveClusterMap();
            clusterMapLoaded = true;
        }

        if (saveSize <= SAVE_DATA_SIZE)
        {
            f_rewind(&gSaveFile);
            UINT read = 0;
            f_read(&gSaveFile, gSaveData, saveSize, &read);
        }

        if (Environment::IsIsNitroEmulator())
        {
            f_rewind(&gSaveFile);
            UINT read = 0;
            f_read(&gSaveFile, (void*)ISNITRO_SAVE_BUFFER, saveSize, &read);
        }
    }
    else if (!g_useSlot2Save && savePath && !Environment::IsIsNitroEmulator())
    {
        if (f_open(&gSaveFile, savePath, FA_CREATE_NEW | FA_READ | FA_WRITE) == FR_OK)
        {
            if (f_lseek(&gSaveFile, saveSize) == FR_OK)
            {
                f_rewind(&gSaveFile);
                loadSaveClusterMap();
                fillSaveFile(0, saveSize);
            }
        }
    }

    slot2InitializeSave(saveTypeInfo, saveSize);

    gGbaSaveShared.saveState = GBA_SAVE_STATE_CLEAN;
    sSkipSaveCheckInstruction = emu_vblankIrqSkipSaveCheckInstruction;
    if (!saveTypeInfo || (saveTypeInfo->type & SAVE_TYPE_SRAM))
    {
        gGbaSaveShared.saveData = gSaveData;
        gGbaSaveShared.saveDataSize = saveSize;
    }
    else
    {
        gGbaSaveShared.saveData = nullptr;
        gGbaSaveShared.saveDataSize = 0;
    }

    ipc_sendWordDirect(
        ((((u32)&gGbaSaveShared) >> 5) << (IPC_FIFO_MSG_CHANNEL_BITS + 3)) |
        (GBA_SAVE_IPC_CMD_SETUP << IPC_FIFO_MSG_CHANNEL_BITS) |
        IPC_CHANNEL_GBA_SAVE);
    while (ipc_isRecvFifoEmpty());
    ipc_recvWordDirect();
}

extern "C" [[gnu::section(".ewram")]] u8 sav_readSaveByteFromFile(u32 saveAddress)
{
    vm_enableNestedIrqs();
    u8 saveByte;
    if (Environment::IsIsNitroEmulator())
    {
        // save buffer in extended memory
        saveByte = ISNITRO_SAVE_BUFFER[saveAddress];
    }
    else
    {
        if (g_useSlot2Save)
        {
            saveByte = gSaveData[saveAddress];
        }
        else
        {
            // write to file
            f_lseek(&gSaveFile, saveAddress);
            UINT bytesRead = 0;
            f_read(&gSaveFile, &saveByte, 1, &bytesRead);
        }
    }
    vm_disableNestedIrqs();
    return saveByte;
}

extern "C" [[gnu::section(".ewram")]] void sav_writeSaveByteToFile(u32 saveAddress, u8 data)
{
    vm_enableNestedIrqs();
    if (Environment::IsIsNitroEmulator())
    {
        // save buffer in extended memory
        ISNITRO_SAVE_BUFFER[saveAddress] = data;
    }
    else
    {
        if (g_useSlot2Save)
        {
            gSaveData[saveAddress] = data;
        }
        else
        {
            // write to file
            f_lseek(&gSaveFile, saveAddress);
            UINT bytesWritten = 0;
            f_write(&gSaveFile, &data, 1, &bytesWritten);
        }
    }
    vm_disableNestedIrqs();
}

extern "C" [[gnu::section(".ewram")]] void sav_flushSaveFile(void)
{
    vm_enableNestedIrqs();
    if (!Environment::IsIsNitroEmulator() && !g_useSlot2Save)
    {
        f_sync(&gSaveFile);
    }
    vm_disableNestedIrqs();
}

extern "C" [[gnu::section(".ewram")]] void sav_writeSaveToFile(void)
{
    if (gGbaSaveShared.saveDataSize != 0 && !Environment::IsIsNitroEmulator())
    {
        if (g_useSlot2Save)
        {
            // Slot2 mode: write only to the cartridge, nothing to the SD card.
            sav_syncSlot2Save();
        }
        else
        {
            f_lseek(&gSaveFile, 0);
            UINT bytesWritten = 0;
            f_write(&gSaveFile, gSaveData, gGbaSaveShared.saveDataSize, &bytesWritten);
            f_sync(&gSaveFile);
        }
    }

    gGbaSaveShared.saveState = GBA_SAVE_STATE_CLEAN;
    emu_vblankIrqSkipSaveCheckInstruction = sSkipSaveCheckInstruction;
}

