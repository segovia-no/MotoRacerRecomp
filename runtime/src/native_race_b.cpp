// Native versions of hot race functions, group B (docs/25-psp-port.md, the PSP hardware profile).
// Hooked through NATIVE_FUNCS (translate.py): f_<va> calls native_hot_<va>, which returns 0 to run the
// translated body f_<va>_t. MOTORACER_NATIVE=check runs both on the same state and compares
// (native_check.h). Floating point in fpu_t, in the x87 code's operation order (-ffp-contract=off).
//
// The x87 code keeps temporaries in stack slots: a qword slot holds an fpu_t exactly (float or double
// both round-trip through a double), a dword slot rounds to float — the native code rounds exactly
// where the x86 stores a dword. Comparisons go through FCOM_L into the status word the function
// leaves in F.sw, as the translated code does; "test ah, 0x41 / jne" = C0 | C3 set (below, equal or
// unordered), "test ah, 1" = C0.
#include <cstring>

#include "runtime.h"
#include "recomp_mem_local.h"
#include "native_check.h"

extern "C" void f_00469c60_t(void);
extern "C" void f_00467930_t(void);
extern "C" void f_0048e180_t(void);
extern "C" void f_00466a80_t(void);
extern "C" void f_0045b4e0_t(void);
extern "C" void f_0048e180(void);
extern "C" void f_0045ca00(void);
extern "C" void f_00458da0(void);

namespace {
using namespace native;

constexpr uint32_t C0 = 0x100, C0C3 = 0x4100;

inline float bits_f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
inline uint32_t f_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
// fnstsw ax into the low half of eax (the translated code's value of eax afterwards)
inline uint32_t fnstsw_ax(uint32_t eax, uint32_t sw) {
    return (eax & 0xffff0000u) | static_cast<uint16_t>((sw & ~0x3800u) | ((F.top & 7u) << 11));
}

void counted(const char *name, uint32_t &n) {
    if (++n == 1 || n == 100 || n == 1000 || n % 20000 == 0)
        rt::log("native: %s checked %u times (mismatches so far: %llu)\n", name, n,
                static_cast<unsigned long long>(mismatches));
}

// The bike table: 292-byte records at 0x509290 indexed by the bike type.
constexpr uint32_t BIKE_REC = 292;
constexpr uint32_t DT = 0x581014;   // frame time (float)

// ---- 0x48e180: gear from speed (thiscall: ecx = physics object, [esp+4] = int *gear or NULL; ret 4) ----
// The record's max speed k ([+0x5092c0], float) times per-gear thresholds against the speed [ecx+0xc8]
// picks a gear (1..5, or 1..6 when [+0x509398] is 0) and its ratio; returns in st0 the speed / (ratio
// * k), as float, clamped to 0..1 (> 1.0f by its bits, < 0 by compare). eax = the pointer, ecx = the
// unclamped float's bits, edx = gear.
void gear_ratio() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, obj = R.ecx, out = RD32A(sp + 4);
    const uint32_t rec = RD32(obj + 0x54) * BIKE_REC;
    const fpu_t k = static_cast<fpu_t>(RDF32(rec + 0x5092c0));
    const fpu_t v = static_cast<fpu_t>(RDF32(obj + 0xc8));
    uint32_t sw = F.sw;
    static const fpu_t th6[5] = {static_cast<fpu_t>(0x1.70a3d70a3d70ap-3), static_cast<fpu_t>(0x1.51eb851eb851fp-2),
                                 static_cast<fpu_t>(0x1.eb851eb851eb8p-2), static_cast<fpu_t>(0x1.3d70a3d70a3d7p-1),
                                 static_cast<fpu_t>(0x1.7ae147ae147aep-1)};
    static const uint32_t r6[5] = {0x3e6b851f, 0x3ec28f5c, 0x3f07ae14, 0x3f2b851f, 0x3f4a3d71};
    static const fpu_t th5[4] = {static_cast<fpu_t>(0x1.3333333333333p-3), static_cast<fpu_t>(0x1.3333333333333p-2),
                                 static_cast<fpu_t>(0x1.ccccccccccccdp-2), static_cast<fpu_t>(0x1.8000000000000p-1)};
    static const uint32_t r5[4] = {0x3e4ccccd, 0x3eb33333, 0x3f000000, 0x3f4ccccd};
    const bool five = RD32(rec + 0x509398) != 0;
    const fpu_t *th = five ? th5 : th6;
    const uint32_t *ratios = five ? r5 : r6;
    const uint32_t n = five ? 4 : 5;
    uint32_t gear = n + 1, ratio = 0x3f800000;
    for (uint32_t i = 0; i < n; i++) {
        FCOM_L(sw, k * th[i], v);
        if (!(sw & C0C3)) {
            gear = i + 1;
            ratio = ratios[i];
            break;
        }
    }
    fpu_t x = static_cast<fpu_t>(bits_f(ratio)) * k;
    x = static_cast<fpu_t>(RDF32(obj + 0xc8)) / x;
    const float f = static_cast<float>(x);
    const uint32_t fb = f_bits(f);
    uint32_t res = fb;
    if (static_cast<int32_t>(fb) > 0x3f800000) {
        res = 0x3f800000;
    } else {
        FCOM_L(sw, static_cast<fpu_t>(0.0), static_cast<fpu_t>(f));
        if (!(sw & C0C3)) res = 0;
    }
    if (out) WR32(out, gear);
    FPUSH(static_cast<fpu_t>(bits_f(res)));
    F.sw = static_cast<uint16_t>(sw);
    R.eax = out;
    R.ecx = fb;
    R.edx = gear;
    R.esp = sp + 8;
}

// ---- 0x45b4e0: blend two angle sets (thiscall: ecx = out; [esp+4] = a, [esp+8] = b, [esp+12] = t; ret 12) ----
// Sets are {int count; float xyz[count][3]}; out count = min(a, b). t <= 0 (by its bits): copy a,
// t >= 1.0: copy b (rep movsd; nothing when out is that set); else each angle a + (b' - a) * t with b'
// moved by 2 pi to within pi of a, the result wrapped into [0, 2 pi].
const fpu_t PI = static_cast<fpu_t>(0x1.921fb54442d18p+1), TWO_PI = static_cast<fpu_t>(0x1.921fb54442d18p+2);

uint32_t blend_angle(uint32_t abits, uint32_t bbits, fpu_t t, uint32_t &sw) {
    const fpu_t A = static_cast<fpu_t>(bits_f(abits));
    const fpu_t B = static_cast<fpu_t>(bits_f(bbits));
    float bsel = bits_f(bbits);
    FCOM_L(sw, A + PI, B);
    if (sw & C0) bsel = static_cast<float>(B - TWO_PI);
    const fpu_t b1 = static_cast<fpu_t>(bsel);
    FCOM_L(sw, A - PI, b1);
    if (!(sw & C0C3)) bsel = static_cast<float>(b1 + TWO_PI);
    fpu_t x = static_cast<fpu_t>(bsel) - A;
    x = x * t;
    x = x + A;
    FCOM_L(sw, x, static_cast<fpu_t>(0.0));
    float r = static_cast<float>(x);
    if (sw & C0) {
        x = static_cast<fpu_t>(r);
        do {
            x = x + TWO_PI;
            FCOM_L(sw, x, static_cast<fpu_t>(0.0));
        } while (sw & C0);
        r = static_cast<float>(x);
    }
    FCOM_L(sw, static_cast<fpu_t>(r), TWO_PI);
    if (!(sw & C0C3)) {
        x = static_cast<fpu_t>(r);
        do {
            x = x - TWO_PI;
            FCOM_L(sw, x, TWO_PI);
        } while (!(sw & C0C3));
        r = static_cast<float>(x);
    }
    return f_bits(r);
}

void angle_blend() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, out = R.ecx, a = RD32A(sp + 4), b = RD32A(sp + 8), tb = RD32A(sp + 12);
    const int32_t na = static_cast<int32_t>(RD32(a)), nb = static_cast<int32_t>(RD32(b));
    const int32_t count = nb < na ? nb : na;
    WR32(out, static_cast<uint32_t>(count));
    R.esp = sp + 16;
    R.edx = b;
    auto copy = [&](uint32_t src) {   // rep movsd of 3 * count dwords, forward
        const uint32_t dw = static_cast<uint32_t>(count) * 3u;
        for (uint32_t i = 0; i < dw; i++) WR32(out + 4 + 4 * i, RD32(src + 4 + 4 * i));
        R.eax = dw;
        R.ecx = 0;
    };
    if (static_cast<int32_t>(tb) <= 0) {
        if (a == out) { R.eax = static_cast<uint32_t>(count); R.ecx = static_cast<uint32_t>(na); return; }
        copy(a);
        return;
    }
    if (static_cast<int32_t>(tb) >= 0x3f800000) {
        if (out == b) { R.eax = static_cast<uint32_t>(count); R.ecx = tb; return; }
        copy(b);
        return;
    }
    R.ecx = 0;
    R.eax = static_cast<uint32_t>(count);
    if (count <= 0) return;
    const fpu_t t = static_cast<fpu_t>(bits_f(tb));
    uint32_t sw = F.sw, pa = a + 4, pb = b + 4, po = out + 4, i = 0, last = 0;
    do {
        for (uint32_t c = 0; c < 3; c++) {
            last = blend_angle(RD32(pa + 4 * c), RD32(pb + 4 * c), t, sw);
            WR32(po + 4 * c, last);
        }
        pa += 12;
        pb += 12;
        po += 12;
        i++;
    } while (static_cast<int32_t>(RD32(out)) > static_cast<int32_t>(i));
    F.sw = static_cast<uint16_t>(sw);
    R.eax = last;
    R.ecx = i;
    R.edx = pb;
}

// ---- 0x467930: bike speed / revs (thiscall: ecx = bike; no args; ret) ----
// Speed from the distance moved this frame ([+0x338..] - [+0x344..]) / dt; for AI bikes ([+8] == 0)
// the acceleration [+0x120] (clamped -50..40) and the integrated speed [+0x118] (through
// game_hook_ai_speed, the runtime's fractional carry), then the revs [+0x11c] from the gear ratio
// (0x48e180) or, for the player ([+8] != 0 and [+4] != 0), from the engine table.
// replay: check mode runs this after the translated version; the hook keeps state between calls, so
// the native run replays the value the translated run stored instead of calling it a second time.
constexpr uint32_t GEAR_RET = 0x467ada;   // return address of the call to 0x48e180

void ai_speed(bool replay, uint32_t replay_val) {
    MEM_LOCAL();
    const uint32_t sp = R.esp, bike = R.ecx, esi0 = R.esi, edi0 = R.edi;
    uint32_t sw = F.sw;
    const fpu_t dx = static_cast<fpu_t>(RDF32(bike + 0x338)) - static_cast<fpu_t>(RDF32(bike + 0x344));
    const fpu_t dy = static_cast<fpu_t>(RDF32(bike + 0x33c)) - static_cast<fpu_t>(RDF32(bike + 0x348));
    const fpu_t dz = static_cast<fpu_t>(RDF32(bike + 0x340)) - static_cast<fpu_t>(RDF32(bike + 0x34c));
    const fpu_t fdx = static_cast<fpu_t>(static_cast<float>(dx)), fdy = static_cast<fpu_t>(static_cast<float>(dy)),
                fdz = static_cast<fpu_t>(static_cast<float>(dz));
    const uint32_t old = RD32(bike + 0x118);
    fpu_t sum = dz * fdz;
    sum = sum + fdy * fdy;
    sum = sum + fdx * fdx;
    const fpu_t dt = static_cast<fpu_t>(RDF32A(DT));
    fpu_t x = FM(sqrt)(sum) / (dt * static_cast<fpu_t>(0x1.47ae147ae147bp-7));
    x = x * static_cast<fpu_t>(0x1.ccccccccccccdp-6);
    int64_t r = FTOL(x);
    uint32_t eax = static_cast<uint32_t>(r), edx, ecx;
    const uint32_t ebx = RD32(bike + 8);
    WR32(bike + 0x114, eax);
    WR32(bike + 0x118, eax);
    if (ebx != 0 && RD32(bike + 4) != 0) {
        const uint32_t k73 = RD32(bike + 0x78) * 73u;
        x = static_cast<fpu_t>(RDF32(k73 * 4u + 0x5092ec)) / static_cast<fpu_t>(RDF32(k73 * 4u + 0x509290));
        x = x * static_cast<fpu_t>(RDF32(bike + 0xec));
        r = FTOL(x);
        WR32(bike + 0x11c, static_cast<uint32_t>(r));
        R.eax = static_cast<uint32_t>(r);
        R.edx = static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32);
        R.ecx = k73;
        R.esp = sp + 4;
        F.sw = static_cast<uint16_t>(sw);
        return;
    }
    if (ebx == 0) {
        x = static_cast<fpu_t>(static_cast<int32_t>(eax)) - static_cast<fpu_t>(static_cast<int32_t>(old));
        x = x / dt;
        x = x * static_cast<fpu_t>(0x1.9000000000000p+6);
        x = x + static_cast<fpu_t>(RDF32(bike + 0x120));
        x = x * static_cast<fpu_t>(0x1.0000000000000p-1);
        WRF32(bike + 0x120, static_cast<float>(x));
        const uint32_t acc = RD32(bike + 0x120);
        if (static_cast<int32_t>(acc) > 0x42200000) WR32(bike + 0x120, 0x42200000u);
        else if (acc > 0xc2480000u) WR32(bike + 0x120, 0xc2480000u);
        x = static_cast<fpu_t>(RDF32(bike + 0x120)) * dt;
        x = x * static_cast<fpu_t>(0x1.47ae147ae147bp-7);
        r = FTOL(x);
        eax = static_cast<uint32_t>(r);
        if (replay) {
            eax = replay_val;
        } else {
            R.esi = bike;
            R.edi = old;
            R.eax = eax;
            F.sw = static_cast<uint16_t>(sw);
            game_hook_ai_speed();
            sw = F.sw;
            eax = R.eax;
        }
        WR32(bike + 0x118, eax);
        FCOM_L(sw, static_cast<fpu_t>(static_cast<int32_t>(eax)), static_cast<fpu_t>(0.0));
        if (sw & C0) WR32(bike + 0x118, 0);
    }
    // 0x467a9b: revs from the gear ratio, with [+0xec] temporarily the speed-derived value
    x = static_cast<fpu_t>(static_cast<int32_t>(RD32(bike + 0x118)));
    const uint32_t saved_ec = RD32(bike + 0xec);
    x = x * static_cast<fpu_t>(0x1.1c71c71c71c72p-2);
    WRF32(bike + 0xec, static_cast<float>(x));
    if (RD32(bike + 0xec) > 0x80000000u) WR32(bike + 0xec, 0);
    // call 0x48e180 (ecx = bike + 0x24, arg = bike + 0x7c) from esp = sp - 0x1c
    const uint32_t frame = sp - 0x1c;
    WR32A(frame + 0x18, saved_ec);   // the x86's own stack slot for it
    WR32A(frame - 4, bike + 0x7c);
    WR32A(frame - 8, GEAR_RET);
    R.esp = frame - 8;
    R.ecx = bike + 0x24;
    F.sw = static_cast<uint16_t>(sw);
    f_0048e180();
    fpu_t g = ST(0);
    FPOP();
    sw = F.sw;
    WR32(bike + 0xec, saved_ec);
    ecx = RD32(bike + 0x78) * 73u;
    g = g * static_cast<fpu_t>(RDF32(ecx * 4u + 0x5092ec));
    r = FTOL(g);
    eax = static_cast<uint32_t>(r);
    edx = static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32);
    WR32(bike + 0x11c, eax);
    FCOM_L(sw, static_cast<fpu_t>(static_cast<int32_t>(eax)), static_cast<fpu_t>(0x1.9000000000000p+9));
    eax = fnstsw_ax(eax, sw);
    if (sw & C0) WR32(bike + 0x11c, 0x320u);
    R.eax = eax;
    R.ecx = ecx;
    R.edx = edx;
    R.esi = esi0;
    R.edi = edi0;
    R.esp = sp + 4;
    F.sw = static_cast<uint16_t>(sw);
}

// ---- 0x466a80: bike lean (thiscall: ecx = bike; no args; ret) ----
// Lean speed [+0x2e9d] (scaled by the bike table, its minimum [+0x5092d8]), lean phase [+0x2e99] and
// its sine [+0x2e91], the lean angle [+0x2ea1] turning toward the target (0x45ca00 - [+0x2ea5] +
// [+0x2e39]) by the shortest way round, wrapped into [0, 2 pi]; [+0x2e8d] = the change, wrapped by
// 0x458da0 and clamped to +-[+0x5092a8]. dt clamped to 3..5 (by its bits).
constexpr uint32_t LEAN_RET1 = 0x466ba4, LEAN_RET2 = 0x466d12;

void bike_lean() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, bike = R.ecx, ebx0 = R.ebx, esi0 = R.esi;
    const uint32_t frame = sp - 0x20;   // esp after sub 0x18 and two pushes
    uint32_t sw = F.sw;
    uint32_t dts = RD32A(DT);
    if (static_cast<int32_t>(dts) > 0x40a00000) dts = 0x40a00000;
    else if (static_cast<int32_t>(dts) < 0x40400000) dts = 0x40400000;
    const uint32_t k73 = RD32(bike + 0x78) * 73u, rec = k73 * 4u;
    fpu_t x = static_cast<fpu_t>(RDF32(rec + 0x5092d4)) * static_cast<fpu_t>(RDF32(bike + 0x2e9d));
    const fpu_t y = static_cast<fpu_t>(bits_f(dts)) * static_cast<fpu_t>(0x1.4bc6a7ef9db23p+2);
    WRF32(bike + 0x2e9d, static_cast<float>(x));
    WRF32(bike + 0x2e95, static_cast<float>(y + static_cast<fpu_t>(RDF32(bike + 0x2e95))));
    const uint32_t ebx = RD32(bike + 0x2e95);
    if (static_cast<int32_t>(ebx) > 0) WR32(bike + 0x2e95, 0);
    const float lim = static_cast<float>(static_cast<fpu_t>(RDF32(bike + 0x2e95)) / static_cast<fpu_t>(bits_f(dts)));
    x = static_cast<fpu_t>(RDF32(bike + 0x2e9d)) / static_cast<fpu_t>(RDF32(rec + 0x5092dc));
    x = x + static_cast<fpu_t>(RDF32(bike + 0x2e99));
    WRF32(bike + 0x2e99, static_cast<float>(x));
    FCOM_L(sw, static_cast<fpu_t>(RDF32(bike + 0x2e9d)), static_cast<fpu_t>(lim));
    uint32_t eax = fnstsw_ax(k73, sw);
    if (!(sw & C0C3)) {
        x = static_cast<fpu_t>(lim) - static_cast<fpu_t>(RDF32(bike + 0x2e9d));
        WR32(bike + 0x2e99, 0x3fc90fdbu);
        x = x * static_cast<fpu_t>(0x1.0000000000000p-1);
        x = x + static_cast<fpu_t>(RDF32(bike + 0x2e9d));
        WRF32(bike + 0x2e9d, static_cast<float>(x));
    }
    FCOM_L(sw, static_cast<fpu_t>(RDF32(rec + 0x5092d8)), static_cast<fpu_t>(RDF32(bike + 0x2e9d)));
    eax = fnstsw_ax(eax, sw);
    if (!(sw & C0C3)) {
        eax = RD32(rec + 0x5092d8);
        WR32(bike + 0x2e9d, eax);
    }
    x = FM(sin)(static_cast<fpu_t>(RDF32(bike + 0x2e99)));
    sw &= ~0x400u;
    WR32(bike + 0x2e8d, 0);
    x = x * static_cast<fpu_t>(RDF32(bike + 0x2e9d));
    WRF32(bike + 0x2e91, static_cast<float>(x));
    // call 0x45ca00 (registers as the translated code spills them)
    WR32A(frame - 4, LEAN_RET1);
    R.esp = frame - 4;
    R.ecx = bike;
    R.eax = eax;
    R.edx = k73 / 73u * 9u;
    R.ebx = ebx;
    R.esi = bike;
    F.sw = static_cast<uint16_t>(sw);
    f_0045ca00();
    x = ST(0);
    FPOP();
    sw = F.sw;
    x = x - static_cast<fpu_t>(RDF32(bike + 0x2ea5));
    x = x + static_cast<fpu_t>(RDF32(bike + 0x2e39));
    float s8 = static_cast<float>(x);
    const uint32_t target = f_bits(s8);   // [esp+0x14]
    if (target > 0x80000000u) {
        x = static_cast<fpu_t>(s8);
        do {
            x = x + TWO_PI;
            FCOM_L(sw, x, static_cast<fpu_t>(0.0));
        } while (sw & C0);
        s8 = static_cast<float>(x);
    }
    FCOM_L(sw, static_cast<fpu_t>(s8), TWO_PI);
    if (!(sw & C0C3)) {
        x = static_cast<fpu_t>(s8);
        do {
            x = x - TWO_PI;
            FCOM_L(sw, x, TWO_PI);
        } while (!(sw & C0C3));
        s8 = static_cast<float>(x);
    }
    const uint32_t ang = bike + 0x2ea1;
    if (RD32(ang) > 0x80000000u) {
        do WRF32(ang, static_cast<float>(static_cast<fpu_t>(RDF32(ang)) + TWO_PI));
        while (RD32(ang) > 0x80000000u);
    }
    FCOM_L(sw, static_cast<fpu_t>(RDF32(ang)), TWO_PI);
    if (!(sw & C0C3)) {
        do {
            WRF32(ang, static_cast<float>(static_cast<fpu_t>(RDF32(ang)) - TWO_PI));
            FCOM_L(sw, static_cast<fpu_t>(RDF32(ang)), TWO_PI);
        } while (!(sw & C0C3));
    }
    x = static_cast<fpu_t>(s8) - static_cast<fpu_t>(RDF32(ang));
    float sc = static_cast<float>(x);
    FCOM_L(sw, FM(fabs)(x), PI);
    if (!(sw & C0C3)) {
        const fpu_t A = static_cast<fpu_t>(RDF32(ang)), S = static_cast<fpu_t>(s8);
        FCOM_L(sw, A + PI, S);
        if (sw & C0) s8 = static_cast<float>(S - TWO_PI);
        const fpu_t S1 = static_cast<fpu_t>(s8);
        FCOM_L(sw, A - PI, S1);
        if (!(sw & C0C3)) s8 = static_cast<float>(S1 + TWO_PI);
        sc = static_cast<float>(static_cast<fpu_t>(s8) - static_cast<fpu_t>(RDF32(ang)));
    }
    const uint32_t k73b = RD32(bike + 0x78) * 73u;
    x = static_cast<fpu_t>(sc) / static_cast<fpu_t>(RDF32(k73b * 4u + 0x509298));
    x = x + static_cast<fpu_t>(RDF32(ang));
    WRF32(ang, static_cast<float>(x));
    x = static_cast<fpu_t>(RDF32(ang)) - static_cast<fpu_t>(bits_f(target));
    WRF32(bike + 0x2e8d, static_cast<float>(x));
    const uint32_t change = RD32(bike + 0x2e8d);
    // call 0x458da0 (stdcall, the change as its argument)
    WR32A(frame - 4, change);
    WR32A(frame - 8, LEAN_RET2);
    R.esp = frame - 8;
    R.eax = k73b;
    R.ecx = change;
    R.edx = ang;
    R.ebx = k73b / 73u * 9u;
    F.sw = static_cast<uint16_t>(sw);
    f_00458da0();
    x = ST(0);
    FPOP();
    sw = F.sw;
    WRF32(bike + 0x2e8d, static_cast<float>(x));
    const uint32_t k73c = RD32(bike + 0x78) * 73u;
    const uint32_t maxp = k73c * 4u + 0x5092a8;
    FCOM_L(sw, x, static_cast<fpu_t>(RDF32(maxp)));
    eax = fnstsw_ax(k73c, sw);
    if (!(sw & C0C3)) {
        eax = RD32(maxp);
        WR32(bike + 0x2e8d, eax);
    }
    x = -static_cast<fpu_t>(RDF32(maxp));
    FCOM_L(sw, x, static_cast<fpu_t>(RDF32(bike + 0x2e8d)));
    const float neg = static_cast<float>(x);
    eax = fnstsw_ax(eax, sw);
    if (!(sw & C0C3)) {
        eax = f_bits(neg);
        WR32(bike + 0x2e8d, eax);
    }
    R.eax = eax;
    R.ecx = maxp;
    R.edx = ang;
    R.ebx = ebx0;
    R.esi = esi0;
    R.esp = sp + 4;
    F.sw = static_cast<uint16_t>(sw);
}

}  // namespace

extern "C" int native_hot_48e180(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { gear_ratio(); return 1; }
    MEM_LOCAL();
    std::vector<Region> regions;
    if (const uint32_t out = RD32A(R.esp + 4)) regions.push_back({out, 4});
    check("gear ratio 0x48e180", gear_ratio, f_0048e180_t, regions, 1);
    static uint32_t n = 0;
    counted("gear ratio 0x48e180", n);
    return 1;
}

extern "C" int native_hot_45b4e0(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { angle_blend(); return 1; }
    MEM_LOCAL();
    const uint32_t out = R.ecx, a = RD32A(R.esp + 4), b = RD32A(R.esp + 8);
    const int32_t na = static_cast<int32_t>(RD32(a)), nb = static_cast<int32_t>(RD32(b));
    int32_t count = nb < na ? nb : na;
    if (count < 0) count = 0;
    check("angle blend 0x45b4e0", angle_blend, f_0045b4e0_t, {{out, 4 + 12u * static_cast<uint32_t>(count)}});
    static uint32_t n = 0;
    counted("angle blend 0x45b4e0", n);
    return 1;
}

extern "C" int native_hot_467930(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { ai_speed(false, 0); return 1; }
    MEM_LOCAL();
    // translated first (the hook's state advances once), then native with the hook's result replayed
    const uint32_t bike = R.ecx;
    const std::vector<Region> regions = {{bike + 0x114, 12}, {bike + 0xec, 4}, {bike + 0x120, 4}, {bike + 0x7c, 4}};
    Snapshot before = take(regions);
    f_00467930_t();
    const uint32_t hooked = RD32(bike + 0x118);
    Snapshot translated = take(regions);
    restore(before);
    ai_speed(true, hooked);
    Snapshot native = take(regions);
    compare("ai speed 0x467930", native, translated);
    restore(translated);
    static uint32_t n = 0;
    counted("ai speed 0x467930", n);
    return 1;
}

extern "C" int native_hot_466a80(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { bike_lean(); return 1; }
    const uint32_t bike = R.ecx;
    check("bike lean 0x466a80", bike_lean, f_00466a80_t, {{bike + 0x2e8d, 0x2ea5 - 0x2e8d + 4}});
    static uint32_t n = 0;
    counted("bike lean 0x466a80", n);
    return 1;
}
