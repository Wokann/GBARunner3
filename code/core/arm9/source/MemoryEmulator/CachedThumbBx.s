.section ".text.memu_mapOrCachedThumbBx", "ax"
.arm

#include "AsmMacros.inc"

// Called ONLY from memu_prefetchAbort, after saving
// {guest r0-r3, guest r12, fault PC} on the existing hicode abort stack.
// r0 = canonical/mirrored GBA instruction address; LR = handler continuation.
// No additional stack, RAM state, I/O or cache publication is introduced.
arm_func memu_mapOrCachedThumbBx
    mrs r1, spsr
    and r2, r1, #0x3F
    cmp r2, #0x30             // physical user mode, Thumb instruction
    bne 9f

    cmp r0, #0x08000000
    blo 9f
    cmp r0, #0x0E000000
    bhs 9f
    tst r0, #1
    bne 9f                   // ambiguous/non-halfword-aligned fault PC

    mov r2, r0, lsr #12
    ldr r3, =0x1FFF
    and r2, r2, r3
    ldr r3, =sdc_romBlockToCacheBlock
    ldr r2, [r3, r2, lsl #2]
    cmp r2, #0
    beq 9f                   // never fetch data or publish a new mapping here

    mov r3, r0, lsl #20
    mov r3, r3, lsr #20
    ldrh r2, [r2, r3]        // actual RAM-cache bytes, after normal patch handling
    bic r3, r2, #0x18
    cmp r3, #0x4700          // only BX r0-r3; BLX/high registers/PC all fall back
    bne 9f

    and r3, r2, #0x18
    mov r3, r3, lsr #1       // register number * 4 in saved abort frame
    ldr r2, [sp, r3]

    // Only canonical GBA EWRAM, IWRAM and low 2MB ROM destinations.
    // RAM mirrors and DS-private/linear-relocated addresses fall back.
    // Keep a native execution point; never chain high-ROM BX shortcuts.
    cmp r2, #0x08000000
    blo 2f
    cmp r2, #0x08200000
    bhs 9f
    b 1f
2:
    sub r3, r2, #0x02000000
    cmp r3, #0x00040000
    blo 1f
    sub r3, r2, #0x03000000
    cmp r3, #0x00008000
    bhs 9f
1:
    tst r2, #1
    biceq r1, r1, #0x20
    orrne r1, r1, #0x20
    biceq r2, r2, #3
    bicne r2, r2, #1
    str r2, [sp, #20]        // replace retry PC with native BX destination
    msr spsr_cxsf, r1        // all saved status bits except T remain unchanged
    bx lr                   // original LDM^ restores guest regs/status/PC
9:
    ldr pc, =hic_mapRomBlock // tail-call: retain original continuation and frame
    .ltorg
