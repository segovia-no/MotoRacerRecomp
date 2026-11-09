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
 * precision, docs/notes/21-psp-feasibility.md). FM(sin) is sin or sinf to match. */
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

/* Data of the program that a Windows game-code module reads through the program's import table */
#if defined(RECOMP_MODULE) && defined(_WIN32)
#define RECOMP_HOST_DATA __declspec(dllimport)
#else
#define RECOMP_HOST_DATA
#endif
RECOMP_HOST_DATA extern X86Regs R;
RECOMP_HOST_DATA extern X87State F;
RECOMP_HOST_DATA extern uint8_t *M;

typedef void (*RecompFn)(void);

/* ---------- tables of the generated code (recompiler/recomp_main.py) ---------- */
typedef struct { uint32_t va; RecompFn fn; } RecompEntry;   /* guest function at va */
typedef struct { uint32_t iat_va; const char *dll; const char *name; RecompFn fn; } ImportEntry;

/* A game-code module (MOTORACER_GAME_MODULE, docs/notes/28-easy-install.md) exports one of these as
   `recomp_module`; the host checks the version, then fills the import slots with its shims. */
#define RECOMP_MODULE_VERSION 1u
typedef struct {
    uint32_t version;
    const RecompEntry *functions;   /* sorted by va: the dispatch table */
    uint32_t function_count;
    const RecompEntry *fallbacks;   /* translated bodies (f_<va>_t) of the functions with a native version */
    uint32_t fallback_count;
    const ImportEntry *imports;     /* every IAT slot */
    uint32_t import_count;
    const char *const *slot_names;  /* imp_<DLL>_<NAME> of each import shim the code calls */
    RecompFn *slots;                /* filled by the host (NULL: not implemented) */
    uint32_t slot_count;
} RecompModule;
#if defined(RECOMP_MODULE) && defined(_WIN32)
#define RECOMP_EXPORT __declspec(dllexport)
#elif defined(RECOMP_MODULE)
#define RECOMP_EXPORT __attribute__((visibility("default")))
#else
#define RECOMP_EXPORT
#endif

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
#ifdef RECOMP_FPU_FLOAT
/* Guest doubles <-> the float x87 emulation by integer bit operations: on the PSP's single-precision
 * FPU the compiler's conversions are libgcc calls (__truncdfsf2 / __extendsfdf2, soft-float), which
 * the translated code made at every fld / fstp qword. Exactly the same results (round to nearest
 * even, as the libgcc routines in the default mode); zero, subnormal, infinite and NaN values outside
 * the fast paths still take the library conversion. */
float recomp_d2f_slow(uint64_t b);     /* x86_helpers.c: the library conversions, out of line */
uint64_t recomp_f2d_slow(float f);
static inline float recomp_d2f(uint64_t b) {
    const uint32_t hi = (uint32_t)(b >> 32), lo = (uint32_t)b, e = (hi >> 20) & 0x7ffu;
    uint32_t r;
    float f;
    if (__builtin_expect(e - 897u < 254u, 1)) {   /* a normal float exponent (1..254) */
        const uint32_t m = ((hi & 0xfffffu) << 3) | (lo >> 29), rest = lo & 0x1fffffffu;
        r = ((hi & 0x80000000u) | ((e - 896u) << 23)) + m;
        if (rest > 0x10000000u || (rest == 0x10000000u && (m & 1u))) r++;   /* a carry may reach infinity: correct */
        memcpy(&f, &r, 4);
        return f;
    }
    if (!(hi & 0x7fffffffu) && !lo) {   /* +-0 */
        r = hi;
        memcpy(&f, &r, 4);
        return f;
    }
    return recomp_d2f_slow(b);
}
static inline uint64_t recomp_f2d(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    const uint32_t e = (u >> 23) & 0xffu;
    if (__builtin_expect(e - 1u < 254u, 1))
        return (uint64_t)((u & 0x80000000u) | ((e + 896u) << 20) | ((u & 0x7fffffu) >> 3)) << 32 | (uint32_t)(u << 29);
    if (!(u & 0x7fffffffu)) return (uint64_t)u << 32;
    return recomp_f2d_slow(f);
}
static inline fpu_t RDF64(uint32_t a) { uint64_t v; memcpy(&v, M + a, 8); return recomp_d2f(v); }
static inline void WRF32(uint32_t a, float v) { memcpy(M + a, &v, 4); }
static inline void WRF64(uint32_t a, fpu_t v) { const uint64_t b = recomp_f2d(v); memcpy(M + a, &b, 8); }
#else
static inline double RDF64(uint32_t a) { double v; memcpy(&v, M + a, 8); return v; }
static inline void WRF32(uint32_t a, float v) { memcpy(M + a, &v, 4); }
static inline void WRF64(uint32_t a, double v) { memcpy(M + a, &v, 8); }
#endif
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
RECOMP_HOST_DATA extern uint32_t eh_resume_va;

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
int game_hook_ai_lod(void);        /* jump hook: nonzero = skip this bike's update this frame */
void game_hook_ai_lod_after(void);
void game_hook_opponents(void);
/* native versions (runtime/src/game/natives/native_render.cpp) */
int native_flat_vertices(void);
int native_tex_vertices(void);
void native_vertices_check(void);
void native_bucket_skip(void);
int native_flush_polygon(void);
int native_emit_triangle(void);
int native_emit_line(void);
/* sampling profiler (runtime/src/core/prof.cpp, MOTORACER_PROF=1): the region the running code is in,
   sampled by a timer. Generated code marks each native hook with a region of its own. */
RECOMP_HOST_DATA extern volatile uint8_t prof_region;
/* MOTORACER_NATIVE_OFF=name,name (dev): native hooks to skip (their translated code runs), by the
   region id their name gets (prof_register) */
RECOMP_HOST_DATA extern uint8_t native_off[256];
/* one call statement for the translator's register spill pass, also when the hook is skipped */
#define NATIVE_CALL(fn) (native_off[prof_id_] ? 0 : fn())
uint8_t prof_register(const char *name);
#define PROF_IN(NAME)                                                                            \
    static uint8_t prof_id_;                                                                     \
    const uint8_t prof_prev_ = prof_region;                                                      \
    prof_region = prof_id_ ? prof_id_ : (prof_id_ = prof_register(NAME))
#define PROF_OUT() (prof_region = prof_prev_)
/* RECOMP_PROF_FUNCS (CMake MOTORACER_PROF_FUNCS, a profiling build): every translated function
   records its guest address while it runs (restored on return), and the sampler counts them: the
   hot functions on real hardware. A load and two stores per call; nothing otherwise. */
#ifdef RECOMP_PROF_FUNCS
RECOMP_HOST_DATA extern volatile uint32_t prof_fn;
static inline void prof_fn_restore_(uint32_t *prev) { prof_fn = *prev; }
#define PROF_FN(VA) uint32_t prof_fn_prev_ __attribute__((cleanup(prof_fn_restore_))) = prof_fn; prof_fn = (VA)
#else
#define PROF_FN(VA) ((void)0)
#endif
int native_blend_states(void);
int native_wrap_states(void);
int native_state_1a(void);
void native_flush_check(void);
void game_hook_bikes_begin(void);   /* cockpit view with the Z-buffer (native_render.cpp) */
void game_hook_cockpit_begin(void);
void game_hook_cockpit_end(void);
/* whole functions (NATIVE_FUNCS in translate.py, runtime/src/game/natives/native_geom.cpp): nonzero = done */
int native_transform(void);
int native_faces(void);
int native_poly_setup(void);
int native_faces2(void);
int native_poly_special(void);
int native_clip(void);
int native_poly_record_init(void);
int native_sector_search(void);
int native_getc(void);   /* runtime/src/game/natives/native_load.cpp */
int native_cfile_read(void);
int native_rgb_row(void);
int native_sprintf(void);
int native_vsprintf(void);
int native_output(void);
int native_sprite_blit(void);   /* runtime/src/game/natives/native_menu.cpp */
int native_tile_fill(void);
/* hot race functions (runtime/src/native_race_a..d.cpp) */
int native_hot_46b120(void);
int native_hot_46bdc0(void);
int native_hot_46cd20(void);
int native_hot_46c970(void);
int native_hot_410010(void);
int native_hot_40fd40(void);
int native_hot_469c60(void);
int native_hot_467930(void);
int native_hot_48e180(void);
int native_hot_466a80(void);
int native_hot_45b4e0(void);
int native_hot_46bf80(void);
int native_hot_461aa0(void);
int native_hot_490220(void);
int native_hot_403a80(void);
int native_hot_4b0f60(void);
int native_hot_40c960(void);
int native_hot_493820(void);
int native_hot_493490(void);
int native_hot_492b10(void);
int native_hot_485e50(void);
int native_hot_4b3400(void);
int native_hot_495bd0(void);
int native_hot_473a90(void);
int native_hot_4b32d0(void);
int native_hot_4039c0(void);
int native_hot_403a20(void);
int native_hot_403740(void);
int native_hot_458b80(void);
int native_hot_41e1b0(void);
int native_hot_490b90(void);
int native_hot_40d3b0(void);
int native_hot_49a160(void);
int native_hot_46e8b0(void);
int native_hot_422540(void);
int native_hot_464190(void);
int native_hot_403710(void);
int native_hot_40ce60(void);
int native_hot_497e40(void);
int native_hot_497d60(void);
void native_rgb_row_check(void);
int native_formatv_len(void);
void native_formatv_len_check(void);
int game_hook_options_rows(void);     /* jump hooks: nonzero = take the jump */
int game_hook_options_action(void);
int game_hook_options_info(void);
int game_hook_joy_label(void);
void game_hook_tracks_loaded(void);
void game_hook_tracks_save_begin(void);
void game_hook_tracks_save_end(void);
void game_hook_pitch_sim(void);
void game_hook_pitch_view(void);

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
