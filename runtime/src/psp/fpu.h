// PSP FPU exceptions off for the calling thread (the control register is per thread).
//
// x86 code runs with the x87 exceptions masked: a division by zero gives an infinity, an invalid
// operation a NaN, and the game relies on that. The Allegrex FPU can trap on the same operations; a
// trap stops a real PSP (it powers off), while PPSSPP never traps. Every thread that runs float code
// calls this first. Returns the previous control register (FCR31) for the log.
#pragma once
#include <cstdint>

namespace psp_fpu {
inline uint32_t quiet() {
    uint32_t old;
    asm volatile("cfc1 %0, $31" : "=r"(old));
    // keep only the condition bit (pspSdkDisableFPUExceptions): enables, causes and flags cleared
    const uint32_t v = old & 0x00800000u;
    asm volatile("ctc1 %0, $31" : : "r"(v));
    return old;
}
}  // namespace psp_fpu
