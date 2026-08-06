#pragma once
#include "common.h"
#include "Enums/GbaSaveType.h"

class GameSettings
{
public:
    /// @brief Specifies the save type to use.
    GbaSaveType saveType = GbaSaveType::Auto;

    /// @brief Specifies whether save data is read/written directly on the
    ///        GBA cartridge (slot2). When false the original SD card file
    ///        logic is used instead.
    bool16 slot2Save = true;
};
