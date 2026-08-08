#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Sources for slot2DiagTrace.
#define SLOT2DIAG_SRC_CART 0 // direct mem_copy32 from the slot2 cart
#define SLOT2DIAG_SRC_PRE  1 // raw sector read from the .pre sidecar
#define SLOT2DIAG_SRC_OOB  2 // out-of-bounds emulated fill (cart mode)
#define SLOT2DIAG_SRC_SD   3 // SD ROM file (fallback mode)

// States for slot2DiagTrace.
#define SLOT2DIAG_STATE_START 0
#define SLOT2DIAG_STATE_DONE  1

/// @brief Records one ROM block load into the diagnostics ring.
///        Safe to call with IRQs disabled (plain memory writes only).
void slot2DiagTrace(u32 romBlock, u8 source, u8 state);

/// @brief Called from VBlankIrq.s on every VBlank; throttled, flushes the ring
///        and counters to the SD log via gLogger.
void slot2DiagVblankHook(void);

#ifdef __cplusplus
}
#endif
