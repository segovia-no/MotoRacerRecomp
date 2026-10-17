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

inline void compare(const char *fn, const Snapshot &native, const Snapshot &translated) {
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
void check(const char *name, Fn fn, void (*t)(void), const std::vector<Region> &regions) {
    Snapshot before = take(regions);
    fn();
    Snapshot native = take(regions);
    restore(before);
    t();
    compare(name, native, take(regions));
}

}  // namespace native
