// Win32 structured exception handling for recompiled code (what MSVC C++ exceptions use).
//
// RaiseException walks the guest fs:[0] chain and calls each handler like Windows does.
// MSVC's __CxxFrameHandler then unwinds inner frames through RtlUnwind, runs the catch block and
// jumps to a continuation inside the catching function with that frame's esp/ebp restored.
// That jump is non-local for the host: every recompiled function that links a registration
// record also records a setjmp point (eh_register); nonlocal_jump longjmps to the function
// owning the nearest record at or above the new esp, which then calls the continuation.
#include <map>

#include "runtime.h"

using namespace rt;

namespace {

std::map<uint32_t, jmp_buf *> frames;   // registration record VA -> setjmp point in its host frame

constexpr uint32_t EXCEPTION_NONCONTINUABLE = 1, EXCEPTION_UNWINDING = 2, EXCEPTION_EXIT_UNWIND = 4;
constexpr uint32_t STATUS_UNWIND = 0xC0000027;
constexpr uint32_t CHAIN_END = 0xFFFFFFFF;

uint32_t chain_head() { return RD32(R.fs_base); }

// CONTEXT (x86, 0x2CC bytes): enough for handlers that look at esp/ebp/eip.
void fill_context(uint32_t ctx, uint32_t eip, uint32_t esp) {
    memset(M + ctx, 0, 0x2CC);
    WR32(ctx + 0x00, 0x10007);   // CONTEXT_FULL
    WR32(ctx + 0x9C, R.edi);
    WR32(ctx + 0xA0, R.esi);
    WR32(ctx + 0xA4, R.ebx);
    WR32(ctx + 0xA8, R.edx);
    WR32(ctx + 0xAC, R.ecx);
    WR32(ctx + 0xB0, R.eax);
    WR32(ctx + 0xB4, R.ebp);
    WR32(ctx + 0xB8, eip);
    WR32(ctx + 0xC4, esp);
}

}  // namespace

extern "C" {

uint32_t eh_resume_va;

void eh_register(uint32_t record, jmp_buf *jb) { frames[record] = jb; }

void eh_unlink(uint32_t new_head) {
    // Records below the new chain head (deeper in the stack) belong to frames that are gone.
    frames.erase(frames.begin(), frames.lower_bound(new_head));
}

void nonlocal_jump(uint32_t target) {
    auto it = frames.lower_bound(R.esp);
    if (it == frames.end()) fatal("nonlocal_jump to 0x%08x: no registered frame at/above esp 0x%08x", target, R.esp);
    if (it->first - R.esp > 0x10000)
        log("nonlocal_jump: frame record 0x%08x is far above esp 0x%08x\n", it->first, R.esp);
    eh_resume_va = target;
    jmp_buf *jb = it->second;
    frames.erase(frames.begin(), it);   // inner frames are unwound
    _longjmp(*jb, 1);
}

}  // extern "C"

STDCALL(KERNEL32, RaiseException, 4) {
    uint32_t code = arg(0), flags = arg(1) & EXCEPTION_NONCONTINUABLE, nargs = arg(2), args = arg(3);
    uint32_t ret = RD32(R.esp);
    uint32_t saved_esp = R.esp, saved[4] = {R.ebx, R.esi, R.edi, R.ebp};
    if (nargs > 15) nargs = 15;

    R.esp -= 0x2D0;
    uint32_t ctx = R.esp;
    fill_context(ctx, ret, saved_esp + 4 + 16);
    R.esp -= 0x50;
    uint32_t rec = R.esp;
    memset(M + rec, 0, 0x50);
    WR32(rec + 0, code);
    WR32(rec + 4, flags);
    WR32(rec + 12, ret);
    WR32(rec + 16, nargs);
    for (uint32_t i = 0; i < nargs; i++) WR32(rec + 20 + 4 * i, RD32(args + 4 * i));
    R.esp -= 16;
    uint32_t dispatcher = R.esp;

    if (trace_imports) log("RaiseException 0x%08x from 0x%08x\n", code, ret);
    for (uint32_t frame = chain_head(); frame != CHAIN_END && frame; frame = RD32(frame)) {
        uint32_t handler = RD32(frame + 4);
        uint32_t r = guest_call_cdecl(handler, {rec, frame, ctx, dispatcher});
        if (r == 0) {   // ExceptionContinueExecution
            if (flags & EXCEPTION_NONCONTINUABLE) fatal("handler tried to continue a noncontinuable exception");
            R.esp = saved_esp;
            R.ebx = saved[0]; R.esi = saved[1]; R.edi = saved[2]; R.ebp = saved[3];
            return 0;
        }
    }
    fatal("unhandled guest exception 0x%08x raised at 0x%08x", code, ret);
}

STDCALL(KERNEL32, RtlUnwind, 4) {
    uint32_t target = arg(0), target_ip = arg(1), rec = arg(2), retval = arg(3);
    uint32_t ret = RD32(R.esp);
    uint32_t saved_esp = R.esp, saved[4] = {R.ebx, R.esi, R.edi, R.ebp};
    if (target_ip && target_ip != ret)
        log("RtlUnwind: TargetIp 0x%08x differs from return address 0x%08x (returning normally)\n", target_ip, ret);

    R.esp -= 0x2D0;
    uint32_t ctx = R.esp;
    fill_context(ctx, ret, saved_esp + 4 + 16);
    if (!rec) {
        R.esp -= 0x50;
        rec = R.esp;
        memset(M + rec, 0, 0x50);
        WR32(rec + 0, STATUS_UNWIND);
        WR32(rec + 12, ret);
    }
    WR32(rec + 4, RD32(rec + 4) | EXCEPTION_UNWINDING | (target ? 0 : EXCEPTION_EXIT_UNWIND));
    R.esp -= 16;
    uint32_t dispatcher = R.esp;

    for (uint32_t frame = chain_head(); frame != CHAIN_END && frame && frame != target;) {
        uint32_t handler = RD32(frame + 4);
        guest_call_cdecl(handler, {rec, frame, ctx, dispatcher});
        uint32_t next = RD32(frame);
        WR32(R.fs_base, next);
        eh_unlink(next);
        frame = next;
    }
    R.esp = saved_esp;
    R.ebx = saved[0]; R.esi = saved[1]; R.edi = saved[2]; R.ebp = saved[3];
    return retval;
}
