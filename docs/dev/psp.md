# PSP development

The PSP version is the same recompiler and runtime as the desktop versions, built with the pspdev
toolchain and a PSP backend. Players use `./setup.sh --psp` ([guides/psp-build.md](../guides/psp-build.md));
this page is for working on the port. History, measurements and decisions: [notes/25-psp-port.md](../notes/25-psp-port.md),
[notes/26-psp-profile.md](../notes/26-psp-profile.md), [notes/21-psp-feasibility.md](../notes/21-psp-feasibility.md).

## What differs from the desktop build
| Area | PSP | Where |
|---|---|---|
| Rendering | The PSP's GPU (GE) instead of SDL_GPU: Direct3D execute buffers and DirectDraw blits become GE display lists; textured 3D in the GE's 3D mode, indexed per vertex batch | `runtime/src/psp/d3d_ge.cpp`, `ge.cpp`, `gpu_psp.cpp` |
| Memory | No virtual memory: one compact guest block (image, stack, heap, VirtualAlloc arena) instead of a 4 GB reservation; `MOTORACER_PSP_1000=ON` fits the PSP-1000's 32 MB (guest heap `MOTORACER_PSP_HEAP_KB`, 13056) | `runtime/include/runtime.h`, `runtime/src/core/memory.cpp` |
| FPU | x87 emulated in single precision (`MOTORACER_FPU_FLOAT`, the Allegrex has no double-precision FPU) | `runtime/include/recomp.h` |
| Platform | Files, threads, input, audio through the PSP's own libraries (a small SDL subset, no SDL port) | `runtime/src/platform/psp.cpp`, `runtime/src/psp/sdl_psp.cpp` |
| CD music | MP3 decoded on the Media Engine (`sceMp3`); the package converts the WAV tracks | `runtime/src/psp/mp3_psp.cpp`, `scripts/psp/cd_mp3.sh` |
| Speed | Native rewrites of the hottest game functions (shared with the desktop), AI level of detail, draw distance 1 | `runtime/src/game/natives/`, `runtime/src/game/game_hooks.cpp` |

The CMake switch is the toolchain file: `if(PSP)` blocks in `CMakeLists.txt` select all of the above.

## Building by hand
Toolchain: pspdev `v20261001` ([releases](https://github.com/pspdev/pspdev/releases)); `./setup.sh --psp`
downloads it into `build/pspdev`, or use your own (`export PSPDEV=~/pspdev PATH=$PSPDEV/bin:$PATH`;
on macOS it needs Homebrew's `libmpc gmp mpfr zstd`).
```sh
cmake -S . -B build/psp -DCMAKE_TOOLCHAIN_FILE=$PSPDEV/psp/share/pspdev.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DGAME_EXE="<game folder>/game.EXE" -DRECOMP_PYTHON=$PWD/.venv/bin/python -DMOTORACER_PSP_1000=ON
cmake --build build/psp -j               # -> build/psp/EBOOT.PBP (with debug info, ~50 MB)
scripts/psp/package_hw.sh "<game folder>"   # -> build/psp-hw/PSP/GAME/MOTORACER (stripped EBOOT, game files, MP3)
```
`package_hw.sh`: `PSP_BUILD` / `PSP_OUT` choose the build and output folders; `PROF=1` adds a
`psp_env.txt` with the profiler on; `PROFILE_BUILD=<dir>` (and `_B`, `_C`, with `PROFILE_ENV_x`,
`PROFILE_TITLE_x`) adds more apps next to the game for hardware A/B tests. Home-menu images:
`scripts/psp/xmb_assets.sh`.

Without `MOTORACER_PSP_1000` the EBOOT asks for the PSP-2000/3000's extended memory (more heap, no
PSP-1000).

## Runtime options: `psp_env.txt`
Environment variables do not exist on the PSP: the runtime reads `KEY=VALUE` lines from `psp_env.txt`
next to the EBOOT. The useful ones:

| Option | Effect |
|---|---|
| `MOTORACER_PROF=1` | sampling profiler: every 5 s the log gets the frame time and where it went, by region (game code, natives, GE, audio, ...) |
| `MOTORACER_MEM_STATS=1` | guest heap / host memory statistics in the log (peak, largest free block, by caller) |
| `MOTORACER_GE_STATS=1` | GE backend counters: draws, triangles, textures, list flushes, time in Execute |
| `MOTORACER_NATIVE=0` | run the translated code instead of the native rewrites |
| `MOTORACER_AI_LOD=0` | update every opponent every frame |
| `MOTORACER_GE_STALL_STEP=<bytes>` | move the GE stall address every N bytes of list instead of every draw |
| `MOTORACER_MUTE=1` | audio at volume 0 (test runs) |
| `MOTORACER_LOG_SYNC=1` | flush the log at every line (crash hunting; slow) |
| `MOTORACER_GAME_DIR=<path>` | game files elsewhere (e.g. `ms0:/PSP/GAME/MOTORACER/game`) |

## Testing in PPSSPP
`scripts/psp/e2e_psp.sh <name> "<keys>" [seconds] [KEY=VALUE ...]` (macOS, PPSSPP in
`~/Applications/PPSSPPSDL.app`) stages `build/psp/run` (the EBOOT and a copy of the game files), writes
`psp_env.txt`, drives the game with scripted keys and makes a contact sheet `build/e2e/<name>.png` and
the log `build/e2e/<name>.log`. Keys use `MOTORACER_AUTOKEYS` syntax ([notes/13-e2e-testing.md](../notes/13-e2e-testing.md));
`PAD:A`, `PAD:DOWN`, ... send gamepad events (the TOP 5 screen after a race needs them). A Speed Bay race:
```sh
scripts/psp/e2e_psp.sh race "MOUSE:0.01,0.01 4000 ESC 1500 ESC 1500 ENTER 1500 ENTER 2000 LEFT 500 ENTER 1500 UP 300 UP 500 ENTER 1500 UP 300 UP 300 UP 500 ENTER 1500 DOWN 300 DOWN 500 ENTER 10000 +UP 15000 -UP" 70 MOTORACER_PROF=1
```

**PPSSPP is not a performance reference.** It models neither the PSP's 16 KB caches nor the cost of
cache misses (~79 CPU cycles per line) and kernel calls: memory-bound code runs about 3x slower on a real
PSP than PPSSPP suggests. Use PPSSPP for correctness; decide optimizations from hardware profiles.

## Testing on hardware
1. Package with a profiling app next to the game: build `build/psp-pf` (`-DMOTORACER_PROF_FUNCS=ON`: the
   profiler also names the hot translated functions; `-DMOTORACER_PSP_HEAP_KB=12800`, its code is larger),
   then `PROFILE_BUILD=build/psp-pf scripts/psp/package_hw.sh`.
2. Copy `build/psp-hw/PSP` to the memory stick, play a race for ~30 s in each app.
3. The logs are `PSP/GAME/MOTORACER*/motoracer.log` on the stick.

A crash writes its reason to the log (`FATAL: ...`): guest heap exhausted (with the largest free block),
host memory, an unaligned access (the Allegrex traps on misaligned `lw` / `sw`).

## Profiling without hardware
- Executed x86 instructions per guest function: a desktop counting build (`-DMOTORACER_COUNT_INSNS=ON
  -DMOTORACER_FPU_FLOAT=ON`), run with `MOTORACER_ICOUNT_DUMP=<dir>`, ranked by `scripts/dev/icprof.py`.
  The guest instruction stream is the same on every host, so this ranks translated code for the PSP too
  (not its memory costs).
- The PSP's frame split in PPSSPP: `MOTORACER_PROF=1` (see the caveat above).

## Profile-guided game code
`scripts/psp/pgo.sh` builds an instrumented EBOOT, trains it with a scripted PPSSPP race and rebuilds
`build/psp` with `MOTORACER_PGO=use`. The counts (`build/pgo-data/`) come from the player's `game.EXE` and
are not committed; the regular build does without them.

## Native rewrites
Hot game functions get native C++ versions (`runtime/src/game/natives/`, hooked through `NATIVE_FUNCS`
in `recompiler/recomp/translate.py`). Check each one on the desktop with `MOTORACER_NATIVE=check` (both
versions run and are compared; the log reports `MISMATCH` lines) *and* in a normal desktop race: some
tables are relocated by the translator for resolutions above the PSP's ([notes/25-psp-port.md](../notes/25-psp-port.md)).
Findings about the game's code go to [docs/decomp/](../decomp/README.md).
