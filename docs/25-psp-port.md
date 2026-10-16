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
- After aligned accesses and fewer spills (23-translator-speed.md, 2026-10-17): MIPS game code
  5.05 → 3.93 MB (−22%); in one file 60,127 → 42,246 instructions, unaligned pairs 15,573 → 2,016.

## GE render backend (2026-10-17)
- `runtime/src/psp/ge.cpp`: GE context. VRAM: two 480x272 16-bit colour buffers and a 16-bit depth
  buffer (stride 512, 835 KB). Display lists (512 KB, flushed early when half full), `sceGuSync` at
  frame end, the drawing region set explicitly (pspgu sizes it from `sceGuDispBuffer`). Pixel
  transfers between VRAM and RAM use GE block transfers (`sceGuCopyImage`): PPSSPP keeps GE output on
  the host GPU and does not reflect it in emulated VRAM for CPU reads (snapshots and back-buffer
  downloads read stale pixels until this was changed); on hardware it is a DMA.
- A first layout drew a 480x360 buffer and displayed a 272-row window of it: in PPSSPP depth was
  lost below row 272 (emulated framebuffers sized from the display) → buffers are exactly 480x272.
- `runtime/src/psp/d3d_ge.cpp` (`dx::gpu`): TLVERTEX in the GE's through (2D) mode: positions 1:1 for
  the 16:9 band (480x270, centred) or scaled to fit (640x480 → 363x272), colour ARGB → ABGR, depth
  `65535·(1 − sz)` with range 65535..0 and inverted compare functions, uv in texels. Render states:
  depth test / write / function, blending (D3D factors → GE incl. fixed ONE/ZERO), alpha test and
  colour key (keyed textures get alpha 0), flat / gouraud, texture blend (MODULATE with texture alpha
  only when the format has alpha, DECAL, DECALALPHA, MODULATEALPHA, ADD), filtering, wrap / clamp.
  Textures converted on change: 565 → 5650 (R/B swapped), keyed 565 / 1555 → 5551, 4444 → 4444,
  8-bit → 8888 through the palette.
- Fog / specular: not needed — the game never enables Direct3D fog (render state 28; the only
  `mov [reg], 0x1c` in the code is the CRT's errno) and SPECULARENABLE is always 0 (render-state log).
- Perspective-correct texturing (2026-10-17): through mode interpolates uv affinely. Instead of the
  GE's 3D pipeline (its depth would be 1/w with 16 bits instead of the game's linear depth, and the
  HUD would need a separate path), textured triangles larger than 24 px with a depth ratio > 1.15 are
  split (up to 3 levels) at edge midpoints with perspective-correct uv; position, depth and colour stay
  screen-linear as in Direct3D. Race: ~150k extra triangles per 5 s, ~3% of the frame time, frame
  rate unchanged (PPSSPP). `MOTORACER_GE_PERSPECTIVE=0` turns it off.
- `runtime/src/psp/gpu_psp.cpp` (`host::gpu`): CPU-drawn frames (menus) as scaled 5650 sprites.
- PSP startup sets the CPU to 333 MHz (`scePowerSetClockFrequency`; homebrew starts at 222 MHz).
- Dev tooling: `psp_env.txt` next to the EBOOT sets environment variables (autokeys, snapshots,
  stats); `tools/psp/e2e_psp.sh <name> "<keys>" [secs] [KEY=VALUE...]` stages, runs PPSSPP and makes
  a contact sheet; `MOTORACER_GE_STATS=1` logs draws / presents / transfers / time every 5 s.

## Controls, audio, HUD (2026-10-17)
- Input: SDL3 exposes the PSP pad as a gamepad ("PSP builtin joypad"); the runtime's gamepad layer
  applies. PSP layout (instead of the Xbox preset, same rule: only over the game's factory
  bindings): Cross accelerate, Square brake, nub / D-pad steer, R / L gear up / down, Circle stunts /
  turbo, Start pause, Select speedometer; menus: D-pad, Cross confirm, Circle back. Every PSP start
  counts as a first run (no recomp_state.txt), so the controller is set to joystick.
- Audio: SDL3's PSP audio device opens; DirectSound buffers and the CD music (the WAV tracks, 12 on
  the virtual disc) are SDL streams as on the desktop. Race frame rate with / without audio
  (`MOTORACER_NO_AUDIO=1`): 120–186 / 114–132 frames per 5 s — no measurable cost in PPSSPP.
  `tools/psp/e2e_psp.sh` copies the WAVs with `WITH_MUSIC=1` (~340 MB).
- HUD (lap times, position, time, gauge backgrounds) was missing / wrong: Direct3D MODULATE takes the
  alpha from a texture that has an alpha channel, the GE multiplies it with the vertex alpha, and the
  game's HUD vertices carry alpha 0. Vertex alpha is now forced to 255 for those draws.
- Doubled speedometer / tachometer: the gauge cluster is one 256x256 4444 texture drawn as two quads
  over the same rectangle, one of them mirrored (back-facing); the game relies on Direct3D culling
  (CULLMODE CCW) to drop it, and the GE backend did not cull. Now culled on the CPU while building the
  triangle list, with the desktop backend's convention (CCW: screen-space cross product < 0).
  Found with `MOTORACER_GE_DUMP=<frame>` (logs every draw of that frame and saves each texture as
  the guest surface and the converted GE copy into the snapshot dir).

## Measured in PPSSPP (emulated time; Speed Bay, 23 opponents, detail 5, 480x270)
- Race: 16–35 fps (82–177 presented frames per 5 s; earlier runs at 222 MHz: 3–4.5 fps).
- Time per 5 s in the race: Direct3D Execute (parsing + GE backend) 400–660 ms (8–13%), of which the
  GE backend 190–300 ms; texture conversions only after loads; waiting for the GE ~0. The rest is
  the game's own code.
- Menus: were ~5 fps — the game draws them with the CPU into the Direct3D back buffer, so every
  frame downloaded and uploaded it (GE transfers + format conversion). Fixed (2026-10-17): a Clear
  with nothing drawn yet is kept pending; when the CPU then touches the back buffer the colour clear
  is applied to guest memory by the CPU (no readback), and a frame without GE drawing is presented as
  a CPU frame; 16-bit CPU frames go straight to the GE's 5650 (no RGBA8 detour). Main menu now
  ~43 fps (215–219 frames per 5 s), menus with 3D (bike selection) 15–31 fps.
- PPSSPP times the CPU from instruction counts, not cycle-exact; real hardware (cache misses, the
  in-order pipeline) is likely slower.

## Status
- 2026-10-16: the EBOOT boots in PPSSPP and the recompiled game runs through the CRT start, Win32
  setup, DirectDraw (640×480 display mode, surfaces) and Direct3D device enumeration, then stops at
  `SDL_CreateGPUDevice` (no SDL_GPU on the PSP) — next: the GE backend.
- 2026-10-17: GE backend: intro, title, menus, bike selection, loading and races render correctly in
  PPSSPP (scripted E2E). Race 16–35 fps (emulated).
- Next candidates: a real PSP, the PSP-1000 memory budget.
  Writing GE vertices straight from the native vertex loops (skipping Direct3D) would save at most
  the Execute share (~10%).
