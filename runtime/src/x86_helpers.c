/* Out-of-line pieces of the guest CPU model: x87 conversions, string instructions, CPUID. */
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp.h"

X86Regs R;
X87State F = {.cw = 0x27f}; /* MSVC CRT default: 53-bit precision, round to nearest, all masked */
uint8_t *M;

/* ---------- 80-bit extended <-> double ---------- */
fpu_t RDF80(uint32_t a) {
    uint64_t mant = RD64(a);
    uint16_t se = RD16(a + 8);
    int sign = se >> 15;
    int exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7fff) v = (mant << 1) ? NAN : INFINITY;
    else if (exp != 0 && !(mant >> 63)) v = NAN;   /* unnormal: invalid operand on 387+ */
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void WRF80(uint32_t a, fpu_t v) {
    uint16_t se = signbit(v) ? 0x8000 : 0;
    uint64_t mant = 0;
    if (isnan(v)) { se |= 0x7fff; mant = 0xC000000000000000ull; }
    else if (isinf(v)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (v != 0.0) {
        int e;
        double f = frexp(fabs(v), &e);           /* f in [0.5, 1) */
        mant = (uint64_t)ldexp(f, 64);           /* exact: double has 53 bits */
        se |= (uint16_t)(e - 1 + 16383);
    }
    WR64(a, mant);
    WR16(a + 8, se);
}

/* ---------- rounding per control word ---------- */
fpu_t F_ROUND(fpu_t v) {
    switch ((F.cw >> 10) & 3) {
    case 0: return FM(nearbyint)(v); /* host default mode is round-to-nearest-even */
    case 1: return FM(floor)(v);
    case 2: return FM(ceil)(v);
    default: return FM(trunc)(v);
    }
}

#if defined(__mips__) && defined(RECOMP_FPU_FLOAT)
/* PSP: the FPU's own conversions (cvt.w.s rounds like the x87 default, round-to-nearest-even), no
 * double arithmetic (software on the Allegrex). */
static int32_t mips_round_w(float f, int mode) {
    float t;
    switch (mode) {
    case 0: __asm__("cvt.w.s %0, %1" : "=f"(t) : "f"(f)); break;
    case 1: __asm__("floor.w.s %0, %1" : "=f"(t) : "f"(f)); break;
    case 2: __asm__("ceil.w.s %0, %1" : "=f"(t) : "f"(f)); break;
    default: __asm__("trunc.w.s %0, %1" : "=f"(t) : "f"(f)); break;
    }
    int32_t r;
    memcpy(&r, &t, 4);
    return r;
}
#endif

int32_t F_TO_I32(fpu_t v) {
#if defined(__mips__) && defined(RECOMP_FPU_FLOAT)
    if (v > -2147483520.0f && v < 2147483520.0f) return mips_round_w(v, (F.cw >> 10) & 3);
#endif
    fpu_t r = F_ROUND(v);
    if (!(r >= (fpu_t)-2147483648.0 && r < (fpu_t)2147483648.0)) { F.sw |= 1; return INT32_MIN; }
    return (int32_t)r;
}

int16_t F_TO_I16(fpu_t v) {
    fpu_t r = F_ROUND(v);
    if (!(r >= (fpu_t)-32768.0 && r <= (fpu_t)32767.0)) { F.sw |= 1; return INT16_MIN; }
    return (int16_t)r;
}

int64_t F_TO_I64(fpu_t v) {
    fpu_t r = F_ROUND(v);
    if (!(r >= (fpu_t)-9223372036854775808.0 && r < (fpu_t)9223372036854775808.0)) { F.sw |= 1; return INT64_MIN; }
    return (int64_t)r;
}

void FXAM(void) {
    fpu_t v = ST(0);
    uint16_t c;                         /* C3 C2 C0 class bits; "empty" is never reported */
    if (isnan(v)) c = 0x0100;
    else if (isinf(v)) c = 0x0500;
    else if (v == 0.0) c = 0x4000;
    else if (fpclassify(v) == FP_SUBNORMAL) c = 0x4400;
    else c = 0x0400;
    F.sw = (uint16_t)((F.sw & ~0x4700) | c | (signbit(v) ? 0x0200 : 0));
}

/* Partial remainder (truncating). C0/C3/C1 receive quotient bits 2/1/0; C2=0 (complete). */
void FPREM(void) {
    fpu_t a = ST(0), b = ST(1);
    fpu_t r = FM(fmod)(a, b);
    uint64_t q = (uint64_t)fabs(trunc((a - r) / b));
    F.sw &= ~0x4700;
    if (q & 4) F.sw |= 0x0100;
    if (q & 2) F.sw |= 0x4000;
    if (q & 1) F.sw |= 0x0200;
    ST(0) = r;
}

/* 32-bit protected-mode environment layout (28 bytes). Tags are not tracked precisely. */
void FNSTENV(uint32_t a) {
    WR32(a + 0, 0xffff0000u | F.cw);
    WR32(a + 4, 0xffff0000u | FSTSW());
    WR32(a + 8, 0xffff0000u | F.tag);
    for (int i = 12; i < 28; i += 4) WR32(a + i, 0);
    F.cw |= 0x3f; /* fnstenv masks all exceptions */
}

void FLDENV(uint32_t a) {
    F.cw = RD16(a + 0);
    uint16_t sw = RD16(a + 4);
    F.top = (sw >> 11) & 7;
    F.sw = sw & ~0x3800;
    F.tag = RD16(a + 8);
}

/* ---------- string instructions ---------- */
static inline uint32_t rd_n(uint32_t a, int n) { return n == 1 ? RD8(a) : n == 2 ? RD16(a) : RD32(a); }
static inline void wr_n(uint32_t a, uint32_t v, int n) {
    if (n == 1) WR8(a, (uint8_t)v); else if (n == 2) WR16(a, (uint16_t)v); else WR32(a, v);
}

void REP_MOVS(int n, int rep) {
    int32_t step = R.df ? -n : n;
    if (!rep) {
        wr_n(R.edi, rd_n(R.esi, n), n);
        R.esi += step; R.edi += step;
        return;
    }
    uint32_t cnt = R.ecx;
    if (!R.df && (R.edi - R.esi >= cnt * (uint32_t)n)) {   /* forward, no destructive overlap */
        memmove(M + R.edi, M + R.esi, (size_t)cnt * n);
        R.esi += cnt * n; R.edi += cnt * n; R.ecx = 0;
        return;
    }
    for (; cnt; cnt--) { wr_n(R.edi, rd_n(R.esi, n), n); R.esi += step; R.edi += step; }
    R.ecx = 0;
}

void REP_STOS(int n, int rep) {
    int32_t step = R.df ? -n : n;
    uint32_t v = R.eax;
    if (!rep) { wr_n(R.edi, v, n); R.edi += step; return; }
    uint32_t cnt = R.ecx;
    if (!R.df && n == 1) { memset(M + R.edi, (uint8_t)v, cnt); R.edi += cnt; R.ecx = 0; return; }
    for (; cnt; cnt--) { wr_n(R.edi, v, n); R.edi += step; }
    R.ecx = 0;
}

static void cmp_flags(uint32_t a, uint32_t b, int n, uint32_t *cf, uint32_t *zf, uint32_t *sf,
                      uint32_t *of, uint32_t *pf, uint32_t *af) {
    uint32_t mask = n == 4 ? 0xffffffffu : (1u << (8 * n)) - 1, sb = 1u << (8 * n - 1);
    uint32_t r = (a - b) & mask;
    *cf = a < b; *zf = r == 0; *sf = (r & sb) != 0; *of = ((a ^ b) & (a ^ r) & sb) != 0;
    *pf = PARITY(r); *af = ((a ^ b ^ r) >> 4) & 1;
}

void REP_SCAS(int n, int rep, uint32_t *cf, uint32_t *zf, uint32_t *sf, uint32_t *of,
              uint32_t *pf, uint32_t *af) {
    int32_t step = R.df ? -n : n;
    uint32_t a = n == 1 ? (uint8_t)R.eax : n == 2 ? (uint16_t)R.eax : R.eax;
    if (!rep) { cmp_flags(a, rd_n(R.edi, n), n, cf, zf, sf, of, pf, af); R.edi += step; return; }
    while (R.ecx) {
        cmp_flags(a, rd_n(R.edi, n), n, cf, zf, sf, of, pf, af);
        R.edi += step; R.ecx--;
        if (rep == 1 ? !*zf : *zf) break;
    }
}

void REP_CMPS(int n, int rep, uint32_t *cf, uint32_t *zf, uint32_t *sf, uint32_t *of,
              uint32_t *pf, uint32_t *af) {
    int32_t step = R.df ? -n : n;
    if (!rep) {
        cmp_flags(rd_n(R.esi, n), rd_n(R.edi, n), n, cf, zf, sf, of, pf, af);
        R.esi += step; R.edi += step;
        return;
    }
    while (R.ecx) {
        cmp_flags(rd_n(R.esi, n), rd_n(R.edi, n), n, cf, zf, sf, of, pf, af);
        R.esi += step; R.edi += step; R.ecx--;
        if (rep == 1 ? !*zf : *zf) break;
    }
}

/* ---------- CPU identification ----------
 * Report a plain Pentium (family 5) without MMX so the game picks its baseline code paths.
 * Revisit if MMX paths turn out faster or required. */
void x86_cpuid(void) {
    switch (R.eax) {
    case 0:
        R.eax = 1;
        R.ebx = 0x756e6547; R.edx = 0x49656e69; R.ecx = 0x6c65746e; /* "GenuineIntel" */
        break;
    case 1:
        R.eax = 0x0543;           /* family 5, model 4, stepping 3 */
        R.ebx = 0;
        R.ecx = 0;
        R.edx = 0x000001bf;       /* FPU VME DE PSE TSC MSR MCE CX8, no MMX */
        break;
    default:
        R.eax = R.ebx = R.ecx = R.edx = 0;
    }
}
