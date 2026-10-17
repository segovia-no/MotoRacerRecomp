/* Guest CPU model shared by generated code and the runtime.
 *
 * The guest is a single-threaded 32-bit x86 program. Its whole address space lives in one
 * host allocation `M` (4 GB reserved), so guest pointers are plain uint32_t offsets and the
 * PE image sits at its original virtual addresses.
 */
#pragma once
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df;      /* direction flag: survives calls, unlike the arithmetic flags */
    uint32_t fs_base; /* guest VA of the TEB */
} X86Regs;

/* x87 registers are emulated in double precision, or in float with RECOMP_FPU_FLOAT (CMake
 * MOTORACER_FPU_FLOAT: for CPUs with a single-precision FPU, e.g. the PSP; the game computes in single
 * precision, docs/21-psp-feasibility.md). FM(sin) is sin or sinf to match. */
/* RECOMP_COUNT_INSNS (CMake MOTORACER_COUNT_INSNS): every translated instruction counts itself, in
 * total, per guest function (indexed by the function's entry address - 0x400000) and per
 * instruction address. */
#ifdef RECOMP_COUNT_INSNS
extern uint64_t recomp_icount;
extern uint64_t recomp_icf[0x100000];
extern uint32_t recomp_icv[0x100000];   /* per instruction address - 0x400000 (hot spots inside functions) */
#define IC(fn, va) (recomp_icount++, recomp_icf[((fn) - 0x400000u) & 0xFFFFFu]++, recomp_icv[((va) - 0x400000u) & 0xFFFFFu]++)
#else
#define IC(fn, va) ((void)0)
#endif

#ifdef RECOMP_FPU_FLOAT
typedef float fpu_t;
#define FM(fn) fn##f
#else
typedef double fpu_t;
#define FM(fn) fn
#endif

typedef struct {
    fpu_t st[8];
    uint32_t top;
    uint16_t cw;  /* control word (rounding mode in bits 10-11) */
    uint16_t sw;  /* status word without TOP; C0=0x100 C1=0x200 C2=0x400 C3=0x4000 */
    uint16_t tag;
    uint64_t mm[8]; /* MMX registers (not aliased onto st[]; game never mixes them) */
} X87State;

extern X86Regs R;
extern X87State F;
extern uint8_t *M;

typedef void (*RecompFn)(void);

/* ---------- guest memory ---------- */
static inline uint8_t RD8(uint32_t a) { return M[a]; }
static inline uint16_t RD16(uint32_t a) { uint16_t v; memcpy(&v, M + a, 2); return v; }
static inline uint32_t RD32(uint32_t a) { uint32_t v; memcpy(&v, M + a, 4); return v; }
static inline uint64_t RD64(uint32_t a) { uint64_t v; memcpy(&v, M + a, 8); return v; }
static inline void WR8(uint32_t a, uint8_t v) { M[a] = v; }
static inline void WR16(uint32_t a, uint16_t v) { memcpy(M + a, &v, 2); }
static inline void WR32(uint32_t a, uint32_t v) { memcpy(M + a, &v, 4); }
static inline void WR64(uint32_t a, uint64_t v) { memcpy(M + a, &v, 8); }
static inline float RDF32(uint32_t a) { float v; memcpy(&v, M + a, 4); return v; }
static inline double RDF64(uint32_t a) { double v; memcpy(&v, M + a, 8); return v; }
static inline void WRF32(uint32_t a, float v) { memcpy(M + a, &v, 4); }
static inline void WRF64(uint32_t a, double v) { memcpy(M + a, &v, 8); }
fpu_t RDF80(uint32_t a);
void WRF80(uint32_t a, fpu_t v);

/* ---------- stack ---------- */
static inline void PUSH32(uint32_t v) { R.esp -= 4; WR32(R.esp, v); }
static inline void PUSH16(uint16_t v) { R.esp -= 2; WR16(R.esp, v); }
static inline uint32_t POP32(void) { uint32_t v = RD32(R.esp); R.esp += 4; return v; }
static inline uint16_t POP16(void) { uint16_t v = RD16(R.esp); R.esp += 2; return v; }

/* ---------- register caching (generated code, see cache_registers in translate.py) ----------
 * A translated function keeps eax..edi in locals; SPILL writes them to R before calls that may use
 * R and before returning, RELOAD reads them back after such calls. LPUSH32 & co. use the local esp. */
#define REGS_LOCAL()                                                                              \
    uint32_t eax = R.eax, ecx = R.ecx, edx = R.edx, ebx = R.ebx, esp = R.esp, ebp = R.ebp,        \
             esi = R.esi, edi = R.edi
#define SPILL()                                                                                   \
    (R.eax = eax, R.ecx = ecx, R.edx = edx, R.ebx = ebx, R.esp = esp, R.ebp = ebp, R.esi = esi,   \
     R.edi = edi)
#define RELOAD()                                                                                  \
    (eax = R.eax, ecx = R.ecx, edx = R.edx, ebx = R.ebx, esp = R.esp, ebp = R.ebp, esi = R.esi,   \
     edi = R.edi)
#define LPUSH32(v) do { uint32_t p_ = (v); esp -= 4; WR32(esp, p_); } while (0)
#define LPUSH16(v) do { uint16_t p_ = (v); esp -= 2; WR16(esp, p_); } while (0)
#define LPOP32() (esp += 4, RD32(esp - 4))
#define LPOP16() (esp += 2, RD16(esp - 2))
#define LPUSH32A(v) do { uint32_t p_ = (v); esp -= 4; WR32A(esp, p_); } while (0)
#define LPUSH16A(v) do { uint16_t p_ = (v); esp -= 2; WR16A(esp, p_); } while (0)
#define LPOP32A() (esp += 4, RD32A(esp - 4))
#define LPOP16A() (esp += 2, RD16A(esp - 2))
/* optimize_spills (translate.py): SPILL_M stores only the registers in mask m (bits: eax ecx edx
 * ebx esp ebp esi edi) — the others still equal R; RELOAD_ABI after a call that follows the x86
 * calling convention reads back only eax, ecx, edx and esp. RECOMP_CHECK_SAVED checks both. */
#ifdef RECOMP_CHECK_SAVED
void recomp_check_saved(uint32_t va, int what, uint32_t mask);
#define SPILL_M(va, m)                                                                            \
    (recomp_check_saved((va), 0, ((R.eax != eax) << 0 | (R.ecx != ecx) << 1 | (R.edx != edx) << 2 |  \
                                  (R.ebx != ebx) << 3 | (R.esp != esp) << 4 | (R.ebp != ebp) << 5 |  \
                                  (R.esi != esi) << 6 | (R.edi != edi) << 7) & ~(m)), SPILL())
#define RELOAD_ABI(va)                                                                            \
    (recomp_check_saved((va), 1, (R.ebx != ebx) << 3 | (R.ebp != ebp) << 5 | (R.esi != esi) << 6 |   \
                                 (R.edi != edi) << 7), RELOAD())
#else
#define SPILL_M(va, m)                                                                            \
    ((void)(va), ((m) & 0x01) ? (void)(R.eax = eax) : (void)0, ((m) & 0x02) ? (void)(R.ecx = ecx) : (void)0, \
     ((m) & 0x04) ? (void)(R.edx = edx) : (void)0, ((m) & 0x08) ? (void)(R.ebx = ebx) : (void)0,          \
     ((m) & 0x10) ? (void)(R.esp = esp) : (void)0, ((m) & 0x20) ? (void)(R.ebp = ebp) : (void)0,          \
     ((m) & 0x40) ? (void)(R.esi = esi) : (void)0, ((m) & 0x80) ? (void)(R.edi = edi) : (void)0)
#define RELOAD_ABI(va) ((void)(va), eax = R.eax, ecx = R.ecx, edx = R.edx, esp = R.esp)
#endif
/* x87 stack in locals (x87_locals in translate.py): FCOM on the local status word. */
#define FCOM_L(sw, a, b)                                                                         \
    do { fpu_t a_ = (a), b_ = (b); sw &= ~0x4500u;                                               \
         if (a_ != a_ || b_ != b_) sw |= 0x4500u; else if (a_ < b_) sw |= 0x0100u;               \
         else if (a_ == b_) sw |= 0x4000u; } while (0)

/* ---------- flags ---------- */
#if defined(__mips__)
/* no popcount on MIPS: __builtin_parity is a libgcc call there */
static inline uint32_t PARITY(uint32_t v) { v &= 0xff; v ^= v >> 4; return (0x9669u >> (v & 15)) & 1; }
#else
static inline uint32_t PARITY(uint32_t v) { return !__builtin_parity(v & 0xff); }
#endif
static inline uint32_t EFLAGS_PACK(uint32_t cf, uint32_t pf, uint32_t af, uint32_t zf,
                                   uint32_t sf, uint32_t of) {
    return 0x202u | cf | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | (R.df << 10) | (of << 11);
}
#define EFLAGS_UNPACK(v)                                                                     \
    do { uint32_t _f = (v); cf = _f & 1; pf = (_f >> 2) & 1; af = (_f >> 4) & 1;             \
         zf = (_f >> 6) & 1; sf = (_f >> 7) & 1; R.df = (_f >> 10) & 1; of = (_f >> 11) & 1; \
    } while (0)

/* ---------- x87 ---------- */
#define ST(i) F.st[(F.top + (i)) & 7]
static inline void FPUSH(fpu_t v) { F.top = (F.top - 1) & 7; F.st[F.top] = v; }
static inline void FPOP(void) { F.top = (F.top + 1) & 7; }
static inline uint16_t FSTSW(void) { return (uint16_t)((F.sw & ~0x3800) | ((F.top & 7) << 11)); }
static inline void FCOM(fpu_t a, fpu_t b) {
    F.sw &= ~0x4500;
    if (a != a || b != b) F.sw |= 0x4500;
    else if (a < b) F.sw |= 0x0100;
    else if (a == b) F.sw |= 0x4000;
}
/* MSVC _ftol, inlined by the translator: truncate to edx:eax (indefinite on overflow / NaN). The
 * int32 range takes a single conversion instruction (trunc.w.s on MIPS, no 64-bit libgcc call). */
static inline int64_t FTOL(fpu_t v) {
    if (v > (fpu_t)-2147483648.0 && v < (fpu_t)2147483648.0) return (int64_t)(int32_t)v;
    if (!(v >= (fpu_t)-9223372036854775808.0 && v < (fpu_t)9223372036854775808.0)) return INT64_MIN;
    return (int64_t)v;
}
void FXAM(void);
void FPREM(void);
fpu_t F_ROUND(fpu_t v);             /* round per F.cw rounding control */
int16_t F_TO_I16(fpu_t v);
int32_t F_TO_I32(fpu_t v);
int64_t F_TO_I64(fpu_t v);
void FNSTENV(uint32_t a);
void FLDENV(uint32_t a);

/* ---------- string instructions (honour R.df) ---------- */
void REP_MOVS(int size, int rep);
void REP_STOS(int size, int rep);
/* rep: 0 = none, 1 = repe/rep, 2 = repne. Flags written through pointers. */
void REP_SCAS(int size, int rep, uint32_t *cf, uint32_t *zf, uint32_t *sf, uint32_t *of,
              uint32_t *pf, uint32_t *af);
void REP_CMPS(int size, int rep, uint32_t *cf, uint32_t *zf, uint32_t *sf, uint32_t *of,
              uint32_t *pf, uint32_t *af);

/* ---------- structured / C++ exceptions ----------
 * A function that links an SEH registration record (mov fs:[0], esp) also records a setjmp
 * point; when the CRT later jumps to a catch continuation inside that frame, nonlocal_jump
 * longjmps back to the function, which then runs the continuation. */
#if defined(_WIN32) || defined(__PSP__)
/* Windows CRT: setjmp records the frame and longjmp unwinds to it through SEH (no signal masks).
 * PSP (newlib): no signal masks to skip, no _setjmp. */
#define RECOMP_SETJMP(jb) setjmp(jb)
#define RECOMP_LONGJMP(jb, v) longjmp(jb, v)
#else
/* POSIX: the variants that skip saving/restoring the signal mask. */
#define RECOMP_SETJMP(jb) _setjmp(jb)
#define RECOMP_LONGJMP(jb, v) _longjmp(jb, v)
#endif
void eh_register(uint32_t record, jmp_buf *jb);
void eh_unlink(uint32_t new_head);
void nonlocal_jump(uint32_t target);
extern uint32_t eh_resume_va;

/* ---------- game hooks ----------
 * Called by translated code just before specific instructions (GAME_HOOKS in translate.py). */
void game_hook_screen_mode_loaded(void);
void game_hook_mode_label(void);
void game_hook_detail_loaded(void);
void game_hook_bike_lod(void);
void game_hook_input_loaded(void);
void game_hook_band(void);
void game_hook_counter(void);
void game_hook_lean(void);
void game_hook_options_init(void);
void game_hook_button_unpacked(void);
void game_hook_options_button_nav(void);
void game_hook_main_menu_init(void);
void game_hook_main_menu_button_nav(void);
void game_hook_ai_speed(void);
void game_hook_opponents(void);
/* native versions (runtime/src/native_render.cpp) */
int native_flat_vertices(void);
int native_tex_vertices(void);
void native_vertices_check(void);
void native_bucket_skip(void);
/* whole functions (NATIVE_FUNCS in translate.py, runtime/src/native_geom.cpp): nonzero = done */
int native_transform(void);
int native_faces(void);
int native_poly_setup(void);
int native_faces2(void);
int native_poly_special(void);
int native_clip(void);
int native_getc(void);   /* runtime/src/native_load.cpp */
int native_cfile_read(void);
int native_rgb_row(void);
int native_sprintf(void);
int native_vsprintf(void);
int native_output(void);
void native_rgb_row_check(void);
int game_hook_options_rows(void);     /* jump hooks: nonzero = take the jump */
int game_hook_options_action(void);
int game_hook_options_info(void);

/* ---------- control transfer / traps ---------- */
void call_indirect(uint32_t va);   /* guest VA -> recompiled fn, import shim or COM thunk */
void x86_cpuid(void);
void x86_rdtsc(void);
uint32_t x86_in(uint32_t port, int size);
void x86_out(uint32_t port, uint32_t value, int size);
void x86_int(uint32_t va, int vector);
void x86_div_fault(uint32_t va);
void x86_unimpl(uint32_t va, const char *what);

/* Generated code (RECOMP_GENERATED): guest memory through a local copy of M (`mb`, declared by
 * MEM_LOCAL at the top of every translated function), so stores through it don't make the compiler
 * reload the global M. Defined last: the inline helpers above keep using M. */
#ifdef RECOMP_GENERATED
#include "recomp_mem_local.h"
#else
#define MEMBASE M
#define MEM_LOCAL() ((void)0)
#define RD16A RD16
#define RD32A RD32
#define RD64A RD64
#define RDF32A RDF32
#define RDF64A RDF64
#define WR16A WR16
#define WR32A WR32
#define WR64A WR64
#define WRF32A WRF32
#define WRF64A WRF64
#endif

#ifdef __cplusplus
}
#endif
