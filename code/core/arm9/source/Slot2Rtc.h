#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Handles a 16-bit store to the GBA ROM window from the emulated CPU.
///        Writes to the cart GPIO/RTC registers (0x080000C4 data, 0x080000C6
///        direction, 0x080000C8 control) are forwarded to the physical slot2
///        bus and mirrored into the ROM cache, so subsequent GBA loads from
///        those addresses return the real register state without a load hook.
void slot2RtcStore16(u32 address, u16 value);

#ifdef __cplusplus
}
#endif
