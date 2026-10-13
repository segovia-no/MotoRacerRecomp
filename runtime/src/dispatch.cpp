// Indirect-call dispatch, host thunks, traps, logging and crash reporting.
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <vector>

#include "runtime.h"

extern "C" {
typedef struct { uint32_t va; RecompFn fn; } RecompEntry;
extern const RecompEntry recomp_table[];
extern const uint32_t recomp_table_len;
}

namespace rt {

bool trace_imports = false;

// ---------- log file + flight recorder ----------
namespace {
FILE *log_file = nullptr;
std::string log_path;

struct CallRecord { const char *dll, *name; uint32_t ret, a0, a1, a2; };
constexpr int RING = 256;
CallRecord ring[RING];
uint32_t ring_pos = 0;

void vout(const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    vfprintf(stderr, fmt, ap);
    if (log_file) vfprintf(log_file, fmt, ap2);
    va_end(ap2);
}

void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vout(fmt, ap);
    va_end(ap);
}
}  // namespace

// ~/Library/Logs/MotoRacerRecomp/motoracer.log (previous run kept as .log.1).
void open_log_file() {
    const char *home = getenv("HOME");
    if (!home) return;
    std::string dir = std::string(home) + "/Library/Logs/MotoRacerRecomp";
    mkdir(dir.c_str(), 0755);
    log_path = dir + "/motoracer.log";
    rename(log_path.c_str(), (log_path + ".1").c_str());
    log_file = fopen(log_path.c_str(), "w");
    if (log_file) setvbuf(log_file, nullptr, _IOLBF, 1 << 16);
}

const std::string &log_file_path() { return log_path; }

void note_call(const char *dll, const char *name) {
    CallRecord &r = ring[ring_pos++ % RING];
    r.dll = dll;
    r.name = name;
    r.ret = RD32(R.esp);
    r.a0 = RD32(R.esp + 4);
    r.a1 = RD32(R.esp + 8);
    r.a2 = RD32(R.esp + 12);
}

void log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vout(fmt, ap);
    va_end(ap);
}

void dump_state() {
    out("  eax=%08x ecx=%08x edx=%08x ebx=%08x\n  esp=%08x ebp=%08x esi=%08x edi=%08x\n",
        R.eax, R.ecx, R.edx, R.ebx, R.esp, R.ebp, R.esi, R.edi);
    out("  guest stack:");
    for (int i = 0; i < 12; i++)
        if (R.esp + 4 * i < STACK_HI) out(" %08x", RD32(R.esp + 4 * i));
    out("\n  last Win32/COM calls (oldest first):\n");
    uint32_t n = ring_pos < RING ? ring_pos : RING;
    for (uint32_t i = ring_pos - n; i != ring_pos; i++) {
        const CallRecord &r = ring[i % RING];
        out("    [%08x] %s!%s(%x, %x, %x, ...)\n", r.ret, r.dll, r.name, r.a0, r.a1, r.a2);
    }
    out("  host backtrace (f_XXXXXXXX = guest function VA):\n");
    void *frames[48];
    int fn = backtrace(frames, 48);
    backtrace_symbols_fd(frames, fn, 2);
    if (log_file) {
        fflush(log_file);
        backtrace_symbols_fd(frames, fn, fileno(log_file));
        fflush(log_file);
    }
}

void fatal(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    out("FATAL: %s\n", msg);
    dump_state();
    std::string box = std::string(msg) + "\n\nDetails were written to:\n" + log_path;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Moto Racer crashed", box.c_str(), nullptr);
    _exit(1);
}

void trace_call(const char *dll, const char *name, int nargs) {
    out("[%08x] %s!%s(", RD32(R.esp), dll, name);
    for (int i = 0; i < nargs; i++) out(i ? ", %x" : "%x", arg(i));
    out(")\n");
}

// ---------- thunks: fake guest code addresses that land in host functions ----------
namespace {
struct Thunk { RecompFn fn; const char *name; };
std::vector<Thunk> thunks;
}  // namespace

uint32_t register_thunk(RecompFn fn, const char *name) {
    uint32_t va = THUNK_LO + 16 * static_cast<uint32_t>(thunks.size());
    if (va >= THUNK_HI) fatal("too many thunks");
    thunks.push_back({fn, name});
    return va;
}

const char *thunk_name(uint32_t va) {
    if (va < THUNK_LO || va >= THUNK_HI || (va & 15)) return nullptr;
    uint32_t i = (va - THUNK_LO) / 16;
    return i < thunks.size() ? thunks[i].name : nullptr;
}

RecompFn find_function(uint32_t va) {
    const RecompEntry *lo = recomp_table, *hi = recomp_table + recomp_table_len;
    const RecompEntry *it = std::lower_bound(lo, hi, va, [](const RecompEntry &e, uint32_t v) { return e.va < v; });
    return (it != hi && it->va == va) ? it->fn : nullptr;
}

uint32_t guest_call_stdcall(uint32_t fn, std::initializer_list<uint32_t> args) {
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) PUSH32(*it);
    PUSH32(RETURN_MAGIC);
    call_indirect(fn);
    return R.eax;
}

uint32_t guest_call_cdecl(uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t r = guest_call_stdcall(fn, args);
    R.esp += 4 * static_cast<uint32_t>(args.size());
    return r;
}

[[noreturn]] void exit_process(uint32_t code) {
    log("guest exited with code %u\n", code);
    fflush(stdout);
    exit(static_cast<int>(code));
}

}  // namespace rt

using namespace rt;

extern "C" {

void call_indirect(uint32_t va) {
    if (RecompFn f = find_function(va)) { f(); return; }
    if (va >= THUNK_LO && va < THUNK_HI && !(va & 15)) {
        uint32_t i = (va - THUNK_LO) / 16;
        if (i < thunks.size()) { thunks[i].fn(); return; }
    }
    fatal("indirect call/jump to unknown guest address 0x%08x", va);
}

void import_unimplemented(const char *dll, const char *name) {
    fatal("unimplemented import %s!%s (called from 0x%08x)", dll, name, RD32(R.esp));
}

void x86_rdtsc(void) {
    // ~200 MHz Pentium-class clock derived from the host monotonic clock.
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 200000000ull + (uint64_t)ts.tv_nsec / 5;
    R.eax = (uint32_t)t;
    R.edx = (uint32_t)(t >> 32);
}

uint32_t x86_in(uint32_t port, int size) {
    (void)size;
    log("guest IN from port 0x%x -> 0xff\n", port);
    return 0xFF;
}

void x86_out(uint32_t port, uint32_t value, int size) {
    (void)size;
    log("guest OUT 0x%x -> port 0x%x ignored\n", value, port);
}

void x86_int(uint32_t va, int vector) { fatal("guest executed INT %d at 0x%08x", vector, va); }
void x86_div_fault(uint32_t va) { fatal("guest divide error at 0x%08x", va); }
void x86_unimpl(uint32_t va, const char *what) { fatal("unimplemented at 0x%08x: %s", va, what); }

}  // extern "C"

namespace rt {

static void on_segv(int sig, siginfo_t *si, void *) {
    uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
    uintptr_t base = reinterpret_cast<uintptr_t>(M);
    if (a >= base && a < base + (1ull << 32))
        out("FATAL: guest memory fault at guest address 0x%08lx (signal %d)\n", (unsigned long)(a - base), sig);
    else if (sig == SIGABRT)
        out("FATAL: host abort (signal %d) - see backtrace (Metal/SDL validation, assertion)\n", sig);
    else
        out("FATAL: host fault at %p (signal %d)\n", si->si_addr, sig);
    dump_state();
    _exit(2);
}

void install_crash_handlers() {
    static char altstack[1 << 16];
    stack_t ss{};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);   // Metal/SDL assertions, C++ aborts
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
}

}  // namespace rt
