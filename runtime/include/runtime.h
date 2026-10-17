// Host-side runtime API shared by the loader, dispatcher and Win32/DirectX shims.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <initializer_list>
#include <string>

#include "recomp.h"

namespace rt {

// ---------- guest address-space layout ----------
#ifndef RECOMP_COMPACT_GUEST
// Desktop: a 4 GB reservation, regions far apart; pages cost memory only once touched.
constexpr uint32_t NULL_GUARD = 0x00010000;   // [0, 64K) inaccessible: catches null derefs
constexpr uint32_t STACK_LO = 0x00100000;     // main thread stack [1 MB, 3 MB)
constexpr uint32_t STACK_HI = 0x00300000;
constexpr uint32_t IMAGE_BASE = 0x00400000;
constexpr uint32_t EXT_LO = 0x00800000;       // [8 MB, 16 MB): engine tables relocated by the
constexpr uint32_t EXT_HI = 0x01000000;       // translator (RELOCATED_TABLES in translate.py)
constexpr uint32_t HEAP_LO = 0x01000000;      // HeapAlloc/GlobalAlloc/LocalAlloc arena
constexpr uint32_t HEAP_HI = 0x40000000;
constexpr uint32_t VALLOC_LO = 0x40000000;    // VirtualAlloc arena
constexpr uint32_t VALLOC_HI = 0x70000000;
constexpr uint32_t TEB_VA = 0x7FFDE000;
constexpr uint32_t PEB_VA = 0x7FFDF000;
constexpr uint32_t GUEST_LO = 0;              // the host block covers [GUEST_LO, GUEST_HI)
constexpr uint64_t GUEST_HI = 1ull << 32;
#else
// Small hosts without virtual memory (PSP, docs/25-psp-port.md): one contiguous host block from
// GUEST_LO; M = block - GUEST_LO. The image keeps its addresses; everything else is packed around it.
// The VirtualAlloc arena comes last: the CRT reserves 4 MB there but commits only the first few
// 64 KB, so the block ends VALLOC_BACKED bytes into it and commits past that fail loudly.
constexpr uint32_t NULL_GUARD = 0x00300000;   // nothing below the block (no MMU: not enforced)
constexpr uint32_t STACK_LO = 0x00300000;     // main thread stack [3 MB, 4 MB) (PE reserve: 1 MB)
constexpr uint32_t STACK_HI = 0x00400000;
constexpr uint32_t IMAGE_BASE = 0x00400000;   // image [0x400000, 0x72f000)
constexpr uint32_t TEB_VA = 0x0072F000;
constexpr uint32_t PEB_VA = 0x00730000;
constexpr uint32_t EXT_LO = 0x00800000;       // relocated engine tables (2 x 16 KB from 0x800010)
constexpr uint32_t EXT_HI = 0x00810000;
constexpr uint32_t HEAP_LO = 0x00810000;
#ifndef RECOMP_GUEST_HEAP_MB
#define RECOMP_GUEST_HEAP_MB 18
#endif
constexpr uint32_t HEAP_HI = HEAP_LO + (RECOMP_GUEST_HEAP_MB << 20);
constexpr uint32_t VALLOC_LO = HEAP_HI;
constexpr uint32_t VALLOC_HI = VALLOC_LO + 0x01000000;
constexpr uint32_t VALLOC_BACKED = 0x00100000;
constexpr uint32_t GUEST_LO = STACK_LO;
constexpr uint64_t GUEST_HI = VALLOC_LO + VALLOC_BACKED;
#endif
// Host stack for the recompiled code (one host frame per guest call).
#ifndef RECOMP_COMPACT_GUEST
constexpr uint64_t HOST_STACK_BYTES = 512ull << 20;
#else
constexpr uint64_t HOST_STACK_BYTES = 2ull << 20;
#endif
constexpr uint32_t THUNK_LO = 0xFFC00000;     // fake code addresses that map to host functions
constexpr uint32_t THUNK_HI = 0xFFF00000;
constexpr uint32_t RETURN_MAGIC = 0xFFFFFFF0; // return address pushed by guest_call

// ---------- logging ----------
extern bool trace_imports;
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void dump_state();

// ---------- guest memory ----------
void memory_init();
uint32_t heap_alloc(uint32_t size, bool zero = false);
void heap_free(uint32_t va);
uint32_t heap_realloc(uint32_t va, uint32_t size, bool zero = false);
uint32_t heap_size(uint32_t va);   // 0xFFFFFFFF if not a live block
void memory_stats_log();           // MOTORACER_MEM_STATS
void check_guest_range(uint32_t va, uint32_t size);   // fatal if outside the host block (compact layout)
void thread_counts(uint64_t *insns, uint64_t *cycles);   // this thread's instructions / cycles (macOS; else 0)
uint32_t valloc_reserve(uint32_t addr, uint32_t size);
void valloc_release(uint32_t addr);

inline char *host_ptr(uint32_t va) { return reinterpret_cast<char *>(M) + va; }
std::string read_cstr(uint32_t va, uint32_t max = 0x10000);
std::u16string read_wstr(uint32_t va, uint32_t max = 0x10000);
// Copies s (NUL-terminated) into a guest buffer of `cap` bytes; returns chars written w/o NUL.
uint32_t write_cstr(uint32_t va, const std::string &s, uint32_t cap);
uint32_t alloc_cstr(const std::string &s);   // heap-allocated guest copy

// ---------- code dispatch ----------
uint32_t register_thunk(RecompFn fn, const char *name);  // host fn reachable from guest code
const char *thunk_name(uint32_t va);
RecompFn find_function(uint32_t va);                      // nullptr if not a recompiled entry
// Call guest code: pushes args right-to-left and a magic return address.
uint32_t guest_call_stdcall(uint32_t fn, std::initializer_list<uint32_t> args);
uint32_t guest_call_cdecl(uint32_t fn, std::initializer_list<uint32_t> args);
uint32_t guest_call_thiscall(uint32_t fn, uint32_t self, std::initializer_list<uint32_t> args);   // callee pops

// ---------- shim ABI (valid at shim entry: [esp] = return address, args above) ----------
inline uint32_t arg(int i) { return RD32(R.esp + 4 + 4 * i); }

// ---------- loader ----------
struct LoadedImage {
    uint32_t entry;
    uint32_t image_size;
    std::string exe_path;     // host path
    std::string game_dir;     // host directory containing game.EXE
};
extern LoadedImage image;
void load_image(const std::string &exe_path);
void setup_thread_environment(const std::string &command_line);

// counting builds (MOTORACER_COUNT_INSNS) with MOTORACER_ICOUNT_DUMP=<dir>: write the per-function
// instruction counts now (game_hooks.cpp); no-op otherwise
void icount_dump(const char *tag);
inline uint64_t icount_frames = 0;

// ---------- files ----------
// ReadFile for native code (native_load.cpp): false when `handle` is not a file
bool read_file(uint32_t handle, uint32_t dst, uint32_t n, uint32_t *got);

// ---------- process ----------
extern std::string guest_command_line;
void set_last_error(uint32_t e);
uint32_t get_last_error();
[[noreturn]] void exit_process(uint32_t code);

}  // namespace rt

// Defines the exported import shim `imp_<DLL>_<NAME>` with `NARGS` stdcall arguments.
// The body returns the value for EAX; the macro pops the return address and arguments.
#define STDCALL(DLL, NAME, NARGS)                                                        \
    static uint32_t impl_##DLL##_##NAME();                                               \
    extern "C" void imp_##DLL##_##NAME(void) {                                          \
        rt::note_call(#DLL, #NAME);                                                     \
        if (rt::trace_imports) rt::trace_call(#DLL, #NAME, NARGS);                      \
        uint32_t r_ = impl_##DLL##_##NAME();                                             \
        R.eax = r_;                                                                     \
        R.esp += 4 + 4 * (NARGS);                                                       \
    }                                                                                   \
    static uint32_t impl_##DLL##_##NAME()

// cdecl variadic shims (wsprintfA): caller pops arguments.
#define CDECL(DLL, NAME)                                                                 \
    static uint32_t impl_##DLL##_##NAME();                                               \
    extern "C" void imp_##DLL##_##NAME(void) {                                          \
        rt::note_call(#DLL, #NAME);                                                     \
        if (rt::trace_imports) rt::trace_call(#DLL, #NAME, 4);                          \
        uint32_t r_ = impl_##DLL##_##NAME();                                             \
        R.eax = r_;                                                                     \
        R.esp += 4;                                                                     \
    }                                                                                   \
    static uint32_t impl_##DLL##_##NAME()

namespace rt {
void trace_call(const char *dll, const char *name, int nargs);
void note_call(const char *dll, const char *name);   // always-on flight recorder
void open_log_file();
const std::string &log_file_path();
}
