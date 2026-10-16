# Translator speed pass (2026-10-16)

Owner request (PSP step 2, 21-psp-feasibility.md): make the translated code smaller and faster for
every function, before native rewrites of the hot ones (22-hot-functions.md).

## What made the old output slow
- Guest registers lived only in the global `R` (`R.eax`, …). Guest memory is written through
  `uint8_t` pointers (`memcpy` into `M + a`), which may alias anything, and the game code is built
  with `-fno-strict-aliasing`: after every guest store the compiler had to reload every register it
  used from `R`, and the global `M` itself.
- Arithmetic flags were already C locals per function (the compiler drops the unused ones).

## Changes
1. **Registers in locals** (`cache_registers` in `translate.py`, macros in `recomp.h`):
   - every function starts with `REGS_LOCAL()` (`uint32_t eax = R.eax, …` for eax..edi incl. esp);
   - the body uses the locals; `PUSH32`/`POP32` become `LPUSH32`/`LPOP32` on the local `esp`;
   - `SPILL()` writes them to `R` right before each call that can read or write registers (guest
     functions, imports, `call_indirect`, `REP_*`, `x86_*` helpers, game hooks, EH helpers) and before
     every `return`; `RELOAD()` reads them back right after the call;
   - helpers that never touch `R` (memory, flags, x87, `IC`) are listed in `PURE_CALLS`; any other
     call must be a whole statement so it can be wrapped, otherwise the function keeps the old form;
   - functions that register an SEH frame (`setjmp`; 228 of 4,106) keep the old form, since locals
     are not reliable after `longjmp`;
   - jump hooks and `in` now evaluate their call as a statement (`h = hook(); if (h) goto …`).
2. **Local memory base**: every function starts with `MEM_LOCAL()` (`uint8_t *const mb = M`), and in
   generated code (`RECOMP_GENERATED`, set for the `gamecode` target) `RD*`/`WR*` are macros on `mb`.
   Their temporaries have distinct names (`r_`, `w_`, `p_`): with one shared name `LPUSH32(x)` →
   `WR32(esp, v_)` initialised the inner `v_` from itself (caught by the differential tester).
3. **x87 stack in locals** (`x87_locals` in `translate.py`):
   - MSVC code keeps the x87 stack empty at calls and returns (a float result stays in ST0), so the
     depth relative to function entry is known at every instruction. A dataflow pass over the
     function's instructions (successors recorded during emission) computes it; `ST(i)` at depth d
     becomes the C local `x{d-1-i}`, `FPUSH(v)` `x{d} = v`, `FPOP()` nothing, the status word the
     local `fsw` (`FCOM_L`, `FSTSW` with TOP = `F.top - d`);
   - the global stack `F` stays at its entry `top` inside the function and is used only across
     calls: before a call the live locals are pushed onto it, after the call the callee's net effect
     is applied and the values read back (`FXAM`/`FPREM` likewise);
   - net effects ("deltas") come from a pre-pass over all functions (`Translator.analyze`, fixpoint
     over the call graph): +1 for a float result, −1 for `_ftol` (pops its argument), usually 0;
   - a function keeps the global form if the depth is inconsistent at a join, goes below entry (it
     reads values the caller left, like `_ftol`: 72 functions), exceeds 8, calls a function without a
     known delta (3), makes an indirect call with values on the stack (1), or touches `F.top` /
     the FPU environment (1). 294 functions use locals, including all hot ones (22-hot-functions.md).
4. **Aligned accesses** (`align_accesses`, 2026-10-17): accesses whose address is provably aligned
   use `RD32A` / `WR32A` & co. (`memcpy` with `__builtin_assume_aligned`): absolute addresses that are
   multiples of the size (4 for 32/64-bit, 2 for 16-bit), and esp / ebp + a multiple of 4 in functions
   that keep esp 4-aligned (no 16-bit pushes, no odd esp arithmetic) or have a standard `push ebp;
   mov ebp, esp` frame with no other ebp writes; push/pop on the local esp likewise (`LPUSH32A`).
   No effect on x86/arm64 code; on the PSP's MIPS (strict alignment) one `lw`/`sw` instead of
   `lwl`+`lwr` / `swl`+`swr`.
5. **Fewer register spills** (`optimize_spills`, 2026-10-17):
   - a dataflow pass (per instruction, union at joins) tracks which locals were written since the
     last call or function entry; before a call only those are stored (`SPILL_M(va, mask)`);
   - after a call that follows the x86 calling convention (guest functions, imports, call_indirect)
     only eax, ecx, edx and esp are read back (`RELOAD_ABI`); ebx, esi, edi, ebp are callee-saved;
   - except for guest functions that change callee-saved registers: a pre-pass (`own_clobbers`,
     fixpoint over calls and tail jumps) marks functions that write ebx/esi/edi/ebp without pushing
     them, e.g. MSVC's `_EH_prolog` (0x4b6768), which sets the caller's ebp, and function fragments
     entered by (conditional) tail jumps that run their parent's epilogue; calls to them read back
     everything;
   - calls inside a block that then returns (tail jumps, faults: `if (c) { f(); return; }`) restore
     the dirty set for the code after the block, and use a plain read-back;
   - check mode: CMake `MOTORACER_CHECK_SAVED` (`RECOMP_CHECK_SAVED`) logs "check saved: ..." when a
     register considered clean differs from R before a call, or a convention callee changed a
     callee-saved register. Found, in order: `_EH_prolog`; the in-line conditional calls; fragments
     entered by conditional tail jumps; tail-call read-backs (false positives). After the fixes a
     race with menus (60 fps, steering) logs nothing.
- CMake `MOTORACER_GUEST_OPT` (default ON); OFF builds the old translation (`recomp_main.py
  --no-regcache`, no `RECOMP_GENERATED`) for benchmarks. `MOTORACER_TRANSLATOR_FLAGS` passes extra
  translator flags, e.g. `--no-x87-locals`, `--no-spill-opt`.

## Verification
- `tools/recomp/tests/difftest.py` (now compiled with `RECOMP_GENERATED` like the game): 2,405
  instruction forms, 0 failing cases, with both changes.
- For the x87 locals it also runs every x87 form a second time (`[locals]` cases, 266): no `fninit` in
  the test code (the reference runs one separately), 4 preloaded values, and exactly as many `fstp` as
  are left, so the depth is static; 213 of them compile with locals (the rest use `F.top` directly).
  2,671 cases, 0 failing. Mutation check: mapping `ST(i)` to `x{i}` instead of `x{d-1-i}` fails the
  `fxch` cases at once.
- All generated files compile with `-Wuninitialized` without warnings (self-initialised temporaries).
- E2E races (30 and 60 fps): rendering, steering, lean and AI as before (owner watched two runs).

## Measured
| | Before | Registers in locals | + local memory base |
|---|---|---|---|
| Game code (`__text` of the generated objects, arm64) | 3.99 MB | 3.60 MB | 2.91 MB |

Game thread per race frame (Speed Bay, PSP profile 480×270, 30 fps, vsync off, 23 opponents; median of
the last 14 two-second windows; two alternating rounds; includes the runtime's work on that thread,
which did not change):
| | Before (`MOTORACER_GUEST_OPT=OFF`) | After |
|---|---|---|
| Instructions | 11.71 / 11.64 M | 6.89 / 6.88 M (−41%) |
| Cycles | 7.25 / 6.25 M | 4.63 / 3.73 M (−37%) |
| Wall time outside pacing | 3.72 / 3.85 ms | 2.48 / 2.55 ms |

x87 stack in locals (same setup, but a race with left/right steering — more crashes and recoveries,
so more work per frame than the table above; two alternating rounds):
| | Without (`--no-x87-locals`) | With |
|---|---|---|
| Instructions | 9.97 / 9.21 M | 7.95 / 8.00 M (−13…20%) |
| Cycles | 5.61 / 5.02 M | 4.05 / 4.02 M (−20…28%) |
| Game code (`__text`) | 2.91 MB | 3.09 MB (spill/reload code at calls) |

Other checks (2026-10-16):
- **Unused flags are already free**: stripping every `pf`/`af` computation from a generated file by
  hand changed its object size by 12 bytes (120,856 vs 120,868) — the C compiler removes dead flag
  computations within a function, so no translator flag-liveness pass is needed.
- **`-O2`** for the game code (CMake `MOTORACER_GUEST_OPT_LEVEL`, now the default; existing build
  directories keep their cached value, set it with `-DMOTORACER_GUEST_OPT_LEVEL=-O2`): 2–3.5% fewer
  instructions per frame than `-O1` in two alternating rounds (cycles too noisy to tell), code
  3.09 → 3.01 MB, full parallel build of the game code ~20 s on an M3 Pro.

Aligned accesses + fewer spills (same setup, straight race, PSP profile; second round — the first
round of the optimised build stalled in a menu with the same keys: not reproduced in two reruns,
treated as a missed key):
| | `--no-spill-opt` | With |
|---|---|---|
| Instructions | 7.39 / 7.30 M | 4.23 M (−42%) |
| Cycles | 4.35 / 4.38 M | 3.13 M (−29%) |
| Game code arm64 / MIPS | 3.01 / 5.05 MB | 2.86 / 3.93 MB |

## Benchmarking notes
- Method: build both variants (`cmake -B build/benchbase -DMOTORACER_GUEST_OPT=OFF …`), run the same
  E2E race alternately with `MOTORACER_FRAME_WORK=1` and compare the game thread's instructions and
  cycles per frame (`thread_selfcounts`, macOS only), not wall time.
- Wall time per frame (`MOTORACER_FRAME_WORK=1`, time outside frame pacing) is too noisy on Apple
  Silicon at a capped frame rate: the same build measured 1.1 and 2.2 ms in two runs, menus included —
  a light load lets the scheduler use efficiency cores. Whole-process counters (`proc_pid_rusage`) are
  dominated by other threads (Metal, audio: 8–10 M instructions per frame) and hide the difference.
- Unlimited frame rate is not a usable benchmark: at ~1,000 fps (1 ms frames) the bike's direction
  and lean break (whole-ms frame times, see 16-frame-rate.md); the owner saw it in a benchmark run.
