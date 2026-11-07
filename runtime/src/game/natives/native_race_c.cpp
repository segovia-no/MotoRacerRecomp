// Native versions of hot race functions, group C (docs/notes/25-psp-port.md, the PSP hardware profile).
// Hooked through NATIVE_FUNCS (translate.py): f_<va> calls native_hot_<va>, which returns 0 to run the
// translated body f_<va>_t. MOTORACER_NATIVE=check runs both on the same state and compares
// (native_check.h). Floating point in fpu_t, in the x87 code's operation order (-ffp-contract=off).
#include <cstring>

#include "runtime.h"
#include "recomp_mem_local.h"
#include "game/natives/native_check.h"

extern "C" void f_0046bf80_t(void);
extern "C" void f_0046b120_t(void);
extern "C" void f_00469c60_t(void);
extern "C" void f_00461aa0_t(void);
extern "C" void f_00490220_t(void);
extern "C" void f_00403a80_t(void);
extern "C" void f_004b0f60_t(void);
extern "C" void f_0040c960_t(void);

#include <cmath>
#include <initializer_list>

namespace {
using namespace native;

constexpr uint32_t PLANE_CALLS = 0x709378;   // plane-test counter (0x4b10c0 and 0x4b0f60 count)

inline float f32(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
inline uint32_t bits32(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
#define LD32(a) static_cast<fpu_t>(RDF32(a))   // fld dword (MEM_LOCAL in scope)

// x87 status word as fnstsw leaves it in ax (TOP from F.top)
inline uint16_t fstsw(uint32_t sw) { return static_cast<uint16_t>((sw & ~0x3800u) | ((F.top & 7u) << 11)); }

// per-function check statistics: calls checked, calls with a mismatch (mismatch_base: the global
// count before the check)
struct Stat { uint32_t n = 0, bad = 0; };
void count_check(const char *name, Stat &st, uint64_t mismatch_base) {
    st.n++;
    if (mismatches != mismatch_base) st.bad++;
    if (st.n == 1 || st.n % 20000 == 0)
        rt::log("native: %s checked %u times, %u with a mismatch\n", name, st.n, st.bad);
}

// ---- 0x403a80: 3x4 matrix product (stdcall dst, a, b; ret 0xc) ----
// dst = a * b for the game's 3x3 rotation + translation matrices (12 floats: rows of 3, then the
// translation, which is a's translation through b's rotation plus b's). All twelve results are
// computed before the first store (dst may be a or b). Leaves eax = dst, ecx / edx = the bits of the
// last two floats (the x86's copy loop), x87 unchanged.
void mat_mul() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, dst = RD32A(sp + 4), a = RD32A(sp + 8), b = RD32A(sp + 12);
#define LD(x) static_cast<fpu_t>(RDF32(x))
    float r[12];
    const fpu_t t1 = LD(a);
    const fpu_t t2 = t1 * LD(b+4);
    const fpu_t t3 = LD(a+8);
    const fpu_t t4 = t3 * LD(b + 0x1c);
    const fpu_t t5 = LD(a+4);
    const fpu_t t6 = t5 * LD(b + 0x10);
    const fpu_t t7 = t2 + t4;
    const fpu_t t8 = LD(a);
    const fpu_t t9 = t8 * LD(b+8);
    const fpu_t t10 = t7 + t6;
    const fpu_t t11 = LD(b + 0x14);
    const fpu_t t12 = t11 * LD(a+4);
    const fpu_t t13 = LD(a+8);
    const fpu_t t14 = t13 * LD(b + 0x20);
    r[1] = static_cast<float>(t10);
    const fpu_t t15 = t9 + t12;
    const fpu_t t16 = LD(b);
    const fpu_t t17 = t16 * LD(a + 0xc);
    const fpu_t t18 = t15 + t14;
    const fpu_t t19 = LD(a + 0x14);
    const fpu_t t20 = t19 * LD(b + 0x18);
    const fpu_t t21 = LD(b + 0xc);
    const fpu_t t22 = t21 * LD(a + 0x10);
    r[2] = static_cast<float>(t18);
    const fpu_t t23 = t17 + t20;
    const fpu_t t24 = LD(a + 0x14);
    const fpu_t t25 = t24 * LD(b + 0x1c);
    const fpu_t t26 = t23 + t22;
    const fpu_t t27 = LD(b+4);
    const fpu_t t28 = t27 * LD(a + 0xc);
    const fpu_t t29 = LD(b + 0x10);
    const fpu_t t30 = t29 * LD(a + 0x10);
    r[3] = static_cast<float>(t26);
    const fpu_t t31 = t25 + t28;
    const fpu_t t32 = LD(a + 0x14);
    const fpu_t t33 = t32 * LD(b + 0x20);
    const fpu_t t34 = t31 + t30;
    const fpu_t t35 = LD(b + 0x14);
    const fpu_t t36 = t35 * LD(a + 0x10);
    const fpu_t t37 = LD(b+8);
    const fpu_t t38 = t37 * LD(a + 0xc);
    r[4] = static_cast<float>(t34);
    const fpu_t t39 = t33 + t36;
    const fpu_t t40 = LD(b);
    const fpu_t t41 = t40 * LD(a + 0x18);
    const fpu_t t42 = t39 + t38;
    const fpu_t t43 = LD(a + 0x1c);
    const fpu_t t44 = t43 * LD(b + 0xc);
    const fpu_t t45 = LD(a + 0x20);
    const fpu_t t46 = t45 * LD(b + 0x18);
    r[5] = static_cast<float>(t42);
    const fpu_t t47 = t41 + t44;
    const fpu_t t48 = LD(b+4);
    const fpu_t t49 = t48 * LD(a + 0x18);
    const fpu_t t50 = t47 + t46;
    const fpu_t t51 = LD(b + 0x1c);
    const fpu_t t52 = t51 * LD(a + 0x20);
    const fpu_t t53 = LD(a + 0x1c);
    const fpu_t t54 = t53 * LD(b + 0x10);
    r[6] = static_cast<float>(t50);
    const fpu_t t55 = t49 + t52;
    const fpu_t t56 = LD(b + 0x14);
    const fpu_t t57 = t56 * LD(a + 0x1c);
    const fpu_t t58 = t55 + t54;
    const fpu_t t59 = LD(a + 0x18);
    const fpu_t t60 = t59 * LD(b+8);
    const fpu_t t61 = LD(b + 0x20);
    const fpu_t t62 = t61 * LD(a + 0x20);
    r[7] = static_cast<float>(t58);
    const fpu_t t63 = t57 + t60;
    const fpu_t t64 = LD(b);
    const fpu_t t65 = t64 * LD(a + 0x24);
    const fpu_t t66 = t63 + t62;
    const fpu_t t67 = LD(a + 0x2c);
    const fpu_t t68 = t67 * LD(b + 0x18);
    const fpu_t t69 = LD(b + 0xc);
    const fpu_t t70 = t69 * LD(a + 0x28);
    r[8] = static_cast<float>(t66);
    const fpu_t t71 = t65 + t68;
    const fpu_t t72 = LD(a + 0x24);
    const fpu_t t73 = t72 * LD(b+4);
    const fpu_t t74 = t71 + t70;
    const fpu_t t75 = LD(a + 0x2c);
    const fpu_t t76 = t75 * LD(b + 0x1c);
    const fpu_t t77 = t74 + LD(b + 0x24);
    const fpu_t t78 = LD(b + 0x10);
    const fpu_t t79 = t78 * LD(a + 0x28);
    r[9] = static_cast<float>(t77);
    const fpu_t t80 = LD(a + 0x24);
    const fpu_t t81 = t80 * LD(b+8);
    const fpu_t t82 = t73 + t76;
    const fpu_t t83 = LD(b + 0x14);
    const fpu_t t84 = t83 * LD(a + 0x28);
    const fpu_t t85 = t82 + t79;
    const fpu_t t86 = LD(a + 0x2c);
    const fpu_t t87 = t86 * LD(b + 0x20);
    const fpu_t t88 = t85 + LD(b + 0x28);
    const fpu_t t89 = LD(b);
    const fpu_t t90 = t89 * LD(a);
    r[10] = static_cast<float>(t88);
    const fpu_t t91 = t81 + t84;
    const fpu_t t92 = LD(a+4);
    const fpu_t t93 = t92 * LD(b + 0xc);
    const fpu_t t94 = t91 + t87;
    const fpu_t t95 = LD(a+8);
    const fpu_t t96 = t95 * LD(b + 0x18);
    const fpu_t t97 = t94 + LD(b + 0x2c);
    r[11] = static_cast<float>(t97);
    const fpu_t t98 = t93 + t90;
    const fpu_t t99 = t96 + t98;
    r[0] = static_cast<float>(t99);
#undef LD
    for (int i = 0; i < 12; i++) WRF32(dst + 4 * i, r[i]);
    R.eax = dst;
    R.ecx = bits32(r[11]);
    R.edx = bits32(r[10]);
    R.esp = sp + 16;
}

// ---- 0x490220: an angle of an object (thiscall, no stack args; st0 = result) ----
// From the sums of three vector pairs of the object: u = ([+0x18]+[+0x24], [+0x1c]+[+0x28],
// [+0x2c]+[+0x20]), w = ([+0xc]+[+0x24], [+0x10]+[+0x28]); len = sqrt([0x4d1e48]) * |u|; below
// [0x4d1e50] the result is +-pi/2 ([0x4d1e5c] / [0x4d1e60]) by the sign of u.x, else
// atan2((w.x * u.y - u.x * w.y) / len, u.z / len). Locals rounded to float where the x86 stores them.
fpu_t angle_core(uint32_t c, uint32_t &fsw, uint32_t &eax) {
    MEM_LOCAL();
    const float l4 = static_cast<float>(LD32(c + 0x18) + LD32(c + 0x24));
    const float l8 = static_cast<float>(LD32(c + 0x1c) + LD32(c + 0x28));
    const float lc = static_cast<float>(LD32(c + 0x2c) + LD32(c + 0x20));
    const float l10 = static_cast<float>(LD32(c + 0xc) + LD32(c + 0x24));
    const float l14 = static_cast<float>(LD32(c + 0x10) + LD32(c + 0x28));
    const fpu_t t12 = static_cast<fpu_t>(l8) * static_cast<fpu_t>(l8);
    const fpu_t t14 = static_cast<fpu_t>(lc) * static_cast<fpu_t>(lc);
    const fpu_t t16 = static_cast<fpu_t>(l4) * static_cast<fpu_t>(l4);
    const fpu_t t17 = t12 + t14;
    const fpu_t t19 = FM(sqrt)(static_cast<fpu_t>(RDF64(0x4d1e48)));
    const fpu_t t20 = t17 + t16;
    const float l0a = static_cast<float>(FM(sqrt)(t20));
    const fpu_t t22 = t19 * static_cast<fpu_t>(l0a);
    const float l0 = static_cast<float>(t22);
    FCOM_L(fsw, FM(fabs)(t22), static_cast<fpu_t>(RDF64(0x4d1e50)));
    eax = (eax & 0xffff0000u) | fstsw(fsw);
    fpu_t result;
    if (fsw & 0x100u) {
        eax = bits32(l4);
        result = static_cast<fpu_t>(RDF32(static_cast<int32_t>(eax) > 0 ? 0x4d1e5c : 0x4d1e60));
    } else {
        const fpu_t t2 = -static_cast<fpu_t>(l4);
        const fpu_t t4 = static_cast<fpu_t>(l10) * static_cast<fpu_t>(l8);
        const fpu_t t6 = t2 * static_cast<fpu_t>(l14);
        const fpu_t t8 = static_cast<fpu_t>(lc) / static_cast<fpu_t>(l0);
        const fpu_t t9 = t6 + t4;
        const fpu_t t10 = t9 / static_cast<fpu_t>(l0);
        result = FM(atan2)(t10, t8);
    }
    return result;
}

void angle() {
    const uint32_t sp = R.esp;
    uint32_t fsw = F.sw, eax = R.eax;
    const fpu_t result = angle_core(R.ecx, fsw, eax);
    R.eax = eax;
    R.esp = sp + 4;
    F.sw = static_cast<uint16_t>(fsw);
    F.top = (F.top - 1u) & 7u;
    F.st[F.top] = result;
}

// ---- 0x4b0f60: segment / plane intersection (thiscall plane, stdcall out, p1 (3 floats), p2; ret 0x1c) ----
// out = p1 if p1 and p2 are within [0x4d2090] on every axis or the segment is parallel to the plane
// (n . d == [0x4d2094]); else p1 + d * -(n . p1 + n.w) / (n . d), d = p2 - p1 (counted in [0x709378]).
// Leaves eax = out, ecx / edx = bits of out.x / out.z, the x87 status of the last compare.
void seg_plane(uint32_t plane, const uint32_t p1[3], const uint32_t p2[3], uint32_t out[3], uint32_t &fsw) {
    MEM_LOCAL();
    const fpu_t eps = static_cast<fpu_t>(RDF32(0x4d2090));
    bool near = true;
    for (int i = 0; i < 3 && near; i++) {
        FCOM_L(fsw, FM(fabs)(static_cast<fpu_t>(f32(p1[i])) - static_cast<fpu_t>(f32(p2[i]))), eps);
        near = (fsw & 0x4100u) != 0;
    }
    if (!near) {
        const float dy = static_cast<float>(static_cast<fpu_t>(f32(p2[1])) - static_cast<fpu_t>(f32(p1[1])));
        const float dz = static_cast<float>(static_cast<fpu_t>(f32(p2[2])) - static_cast<fpu_t>(f32(p1[2])));
        const fpu_t t6 = static_cast<fpu_t>(f32(p2[0])) - static_cast<fpu_t>(f32(p1[0]));
        const float dx = static_cast<float>(t6);
        const fpu_t t13 = (t6 * LD32(plane) + LD32(plane + 4) * static_cast<fpu_t>(dy)) +
                          LD32(plane + 8) * static_cast<fpu_t>(dz);
        FCOM_L(fsw, t13, static_cast<fpu_t>(RDF32(0x4d2094)));
        const float den = static_cast<float>(t13);
        if (!(fsw & 0x4000u)) {
            WR32A(PLANE_CALLS, RD32A(PLANE_CALLS) + 1);
            const fpu_t x = static_cast<fpu_t>(f32(p1[0])), y = static_cast<fpu_t>(f32(p1[1])),
                        z = static_cast<fpu_t>(f32(p1[2]));
            const fpu_t t2 = LD32(plane) * x, t4 = LD32(plane + 8) * z, t6b = LD32(plane + 4) * y;
            const fpu_t t11 = ((t2 + t4) + t6b) + LD32(plane + 0xc);
            const fpu_t t = -(t11 / static_cast<fpu_t>(den));
            out[0] = bits32(static_cast<float>(static_cast<fpu_t>(dx) * t + x));
            out[1] = bits32(static_cast<float>(static_cast<fpu_t>(dy) * t + y));
            out[2] = bits32(static_cast<float>(t * static_cast<fpu_t>(dz) + z));
            return;
        }
    }
    out[0] = p1[0];
    out[1] = p1[1];
    out[2] = p1[2];
}

void seg_plane_call() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, outp = RD32A(sp + 4);
    const uint32_t p1[3] = {RD32A(sp + 8), RD32A(sp + 12), RD32A(sp + 16)};
    const uint32_t p2[3] = {RD32A(sp + 20), RD32A(sp + 24), RD32A(sp + 28)};
    uint32_t out[3], fsw = F.sw;
    seg_plane(R.ecx, p1, p2, out, fsw);
    WR32(outp, out[0]);
    WR32(outp + 4, out[1]);
    WR32(outp + 8, out[2]);
    F.sw = static_cast<uint16_t>(fsw);
    R.eax = outp;
    R.ecx = out[0];
    R.edx = out[2];
    R.esp = sp + 32;
}

// ---- 0x40c960: point on a sector's plane (thiscall this = {sectors, count}; out, index, point; ret 0xc) ----
// count <= 0: out = 0. Else the segment from the point to the point raised by [0x4d02d4] (y) is cut
// with the sector's plane at +0x80 (point on or above the plane at +0xa0, 0x4b10c0) or +0x90 (below).
// Sectors are 252 bytes. Leaves eax = out, ecx / edx = bits of out.x / out.z.
void sector_point_core(uint32_t self, uint32_t idx, uint32_t pt, uint32_t out[3], uint32_t &fsw) {
    MEM_LOCAL();
    out[0] = out[1] = out[2] = 0;
    if (static_cast<int32_t>(RD32(self + 4)) > 0) {
        const uint32_t sector = RD32(self) + idx * 252u;
        const uint32_t p[3] = {RD32(pt), RD32(pt + 4), RD32(pt + 8)};
        const uint32_t q[3] = {p[0], bits32(static_cast<float>(static_cast<fpu_t>(f32(p[1])) +
                                                               static_cast<fpu_t>(RDF32(0x4d02d4)))), p[2]};
        // 0x4b10c0 on the plane at +0xa0, in its x87 order; counted
        const uint32_t pl = sector + 0xa0;
        WR32A(PLANE_CALLS, RD32A(PLANE_CALLS) + 1);
        const fpu_t a = LD32(pl + 4) * LD32(pt + 4), b = LD32(pl + 8) * LD32(pt + 8), c = LD32(pl) * LD32(pt);
        const fpu_t dist = ((a + b) + c) + LD32(pl + 0xc);
        FCOM_L(fsw, dist, static_cast<fpu_t>(RDF32(0x4d02d0)));
        seg_plane(sector + ((fsw & 0x100u) ? 0x90u : 0x80u), p, q, out, fsw);
    }
}

void sector_point() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, outp = RD32A(sp + 4);
    uint32_t out[3], fsw = F.sw;
    sector_point_core(R.ecx, RD32A(sp + 8), RD32A(sp + 12), out, fsw);
    WR32(outp, out[0]);
    WR32(outp + 4, out[1]);
    WR32(outp + 8, out[2]);
    F.sw = static_cast<uint16_t>(fsw);
    R.eax = outp;
    R.ecx = out[0];
    R.edx = out[2];
    R.esp = sp + 16;
}

// ---- small vector / matrix helpers of the bike code, as the x86 computes them ----
// Vectors are 3 floats, matrices 12 (3x3 + translation); every result is rounded to float where
// the x86 stores it.
using V3 = float[3];
using M12 = float[12];
inline fpu_t F_(float v) { return static_cast<fpu_t>(v); }

void load3(uint32_t a, float v[3]) { MEM_LOCAL(); for (int i = 0; i < 3; i++) v[i] = RDF32(a + 4 * i); }
void store3(uint32_t a, const float v[3]) { MEM_LOCAL(); for (int i = 0; i < 3; i++) WRF32(a + 4 * i, v[i]); }
void load12(uint32_t a, float m[12]) { MEM_LOCAL(); for (int i = 0; i < 12; i++) m[i] = RDF32(a + 4 * i); }

// 0x40ffc0 (out = a + b), 0x4570d0 (out = a - b), 0x46d590 (out = v * (1 / s))
void vadd(const float a[3], const float b[3], float o[3]) { for (int i = 0; i < 3; i++) o[i] = static_cast<float>(F_(b[i]) + F_(a[i])); }
void vsub(const float a[3], const float b[3], float o[3]) { for (int i = 0; i < 3; i++) o[i] = static_cast<float>(F_(a[i]) - F_(b[i])); }
void vdiv(const float v[3], float s, float o[3]) {
    const fpu_t r = static_cast<fpu_t>(1) / F_(s);
    for (int i = 0; i < 3; i++) o[i] = static_cast<float>(F_(v[i]) * r);
}
// 0x410010: o = m * v (rotation, then the translation m[9..11])
void xform(const float v[3], const float m[12], float o[3]) {
    o[0] = static_cast<float>(((F_(m[6]) * F_(v[2]) + F_(m[3]) * F_(v[1])) + F_(m[0]) * F_(v[0])) + F_(m[9]));
    o[1] = static_cast<float>(((F_(m[1]) * F_(v[0]) + F_(m[4]) * F_(v[1])) + F_(m[7]) * F_(v[2])) + F_(m[10]));
    o[2] = static_cast<float>(((F_(m[8]) * F_(v[2]) + F_(m[5]) * F_(v[1])) + F_(m[2]) * F_(v[0])) + F_(m[11]));
}
// 0x40fe80: inverse of a rotation + translation (transpose, -R^T t)
void inverse(const float m[12], float o[12]) {
    const fpu_t t10 = ((F_(m[2]) * F_(m[11]) + F_(m[1]) * F_(m[10])) + F_(m[9]) * F_(m[0]));
    const fpu_t t19 = ((F_(m[5]) * F_(m[11]) + F_(m[9]) * F_(m[3])) + F_(m[10]) * F_(m[4]));
    const fpu_t t26 = ((F_(m[10]) * F_(m[7]) + F_(m[9]) * F_(m[6])) + F_(m[11]) * F_(m[8]));
    const float r[12] = {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8],
                         static_cast<float>(-t10), static_cast<float>(-t19), static_cast<float>(-t26)};
    memcpy(o, r, sizeof r);
}
// 0x40fd40: rotation matrix from three angles through the sine table [0x5fcb80] (16384 entries per
// turn, index = _ftol(angle * [0x4d0300]); the cosine is the entry 0x1000 before), no translation
void euler(const float a[3], float m[12]) {
    MEM_LOCAL();
    const fpu_t scale = static_cast<fpu_t>(RDF64(0x4d0300));
    float s[3], c[3];
    for (int i = 0; i < 3; i++) {
        const uint32_t k = static_cast<uint32_t>(FTOL(F_(a[i]) * scale));
        s[i] = RDF32(0x5fcb80 + (k & 0x3fff) * 4);
        c[i] = RDF32(0x5fcb80 + ((k - 0x1000) & 0x3fff) * 4);
    }
    const fpu_t T0 = F_(s[0]), T1 = F_(c[0]), A2 = F_(s[1]), B2 = F_(c[1]), C3 = F_(s[2]);
    const float D3 = c[2];
    const fpu_t t2 = B2 * T1;
    const float l4 = static_cast<float>(t2);
    const fpu_t t7 = B2 * T0;
    const float l8 = static_cast<float>(t7);
    const fpu_t t14 = F_(D3) * A2;
    m[0] = static_cast<float>(A2 * C3);
    m[1] = static_cast<float>(t2 * C3 + F_(D3) * T0);
    m[2] = static_cast<float>(F_(D3) * T1 - F_(l8) * C3);
    m[3] = static_cast<float>(-t14);
    m[4] = static_cast<float>(T0 * C3 - F_(l4) * F_(D3));
    m[5] = static_cast<float>(C3 * T1 + F_(l8) * F_(D3));
    m[6] = static_cast<float>(B2);
    m[7] = static_cast<float>(-(A2 * T1));
    m[8] = static_cast<float>(A2 * T0);
    m[9] = m[10] = m[11] = 0.f;
}
// 0x458e20: the angles of the direction a -> b (pitch from the horizontal length, yaw); straight
// up / down: +-pi/2, 0, 0; a == b: 0
void dir_angles(const float a[3], const float b[3], float o[3], uint32_t &fsw) {
    MEM_LOCAL();
    const float dx = static_cast<float>(F_(b[0]) - F_(a[0]));
    const float dy = static_cast<float>(F_(b[1]) - F_(a[1]));
    const float dz = static_cast<float>(F_(b[2]) - F_(a[2]));
    const fpu_t len = FM(sqrt)((F_(dy) * F_(dy) + F_(dz) * F_(dz)) + F_(dx) * F_(dx));
    const fpu_t nz_ = F_(dz) / len;
    const float nz = static_cast<float>(nz_);
    const float nx = static_cast<float>(F_(dx) / len);
    const float ny = static_cast<float>(F_(dy) / len);
    const fpu_t h_ = FM(sqrt)(nz_ * F_(nz) + F_(nx) * F_(nx));
    FCOM_L(fsw, h_, F_(RDF32(0x4d0e30)));
    const float h = static_cast<float>(h_);
    if (!(fsw & 0x4000u)) {
        o[0] = static_cast<float>(FM(atan2)(F_(ny), F_(h)));
        o[1] = static_cast<float>(FM(atan2)(-(F_(nx) / F_(h)), F_(nz) / F_(h)));
        o[2] = 0.f;
        return;
    }
    uint32_t x = 0;
    FCOM_L(fsw, F_(a[1]), F_(b[1]));
    if (fsw & 0x100u) x = 0x3fc90fdb;
    else {
        FCOM_L(fsw, F_(a[1]), F_(b[1]));
        if (!(fsw & 0x4100u)) x = 0xbfc90fdb;
    }
    o[0] = f32(x);
    o[1] = o[2] = 0.f;
}
// 0x40c8b0 (thiscall track; sector index, point; st0): the height of the sector's floor plane at
// the point (plane at +0x80, or +0x90 when the point is below the plane at +0xa0), 0 without sectors
// or for a vertical plane
fpu_t sector_height(uint32_t self, uint32_t idx, uint32_t pt, uint32_t &fsw) {
    MEM_LOCAL();
    const fpu_t zero = F_(RDF32(0x4d02d0));
    if (static_cast<int32_t>(RD32(self + 4)) <= 0) return zero;
    const uint32_t sector = RD32(self) + idx * 252u, pl = sector + 0xa0;
    WR32A(PLANE_CALLS, RD32A(PLANE_CALLS) + 1);
    const fpu_t a = LD32(pl + 4) * LD32(pt + 4), b = LD32(pl + 8) * LD32(pt + 8), c = LD32(pl) * LD32(pt);
    FCOM_L(fsw, ((a + b) + c) + LD32(pl + 0xc), zero);
    const uint32_t plane = sector + ((fsw & 0x100u) ? 0x90u : 0x80u);
    if (!(RD32(plane + 4) & 0x7fffffffu)) return zero;
    return -(((LD32(pt + 8) * LD32(plane + 8) + LD32(plane) * LD32(pt)) + LD32(plane + 0xc)) / LD32(plane + 4));
}

constexpr uint32_t TRACK = 0x52c108;
extern "C" void f_0040c7e0(void);

// the track sector search 0x40c7e0 (native itself), called as the x86 does: thiscall track,
// (start, point, out = NULL), the return address and arguments on the guest stack at esp
uint32_t sector_search(uint32_t esp, uint32_t ret, uint32_t start, uint32_t point, uint32_t &fsw) {
    MEM_LOCAL();
    WR32A(esp, ret);
    WR32A(esp + 4, start);
    WR32A(esp + 8, point);
    WR32A(esp + 12, 0);
    R.esp = esp;
    R.ecx = TRACK;
    F.sw = static_cast<uint16_t>(fsw);
    f_0040c7e0();
    fsw = F.sw;
    return R.eax;
}

uint32_t n_ground = 0;   // check mode statistics: calls that took the four-point branch

// A guest function called as the x86 does: ecx, the arguments pushed below `esp` (the caller's esp
// before the pushes), the return address under them; the callee pops its arguments. The x87 status
// word goes through F.sw. Returns eax.
uint32_t gcall(void (*fn)(void), uint32_t esp, uint32_t ret, uint32_t ecx, std::initializer_list<uint32_t> args,
               uint32_t &fsw) {
    MEM_LOCAL();
    const uint32_t at = esp - 4 * static_cast<uint32_t>(args.size()) - 4;
    WR32A(at, ret);
    uint32_t a = at + 4;
    for (uint32_t v : args) { WR32A(a, v); a += 4; }
    R.esp = at;
    R.ecx = ecx;
    F.sw = static_cast<uint16_t>(fsw);
    fn();
    fsw = F.sw;
    if (R.esp != esp) {
        static int warned = 0;
        if (warned++ < 5) rt::log("native: guest call returning to 0x%08x left esp 0x%08x (expected 0x%08x)\n", ret, R.esp, esp);
    }
    return R.eax;
}

// 0x40fcd0: normalize in place (unless the length is [0x4d02d0]); 0x40fcb0: the length (st0)
void normalize(float v[3], uint32_t &fsw) {
    MEM_LOCAL();
    const fpu_t len = FM(sqrt)((F_(v[0]) * F_(v[0]) + F_(v[1]) * F_(v[1])) + F_(v[2]) * F_(v[2]));
    FCOM_L(fsw, len, F_(RDF32(0x4d02d0)));
    const float l = static_cast<float>(len);
    if (fsw & 0x4000u) return;
    const fpu_t r = static_cast<fpu_t>(1) / F_(l);
    v[0] = static_cast<float>(F_(v[0]) * r);
    v[1] = static_cast<float>(F_(v[1]) * r);
    v[2] = static_cast<float>(r * F_(v[2]));
}
fpu_t length(const float v[3]) { return FM(sqrt)((F_(v[0]) * F_(v[0]) + F_(v[1]) * F_(v[1])) + F_(v[2]) * F_(v[2])); }
// 0x46d610: o = a x b
void cross(const float a[3], const float b[3], float o[3]) {
    const float r[3] = {static_cast<float>(F_(a[1]) * F_(b[2]) - F_(b[1]) * F_(a[2])),
                        static_cast<float>(F_(b[0]) * F_(a[2]) - F_(b[2]) * F_(a[0])),
                        static_cast<float>(F_(b[1]) * F_(a[0]) - F_(b[0]) * F_(a[1]))};
    memcpy(o, r, sizeof r);
}


extern "C" void f_0046cd20(void);
extern "C" void f_0046bdc0(void);
extern "C" void f_0045cb00(void);

inline int32_t wrap_track(int32_t x) {   // 0x40ec30: a track position wrapped to 0 .. [0x52c10c) - 1
    MEM_LOCAL();
    while (x < 0) x += static_cast<int32_t>(RD32A(0x52c10c));
    for (int32_t n = static_cast<int32_t>(RD32A(0x52c10c)); x >= n; n = static_cast<int32_t>(RD32A(0x52c10c))) x -= n;
    return x;
}

// angles kept in 0 .. 2 pi as the bike code does it: below 0 (sign bit / a compare), add 2 pi ([0x4d0e58])
// until >= 0 in x87 precision, then subtract it while >= 2 pi; each loop stores the float once
float wrap_up(float v, uint32_t &fsw) {
    MEM_LOCAL();
    fpu_t x = F_(v);
    do { x += static_cast<fpu_t>(RDF64(0x4d0e58)); FCOM_L(fsw, x, LD32(0x4d0e54)); } while (fsw & 0x100u);
    return static_cast<float>(x);
}
float wrap_down(float v, uint32_t &fsw) {
    MEM_LOCAL();
    const fpu_t two_pi = static_cast<fpu_t>(RDF64(0x4d0e58));
    FCOM_L(fsw, F_(v), two_pi);
    if (fsw & 0x100u) return v;
    fpu_t x = F_(v);
    do { x -= two_pi; FCOM_L(fsw, x, two_pi); } while (!(fsw & 0x100u));
    return static_cast<float>(x);
}

// ---- 0x46b120: a bike's heading and speed along the track (thiscall bike, arg dt; ret 4) ----
// T = [0x509398 + 292 * model] (AI steering on), the aim point: the sector a few positions ahead
// ([0x4dc140] * 9 / 16) moved by +0x31d5 along its x axis. Order of the x86: the acceleration of the
// target speed +0x1a4 (capped by +0xb4 scaled by +0x31d9) and 0x46cd20 (the bike it follows); on the
// track (+0x30c1 == 0) the yaw towards the aim point (0x46bdc0, its inverse, 0x490220 angles wrapped to
// 0..2pi, the difference to -pi..pi) sets the steering +0x110 (AI: scaled, clamped to +-pi/4) and the
// rotation 0x45cb00 (matrix by value); the forward vector +0x198 (= +0xd4 + +0xe0, normalized; the
// heading +0x18c flips with it when they point apart); the speed +0xec towards the cap; the direction
// to a second aim point; the sector search 0x40c7e0; the heading: AI bikes on the ground steer it by dt
// and rebuild the orientation (+0xbc..+0xe8) around it, the others blend it towards the aim (+0x31d1);
// the move pos += heading * speed * dt * [0x4d1010] unless [0x4dc0b4]; the sector search again.
// Ends with that call's registers. Stack locals as the x86 keeps them where a callee reads them.
void bike_drive() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, s = R.ecx, dtb = RD32A(sp + 4);
    const uint32_t B = sp - 0xc4;   // esp after the prologue
    uint32_t fsw = F.sw;
    const fpu_t dt = F_(f32(dtb)), c54 = LD32(0x4d0e54), one = LD32(0x4d0e70);
    auto D = [&](uint32_t a) { return static_cast<fpu_t>(RDF64(a)); };
    auto rnd = [](fpu_t v) { return static_cast<float>(v); };
    auto cap = [&] {
        return static_cast<fpu_t>(static_cast<int32_t>(RD32(s + 0x31d9))) * LD32(s + 0xb4) * D(0x4d12b8) + LD32(s + 0xb4);
    };
    const uint32_t T = RD32(0x509398 + 292 * RD32(s + 0x78));
    WR32(s + 0x2dc8, 1);
    bool pick = true;
    if (!RD32A(0x4dc0b4) && (RD32(s + 0x2db8) || !T)) {
        if (RD32(s + 0x30c1)) pick = false;
        else {
            WRF32(s + 0x1a4, rnd(dt * D(0x4d12b0) + LD32(s + 0x1a4)));
            const fpu_t x = cap();
            FCOM_L(fsw, x, LD32(s + 0x1a4));
            if (fsw & 0x100u) WRF32(s + 0x1a4, rnd(x));
        }
    }
    if (pick && !RD32(s + 0x30c1)) gcall(f_0046cd20, B, 0x46b1d5, s, {dtb}, fsw);
    const int32_t g = static_cast<int32_t>(RD32A(0x4dc140));
    const int32_t ahead1 = T ? g * 9 : g * 16, ahead2 = T ? g * 2 : g * 4;

    // the first aim point P = sector + its matrix * (h, 0, 0)
    {
        const uint32_t S = RD32A(TRACK) + static_cast<uint32_t>(wrap_track(static_cast<int32_t>(RD32(s + 0x31c1)) + ahead1)) * 252u;
        const float hv[3] = {rnd(LD32(S + 0xb0) + LD32(s + 0x31d5)), 0.f, 0.f};
        float m[12], o[3], sv[3], P[3];
        load12(S + 0xc, m);
        xform(hv, m, o);
        load3(S, sv);
        vadd(sv, o, P);
        if (!RD32(s + 0x30c1)) {
            store3(B + 0x28, P);
            gcall(f_0046bdc0, B, 0x46b286, B + 0x7c, {s + 0x30, B + 0x28}, fsw);
            float lf[12], inv[12];
            load12(B + 0x7c, lf);
            inverse(lf, inv);
            for (int i = 0; i < 12; i++) WRF32(B + 0x7c + 4 * i, inv[i]);
            uint32_t eax_unused = R.eax;
            const float a1 = rnd(-angle_core(s + 0xbc, fsw, eax_unused));
            const float a2 = rnd(angle_core(B + 0x7c, fsw, eax_unused));
            const fpu_t two_pi = D(0x4d0e58), pi = D(0x4d0e60);
            auto down = [&](float v) { return wrap_down(v, fsw); };
            auto up = [&](float v) { return wrap_up(v, fsw); };
            float A = a1, Bv = a2;
            if (bits32(A) > 0x80000000u) A = up(A);
            A = down(A);
            if (bits32(Bv) > 0x80000000u) Bv = up(Bv);
            Bv = down(Bv);
            FCOM_L(fsw, F_(A) + pi, F_(Bv));
            if (fsw & 0x100u) Bv = rnd(F_(Bv) - two_pi);
            FCOM_L(fsw, F_(A) - pi, F_(Bv));
            if (!(fsw & 0x4100u)) Bv = rnd(F_(Bv) + two_pi);
            const fpu_t d = (F_(Bv) - F_(A)) * LD32(0x4d0f88);
            FCOM_L(fsw, d, c54);
            float dv = rnd(d);
            if (fsw & 0x100u) dv = up(dv);
            dv = down(dv);
            float yaw = dv;
            FCOM_L(fsw, F_(dv), pi);
            if (!(fsw & 0x4100u)) yaw = rnd(F_(dv) - two_pi);
            WR32A(B + 0x10, bits32(yaw));
            if (T) {
                const fpu_t p = F_(yaw) * LD32(0x4d1148);
                FCOM_L(fsw, p, D(0x4d1218));
                float t = (fsw & 0x100u) ? f32(0xbf490fdb) : rnd(p);
                FCOM_L(fsw, F_(t), D(0x4d12c8));
                if (!(fsw & 0x4100u)) t = f32(0x3f490fdb);
                WRF32(s + 0x110, rnd(F_(t) * D(0x4d12d8)));
            } else {
                WR32(s + 0x110, 0);
            }
            const float ang[3] = {0.f, yaw, 0.f};
            float r[12];
            euler(ang, r);
            gcall(f_0045cb00, B, 0x46b4f1, s,
                  {bits32(r[0]), bits32(r[1]), bits32(r[2]), bits32(r[3]), bits32(r[4]), bits32(r[5]), bits32(r[6]),
                   bits32(r[7]), bits32(r[8]), bits32(r[9]), bits32(r[10]), bits32(r[11])},
                  fsw);
        }
    }

    // forward vector +0x198 = +0xd4 + +0xe0, normalized
    {
        const float v[3] = {rnd(LD32(s + 0xe0) + LD32(s + 0xd4)), rnd(LD32(s + 0xe4) + LD32(s + 0xd8)),
                            rnd(LD32(s + 0xe8) + LD32(s + 0xdc))};
        store3(s + 0x198, v);
        const fpu_t len = length(v);
        FCOM_L(fsw, len, c54);
        if (!(fsw & 0x4000u)) {
            const fpu_t r = static_cast<fpu_t>(1) / F_(rnd(len));
            WRF32(s + 0x198, rnd(F_(v[0]) * r));
            WRF32(s + 0x19c, rnd(F_(v[1]) * r));
            WRF32(s + 0x1a0, rnd(r * F_(v[2])));
        }
    }
    auto dot = [&] {
        return (LD32(s + 0x1a0) * LD32(s + 0x194) + LD32(s + 0x19c) * LD32(s + 0x190)) + LD32(s + 0x18c) * LD32(s + 0x198);
    };
    FCOM_L(fsw, dot(), c54);
    if (fsw & 0x100u)
        for (uint32_t off : {0x18cu, 0x190u, 0x194u, 0xecu}) WRF32(s + off, rnd(-LD32(s + off)));

    // the second aim point Q
    float Q[3];
    {
        const uint32_t S = RD32A(TRACK) + static_cast<uint32_t>(wrap_track(static_cast<int32_t>(RD32(s + 0x31c1)) + ahead2)) * 252u;
        const fpu_t h = LD32(S + 0xb0) + LD32(s + 0x31d5);
        const fpu_t hf = F_(rnd(h));
        const fpu_t x0 = h * LD32(S + 0xc) + LD32(S + 0x30);
        const fpu_t y0 = LD32(S + 0x10) * hf + LD32(S + 0x34);
        const fpu_t z0 = LD32(S + 0x14) * hf + LD32(S + 0x38);
        Q[0] = rnd(x0 + LD32(S));
        Q[1] = rnd(y0 + LD32(S + 4));
        Q[2] = rnd(z0 + LD32(S + 8));
    }

    // the speed +0xec
    if ((!(RD32(s + 0x31d1) & 0x7fffffffu) && (RD32(s + 0xec) & 0x7fffffffu)) || RD32(s + 0x30c1)) {
        if (static_cast<int32_t>(RD32(s + 0xec)) > 0) {
            WRF32(s + 0xec, rnd(dt * D(0x4d12e0) + LD32(s + 0xec)));
            if (RD32(s + 0xec) > 0x80000000u) WR32(s + 0xec, 0);
        } else {
            WRF32(s + 0xec, rnd(dt * D(0x4d12e8) + LD32(s + 0xec)));
            if (static_cast<int32_t>(RD32(s + 0xec)) > 0) WR32(s + 0xec, 0);
        }
    }
    const fpu_t kk = LD32(s + 0x31d1) * LD32(0x4d100c);
    FCOM_L(fsw, kk, D(0x4d0ed8));
    const float blend = (fsw & 0x100u) ? f32(0x3c23d70a) : rnd(kk);
    if (RD32(s + 0xec) > 0x80000000u) {
        WR32(s + 0x1a4, 0);
    } else if (RD32(s + 0x2db8) || !T) {
        const uint32_t ab = bits32(rnd(dot())) & 0x7fffffffu;
        const float f = static_cast<int32_t>(ab) < 0x3f000000 ? 0.5f : f32(ab);
        const fpu_t p = LD32(s + 0x1a4) * F_(f);
        const fpu_t G = (one - F_(f)) * LD32(s + 0xec);
        const fpu_t A0 = LD32(s + 0x198) * p, A1 = LD32(s + 0x19c) * p, A2 = p * LD32(s + 0x1a0);
        const fpu_t H0 = LD32(s + 0x18c) * G, H1 = LD32(s + 0x190) * G, H2 = G * LD32(s + 0x194);
        const float X = rnd(H0 + A0);
        const float Y = rnd(A1 + F_(rnd(H1)));
        const fpu_t Z = H2 + A2;
        float L = rnd(FM(sqrt)((Z * F_(rnd(Z)) + F_(Y) * F_(Y)) + F_(X) * F_(X)));
        const fpu_t c = cap();
        FCOM_L(fsw, c, F_(L));
        if (fsw & 0x100u) L = rnd(c);
        WRF32(s + 0xec, rnd(LD32(s + 0xec) * F_(blend) + (one - F_(blend)) * F_(L)));
    }

    // the direction to Q
    float Dv[3];
    {
        Dv[0] = rnd(F_(Q[0]) - LD32(s + 0x30));
        Dv[1] = rnd(F_(Q[1]) - LD32(s + 0x34));
        Dv[2] = rnd(F_(Q[2]) - LD32(s + 0x38));
        const fpu_t l = FM(sqrt)((F_(Dv[1]) * F_(Dv[1]) + F_(Dv[2]) * F_(Dv[2])) + F_(Dv[0]) * F_(Dv[0]));
        FCOM_L(fsw, l, c54);
        if (!(fsw & 0x4000u)) {
            const fpu_t r = static_cast<fpu_t>(1) / F_(rnd(l));
            Dv[0] = rnd(F_(Dv[0]) * r);
            Dv[1] = rnd(F_(Dv[1]) * r);
            Dv[2] = rnd(r * F_(Dv[2]));
        }
    }
    WR32(s + 0x31c1, sector_search(B - 0x10, 0x46b8e9, RD32(s + 0x31c1), s + 0x30, fsw));

    if (!RD32(s + 0x2db8) && T) {
        // AI on the ground: heading * speed, steered by dt; the orientation rebuilt around the forward vector
        const fpu_t e = LD32(s + 0xec);
        WRF32(s + 0x18c, rnd(LD32(s + 0x18c) * e));
        const fpu_t be = LD32(s + 0x190) * e;
        const fpu_t k = dt * D(0x4d12f8);
        WRF32(s + 0x190, rnd(be));
        WRF32(s + 0x194, rnd(e * LD32(s + 0x194)));
        WRF32(s + 0x190, rnd(LD32(s + 0x190) - k));
        const float l = rnd(FM(sqrt)((LD32(s + 0x190) * LD32(s + 0x190) + LD32(s + 0x18c) * LD32(s + 0x18c)) +
                                     LD32(s + 0x194) * LD32(s + 0x194)));
        WRF32(s + 0xec, l);
        if (bits32(l) & 0x7fffffffu) {
            const fpu_t r = static_cast<fpu_t>(1) / F_(l);
            WRF32(s + 0x18c, rnd(LD32(s + 0x18c) * r));
            WRF32(s + 0x190, rnd(LD32(s + 0x190) * r));
            WRF32(s + 0x194, rnd(r * LD32(s + 0x194)));
        }
        WRF32(s + 0x19c, rnd(LD32(s + 0x190)));
        const fpu_t l2 = FM(sqrt)((LD32(s + 0x19c) * LD32(s + 0x19c) + LD32(s + 0x1a0) * LD32(s + 0x1a0)) +
                                  LD32(s + 0x198) * LD32(s + 0x198));
        FCOM_L(fsw, l2, c54);
        if (!(fsw & 0x4000u)) {
            const fpu_t r = static_cast<fpu_t>(1) / F_(rnd(l2));
            WRF32(s + 0x198, rnd(LD32(s + 0x198) * r));
            WRF32(s + 0x19c, rnd(LD32(s + 0x19c) * r));
            WRF32(s + 0x1a0, rnd(r * LD32(s + 0x1a0)));
        }
        float fw[3], n[3], x[3], y[3];
        const float zero[3] = {0.f, 0.f, 0.f}, upv[3] = {0.f, 1.f, 0.f};
        load3(s + 0x198, fw);
        vsub(fw, zero, n);   // 0x4570d0, 0x40ff80
        normalize(n, fsw);   // 0x40fcd0
        cross(upv, n, x);    // 0x46d610
        cross(n, x, y);
        normalize(x, fsw);
        normalize(y, fsw);
        store3(s + 0xbc, x);
        store3(s + 0xc8, y);
        store3(s + 0xd4, n);
        WR32(s + 0xe0, 0);
        WR32(s + 0xe4, 0);
        WR32(s + 0xe8, 0);
        FCOM_L(fsw, LD32(s + 0xb4), LD32(s + 0xec));
        if (fsw & 0x100u) WR32(s + 0xec, RD32(s + 0xb4));
    } else {
        if (!RD32(s + 0x30c1)) {   // the heading blended towards Q
            const fpu_t u = one - F_(blend), kb = F_(blend);
            const fpu_t DX = F_(Dv[0]) * u, DY = F_(Dv[1]) * u, DZ = u * F_(Dv[2]);
            const fpu_t a = LD32(s + 0x18c) * kb, b = LD32(s + 0x190) * kb, c = LD32(s + 0x194) * kb;
            WRF32(s + 0x18c, rnd(a + DX));
            WRF32(s + 0x190, rnd(b + DY));
            WRF32(s + 0x194, rnd(c + DZ));
        }
        if (T) WR32(s + 0x190, RD32(s + 0x19c));
        const fpu_t l = FM(sqrt)((LD32(s + 0x190) * LD32(s + 0x190) + LD32(s + 0x18c) * LD32(s + 0x18c)) +
                                 LD32(s + 0x194) * LD32(s + 0x194));
        FCOM_L(fsw, l, c54);
        if (!(fsw & 0x4000u)) {
            const fpu_t r = static_cast<fpu_t>(1) / F_(rnd(l));
            WRF32(s + 0x18c, rnd(LD32(s + 0x18c) * r));
            WRF32(s + 0x190, rnd(LD32(s + 0x190) * r));
            WRF32(s + 0x194, rnd(r * LD32(s + 0x194)));
        }
    }

    if (!RD32A(0x4dc0b4)) {
        const fpu_t M = LD32(s + 0xec) * dt * D(0x4d1010);
        const fpu_t aM = LD32(s + 0x18c) * M, bM = LD32(s + 0x190) * M, cM = M * LD32(s + 0x194);
        WRF32(s + 0x30, rnd(aM + LD32(s + 0x30)));
        WRF32(s + 0x34, rnd(bM + LD32(s + 0x34)));
        WRF32(s + 0x38, rnd(cM + LD32(s + 0x38)));
    } else {
        WR32(s + 0xec, 0);
        WR32(s + 0x1a4, 0);
    }
    WR32(s + 0x31c1, sector_search(B - 0x10, 0x46bccb, RD32(s + 0x31c1), s + 0x30, fsw));
    F.sw = static_cast<uint16_t>(fsw);
    R.esp = sp + 8;
}

// ---- 0x469c60: a bike's lean and front / back points (thiscall bike, no args) ----
// T = [0x509398 + 292 * model] (the dirt bikes' flag). The look direction blends the heading +0x18c
// (turned towards the bike's forward axis) with that axis by w (from +0x31d1, decayed by [0x581014],
// and the speed ratio +0x124 / +0xb4); 0x46bdc0 builds its frame. T != 0: with +0x307d > 0 the front
// point +0x1b4 swings around the back point +0x338 by sin(+0x307d) in the bike's frame; +0x2e55 = the
// frame from the back to the front point. T == 0: the front / back points at per-model distances
// (0x5092b0 table) along the look frame, put on the floor of their sectors (0x40c7e0, 0x40c8b0), and
// +0x2e55 = the orthonormal frame along them (+0x307d > 0, a crash tumble, runs the translated body).
// The lean +0x2e1d from the yaw between the look frame and the bike (0x490220 angles wrapped to
// 0..2pi, the difference to -pi..pi): T != 0 with [+8] and [+4], atan of the yaw by the speed (at most
// 50), clamped, smoothed with +0x2e21; else the yaw scaled and clamped, then divided by a speed term
// (T != 0), or for the player's bike ([+8], [+4], [0x4e9834] not 1 / 2) by the steering scale
// [0x4dc150] (at least 20 without +0x2e29) and shaped, or negated. Callers read no register afterwards.
void bike_lean() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, s = R.ecx;
    const uint32_t B = sp - 0x128;   // esp after the prologue
    uint32_t fsw = F.sw;
    auto D = [&](uint32_t a) { return static_cast<fpu_t>(RDF64(a)); };
    auto rnd = [](fpu_t v) { return static_cast<float>(v); };
    const fpu_t c54 = LD32(0x4d0e54), one = LD32(0x4d0e70), two_pi = D(0x4d0e58), pi = D(0x4d0e60);
    const uint32_t T = RD32(0x509398 + 292 * RD32(s + 0x78));
    float k = 0.f;
    if (RD32(s + 0x31d1) & 0x7fffffffu) {
        WRF32(s + 0x31d1, rnd(LD32(s + 0x31d1) - LD32(0x581014)));
        if (RD32(s + 0x31d1) > 0x80000000u) WR32(s + 0x31d1, 0);
        k = rnd(LD32(s + 0x31d1) * LD32(0x4d100c));
    }
    const fpu_t x = one - (LD32(s + 0x124) / LD32(s + 0xb4)) * LD32(0x4d1020);
    FCOM_L(fsw, x, c54);
    float w = (fsw & 0x100u) ? 0.f : rnd(x);
    FCOM_L(fsw, F_(k), F_(w));
    if (!(fsw & 0x4100u)) w = k;
    float bm[12], V[3], H[3];
    load12(s + 0xbc, bm);
    const float fwd[3] = {0.f, 0.f, 1.f};
    xform(fwd, bm, V);
    load3(s + 0x18c, H);
    FCOM_L(fsw, (F_(H[2]) * F_(V[2]) + F_(H[1]) * F_(V[1])) + F_(V[0]) * F_(H[0]), c54);   // 0x46d5e0
    if (fsw & 0x100u)
        for (int i = 0; i < 3; i++) H[i] = rnd(-F_(H[i]));   // 0x40ffa0
    const float u = rnd(one - F_(w));
    float Vw[3], Hu[3], dir[3];
    for (int i = 0; i < 3; i++) {   // 0x46d540
        Vw[i] = rnd(F_(V[i]) * F_(w));
        Hu[i] = rnd(F_(H[i]) * F_(u));
    }
    vadd(Hu, Vw, dir);
    const float zero[3] = {0.f, 0.f, 0.f};
    store3(B + 0x11c, dir);
    store3(B + 0x10, zero);
    gcall(f_0046bdc0, B, 0x469e17, B + 0xb8, {B + 0x10, B + 0x11c}, fsw);

    if (!T) {
        const uint32_t tb = 0x5092b0 + 292 * RD32(s + 0x78);
        float Fm[12];
        load12(B + 0xb8, Fm);
        auto f = [&](int i) { return F_(Fm[i]); };
        const fpu_t a = LD32(tb + 0x20) * LD32(0x4d102c);
        const fpu_t l = F_(rnd(LD32(tb + 8) * LD32(0x4d1030) + a));
        const fpu_t c = LD32(tb) * LD32(0x4d0e94), cf = F_(rnd(c));
        WRF32(s + 0x338, rnd(((c * f(6) + f(3) * l) + f(9)) + LD32(s + 0x30)));
        WRF32(s + 0x33c, rnd(((f(7) * cf + f(4) * l) + f(10)) + LD32(s + 0x34)));
        WRF32(s + 0x340, rnd(((f(5) * l + f(8) * cf) + f(11)) + LD32(s + 0x38)));
        const fpu_t lb = F_(rnd(a + LD32(tb + 4) * LD32(0x4d1030)));
        const fpu_t e = LD32(tb) * LD32(0x4d0e98), ef = F_(rnd(e));
        WRF32(s + 0x1b4, rnd(((e * f(6) + lb * f(3)) + f(9)) + LD32(s + 0x30)));
        WRF32(s + 0x1b8, rnd(((ef * f(7) + lb * f(4)) + f(10)) + LD32(s + 0x34)));
        WRF32(s + 0x1bc, rnd(((lb * f(5) + ef * f(8)) + f(11)) + LD32(s + 0x38)));
        const uint32_t i1 = sector_search(B - 0x10, 0x46a13f, RD32(s + 0x31c1), s + 0x1b4, fsw);
        const uint32_t i2 = sector_search(B - 0x10, 0x46a15b, RD32(s + 0x31c1), s + 0x338, fsw);
        WRF32(s + 0x1b8, rnd(sector_height(TRACK, i1, s + 0x1b4, fsw)));
        WRF32(s + 0x33c, rnd(sector_height(TRACK, i2, s + 0x338, fsw)));
        float p1[3], p2[3], n[3], xv[3], yv[3];
        const float upv[3] = {0.f, 1.f, 0.f};
        load3(s + 0x1b4, p1);
        load3(s + 0x338, p2);
        vsub(p1, p2, n);   // 0x4570d0, 0x40ff80
        normalize(n, fsw);
        cross(upv, n, xv);   // 0x46d610
        cross(n, xv, yv);
        normalize(xv, fsw);
        normalize(yv, fsw);
        store3(s + 0x2e55, xv);
        store3(s + 0x2e61, yv);
        store3(s + 0x2e6d, n);
        WR32(s + 0x2e79, 0);
        WR32(s + 0x2e7d, 0);
        WR32(s + 0x2e81, 0);
    } else {
        if (static_cast<int32_t>(RD32(s + 0x307d)) > 0) {
            float inv[12], p1[3], p2[3], d[3], l[3], rm[12], l2[3], wv[3];
            inverse(bm, inv);
            load3(s + 0x1b4, p1);
            load3(s + 0x338, p2);
            vsub(p1, p2, d);
            xform(d, inv, l);
            const fpu_t sn = FM(sin)(LD32(s + 0x307d));
            fsw &= ~0x400u;
            const float ang[3] = {rnd(sn * D(0x4d1218)), 0.f, 0.f};
            euler(ang, rm);
            xform(l, rm, l2);
            xform(l2, bm, wv);
            const float np[3] = {rnd(F_(p2[0]) + F_(wv[0])), rnd(F_(p2[1]) + F_(wv[1])), rnd(F_(p2[2]) + F_(wv[2]))};
            store3(s + 0x1b4, np);
        }
        gcall(f_0046bdc0, B, 0x469f2a, B + 0x10, {s + 0x338, s + 0x1b4}, fsw);
        for (uint32_t i = 0; i < 12; i++) WR32(s + 0x2e55 + 4 * i, RD32A(B + 0x10 + 4 * i));
    }

    uint32_t eax_unused = R.eax;
    const float a1 = rnd(angle_core(B + 0xb8, fsw, eax_unused));
    const float a2 = rnd(angle_core(s + 0xbc, fsw, eax_unused));
    float A = a1, Bv = a2;
    if (bits32(A) > 0x80000000u) A = wrap_up(A, fsw);
    A = wrap_down(A, fsw);
    if (bits32(Bv) > 0x80000000u) Bv = wrap_up(Bv, fsw);
    Bv = wrap_down(Bv, fsw);
    FCOM_L(fsw, F_(A) + pi, F_(Bv));
    if (fsw & 0x100u) Bv = rnd(F_(Bv) - two_pi);
    FCOM_L(fsw, F_(A) - pi, F_(Bv));
    if (!(fsw & 0x4100u)) Bv = rnd(F_(Bv) + two_pi);
    if (T && RD32(s + 8) && RD32(s + 4)) {
        const fpu_t d = F_(Bv) - F_(A);
        FCOM_L(fsw, d, c54);
        float dv = rnd(d);
        if (fsw & 0x100u) dv = wrap_up(dv, fsw);
        dv = wrap_down(dv, fsw);
        if (bits32(dv) > 0x80000000u) dv = wrap_up(dv, fsw);
        dv = wrap_down(dv, fsw);
        float yaw = dv;
        FCOM_L(fsw, F_(dv), pi);
        if (!(fsw & 0x4100u)) yaw = rnd(F_(dv) - two_pi);
        const float spd = static_cast<int32_t>(RD32(s + 0x124)) > 0x42480000 ? 50.f : f32(RD32(s + 0x124));
        const fpu_t y = (-F_(yaw) / LD32(0x4dc150)) * F_(spd);
        WRF32(s + 0x2e1d, rnd(FM(atan2)(y * D(0x4d1220), static_cast<fpu_t>(1))));
        FCOM_L(fsw, LD32(s + 0x2e1d), D(0x4d1228));
        if (!(fsw & 0x4100u)) {
            WR32(s + 0x2e1d, 0x3f851eb8);
        } else {
            FCOM_L(fsw, LD32(s + 0x2e1d), D(0x4d1238));
            if (fsw & 0x100u) WR32(s + 0x2e1d, 0xbf851eb8);
        }
        const fpu_t q = LD32(0x4dc150) * D(0x4d1248);
        FCOM_L(fsw, q, D(0x4d0ec8));
        const float f = (fsw & 0x4100u) ? rnd(q) : 1.f;
        WRF32(s + 0x2e1d, rnd((LD32(s + 0x2e1d) - LD32(s + 0x2e21)) * F_(f) + LD32(s + 0x2e21)));
        WR32(s + 0x2e21, RD32(s + 0x2e1d));
        R.eax = RD32(s + 0x2e1d);
        R.ecx = bits32(a1);
    } else {
        const fpu_t d = (F_(Bv) - F_(A)) * F_(u);
        FCOM_L(fsw, d, c54);
        float dv = rnd(d);
        if (fsw & 0x100u) dv = wrap_up(dv, fsw);
        dv = wrap_down(dv, fsw);
        const fpu_t e = F_(dv) * LD32(0x4d0f60);
        FCOM_L(fsw, e, c54);
        float ev = rnd(e);
        if (fsw & 0x100u) ev = wrap_up(ev, fsw);
        ev = wrap_down(ev, fsw);
        FCOM_L(fsw, F_(ev), pi);
        if (!(fsw & 0x4100u)) ev = rnd(F_(ev) - two_pi);
        FCOM_L(fsw, F_(ev), D(0x4d1250));
        if (!(fsw & 0x4100u)) ev = f32(0x3f860a92);
        FCOM_L(fsw, F_(ev), D(0x4d1260));
        if (fsw & 0x100u) ev = f32(0xbf860a92);
        if (T) {
            WRF32(s + 0x2e1d, rnd((F_(ev) / (D(0x4d1278) - LD32(s + 0x124) * D(0x4d1270) * D(0x4d0f08))) * D(0x4d1280)));
        } else if (!RD32(s + 8) || !RD32(s + 4) || RD32A(0x4e9834) == 1 || RD32A(0x4e9834) == 2) {
            WRF32(s + 0x2e1d, rnd(-F_(ev)));
        } else {
            float g = f32(RD32A(0x4dc150));
            if (!RD32(s + 0x2e29) && static_cast<int32_t>(RD32A(0x4dc150)) < 0x41a00000) g = 20.f;
            WRF32(s + 0x2e1d, rnd((F_(ev) / F_(g)) * LD32(0x4d1288)));
            const fpu_t v = LD32(s + 0x2e1d);
            FCOM_L(fsw, v, D(0x4d11f8));
            if (!(fsw & 0x4100u)) {
                WRF32(s + 0x2e1d, rnd(v * D(0x4d1290) - D(0x4d1298)));
            } else {
                FCOM_L(fsw, v, D(0x4d12a0));
                if (fsw & 0x100u) WRF32(s + 0x2e1d, rnd(v * D(0x4d1290) + D(0x4d1298)));
            }
            const fpu_t v2 = LD32(s + 0x2e1d);
            FCOM_L(fsw, v2, D(0x4d1228));
            if (!(fsw & 0x4100u)) {
                WR32(s + 0x2e1d, 0x3f851eb8);
            } else {
                FCOM_L(fsw, v2, D(0x4d1238));
                if (fsw & 0x100u) WR32(s + 0x2e1d, 0xbf851eb8);
            }
        }
    }
    F.sw = static_cast<uint16_t>(fsw);
    R.esp = sp + 4;
}

extern "C" void f_00465fa0(void);
extern "C" void f_0046b120(void);
extern "C" void f_0040ca80(void);
extern "C" void f_0046c7a0(void);
extern "C" void f_00458ce0(void);
extern "C" void f_004660f0(void);
extern "C" void f_00469c60(void);
extern "C" void f_00466940(void);
extern "C" void f_00466a80(void);
extern "C" void f_00461aa0(void);
extern "C" void f_00466d70(void);
extern "C" void f_004683e0(void);
extern "C" void f_00467b30(void);
extern "C" void f_00467930(void);
extern "C" void f_004b85c0(void);   // CRT _getptd

// ---- 0x46bf80: one bike's movement step (thiscall bike, one stack arg; ret 4) ----
// Order of the x86: 0x465fa0; 0x46b120(arg); the move against the track (0x40ca80 from the old
// position, a hit puts the bike on the returned point, finds its sector, halves +0x1a4 / +0xec);
// 0x46c7a0 (contact test); the airborne flag +0x2e85; 0x4660f0(arg) unless [0x4dc0b4]; the floor
// height under the bike (0x40c8b0) decides between staying in the air (+0x2db8 = 0) and landing: then
// the vertical speed goes into +0x2e95, the heading (+0x18c / +0x194) is normalized, the bike is put on
// the floor (minus a random amount for some bikes), and its orientation (+0xbc..+0xe8) is rebuilt from
// the floor under two points ahead / behind (per-model distances at [0x5092b0 + 292 * model]). Then the
// front / back points (+0x1b4, +0x338), 0x469c60, the lean +0x2e1d (scaled, clamped to +-pi/4, smoothed),
// 0x466940, 0x466a80, 0x461aa0, +0x2dc8 restored around 0x466d70, 0x4683e0, 0x467b30(1), 0x467930.
// Ends with that last call's registers, as the x86.
void bike_step() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, s = R.ecx, arg0 = RD32A(sp + 4);
    const uint32_t B = sp - 0x9c;   // esp after the prologue
    uint32_t fsw = F.sw;
    const uint32_t T = 0x5092b0 + 292 * RD32(s + 0x78);   // per-model table (73 dwords per model)
    const uint32_t saved_2dc8 = RD32(s + 0x2dc8);
    const uint32_t edi = RD32(T + 0xe8);   // [0x509398 + 292 * model]
    WR32(s + 0x319d, 0);
    WR32(s + 0x2dac, 0);
    gcall(f_00465fa0, B, 0x46bfc1, s, {}, fsw);
    WR32A(B + 0x84, RD32(s + 0x30));
    WR32A(B + 0x88, RD32(s + 0x34));
    WR32A(B + 0x8c, RD32(s + 0x38));
    gcall(f_0046b120, B, 0x46bff0, s, {arg0}, fsw);
    WR32(s + 0x2e95, 0);
    if (gcall(f_0040ca80, B, 0x46c02a, TRACK, {RD32(s + 0x31c1), B + 0x84, s + 0x30, B + 0x6c, s + 0x18c, s + 0x31c5}, fsw)) {
        WR32(s + 0x30, RD32A(B + 0x6c));
        WR32(s + 0x34, RD32A(B + 0x70));
        WR32(s + 0x38, RD32A(B + 0x74));
        WR32(s + 0x31c1, gcall(f_0040c7e0, B, 0x46c05a, TRACK, {RD32(s + 0x31c1), s + 0x30, 0}, fsw));
        WRF32(s + 0x1a4, static_cast<float>(LD32(s + 0x1a4) * LD32(0x4d1018)));
        WRF32(s + 0xec, static_cast<float>(LD32(s + 0xec) * LD32(0x4d1018)));
        WR32(s + 0x31d1, 0x43480000);
        WR32(s + 0x2dac, RD32(s + 0x2dac) | 1);
    }
    WR32(s + 0x124, RD32(s + 0xec));
    uint32_t ebx = 0;
    const uint32_t ebp = gcall(f_0046c7a0, B, 0x46c0b3, s, {RD32(s + 0x31c1), 8}, fsw);
    if (edi && ebp) {
        WR32(s + 0x2e85, 0);
    } else {
        if (edi) ebx = 1;
        if (ebp) {
            WR32(s + 0x2e85, 0);
        } else if (!(gcall(f_00458ce0, B, 0x46c0d0, R.ecx, {0x1af1}, fsw) & 0xff)) {
            FCOM_L(fsw, LD32(s + 0x1a4), LD32(s + 0xb4));
            if (!(fsw & 0x4100u)) WR32(s + 0x2e85, 1);
        }
    }
    if (!RD32A(0x4dc0b4)) gcall(f_004660f0, B, 0x46c115, s, {arg0}, fsw);
    const float h = static_cast<float>(sector_height(TRACK, RD32(s + 0x31c1), s + 0x30, fsw));
    FCOM_L(fsw, F_(h) + LD32(ebx ? 0x4d1020 : 0x4d1024), LD32(s + 0x34));
    if ((fsw & 0x100u) && edi) {
        WR32(s + 0x2db8, 0);
    } else {
        if (!RD32(s + 0x2dbc)) {
            WRF32(s + 0x2e95, static_cast<float>((LD32(s + 0x40) - F_(h)) * LD32(0x4d1028)));
            WR32(s + 0x31d1, 0x43480000);
            WR32(s + 0x190, 0);
            const fpu_t len = FM(sqrt)(LD32(s + 0x18c) * LD32(s + 0x18c) + LD32(s + 0x194) * LD32(s + 0x194));
            FCOM_L(fsw, len, LD32(0x4d0e54));
            const float l = static_cast<float>(len);
            if (!(fsw & 0x4000u)) {
                const fpu_t r = static_cast<fpu_t>(1) / F_(l);
                WRF32(s + 0x18c, static_cast<float>(LD32(s + 0x18c) * r));
                WR32(s + 0x190, 0);
                WRF32(s + 0x194, static_cast<float>(r * LD32(s + 0x194)));
            }
            WR32(s + 0x2dac, RD32(s + 0x2dac) | 0x80);
        }
        uint32_t edx = 0;
        if (edi && !ebp) {
            const uint32_t k = static_cast<uint32_t>(FTOL((LD32(s + 0xec) / LD32(s + 0xb4)) * LD32(0x4d1020)));
            if (k) {
                const uint32_t rv = gcall(f_00458ce0, B, 0x46c23d, R.ecx, {0x1b14}, fsw);
                edx = static_cast<uint32_t>(static_cast<int32_t>(rv) % static_cast<int32_t>(k));
            }
        }
        WRF32(s + 0x34, static_cast<float>(F_(h) - static_cast<fpu_t>(static_cast<int32_t>(edx))));
        WR32(s + 0x31c1, gcall(f_0040c7e0, B, 0x46c266, TRACK, {RD32(s + 0x31c1), s + 0x30, 0}, fsw));
        WR32(s + 0x2db8, 1);
        WR32(s + 0xf0, 0);
        WR32(s + 0x2dc0, 1);
        WR32(s + 0x2dc4, 1);
        // two points ahead / behind along the bike's z axis (m6..m8), on the floor
        auto ahead = [&](fpu_t t2, uint32_t at) {
            const float l = static_cast<float>(t2);
            WRF32(at, static_cast<float>((t2 * LD32(s + 0xd4) + LD32(s + 0xe0)) + LD32(s + 0x30)));
            WRF32(at + 4, static_cast<float>((LD32(s + 0xd8) * F_(l) + LD32(s + 0xe4)) + LD32(s + 0x34)));
            WRF32(at + 8, static_cast<float>((LD32(s + 0xdc) * F_(l) + LD32(s + 0xe8)) + LD32(s + 0x38)));
        };
        ahead(LD32(T) * LD32(0x4d0e94), B + 0x54);
        ahead(LD32(T) * LD32(0x4d0e98), B + 0x60);
        const uint32_t i60 = gcall(f_0040c7e0, B, 0x46c398, TRACK, {RD32(s + 0x31c1), B + 0x60, 0}, fsw);
        const uint32_t i54 = gcall(f_0040c7e0, B, 0x46c3b2, TRACK, {RD32(s + 0x31c1), B + 0x54, 0}, fsw);
        WRF32(B + 0x64, static_cast<float>(sector_height(TRACK, i60, B + 0x60, fsw)));
        WRF32(B + 0x58, static_cast<float>(sector_height(TRACK, i54, B + 0x54, fsw)));
        float v60[3], v54[3], z[3], x[3], y[3];
        load3(B + 0x60, v60);
        load3(B + 0x54, v54);
        vsub(v60, v54, z);
        normalize(z, fsw);
        const float up[3] = {0.f, 1.f, 0.f};
        cross(up, z, x);
        cross(z, x, y);
        const fpu_t xl = length(x);
        FCOM_L(fsw, xl, LD32(0x4d0e54));
        const float xlf = static_cast<float>(xl);
        if (!(fsw & 0x4000u)) {
            const fpu_t r = static_cast<fpu_t>(1) / F_(xlf);
            x[0] = static_cast<float>(F_(x[0]) * r);
            x[1] = static_cast<float>(F_(x[1]) * r);
            x[2] = static_cast<float>(r * F_(x[2]));
        }
        normalize(y, fsw);
        store3(s + 0xbc, x);
        store3(s + 0xc8, y);
        store3(s + 0xd4, z);
        WR32(s + 0xe0, 0);
        WR32(s + 0xe4, 0);
        WR32(s + 0xe8, 0);
    }
    // front / back points
    {
        const fpu_t t2 = LD32(T + 0x20) * LD32(0x4d102c);
        const float sa = static_cast<float>(LD32(T + 8) * LD32(0x4d1030) + t2);
        const float ua = static_cast<float>(LD32(T) * LD32(0x4d0e94));
        const fpu_t m3 = LD32(s + 0xc8), m4 = LD32(s + 0xcc), m5 = LD32(s + 0xd0);
        const fpu_t m6 = LD32(s + 0xd4), m7 = LD32(s + 0xd8), m8 = LD32(s + 0xdc);
        const fpu_t m9 = LD32(s + 0xe0), m10 = LD32(s + 0xe4), m11 = LD32(s + 0xe8);
        const fpu_t px = LD32(s + 0x30), py = LD32(s + 0x34), pz = LD32(s + 0x38);
        WRF32(s + 0x338, static_cast<float>(((m3 * F_(sa) + m6 * F_(ua)) + m9) + px));
        WRF32(s + 0x33c, static_cast<float>(((m7 * F_(ua) + m4 * F_(sa)) + m10) + py));
        WRF32(s + 0x340, static_cast<float>(((m5 * F_(sa) + m8 * F_(ua)) + m11) + pz));
        const float sb = static_cast<float>(t2 + LD32(T + 4) * LD32(0x4d1030));
        const fpu_t ub_ = LD32(T) * LD32(0x4d0e98);
        const float ub = static_cast<float>(ub_);
        WRF32(s + 0x1b4, static_cast<float>(((ub_ * m6 + m3 * F_(sb)) + m9) + px));
        WRF32(s + 0x1b8, static_cast<float>(((m4 * F_(sb) + m7 * F_(ub)) + m10) + py));
        WRF32(s + 0x1bc, static_cast<float>(((m8 * F_(ub) + m5 * F_(sb)) + m11) + pz));
    }
    gcall(f_00469c60, B, 0x46c68e, s, {}, fsw);
    // lean +0x2e1d: scaled (in contact) or damped (in the air, unless +0x31d1), clamped to +-pi/4,
    // then smoothed against the previous lean +0x2e21
    const uint32_t lean = s + 0x2e1d;
    auto clamp = [&] {
        FCOM_L(fsw, LD32(lean), static_cast<fpu_t>(RDF64(0x4d1218)));
        if (fsw & 0x100u) WR32(lean, 0xbf490fdb);
        FCOM_L(fsw, LD32(lean), static_cast<fpu_t>(RDF64(0x4d12c8)));
        if (!(fsw & 0x4100u)) WR32(lean, 0x3f490fdb);
    };
    if (edi) {
        WRF32(lean, static_cast<float>(LD32(lean) * static_cast<fpu_t>(RDF64(0x4d1038))));
        clamp();
    } else if (!(RD32(s + 0x31d1) & 0x7fffffffu)) {
        WRF32(lean, static_cast<float>(LD32(lean) * static_cast<fpu_t>(RDF64(0x4d0fe0))));
        clamp();
    } else {
        WR32(lean, 0);
    }
    WRF32(lean, static_cast<float>((LD32(lean) - LD32(s + 0x2e21)) * static_cast<fpu_t>(RDF64(0x4d0f08)) + LD32(s + 0x2e21)));
    WR32(s + 0x2e21, RD32(lean));
    gcall(f_00466940, B, 0x46c752, s, {}, fsw);
    gcall(f_00466a80, B, 0x46c759, s, {}, fsw);
    gcall(f_00461aa0, B, 0x46c760, s, {}, fsw);
    const uint32_t old_2dc8 = RD32(s + 0x2dc8);
    WR32(s + 0x2dc8, saved_2dc8);
    gcall(f_00466d70, B, 0x46c778, s, {old_2dc8}, fsw);
    gcall(f_004683e0, B, 0x46c77f, s, {}, fsw);
    gcall(f_00467b30, B, 0x46c788, s, {1}, fsw);
    gcall(f_00467930, B, 0x46c78f, s, {}, fsw);
    R.esp = sp + 8;
}

// ---- 0x461aa0: a bike's contact frame (thiscall bike, no args) ----
// Six points around the bike (+0x2ea9.., 12 bytes each) from its position (+0x30), orientation
// matrix (+0xbc) and two offsets (+0x1b4 front, +0x338 back): the frame aims from the position at the
// point 50 units ahead (0x458e20, 0x40fd40, inverted 0x40fe80) and spreads +-40 sideways; the middle
// pair is the average of points 3 and 1, the last one of points 0 and 4 times [0x4d1018]. Each point is
// put on the track (sector search 0x40c7e0, then 0x40c960) and lifted by [0x4d0f80] ([0x4d1020] for
// points 2 and 5). With [0x52c234] and [+0x30c1]: four more points (+0x30ed..) around +0x30b5 from
// the angle 0x490220 (+0x3085), their heights from the floor (0x40c8b0) + [0x4d0f60].
// Callers reload eax / ecx / edx afterwards (check_abi).
void bike_points() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, s = R.ecx;
    const uint32_t B = sp - 0xa0;   // esp after the x86's prologue
    uint32_t fsw = F.sw;
    float mbc[12], pos[3], p1b4[3], p338[3];
    load12(s + 0xbc, mbc);
    load3(s + 0x30, pos);
    load3(s + 0x1b4, p1b4);
    load3(s + 0x338, p338);
    float v40[3] = {0.f, 0.f, f32(0x42480000)}, v64[3], v10[3], v94[3], m10[12], minv[12], v4c[3], v58[3];
    xform(v40, mbc, v64);
    vadd(pos, v64, v10);
    dir_angles(pos, v10, v94, fsw);
    euler(v94, m10);
    inverse(m10, minv);
    v40[0] = f32(0x42200000);
    v40[1] = v40[2] = 0.f;
    xform(v40, minv, v4c);
    v10[0] = v10[1] = 0.f;
    v10[2] = f32(0x42200000);
    xform(v10, mbc, v58);
    vadd(p1b4, v58, v64);
    vsub(v64, v4c, v10);
    store3(s + 0x2ea9, v10);
    vadd(p1b4, v58, v64);
    vadd(v64, v4c, v10);
    store3(s + 0x2eb5, v10);
    vsub(p338, v58, v64);
    vadd(v64, v4c, v10);
    store3(s + 0x2ecd, v10);
    vsub(p338, v58, v64);
    vsub(v64, v4c, v10);
    store3(s + 0x2ed9, v10);
    float a3[3], a1[3];
    load3(s + 0x2ecd, a3);
    load3(s + 0x2eb5, a1);
    vadd(a3, a1, v64);
    vdiv(v64, f32(0x40000000), v10);
    store3(s + 0x2ec1, v10);
    const fpu_t half = LD32(0x4d1018);
    const float mid[3] = {static_cast<float>((LD32(s + 0x2ea9) + LD32(s + 0x2ed9)) * half),
                          static_cast<float>((LD32(s + 0x2edd) + LD32(s + 0x2ead)) * half),
                          static_cast<float>((LD32(s + 0x2ee1) + LD32(s + 0x2eb1)) * half)};
    store3(s + 0x2ee5, mid);
    for (uint32_t i = 0; i < 6; i++) {
        const uint32_t pt = s + 0x2ea9 + 12 * i;
        const uint32_t idx = sector_search(B - 0x10, 0x461cb8, RD32(s + 0x31c1), pt, fsw);
        uint32_t out[3];
        sector_point_core(TRACK, idx, pt, out, fsw);
        WR32(pt, out[0]);
        WR32(pt + 4, out[1]);
        WR32(pt + 8, out[2]);
        WRF32(pt + 4, static_cast<float>(LD32(pt + 4) + LD32((i == 2 || i == 5) ? 0x4d1020 : 0x4d0f80)));
    }
    if (RD32A(0x52c234) && RD32(s + 0x30c1)) {
        n_ground++;
        uint32_t eax_unused = 0;
        const fpu_t ang = angle_core(s + 0x3085, fsw, eax_unused);
        const float rot[3] = {0.f, static_cast<float>(-ang), 0.f};
        float m[12];
        euler(rot, m);
        const fpu_t c60 = LD32(0x4d0f60), c24 = LD32(0x4d1024), c28 = LD32(0x4d1028);
        const fpu_t m0 = F_(m[0]), m1 = F_(m[1]), m2 = F_(m[2]), m6 = F_(m[6]), m7 = F_(m[7]), m8 = F_(m[8]);
        const fpu_t m9 = F_(m[9]), m10_ = F_(m[10]), m11 = F_(m[11]);
        const fpu_t cx = LD32(s + 0x30b5), cy = LD32(s + 0x30b9), cz = LD32(s + 0x30bd);
        const fpu_t a6 = m6 * c60, a7 = m7 * c60, a8 = m8 * c60, b6 = m6 * c28, b7 = m7 * c28, b8 = m8 * c28;
        const float q[12] = {
            static_cast<float>(((a6 - m0) * c24 + m9) + cx), static_cast<float>(((a7 - m1) * c24 + m10_) + cy),
            static_cast<float>(((a8 - m2) * c24 + m11) + cz),
            static_cast<float>(((a6 + m0) * c24 + m9) + cx), static_cast<float>(((a7 + m1) * c24 + m10_) + cy),
            static_cast<float>(((a8 + m2) * c24 + m11) + cz),
            static_cast<float>(((m0 + b6) * c24 + m9) + cx), static_cast<float>(((m1 + b7) * c24 + m10_) + cy),
            static_cast<float>(((m2 + b8) * c24 + m11) + cz),
            static_cast<float>(((b6 - m0) * c24 + m9) + cx), static_cast<float>(((b7 - m1) * c24 + m10_) + cy),
            static_cast<float>(((b8 - m2) * c24 + m11) + cz)};
        for (int i = 0; i < 12; i++) WRF32(s + 0x30ed + 4 * i, q[i]);
        const uint32_t idx = sector_search(B - 0x10, 0x461f3c, RD32(s + 0x31c1), s + 0x30b5, fsw);
        for (uint32_t k = 0; k < 4; k++) {
            const uint32_t pt = s + 0x30ed + 12 * k;
            WRF32(pt + 4, static_cast<float>(sector_height(TRACK, idx, pt, fsw) + c60));
        }
    }
    F.sw = static_cast<uint16_t>(fsw);
    R.esp = sp + 4;
}

Stat n_matmul, n_angle, n_seg, n_sector, n_points;

}  // namespace


Stat n_lean;

extern "C" int native_hot_469c60(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    MEM_LOCAL();
    // T == 0 with +0x307d > 0 (a crash tumble: an inlined rotation of the points): translated
    if (!RD32(0x509398 + 292 * RD32(R.ecx + 0x78)) && static_cast<int32_t>(RD32(R.ecx + 0x307d)) > 0) return 0;
    if (md == ON) { bike_lean(); return 1; }
    const uint32_t s = R.ecx, lo = 0x4db000, hi = 0x70c568;
    std::vector<Region> regions{{lo, hi - lo}};
    if (s < lo || s + 0x3209 > hi) regions.push_back({s, 0x3209});
    const uint64_t before = mismatches;
    Snapshot b0 = take(regions);
    const Snapshot stack0 = take({{R.esp - 0x4000, 0x4000}});
    bike_lean();
    Snapshot nat = take(regions);
    restore(b0);
    restore(stack0);
    R = b0.r;
    F = b0.f;
    f_00469c60_t();
    Snapshot tr = take(regions);
    nat.r.eax = tr.r.eax;   // the callers reload eax / ecx / edx
    nat.r.ecx = tr.r.ecx;
    nat.r.edx = tr.r.edx;
    compare("bike lean 0x469c60", nat, tr);
    if (mismatches != before && mismatches < 25) rt::log("native: (bike lean mismatch: bike 0x%08x)\n", s);
    count_check("bike lean 0x469c60", n_lean, before);
    return 1;
}

Stat n_drive;

// rand()'s seed: [_getptd() + 0x14], the CRT's per-thread data on the heap (restored in check mode)
uint32_t rand_seed_addr() {
    static uint32_t seed = 0;
    if (!seed) {
        const X86Regs r0 = R;
        const X87State f0 = F;
        R.esp -= 0x200;
        MEM_LOCAL();
        WR32A(R.esp, 0);
        f_004b85c0();
        seed = R.eax + 0x14;
        R = r0;
        F = f0;
    }
    return seed;
}

extern "C" int native_hot_46b120(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { bike_drive(); return 1; }
    // check: the callees (0x46cd20, 0x46bdc0, 0x45cb00, the sector search) read and write globals: the
    // whole writable image, rand()'s seed and the bike are restored between the runs and compared
    const uint32_t s = R.ecx, lo = 0x4db000, hi = 0x70c568;
    std::vector<Region> regions{{lo, hi - lo}, {rand_seed_addr(), 4}};
    if (s < lo || s + 0x3209 > hi) regions.push_back({s, 0x3209});
    const uint64_t before = mismatches;
    Snapshot b0 = take(regions);
    const Snapshot stack0 = take({{R.esp - 0x4000, 0x4000}});   // callees' stack, restored only
    bike_drive();
    Snapshot nat = take(regions);
    restore(b0);
    restore(stack0);
    R = b0.r;
    F = b0.f;
    f_0046b120_t();
    compare("bike drive 0x46b120", nat, take(regions));
    if (mismatches != before && mismatches < 25) rt::log("native: (bike drive mismatch: bike 0x%08x)\n", s);
    count_check("bike drive 0x46b120", n_drive, before);
    return 1;
}
Stat n_step;

extern "C" int native_hot_46bf80(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { bike_step(); return 1; }
    // check: the step calls much of the bike code (and rand()), which reads and writes globals, so the
    // whole writable image (.data / .bss, 0x4db000..0x70c568, the bikes included) is restored between
    // the two runs and compared; the bike object too if it lives elsewhere
    // Restored but not compared: the bike's speed (+0x118) and what 0x467930 derives from it (gear +0x7c, +0x11c,
    // +0xec, the acceleration +0x120), nor eax / ecx / edx (0x467930's): game_hook_ai_speed (0x467a37)
    // carries the speed's fraction in host memory, which the second run finds already advanced (both
    // runs call 0x467930 the same way).
    // and rand()'s seed: [_getptd() + 0x14], the CRT's per-thread data on the heap
    static uint32_t seed = 0;
    if (!seed) {
        const X86Regs r0 = R;
        const X87State f0 = F;
        R.esp -= 0x200;
        WR32A(R.esp, 0);
        f_004b85c0();
        seed = R.eax + 0x14;
        R = r0;
        F = f0;
    }
    const uint32_t s = R.ecx, lo = 0x4db000, hi = 0x70c568;
    std::vector<Region> regions{{lo, hi - lo}, {seed, 4}};
    if (s < lo || s + 0x3209 > hi) regions.push_back({s, 0x3209});
    const uint64_t before = mismatches;
    {
        Snapshot b0 = take(regions);
        const Snapshot stack0 = take({{R.esp - 0x4000, 0x4000}});   // callees' stack, restored only
        bike_step();
        Snapshot nat = take(regions);
        restore(b0);
        restore(stack0);
        R = b0.r;
        F = b0.f;
        f_0046bf80_t();
        Snapshot tr = take(regions);
        nat.r.eax = tr.r.eax;
        nat.r.ecx = tr.r.ecx;
        nat.r.edx = tr.r.edx;
        nat.f.sw = tr.f.sw;   // 0x467930's last compare is on the speed
        for (size_t k = 0; k < nat.mem.size(); k++) {
            const uint32_t base = nat.mem[k].first;
            for (uint32_t off : {0x7cu, 0xecu, 0x118u, 0x11cu, 0x120u}) {
                const uint32_t a = s + off;
                if (a >= base && a + 4 <= base + nat.mem[k].second.size())
                    memcpy(nat.mem[k].second.data() + (a - base), tr.mem[k].second.data() + (a - base), 4);
            }
        }
        compare("bike step 0x46bf80", nat, tr);
    }
    if (mismatches != before && mismatches < 25) rt::log("native: (bike step mismatch: bike 0x%08x)\n", s);
    count_check("bike step 0x46bf80", n_step, before);
    return 1;
}
extern "C" int native_hot_461aa0(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { bike_points(); return 1; }
    const uint32_t s = R.ecx;
    const uint64_t base = mismatches;
    check_abi("bike points 0x461aa0", bike_points, f_00461aa0_t,
              {{s + 0x2ea9, 72}, {s + 0x30ed, 48}, {PLANE_CALLS, 4}});
    count_check("bike points 0x461aa0", n_points, base);
    if (n_points.n % 5000 == 0) rt::log("native: bike points: %u of them with the four ground points\n", n_ground);
    return 1;
}

extern "C" int native_hot_490220(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { angle(); return 1; }
    const uint64_t base = mismatches;
    check("angle 0x490220", angle, f_00490220_t, {}, 1);
    count_check("angle 0x490220", n_angle, base);
    return 1;
}

extern "C" int native_hot_403a80(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { mat_mul(); return 1; }
    const uint64_t base = mismatches;
    check("matrix multiply 0x403a80", mat_mul, f_00403a80_t, {{RD32A(R.esp + 4), 48}});
    count_check("matrix multiply 0x403a80", n_matmul, base);
    return 1;
}

extern "C" int native_hot_4b0f60(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { seg_plane_call(); return 1; }
    const uint64_t base = mismatches;
    check("segment/plane 0x4b0f60", seg_plane_call, f_004b0f60_t, {{RD32A(R.esp + 4), 12}, {PLANE_CALLS, 4}});
    count_check("segment/plane 0x4b0f60", n_seg, base);
    return 1;
}

extern "C" int native_hot_40c960(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { sector_point(); return 1; }
    const uint64_t base = mismatches;
    check("sector point 0x40c960", sector_point, f_0040c960_t, {{RD32A(R.esp + 4), 12}, {PLANE_CALLS, 4}});
    count_check("sector point 0x40c960", n_sector, base);
    return 1;
}
