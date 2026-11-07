// Native versions of hot race functions, group E (docs/25-psp-port.md): small functions the race calls
// hundreds of times per frame (instruction counts of the translated code, tools/icprof.py). Hooked
// through NATIVE_FUNCS (translate.py): f_<va> calls native_hot_<va>, which returns 0 to run the
// translated body f_<va>_t. MOTORACER_NATIVE=check runs both on the same state and compares
// (native_check.h). Floating point in fpu_t, in the x87 code's operation order (-ffp-contract=off).
#include <cstring>

#include "runtime.h"
#include "recomp_mem_local.h"
#include "native_check.h"

extern "C" void f_00473a90_t(void);
extern "C" void f_00495bd0_t(void);
extern "C" void f_004b32d0_t(void);
extern "C" void f_004039c0_t(void);
extern "C" void f_00403a20_t(void);
extern "C" void f_00403740_t(void);

namespace {
using namespace native;

void counted(const char *name, uint32_t &n) {
    if (++n == 1 || n % 20000 == 0)
        rt::log("native: %s checked %u times (mismatches so far: %llu)\n", name, n, static_cast<unsigned long long>(mismatches));
}

// ---- 0x473a90: (r, g, b) bytes -> a pixel of the display format (stdcall, ret 0xc) ----
// Each component: (c & 0xff) >> [0x4ee9b8 / bc / c0], then shifted left by [0x4ee9c4 / c8 / cc] as a
// 16-bit value, or-ed. eax = the pixel (upper half 0); cl = the last shift; edx = the blue part.
constexpr uint32_t FMT = 0x4ee9b8;

inline uint16_t part(uint32_t c, uint8_t down, uint8_t up) {
    const uint32_t v = (c & 0xff) >> (down & 31);
    return static_cast<uint16_t>(v << (up & 31));   // a 16-bit shl: 16 or more leaves 0
}

uint32_t pack_rgb(uint32_t r, uint32_t g, uint32_t b, uint32_t &ecx, uint32_t &edx) {
    MEM_LOCAL();
    const uint8_t rd = RD8(FMT), gd = RD8(FMT + 4), bd = RD8(FMT + 8), ru = RD8(FMT + 0xc), gu = RD8(FMT + 0x10),
                  bu = RD8(FMT + 0x14);
    const uint16_t pr = part(r, rd, ru), pg = part(g, gd, gu), pb = part(b, bd, bu);
    ecx = (ecx & 0xffffff00u) | bu;
    edx = pb;
    return static_cast<uint16_t>(pr | pg | pb);
}

void pack_rgb_fn() {
    MEM_LOCAL();
    const uint32_t sp = R.esp;
    uint32_t ecx = R.ecx, edx = R.edx;
    R.eax = pack_rgb(RD32A(sp + 4), RD32A(sp + 8), RD32A(sp + 12), ecx, edx);
    R.ecx = ecx;
    R.edx = edx;
    R.esp = sp + 16;
}

// ---- 0x495bd0: a palette range to display pixels (thiscall: ecx = object; args first, last; ret 8) ----
// With a palette ([obj + 0x549a], [obj + 0x549e] entries of 3 bytes) and last >= first: for i in
// first..last, entry i % count through 0x473a90 into the 16-bit table at obj + 0x418 + 2 i.
void palette_range() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, obj = R.ecx, first = RD32A(sp + 4), last = RD32A(sp + 8);
    R.esp = sp + 12;
    const uint32_t pal = RD32(obj + 0x549a);
    R.eax = pal;
    if (!pal || last < first) return;
    const uint32_t n = RD32(obj + 0x549e);
    uint32_t ecx = R.ecx, edx = R.edx, eax = 0;
    for (uint32_t i = first;; ) {
        const uint32_t p = pal + (i % n) * 3;
        ecx = p;
        eax = pack_rgb(RD8(p), RD8(p + 1), RD8(p + 2), ecx, edx);
        i++;
        WR16(obj + 0x416 + 2 * i, static_cast<uint16_t>(eax));
        if (i > last) break;
    }
    R.eax = eax;
    R.ecx = ecx;
    R.edx = edx;
}

// ---- 0x4b32d0: swap n bytes of a and b (cdecl a, b, n) — the sort's swap ----
// eax = 0 after a swap (unchanged when a == b), ecx / edx = the ends.
void swap_bytes() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, a = RD32A(sp + 4), b = RD32A(sp + 8), n = RD32A(sp + 12);
    R.esp = sp + 4;
    R.ecx = a;
    R.edx = b;
    if (a == b) return;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t x = RD8(a + i);
        WR8(a + i, RD8(b + i));
        WR8(b + i, x);
    }
    R.eax = 0;
    R.ecx = a + n;
    R.edx = b + n;
}

// ---- 0x4039c0 / 0x403a20: copy a 4x3 matrix (thiscall: ecx = dst; arg src; ret 4) ----
// eax = dst, edx = the last dword (the x86's copy registers ebx / ebp are restored).
void mat_copy() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, dst = R.ecx, src = RD32A(sp + 4);
    uint32_t last = 0;
    for (uint32_t k = 0; k < 12; k++) {
        last = RD32(src + 4 * k);
        WR32(dst + 4 * k, last);
    }
    R.eax = dst;
    R.edx = last;
    R.esp = sp + 8;
}

// ---- 0x403740: rotation from three angles and a translation (thiscall: ecx = 4x3 out; args angles, t) ----
// Per angle (radians) * [0x4d00d0] (_ftol) indexes the sine table 0x5fcb80: a = T[i & 0x3fff],
// b = T[(i - 0x1000) & 0x3fff]. The rows as the x87 code computes them, with the float temporaries it
// stores and reloads (t10 = b0 a2, t0c = a0 b2, t08 = b0 b2, t14 = a0 a2); row 3 = t.
// eax = out, ecx / edx = the bits of t.z / t.x.
constexpr uint32_t SIN_TABLE = 0x5fcb80;

inline float rnd(fpu_t v) { return static_cast<float>(v); }
inline fpu_t F_(float v) { return static_cast<fpu_t>(v); }

void rot_translate() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, out = R.ecx, ang = RD32A(sp + 4), t = RD32A(sp + 8);
    const fpu_t k = static_cast<fpu_t>(RDF64(0x4d00d0));
    auto ab = [&](uint32_t a, fpu_t &s, fpu_t &c) {
        const uint32_t i = static_cast<uint32_t>(FTOL(F_(RDF32(a)) * k));
        s = F_(RDF32A(SIN_TABLE + (i & 0x3fff) * 4));
        c = F_(RDF32A(SIN_TABLE + ((i - 0x1000) & 0x3fff) * 4));
    };
    fpu_t a0, b0, a1, b1, a2, b2;
    ab(ang, a0, b0);
    ab(ang + 4, a1, b1);
    ab(ang + 8, a2, b2);
    WRF32(out, rnd(a1 * a2));
    const float t10 = rnd(b0 * a2), t0c = rnd(a0 * b2);
    WRF32(out + 4, rnd(F_(t10) * b1 + F_(t0c)));
    const float t08 = rnd(b0 * b2);
    const fpu_t x = a0 * a2;
    const float t14 = rnd(x);
    WRF32(out + 8, rnd(F_(t08) - x * b1));
    WRF32(out + 0xc, rnd(-(b2 * a1)));
    WRF32(out + 0x10, rnd(F_(t14) - F_(t08) * b1));
    WRF32(out + 0x18, rnd(b1));
    WRF32(out + 0x14, rnd(F_(t0c) * b1 + F_(t10)));
    WRF32(out + 0x1c, rnd(-(a1 * b0)));
    WRF32(out + 0x20, rnd(a1 * a0));
    const uint32_t tx = RD32(t), ty = RD32(t + 4), tz = RD32(t + 8);
    WR32(out + 0x24, tx);
    WR32(out + 0x28, ty);
    WR32(out + 0x2c, tz);
    R.eax = out;
    R.ecx = tz;
    R.edx = tx;
    R.esp = sp + 12;
}

uint32_t n_473a90, n_495bd0, n_4b32d0, n_4039c0, n_403a20, n_403740;
}  // namespace

extern "C" int native_hot_473a90(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { pack_rgb_fn(); return 1; }
    check("pack rgb 0x473a90", pack_rgb_fn, f_00473a90_t, {});
    counted("pack rgb 0x473a90", n_473a90);
    return 1;
}

extern "C" int native_hot_495bd0(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { palette_range(); return 1; }
    const uint32_t obj = R.ecx, first = RD32A(R.esp + 4), last = RD32A(R.esp + 8);
    std::vector<Region> regions;
    if (last >= first && last - first < 0x10000) regions.push_back({obj + 0x418 + 2 * first, 2 * (last - first + 1) + 2});   // compared in dwords
    check("palette range 0x495bd0", palette_range, f_00495bd0_t, regions);
    counted("palette range 0x495bd0", n_495bd0);
    return 1;
}

extern "C" int native_hot_4b32d0(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { swap_bytes(); return 1; }
    const uint32_t a = RD32A(R.esp + 4), b = RD32A(R.esp + 8), n = RD32A(R.esp + 12);
    std::vector<Region> regions;
    if (n < 0x10000) regions = {{a, n}, {b, n}};
    check("swap bytes 0x4b32d0", swap_bytes, f_004b32d0_t, regions);
    counted("swap bytes 0x4b32d0", n_4b32d0);
    return 1;
}

extern "C" int native_hot_4039c0(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { mat_copy(); return 1; }
    check("matrix copy 0x4039c0", mat_copy, f_004039c0_t, {{R.ecx, 48}});
    counted("matrix copy 0x4039c0", n_4039c0);
    return 1;
}

extern "C" int native_hot_403a20(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { mat_copy(); return 1; }
    check("matrix copy 0x403a20", mat_copy, f_00403a20_t, {{R.ecx, 48}});
    counted("matrix copy 0x403a20", n_403a20);
    return 1;
}

extern "C" int native_hot_403740(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { rot_translate(); return 1; }
    check("rotation + translation 0x403740", rot_translate, f_00403740_t, {{R.ecx, 48}});
    counted("rotation + translation 0x403740", n_403740);
    return 1;
}
