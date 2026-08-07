#include "common.h"

/// @brief Contains the original GBA color palette values without correction applied.
///        This is required to ensure palette reads return the expected value.
// In EWRAM BSS: the default .bss (vrama) is full and has no headroom.
[[gnu::section(".ewram.bss")]]
u16 gShadowPalette[512];
