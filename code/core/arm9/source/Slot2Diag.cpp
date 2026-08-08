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

    for (u32 i = 0; i < newCount; i++)
    {
        const Slot2DiagEntry* e = &sDiagRing[(sDiagFlushedHead + i) % SLOT2DIAG_RING_SIZE];
        gLogger->Log(LogLevel::Debug, "slot2diag %u blk=%u src=%u st=%u\n",
            e->seq, e->romBlock, e->source, e->state);
    }

    sDiagFlushedHead = sDiagRingHead;
    sDiagFlushing = false;
}
