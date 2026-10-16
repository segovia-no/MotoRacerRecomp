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
- CMake `MOTORACER_GUEST_OPT` (default ON); OFF builds the old translation (`recomp_main.py
  --no-regcache`, no `RECOMP_GENERATED`) for benchmarks.

## Verification
- `tools/recomp/tests/difftest.py` (now compiled with `RECOMP_GENERATED` like the game): 2,405
  instruction forms, 0 failing cases, with both changes.
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
