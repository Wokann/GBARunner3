#include "common.h"
#include "Slot2Diag.h"

// Runtime ROM-load diagnostics (full diagnostic build).
//
// A small EWRAM ring records every ROM block load (source + start/done state).
// VBlankIrq.s calls slot2DiagVblankHook on every VBlank; the hook throttles
// and flushes new ring entries + counters to /_gba/gbarunner3.log (FileLogger
// writes with f_sync, so the trace survives a freeze). Trace writes are plain
// memory ops and are only ever done with IRQs masked inside loadRomBlock; the
// throttled flush runs in IRQ context, where the main context is suspended.
//
// NOTE: the periodic SD flush also acts as a de-facto keep-alive that avoids
// the slot2 .pre read hang on some flashcards (Pokemon Pinball collection
// screen); removing the flush reproduces the white screen.

#define SLOT2DIAG_RING_SIZE 24
#define SLOT2DIAG_FLUSH_INTERVAL 32 // VBlank ticks, ~0.5s at 60Hz

typedef struct
{
    u32 romBlock;
    u16 seq;
    u8 source;
    u8 state;
} Slot2DiagEntry;

[[gnu::section(".ewram.bss")]]
static Slot2DiagEntry sDiagRing[SLOT2DIAG_RING_SIZE];
[[gnu::section(".ewram.bss")]]
static u32 sDiagRingHead;
[[gnu::section(".ewram.bss")]]
static u32 sDiagFlushedHead;
[[gnu::section(".ewram.bss")]]
static u16 sDiagSeq;
[[gnu::section(".ewram.bss")]]
static u32 sDiagIrqCount;
[[gnu::section(".ewram.bss")]]
static u32 sDiagLoadCount[4];
[[gnu::section(".ewram.bss")]]
static bool sDiagFlushing;

extern "C" [[gnu::section(".ewram")]]
void slot2DiagTrace(u32 romBlock, u8 source, u8 state)
{
    Slot2DiagEntry* e = &sDiagRing[sDiagRingHead % SLOT2DIAG_RING_SIZE];
    e->romBlock = romBlock;
    e->source = source;
    e->state = state;
    e->seq = (u16)(sDiagSeq + 1);
    sDiagSeq++;
    sDiagRingHead++;
    if (state == SLOT2DIAG_STATE_START && source < 4)
        sDiagLoadCount[source]++;
}

extern "C" [[gnu::section(".ewram")]]
void slot2DiagVblankHook(void)
{
    sDiagIrqCount++;
    if ((sDiagIrqCount % SLOT2DIAG_FLUSH_INTERVAL) != 0)
        return;
    if (sDiagFlushing)
        return; // nested IRQ re-entry while flushing
    sDiagFlushing = true;

    u32 loads[4];
    for (u32 i = 0; i < 4; i++)
        loads[i] = sDiagLoadCount[i];

    u32 newCount = sDiagRingHead - sDiagFlushedHead;
    if (newCount > SLOT2DIAG_RING_SIZE)
        newCount = SLOT2DIAG_RING_SIZE;

    gLogger->Log(LogLevel::Debug, "slot2diag hb irq=%u cart=%u pre=%u oob=%u sd=%u new=%u\n",
        sDiagIrqCount, loads[0], loads[1], loads[2], loads[3], newCount);

    // Batch the ring dump: 8 entries per line keeps a flush to a handful of
    // f_syncs instead of one per entry (each f_sync is slow: FAT/dir flush).
    for (u32 i = 0; i < newCount; i += 8)
    {
        u32 blocks[8];
        u32 n = newCount - i;
        if (n > 8)
            n = 8;
        for (u32 j = 0; j < n; j++)
        {
            const Slot2DiagEntry* e = &sDiagRing[(sDiagFlushedHead + i + j) % SLOT2DIAG_RING_SIZE];
            blocks[j] = e->romBlock;
        }
        switch (n)
        {
            case 1: gLogger->Log(LogLevel::Debug, "s2d %u\n", blocks[0]); break;
            case 2: gLogger->Log(LogLevel::Debug, "s2d %u %u\n", blocks[0], blocks[1]); break;
            case 3: gLogger->Log(LogLevel::Debug, "s2d %u %u %u\n", blocks[0], blocks[1], blocks[2]); break;
            case 4: gLogger->Log(LogLevel::Debug, "s2d %u %u %u %u\n", blocks[0], blocks[1], blocks[2], blocks[3]); break;
            case 5: gLogger->Log(LogLevel::Debug, "s2d %u %u %u %u %u\n", blocks[0], blocks[1], blocks[2], blocks[3], blocks[4]); break;
            case 6: gLogger->Log(LogLevel::Debug, "s2d %u %u %u %u %u %u\n", blocks[0], blocks[1], blocks[2], blocks[3], blocks[4], blocks[5]); break;
            case 7: gLogger->Log(LogLevel::Debug, "s2d %u %u %u %u %u %u %u\n", blocks[0], blocks[1], blocks[2], blocks[3], blocks[4], blocks[5], blocks[6]); break;
            default: gLogger->Log(LogLevel::Debug, "s2d %u %u %u %u %u %u %u %u\n", blocks[0], blocks[1], blocks[2], blocks[3], blocks[4], blocks[5], blocks[6], blocks[7]); break;
        }
    }

    sDiagFlushedHead = sDiagRingHead;
    sDiagFlushing = false;
}
