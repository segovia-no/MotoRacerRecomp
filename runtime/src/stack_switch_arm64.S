// void call_on_stack(void *stack_top, void (*fn)(void))
// Runs fn on a different stack while staying on the current thread (Cocoa/SDL need the
// main thread; recompiled code needs a deeper stack than the 8 MB main-thread default).
    .text
    .globl _call_on_stack
    .p2align 2
_call_on_stack:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x2, sp
    and x0, x0, #~15
    mov sp, x0
    str x2, [sp, #-16]!
    blr x1
    ldr x2, [sp], #16
    mov sp, x2
    ldp x29, x30, [sp], #16
    ret
