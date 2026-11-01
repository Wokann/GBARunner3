#include "common.h"
#include <libtwl/rtos/rtosIrq.h>
#include <libtwl/gfx/gfxPalette.h>
#include "Cpsr.h"
#include "ReadFailure.h"

extern "C" [[gnu::section(".ewram"), gnu::noinline, gnu::noreturn]]
void fs_stopOnReadFailure(void)
{
    // Never publish or execute a block whose driver reported a failed read.
    arm_disableIrqs();
    REG_IME = 0;
    GFX_PLTT_BG_MAIN[0] = 0x1F;
    while (true);
}
