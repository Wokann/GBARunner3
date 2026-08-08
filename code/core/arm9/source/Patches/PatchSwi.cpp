#include "common.h"
#include "PatchSwi.h"

extern void* patch_swiTable[32];

// In EWRAM BSS: the default .bss (vrama) is full and has no headroom.
[[gnu::section(".ewram.bss")]]
static int sNextFreePatchNumber = 0;

void patch_resetSwiPatches()
{
    sNextFreePatchNumber = 0;
}

int patch_addSwiPatch(void* function)
{
    int number = sNextFreePatchNumber++;
    patch_swiTable[number] = function;
    return number;
}
