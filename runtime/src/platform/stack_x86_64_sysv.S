// void call_on_stack(void *stack_top, void (*fn)(void))   -- x86-64 System V (Linux, macOS)
// Runs fn on a different stack while staying on the current thread (see stack_arm64.S).
#ifdef __APPLE__
#define SYM(x) _##x
#else
#define SYM(x) x
#endif
    .text
    .globl SYM(call_on_stack)
    .p2align 4
SYM(call_on_stack):
    pushq %rbp
    movq %rsp, %rbp          // rbp is callee-saved: it still holds the old stack after the call
    andq $-16, %rdi
    movq %rdi, %rsp
    callq *%rsi
    movq %rbp, %rsp
    popq %rbp
    ret
#ifdef __ELF__
    .section .note.GNU-stack,"",@progbits
#endif
