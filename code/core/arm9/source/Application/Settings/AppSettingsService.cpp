#include "common.h"
#include "AppSettingsService.h"

[[gnu::section(".ewram.bss")]]
static JsonAppSettingsSerializer sJsonAppSettingsSerializer;

// In EWRAM BSS: the default .bss (vrama) is full and has no headroom.
[[gnu::section(".ewram.bss")]]
AppSettingsService gAppSettingsService(&sJsonAppSettingsSerializer);

bool AppSettingsService::TryLoadAppSettings(const TCHAR* settingsFilePath)
{
    return _appSettingsSerializer->TryDeserialize(settingsFilePath, _appSettings);
}
