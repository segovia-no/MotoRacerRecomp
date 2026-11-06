// Native versions of hot race functions, group A (docs/25-psp-port.md, the PSP hardware profile).
// Hooked through NATIVE_FUNCS (translate.py): f_<va> calls native_hot_<va>, which returns 0 to run the
// translated body f_<va>_t. MOTORACER_NATIVE=check runs both on the same state and compares
// (native_check.h). Floating point in fpu_t, in the x87 code's operation order (-ffp-contract=off).
#include <cstring>
#include <initializer_list>
#include <iterator>

#include "runtime.h"
#include "recomp_mem_local.h"
#include "native_check.h"

extern "C" void f_0046b120_t(void);
extern "C" void f_0046bdc0_t(void);
extern "C" void f_0046cd20_t(void);
extern "C" void f_0046c970_t(void);
extern "C" void f_00410010_t(void);
extern "C" void f_0040fd40_t(void);
extern "C" void f_0046cb40(void);
extern "C" void f_0046ca10(void);

namespace {
using namespace native;

// Guest pointers into the bike records (0x3209 bytes each, at odd addresses) are often unaligned: on
// the PSP an aligned load there would trap. The functions check their pointers once and use plain
// loads / stores only when all are 4-aligned (template A), unaligned-safe ones otherwise.
template <bool A> inline fpu_t ldf(uint32_t a) {
    MEM_LOCAL();
    if (A) return static_cast<fpu_t>(RDF32A(a));
    return static_cast<fpu_t>(RDF32(a));
}
template <bool A> inline void stf(uint32_t a, float v) {
    MEM_LOCAL();
    if (A) WRF32A(a, v); else WRF32(a, v);
}
template <bool A> inline void st32(uint32_t a, uint32_t v) {
    MEM_LOCAL();
    if (A) WR32A(a, v); else WR32(a, v);
}
inline fpu_t ld(uint32_t a) { MEM_LOCAL(); return static_cast<fpu_t>(RDF32A(a)); }   // known aligned (tables, constants)
inline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline float rnd(fpu_t v) { return static_cast<float>(v); }   // an fst / fstp to a float

// Check-mode bookkeeping: one counter per function, logged now and then.
void counted(const char *name, uint32_t &n) {
    if (++n == 1 || n % 20000 == 0)
        rt::log("native: %s checked %u times (mismatches so far: %llu)\n", name, n,
                static_cast<unsigned long long>(mismatches));
}

// ---- 0x410010: out = M * v + t (stdcall: out, v, M; 3x3 rows at M+0, translation at M+0x24) ----
// Results: eax = out, ecx / edx = the bits of out.x / out.z (the x86 copies the result through them).
template <bool A> void mat_apply_t(uint32_t sp, uint32_t out, uint32_t v, uint32_t m) {
    const fpu_t v0 = ldf<A>(v), v1 = ldf<A>(v + 4), v2 = ldf<A>(v + 8);
    const fpu_t x = ((ldf<A>(m + 0x18) * v2 + ldf<A>(m + 0xc) * v1) + ldf<A>(m) * v0) + ldf<A>(m + 0x24);
    const fpu_t y = ((ldf<A>(m + 4) * v0 + ldf<A>(m + 0x10) * v1) + ldf<A>(m + 0x1c) * v2) + ldf<A>(m + 0x28);
    const fpu_t z = ((ldf<A>(m + 0x20) * v2 + ldf<A>(m + 0x14) * v1) + ldf<A>(m + 8) * v0) + ldf<A>(m + 0x2c);
    const uint32_t bx = fbits(rnd(x)), by = fbits(rnd(y)), bz = fbits(rnd(z));
    st32<A>(out, bx);
    st32<A>(out + 4, by);
    st32<A>(out + 8, bz);
    R.ecx = bx;
    R.edx = bz;
}

void mat_apply() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, out = RD32A(sp + 4), v = RD32A(sp + 8), m = RD32A(sp + 12);
    if (!((out | v | m) & 3)) mat_apply_t<true>(sp, out, v, m);
    else mat_apply_t<false>(sp, out, v, m);
    R.eax = out;
    R.esp = sp + 16;
}

// ---- 0x40fd40: rotation matrix from three angles (thiscall: ecx = 4x3 matrix out, arg = angles) ----
// Each angle (radians) * 4096 / (pi / 2) truncated (_ftol) indexes the 16384-entry sine table
// 0x5fcb80: a = T[i & 0x3fff], b = T[(i - 0x1000) & 0x3fff]. Rows 0..2 as the x87 sequence computes
// them (including the float stores it reads back), row 3 = 0. eax = out, ecx = the bits of b2,
// edx = the high half of the last _ftol.
constexpr uint32_t SIN_TABLE = 0x5fcb80;

template <bool A> void rot_matrix_t(uint32_t out, uint32_t ang) {
    MEM_LOCAL();
    const fpu_t k = static_cast<fpu_t>(0x1.45f306dc9ce2bp+11);
    uint32_t hi = 0;
    auto idx = [&](uint32_t a) {
        const int64_t r = FTOL(ldf<A>(a) * k);
        hi = static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32);
        return static_cast<uint32_t>(r);
    };
    const uint32_t i0 = idx(ang), i1 = idx(ang + 4), i2 = idx(ang + 8);
    const fpu_t A0 = ld(SIN_TABLE + (i0 & 0x3fff) * 4), B0 = ld(SIN_TABLE + ((i0 - 0x1000) & 0x3fff) * 4);
    const fpu_t A1 = ld(SIN_TABLE + (i1 & 0x3fff) * 4), B1 = ld(SIN_TABLE + ((i1 - 0x1000) & 0x3fff) * 4);
    const fpu_t A2 = ld(SIN_TABLE + (i2 & 0x3fff) * 4);
    const uint32_t b2bits = RD32A(SIN_TABLE + ((i2 - 0x1000) & 0x3fff) * 4);
    float b2f;
    memcpy(&b2f, &b2bits, 4);
    const fpu_t B2 = b2f;
    const fpu_t Q = B1 * B0;
    const fpu_t L10 = rnd(Q);
    const fpu_t U = Q * A2 + B2 * A0;
    const fpu_t L0c = rnd(B1 * A0);
    const fpu_t Y1 = B2 * B0 - L0c * A2;
    const fpu_t nZ = -(B2 * A1);
    const fpu_t I = A0 * A2 - L10 * B2;
    const fpu_t J = A2 * B0;
    const fpu_t O = J + L0c * B2;
    const fpu_t nN = -(A1 * B0);
    const fpu_t P = A1 * A0;
    stf<A>(out, rnd(A1 * A2));
    stf<A>(out + 4, rnd(U));
    stf<A>(out + 8, rnd(Y1));
    stf<A>(out + 0xc, rnd(nZ));
    stf<A>(out + 0x10, rnd(I));
    stf<A>(out + 0x14, rnd(O));
    stf<A>(out + 0x18, rnd(B1));
    stf<A>(out + 0x1c, rnd(nN));
    stf<A>(out + 0x20, rnd(P));
    st32<A>(out + 0x24, 0);
    st32<A>(out + 0x28, 0);
    st32<A>(out + 0x2c, 0);
    R.ecx = b2bits;
    R.edx = hi;
}

void rot_matrix() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, out = R.ecx, ang = RD32A(sp + 4);
    if (!((out | ang) & 3)) rot_matrix_t<true>(out, ang);
    else rot_matrix_t<false>(out, ang);
    R.eax = out;
    R.esp = sp + 8;
}

// ---- 0x46c970: is a track position in a window (stdcall: pos, from, ahead, behind) ----
// Positions wrap at [0x52c10c] (0x40ec30: add / subtract the count until in range). Returns 1 when
// pos lies between wrap(from - behind) and wrap(from + ahead), allowing for the wrap. ecx = pos;
// edx = the count where the x86 reloads it, else unchanged.
constexpr uint32_t TRACK_COUNT = 0x52c10c;

inline int32_t wrap_pos(int32_t x) {   // 0x40ec30
    MEM_LOCAL();
    while (x < 0) x += static_cast<int32_t>(RD32A(TRACK_COUNT));
    for (int32_t n = static_cast<int32_t>(RD32A(TRACK_COUNT)); x >= n; n = static_cast<int32_t>(RD32A(TRACK_COUNT))) x -= n;
    return x;
}

void in_window() {
    MEM_LOCAL();
    const uint32_t sp = R.esp;
    R.esp = sp + 20;
    const int32_t n = static_cast<int32_t>(RD32A(TRACK_COUNT));
    if (!n) { R.eax = 0; return; }
    const int32_t pos = static_cast<int32_t>(RD32A(sp + 4)), from = static_cast<int32_t>(RD32A(sp + 8));
    const int32_t ahead = static_cast<int32_t>(RD32A(sp + 12)), behind = static_cast<int32_t>(RD32A(sp + 16));
    const int32_t lo = wrap_pos(from - behind), hi = wrap_pos(ahead + from);
    R.ecx = static_cast<uint32_t>(pos);
    bool in;
    if (hi < lo) {   // the window wraps
        if (!(lo > pos)) {
            R.edx = static_cast<uint32_t>(n);   // the count, reloaded
            if (n > pos) { R.eax = 1; return; }
        }
        in = !(pos < 0) && hi > pos;
    } else {
        in = !(lo > pos) && hi > pos;
    }
    R.eax = in ? 1 : 0;
}

// ---- 0x46bdc0: orthonormal frame looking from a to b (thiscall: ecx = 4x3 out; args a, b) ----
// d = b - a (floats), normalized (0x40fcb0: sqrt((x*x + y*y) + z*z), unless 0); side = (d.z, 0, -d.x)
// normalized by sqrt(d.z^2 + d.x^2); up = (-d.x*d.y, d.z^2 - (-d.x)*d.x, -(d.z*d.y)) normalized;
// out rows: side, up, d, 0. Every float store the x87 code makes is rounded here as well.
// eax = out, ecx / edx = the bits of d.y / d.z, x87 status = the last comparison with 0.
constexpr uint32_t ZERO_F = 0x4d0e54;

template <bool A> void look_frame_t(uint32_t out, uint32_t a, uint32_t b) {
    uint32_t sw = F.sw;
    const fpu_t zero = ld(ZERO_F);
    float d0 = rnd(ldf<A>(b) - ldf<A>(a)), d1 = rnd(ldf<A>(b + 4) - ldf<A>(a + 4)), d2 = rnd(ldf<A>(b + 8) - ldf<A>(a + 8));
    {
        const fpu_t len = FM(sqrt)((static_cast<fpu_t>(d0) * d0 + static_cast<fpu_t>(d1) * d1) + static_cast<fpu_t>(d2) * d2);
        FCOM_L(sw, len, zero);
        if (!(sw & 0x4000u)) {
            const fpu_t r = static_cast<fpu_t>(1) / static_cast<fpu_t>(rnd(len));
            d0 = rnd(d0 * r);
            d1 = rnd(d1 * r);
            d2 = rnd(r * d2);
        }
    }
    const float m0 = rnd(-static_cast<fpu_t>(d0));   // -d.x, stored
    const fpu_t P1 = static_cast<fpu_t>(m0) * d1, P2 = static_cast<fpu_t>(d2) * d2, P3 = static_cast<fpu_t>(m0) * d0;
    const float u0 = rnd(P1), u1 = rnd(P2 - P3), u2 = rnd(-(static_cast<fpu_t>(d2) * d1));
    float s0 = d2, s2 = m0;
    {
        const fpu_t n1 = FM(sqrt)(P2 + static_cast<fpu_t>(m0) * m0);
        FCOM_L(sw, n1, zero);
        if (!(sw & 0x4000u)) {
            const fpu_t r = static_cast<fpu_t>(1) / static_cast<fpu_t>(rnd(n1));
            s0 = rnd(static_cast<fpu_t>(d2) * r);
            s2 = rnd(r * m0);
        }
    }
    float v0 = u0, v1 = u1, v2 = u2;
    {
        const fpu_t n2 = FM(sqrt)((static_cast<fpu_t>(u1) * u1 + static_cast<fpu_t>(u2) * u2) + static_cast<fpu_t>(u0) * u0);
        FCOM_L(sw, n2, zero);
        if (!(sw & 0x4000u)) {
            const fpu_t r = static_cast<fpu_t>(1) / static_cast<fpu_t>(rnd(n2));
            v0 = rnd(u0 * r);
            v1 = rnd(u1 * r);
            v2 = rnd(r * u2);
        }
    }
    stf<A>(out, s0);
    st32<A>(out + 4, 0);
    stf<A>(out + 8, s2);
    stf<A>(out + 0xc, v0);
    stf<A>(out + 0x10, v1);
    stf<A>(out + 0x14, v2);
    stf<A>(out + 0x18, d0);
    stf<A>(out + 0x1c, d1);
    stf<A>(out + 0x20, d2);
    st32<A>(out + 0x24, 0);
    st32<A>(out + 0x28, 0);
    st32<A>(out + 0x2c, 0);
    F.sw = static_cast<uint16_t>(sw);
    R.ecx = fbits(d1);
    R.edx = fbits(d2);
}

void look_frame() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, out = R.ecx, a = RD32A(sp + 4), b = RD32A(sp + 8);
    if (!((out | a | b) & 3)) look_frame_t<true>(out, a, b);
    else look_frame_t<false>(out, a, b);
    R.eax = out;
    R.esp = sp + 12;
}

// ---- 0x46cd20: an AI bike picks the bike it follows (thiscall: ecx = bike; arg = dt, ret 4) ----
// Bikes are 0x3209-byte records (array 0x5812xx, count [0x509bac], index 0 = the player); this one's
// track position +0x31c1, target bike +0x31d9, own index +0x2dd0. Scans the bikes for one in a
// window ahead / behind on the track (0x46c970), measures distances (0x46c9f0, |float| _ftol),
// speed comparisons ((int) * 0.2777 as float, the x87 three-way compare), then eases speed +0xec /
// +0x1a4 and the gap +0x31d5. Rare branches call 0x46cb40 / 0x46ca10 (translated). The caller
// (0x46b1d0) reloads eax / ecx / edx afterwards; the x87 status word is the last comparison's.
constexpr uint32_t BIKE_COUNT = 0x509bac, BIKE_POS = 0x5875e2, BIKE_SEG = 0x5875fa, BIKE_STRIDE = 0x3209;
constexpr uint32_t BIKE_SPEED = 0x58132c, BIKE_TRACKPOS = 0x5843d9;
constexpr uint32_t AI_MODE = 0x4dc144, SPEED_SCALE = 0x4dc140;

inline uint32_t rd(uint32_t a) { MEM_LOCAL(); return RD32(a); }   // bike fields: any alignment
inline int32_t rdi(uint32_t a) { return static_cast<int32_t>(rd(a)); }

int32_t window_call(uint32_t pos, uint32_t from, int32_t ahead, int32_t behind) {   // 0x46c970, returns eax
    const int32_t n = static_cast<int32_t>(rd(TRACK_COUNT));
    if (!n) return 0;
    const int32_t p = static_cast<int32_t>(pos);
    const int32_t lo = wrap_pos(static_cast<int32_t>(from) - behind), hi = wrap_pos(ahead + static_cast<int32_t>(from));
    if (hi < lo) {
        if (!(lo > p) && n > p) return 1;
        return (!(p < 0) && hi > p) ? 1 : 0;
    }
    return (!(lo > p) && hi > p) ? 1 : 0;
}

inline int32_t half(int32_t v) { return v / 2; }   // cdq; sub eax, edx; sar eax, 1

// (int) bike speed vs this bike's +0x114, both * 0.2777 (x87, then float stores): 1, -1 or 0 as
// 0x46ceb4 / 0x46d02e compute it; `strict_other` = the 0x46d0ea variant (C0|C3 on the second test).
int32_t speed_cmp(uint32_t bike, uint32_t other, uint32_t &sw, float &l14, bool z_variant) {
    const fpu_t k = static_cast<fpu_t>(0.2777777777777778);
    const fpu_t rk = static_cast<fpu_t>(rdi(BIKE_SPEED + other * BIKE_STRIDE)) * k;
    const fpu_t sk = static_cast<fpu_t>(rdi(bike + 0x114)) * k;
    l14 = rnd(rk);
    FCOM_L(sw, sk, static_cast<fpu_t>(l14));
    const float l10 = rnd(sk);
    if (sw & 0x100u) return 1;
    if (!z_variant) {
        FCOM_L(sw, static_cast<fpu_t>(l14), static_cast<fpu_t>(l10));
        return (sw & 0x100u) ? -1 : 0;
    }
    FCOM_L(sw, static_cast<fpu_t>(l10), static_cast<fpu_t>(l14));
    return (sw & 0x4100u) ? 0 : -1;
}

inline uint32_t abs_float_bits(int32_t v) {   // fild; fstp float; and 0x7fffffff
    return fbits(rnd(static_cast<fpu_t>(v))) & 0x7fffffffu;
}

uint32_t paths[8];   // check mode: which branches the race exercised (logged with the counter)

void pick_target() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, bike = R.ecx, frame = sp - 0x2c;
    uint32_t sw = F.sw;
    int32_t p10, p14, p20, p24;
    if (!rd(AI_MODE)) { p10 = 8; p24 = 0; p14 = 12; p20 = 12; }
    else { p14 = 8; p20 = 0; p10 = 12; p24 = 12; }
    const uint32_t own = rd(bike + 0x2dd0);
    auto count = [] { return static_cast<int32_t>(rd(BIKE_COUNT)); };
    auto call_guest = [&](void (*fn)(), uint32_t ret, std::initializer_list<uint32_t> args) {
        uint32_t esp = frame;
        for (auto it = std::rbegin(args); it != std::rend(args); ++it) { esp -= 4; WR32A(esp, *it); }
        esp -= 4;
        WR32A(esp, ret);
        R.esp = esp;
        R.ecx = bike;
        F.sw = static_cast<uint16_t>(sw);
        fn();
        sw = F.sw;
        return R.eax;
    };
    int32_t target = rdi(bike + 0x31d9);
    if (target > 0) {
        const uint32_t want = static_cast<uint32_t>(target - 1);
        bool found = false;
        for (int32_t i = 1; count() > i; i++) {
            const uint32_t rec = BIKE_SEG + static_cast<uint32_t>(i - 1) * BIKE_STRIDE;
            if (own == static_cast<uint32_t>(i) || rd(rec) != want) continue;
            if (window_call(rd(rec - 0x18), rd(bike + 0x31c1), half(p14), half(p10))) { found = true; break; }
        }
        if (!found) {
            WR32(bike + 0x31d9, rd(bike + 0x31d9) - 1);
            paths[0]++;
            goto tail;
        }
    }
    {
        const uint32_t cur = rd(bike + 0x31d9);
        int32_t best = 0x7ffffffe, best_i = -1;
        for (int32_t i = 1; count() > i; i++) {
            const uint32_t rec = BIKE_POS + static_cast<uint32_t>(i - 1) * BIKE_STRIDE;
            if (own == static_cast<uint32_t>(i)) continue;
            const uint32_t pos = rd(rec);
            if (rd(rec + 0x18) != cur) continue;
            const uint32_t from = rd(bike + 0x31c1);
            if (!window_call(pos, from, p20, p24)) continue;
            int32_t d = static_cast<int32_t>(pos) - static_cast<int32_t>(from);   // 0x46c9f0
            if (!(static_cast<int32_t>(pos) >= static_cast<int32_t>(from))) d += static_cast<int32_t>(rd(TRACK_COUNT));
            const uint32_t ab = abs_float_bits(d);
            float af;
            memcpy(&af, &ab, 4);
            const int32_t dist = static_cast<int32_t>(FTOL(static_cast<fpu_t>(af)));
            if (dist < best) { best = dist; best_i = i; }
        }
        if (best_i < 0) goto tail;
        float l14;
        if (speed_cmp(bike, static_cast<uint32_t>(best_i), sw, l14, false) >= 0) goto tail;
        int32_t found = 0;
        for (;;) {   // 0x46cf0a: how many bikes are in the +-12 window of this one, in rounds
            bool hit = false;
            for (int32_t i = 1; count() > i; i++) {
                const uint32_t rec = BIKE_SEG + static_cast<uint32_t>(i - 1) * BIKE_STRIDE;
                if (own == static_cast<uint32_t>(i) || rd(rec) != static_cast<uint32_t>(found)) continue;
                if (window_call(rd(rec - 0x18), rd(bike + 0x31c1), 12, 12)) { hit = true; break; }
            }
            if (!hit) break;
            if (++found >= 3) { found = -1; break; }
        }
        paths[1]++;
        if (found >= 0 && abs_float_bits(found - rdi(bike + 0x31d9)) < 0x40000000u) {
            paths[2]++;
            WR32(bike + 0x31d9, static_cast<uint32_t>(found));
            goto tail;
        }
        uint32_t sel = static_cast<uint32_t>(best_i);   // [esp+0x18], passed by address to 0x46cb40
        WR32A(frame + 0x18, sel);
        const int32_t r = static_cast<int32_t>(call_guest(f_0046cb40, 0x46cfbc, {static_cast<uint32_t>(p20), static_cast<uint32_t>(p24), frame + 0x18}));
        sel = RD32A(frame + 0x18);
        paths[3]++;
        const int32_t t = rdi(bike + 0x31d9);
        bool keep = false;   // true: the 0x46d0c5 path (sel = best)
        if (t == r || r < 0 || abs_float_bits(r - t) >= 0x40000000u) keep = true;
        else {
            const int32_t q = static_cast<int32_t>(call_guest(f_0046ca10, 0x46d013, {static_cast<uint32_t>(t), static_cast<uint32_t>(p24), static_cast<uint32_t>(half(p20))}));
            float l14b;
            if (q >= 0 && speed_cmp(bike, static_cast<uint32_t>(q), sw, l14b, false) > 0) keep = true;
            else {
                int32_t d = rdi(BIKE_TRACKPOS + sel * BIKE_STRIDE);
                const int32_t from = rdi(bike + 0x31c1);
                d = d >= from ? d - from : d - from + rdi(TRACK_COUNT);
                d = static_cast<int32_t>(static_cast<uint32_t>(d) * rd(SPEED_SCALE));
                if (d > 6) { WR32(bike + 0x31d9, static_cast<uint32_t>(r)); paths[4]++; }
                else keep = true;
            }
        }
        if (keep) sel = static_cast<uint32_t>(best_i);
        if (speed_cmp(bike, sel, sw, l14, true) >= 0) goto tail;
        paths[5]++;   // 0x46d13e: ease the speed towards the target's
        const fpu_t e = ldf<false>(bike + 0xec);
        const float e1 = rnd((static_cast<fpu_t>(l14) - e) * static_cast<fpu_t>(ld(0x4d1158)) + e);
        const float e2 = rnd(static_cast<fpu_t>(e1) * static_cast<fpu_t>(0.95));
        stf<false>(bike + 0xec, e2);
        stf<false>(bike + 0x1a4, e2);
        const float diff = rnd(e - static_cast<fpu_t>(e1) * static_cast<fpu_t>(0.95));
        float ad;
        const uint32_t adb = fbits(diff) & 0x7fffffffu;
        memcpy(&ad, &adb, 4);
        FCOM_L(sw, static_cast<fpu_t>(ad), static_cast<fpu_t>(11.11111111111111));
        if (!(sw & 0x4100u)) { WR32(bike + 0x2dc8, 2); paths[6]++; }
    }
tail:
    // 0x46d1ad: the gap +0x31d5 moves towards target * 240 by dt * 1.0667
    if (rd(bike + 0x2db8)) {
        paths[7]++;
        const int32_t goal_i = static_cast<int32_t>(rd(bike + 0x31d9) * 240u);
        const fpu_t goal = static_cast<fpu_t>(goal_i);
        const fpu_t gap = ldf<false>(bike + 0x31d5);
        const fpu_t step = static_cast<fpu_t>(RDF32A(sp + 4)) * static_cast<fpu_t>(1.0666666666666667);
        FCOM_L(sw, goal, gap);
        const float l10 = rnd(goal);
        if (!(sw & 0x4100u)) {
            const float g = rnd(step + gap);
            stf<false>(bike + 0x31d5, g);
            FCOM_L(sw, static_cast<fpu_t>(g), static_cast<fpu_t>(l10));
            if (!(sw & 0x4100u)) stf<false>(bike + 0x31d5, l10);
        } else {
            FCOM_L(sw, gap, static_cast<fpu_t>(l10));
            if (!(sw & 0x4100u)) {
                const float g = rnd(gap - step);
                stf<false>(bike + 0x31d5, g);
                FCOM_L(sw, static_cast<fpu_t>(g), static_cast<fpu_t>(l10));
                if (sw & 0x100u) stf<false>(bike + 0x31d5, l10);
            }
        }
    }
    F.sw = static_cast<uint16_t>(sw);
    R.esp = sp + 8;
}

uint32_t n_410010, n_40fd40, n_46c970, n_46bdc0, n_46cd20;

}  // namespace

extern "C" int native_hot_410010(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { mat_apply(); return 1; }
    check("mat apply 0x410010", mat_apply, f_00410010_t, {{RD32A(R.esp + 4), 12}});
    counted("mat apply 0x410010", n_410010);
    return 1;
}

extern "C" int native_hot_40fd40(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { rot_matrix(); return 1; }
    check("rotation matrix 0x40fd40", rot_matrix, f_0040fd40_t, {{R.ecx, 48}});
    counted("rotation matrix 0x40fd40", n_40fd40);
    return 1;
}

extern "C" int native_hot_46c970(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { in_window(); return 1; }
    check("track window 0x46c970", in_window, f_0046c970_t, {});
    counted("track window 0x46c970", n_46c970);
    return 1;
}

extern "C" int native_hot_46bdc0(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { look_frame(); return 1; }
    check("look frame 0x46bdc0", look_frame, f_0046bdc0_t, {{R.ecx, 48}});
    counted("look frame 0x46bdc0", n_46bdc0);
    return 1;
}

extern "C" int native_hot_46b120(void) { return 0; }   // not native yet: translated
extern "C" int native_hot_46cd20(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { pick_target(); return 1; }
    check_abi("pick target 0x46cd20", pick_target, f_0046cd20_t, {{R.ecx, BIKE_STRIDE}});
    counted("pick target 0x46cd20", n_46cd20);
    if (n_46cd20 % 20000 == 0)
        rt::log("native: pick target paths %u %u %u %u %u %u %u %u\n", paths[0], paths[1], paths[2], paths[3], paths[4],
                paths[5], paths[6], paths[7]);
    return 1;
}
