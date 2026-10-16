# PSP port (started 2026-10-16)

Owner request: the PSP render path (21-psp-feasibility.md step 3: a minimal PSP build, then GE
rendering, then the native vertex loops writing GE vertices).

## Toolchain and emulator (macOS, arm64)
- pspdev `v20261001` (`pspdev-macos-latest-arm64.tar.gz` from github.com/pspdev/pspdev releases),
  extracted to `~/pspdev`; GCC 15.2. Needs Homebrew `libmpc gmp mpfr zstd` (its cc1 links them).
  Environment: `export PSPDEV=$HOME/pspdev PATH=$HOME/pspdev/bin:$PATH`.
- pspdev ships SDL3 (and SDL2) for the PSP: window, input, audio, timers work through SDL3; SDL_GPU
  does not exist there (the render path needs the GE).
- PPSSPP 1.20.4: `brew install --cask --appdir=$HOME/Applications ppsspp-emulator` →
  `~/Applications/PPSSPPSDL.app` (notarized; the quarantine flag had to be cleared once, since a
  command-line launch can't answer the first-launch dialog).
- Running: `PPSSPPSDL <dir>/EBOOT.PBP` mounts `<dir>` as the UMD (`umd0:`; the working directory);
  files written there land on the host. A test program found 22 MB of free heap (standard 24 MB user
  memory).

## Build
```
export PSPDEV=$HOME/pspdev PATH=$HOME/pspdev/bin:$PATH
psp-cmake -S . -B build/psp -DGAME_EXE=<game.EXE> -DRECOMP_PYTHON=$PWD/.venv/bin/python
cmake --build build/psp -j8          # -> build/psp/EBOOT.PBP
```
Staging for PPSSPP: `build/psp/run/EBOOT.PBP` and the game files (without the CD-audio WAVs) copied
to `build/psp/run/game/` (a copy, so test runs never change the real install). The log is
`build/psp/run/motoracer.log`.

## What the PSP build changes
- CMake (`if(PSP)`): single-precision x87 (`RECOMP_FPU_FLOAT`), `RECOMP_COMPACT_GUEST`, `-G0`,
  `runtime/src/platform/psp.cpp` + `stack_mips.S`, `create_pbp_file(... MEMSIZE 1)` (the extended
  64 MB of PSP-2000/3000).
- Compact guest address space (`runtime.h`): one host block from `GUEST_LO` = 0x300000 (no MMU, no
  4 GB reservation): stack 0x300000–0x400000, image 0x400000–0x72f000, TEB/PEB 0x72f000, relocated
  engine tables 0x800000–0x810000, heap 0x810000 + `RECOMP_GUEST_HEAP_MB` (18), then the VirtualAlloc
  arena, of which only the first `VALLOC_BACKED` (1 MB) is backed: the CRT reserves 4 MB there and
  commits ~0.2 MB. Commits outside the block stop with a clear message (`check_guest_range`).
  `M = block - GUEST_LO`. Host stack for the recompiled code: 2 MB (desktop 512 MB).
- PSP adapter: zeroed `calloc` block, newlib files (no `ftruncate`: `SetEndOfFile` fails), folders
  = the working directory (next to the EBOOT; game files in `game/`), no crash signals.
- Portability fixes: `_setjmp` → `setjmp` (newlib), `int32_t` is `long` on this toolchain
  (`std::max<int32_t>`).

## Sizes
| | |
|---|---|
| Game code, MIPS `-O2` | 5.05 MB (arm64: 2.98 MB) |
| Game code, MIPS `-Os` | 5.79 MB (larger) |
| ELF total (`text` / `bss`) | 8.65 MB / 1.16 MB (runtime, SDL3, libstdc++ included) |
- A quarter of the MIPS game code is unaligned-access pairs (`lwl`/`lwr`, `swl`/`swr`: 15,573 of 60,127
  instructions in one file): guest addresses are not known to be aligned and the Allegrex traps on
  unaligned `lw`/`sw` (`__mips_strict_alignment`). Candidates: aligned accesses where the translator
  knows the address is aligned (esp/ebp-relative with aligned displacements), fewer register spills.
- Memory: game code 5 MB + runtime ~3.6 MB + guest block ~24 MB → fits the 64 MB models (~52 MB for
  the program), not yet the PSP-1000 (~22 MB free).

## Status
- 2026-10-16: the EBOOT boots in PPSSPP and the recompiled game runs through the CRT start, Win32
  setup, DirectDraw (640×480 display mode, surfaces) and Direct3D device enumeration, then stops at
  `SDL_CreateGPUDevice` (no SDL_GPU on the PSP) — next: the GE backend.
