// Host-side runtime API shared by the loader, dispatcher and Win32/DirectX shims.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <initializer_list>
#include <string>

#include "recomp.h"

namespace rt {

// ---------- guest address-space layout ----------
constexpr uint32_t NULL_GUARD = 0x00010000;   // [0, 64K) inaccessible: catches null derefs
constexpr uint32_t STACK_LO = 0x00100000;     // main thread stack [1 MB, 3 MB)
constexpr uint32_t STACK_HI = 0x00300000;
constexpr uint32_t IMAGE_BASE = 0x00400000;
constexpr uint32_t HEAP_LO = 0x01000000;      // HeapAlloc/GlobalAlloc/LocalAlloc arena
constexpr uint32_t HEAP_HI = 0x40000000;
constexpr uint32_t VALLOC_LO = 0x40000000;    // VirtualAlloc arena
constexpr uint32_t VALLOC_HI = 0x70000000;
constexpr uint32_t TEB_VA = 0x7FFDE000;
constexpr uint32_t PEB_VA = 0x7FFDF000;
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
        if (rt::trace_imports) rt::trace_call(#DLL, #NAME, 4);                          \
        uint32_t r_ = impl_##DLL##_##NAME();                                             \
        R.eax = r_;                                                                     \
        R.esp += 4;                                                                     \
    }                                                                                   \
    static uint32_t impl_##DLL##_##NAME()

namespace rt {
void trace_call(const char *dll, const char *name, int nargs);
}
