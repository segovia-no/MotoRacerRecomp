// Shared by the native replacements of guest functions (native_geom.cpp, native_load.cpp):
// MOTORACER_NATIVE mode and check mode (run native and translated on the same state, compare).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "runtime.h"

namespace native {

enum Mode { OFF, ON, CHECK };

inline Mode mode() {
    static const Mode m = [] {
        const char *e = getenv("MOTORACER_NATIVE");
        return !e ? ON : !strcmp(e, "0") ? OFF : !strcmp(e, "check") ? CHECK : ON;
    }();
    return m;
}

// ---- check mode: run native and translated on the same state, compare ----
struct Snapshot {
    X86Regs r;
    X87State f;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> mem;
};

struct Region { uint32_t addr, size; };

inline Snapshot take(const std::vector<Region> &regions) {
    Snapshot s{R, F, {}};
    // a region read through a pointer that is NULL at the time (e.g. no open TRIANGLE header) is empty
    for (const Region &g : regions)
        s.mem.push_back({g.addr, g.addr ? std::vector<uint8_t>(M + g.addr, M + g.addr + g.size) : std::vector<uint8_t>()});
    return s;
}

inline void restore(const Snapshot &s) {
    R = s.r;
    F = s.f;
    for (auto &[a, bytes] : s.mem)
        if (!bytes.empty()) memcpy(M + a, bytes.data(), bytes.size());
}

inline uint64_t checks = 0, mismatches = 0;

// live_st: how many x87 stack registers (st0, st1, ...) are part of the result (a float returned
// in st0: 1); their values are compared bit for bit (other slots may hold stale temporaries).
inline void compare(const char *fn, const Snapshot &native, const Snapshot &translated, int live_st = 0) {
    checks++;
    auto bad = [&](const char *what, uint32_t a, uint32_t b) {
        if (++mismatches <= 20)
            rt::log("native: MISMATCH %s %s: native 0x%08x translated 0x%08x (check %llu)\n", fn, what, a, b,
                    static_cast<unsigned long long>(checks));
    };
    const X86Regs &a = native.r, &b = translated.r;
    if (a.eax != b.eax) bad("eax", a.eax, b.eax);
    if (a.ecx != b.ecx) bad("ecx", a.ecx, b.ecx);
    if (a.edx != b.edx) bad("edx", a.edx, b.edx);
    if (a.ebx != b.ebx) bad("ebx", a.ebx, b.ebx);
    if (a.esp != b.esp) bad("esp", a.esp, b.esp);
    if (a.ebp != b.ebp) bad("ebp", a.ebp, b.ebp);
    if (a.esi != b.esi) bad("esi", a.esi, b.esi);
    if (a.edi != b.edi) bad("edi", a.edi, b.edi);
    if (native.f.cw != translated.f.cw) bad("fpu cw", native.f.cw, translated.f.cw);
    if (native.f.sw != translated.f.sw) bad("fpu sw", native.f.sw, translated.f.sw);
    if (native.f.top != translated.f.top) bad("fpu top", native.f.top, translated.f.top);
    for (int i = 0; i < live_st; i++) {
        const fpu_t x = native.f.st[(native.f.top + i) & 7u], y = translated.f.st[(translated.f.top + i) & 7u];
        if (memcmp(&x, &y, sizeof x)) {
            char what[32];
            snprintf(what, sizeof what, "st%d", i);
            uint32_t u = 0, v = 0;
            memcpy(&u, &x, sizeof u);
            memcpy(&v, &y, sizeof v);
            bad(what, u, v);
        }
    }
    for (size_t k = 0; k < native.mem.size(); k++) {
        const auto &x = native.mem[k].second, &y = translated.mem[k].second;
        for (size_t i = 0; i + 4 <= x.size(); i += 4) {
            uint32_t u, v;
            memcpy(&u, x.data() + i, 4);
            memcpy(&v, y.data() + i, 4);
            if (u != v) {
                char what[64];
                snprintf(what, sizeof what, "mem 0x%08x", static_cast<uint32_t>(native.mem[k].first + i));
                bad(what, u, v);
                break;
            }
        }
    }
    if (checks % 100000 == 0)
        rt::log("native: %llu checks, %llu mismatches\n", static_cast<unsigned long long>(checks),
                static_cast<unsigned long long>(mismatches));
}

// Runs the native function `fn` and the translated `t` on the same input; keeps the translated result.
template <typename Fn>
void check(const char *name, Fn fn, void (*t)(void), const std::vector<Region> &regions, int live_st = 0) {
    Snapshot before = take(regions);
    fn();
    Snapshot native = take(regions);
    restore(before);
    t();
    compare(name, native, take(regions), live_st);
}

// The same for a function whose callers read none of eax / ecx / edx afterwards (they reload them):
// those registers are not compared.
template <typename Fn>
void check_abi(const char *name, Fn fn, void (*t)(void), const std::vector<Region> &regions, int live_st = 0) {
    Snapshot before = take(regions);
    fn();
    Snapshot native = take(regions);
    restore(before);
    t();
    Snapshot translated = take(regions);
    native.r.eax = translated.r.eax;
    native.r.ecx = translated.r.ecx;
    native.r.edx = translated.r.edx;
    compare(name, native, translated, live_st);
}

}  // namespace native
