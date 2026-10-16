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
#ifdef RECOMP_COUNT_INSNS
extern uint64_t recomp_icount;
#define IC() (recomp_icount++)
#else
#define IC() ((void)0)
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

/* ---------- flags ---------- */
static inline uint32_t PARITY(uint32_t v) { return !__builtin_parity(v & 0xff); }
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
#ifdef _WIN32
/* Windows CRT: setjmp records the frame and longjmp unwinds to it through SEH (no signal masks). */
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

#ifdef __cplusplus
}
#endif
