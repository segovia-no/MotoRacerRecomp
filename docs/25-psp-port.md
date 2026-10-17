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
  Black seams in HUD bars / mosaic text: Direct3D 5 has pixel centres at integer coordinates, the
  GE at half-integers, so point-sampled HUD pieces picked neighbouring texels. Vertices are shifted
  by half a game pixel (as on the desktop) and uv by +1/16 texel (exact-boundary samples go to the
  texel Direct3D takes). Correct at PPSSPP's 1x (= hardware); at PPSSPP's 2x rendering 2D art still
  shows seams (emulator upscaling, as with many PSP games).
- Menus' 1-pixel checker ("stippled" transparency, e.g. the yellow arrows) became coarse blocks when
  640x480 frames were scaled to 363x272 by picking pixels (Direct3D back buffer uploads) or by the
  GE's bilinear filter (CPU frames): both now scale on the CPU with 2x2 averaging and draw 1:1.
- PPSSPP keyboard (default `controls.ini`): arrows D-pad, Z Cross, X Circle, A Square, S Triangle,
  Space Start, Enter Select, Q/W L/R, I/J/K/L nub, Esc PPSSPP menu.
  Found with `MOTORACER_GE_DUMP=<frame>` (logs every draw of that frame and saves each texture as
  the guest surface and the converted GE copy into the snapshot dir).

## Menu speed (2026-10-17)
- Owner report: main menu ~17 fps. The menus are 640x480 CPU frames; the 2x2-averaged downscale to
  363x272 (added for the stippled arrows) did a float division, four loads and a per-channel loop
  per output pixel (~99 k pixels per frame). Now `psp_ge::downscale_565` (shared by CPU frames and
  Direct3D back-buffer uploads): source columns from a table built once, rows by integer math, the
  three RGB565 channels averaged in one 32-bit word ((p | p << 16) & 0x07E0F81F, sum of four, >> 2)
  with the red / blue swap folded in. Main menu 104 → 243 frames per 5 s (~21 → ~49 fps); intro /
  title / name entry windows 2–2.5x faster. Same picture.

## Loading time (2026-10-17)
- Owner report: loading too long. `MOTORACER_IO_STATS=1` (kernel32 file shims: opens / reads /
  seeks / time, once a second) showed the race load reading the track with ~8,000 `ReadFile` calls
  per second of a few bytes each, ~890 ms of every second inside the file calls (~0.11 ms per PSP
  `sceIoRead`), for ~24 s.
- Read-only files now go through a 32 KB buffer (`plat::BufferedReadFile`,
  `runtime/src/platform/buffered_file.h`, used by the POSIX, Win32 and PSP adapters since
  2026-10-17; seeks only move the position, large reads go straight to the destination). The same phase: ~48,000 reads per second, ~4 s. Start → race ~29 s → ~12 s in
  PPSSPP; the rest is the game's own processing while it loads.
- `MOTORACER_IO_STATS=1` logs each file open with a timestamp (phase markers), `=2` also counts and
  times every call (two clock reads per call: on the PSP that inflates small-read phases several
  times — measure durations with `=1`). With a counting build (`MOTORACER_COUNT_INSNS`) and
  `MOTORACER_ICOUNT_DUMP=<dir>`, every open also writes an instruction-count dump named after the
  file, so `tools/icprof.py` profiles each loading phase.
- Loading phases (desktop counting build, x86 instructions): track select → bike select:
  ILE01.BKF → ile01.trk 158 M (`0x41c050` 61%, `0x401160` 35%); the .trk itself 16 M (read field
  by field: ~190,000 `CFile::Read` calls); bike-select menu; Start → race (SFX.BKF … bestscor.dat)
  ~145 M, mostly `0x41c050`.
- `getc(handle)` `0x42f650` (archive layer `0x4309e0` over the 16 KB-buffered reader `0x42f690`)
  was ~70% of the instructions of the decoding phases and runs continuously in the animated menus
  (~300,000 bytes per second): native fast path in `runtime/src/native_load.cpp` (buffered byte, no
  seek; refills / seeks / errors run the translated code). Check mode: 11 M calls, 0 mismatches.
  `CFile::Read` `0x4c5e9d` reads the host file directly (`rt::read_file`), without the ReadFile
  import path.
- PPSSPP, Start on bike select → first race frame: ~11.5 s translated, ~9.5 s native (was ~29 s
  before the buffered files).
- Per-instruction counts (counting builds now count per instruction address too: `ic_*.va` dumps
  next to the per-function ones) located the hot loop of the texture loader `0x41c050`: the 24-bit
  row loop `0x41cdb6..0x41d087`, 2.8 M pixels per race load (three getc, colour-key fix-ups, alpha
  value, channels rounded into the 16-bit format). Native as a jump hook (`native_rgb_row`); rows
  touching the colour-conversion rectangle (double-precision helpers) or skipping source pixels
  (128-wide) stay translated. Check mode compares each row at `0x41d08d`: 10,000 rows, 0
  mismatches.
- PPSSPP timeline now: track select → bike select ~3.5 s, Start → first race frame ~3.1 s; first
  race frame 33.0 s after boot in the scripted run (36.5 s all translated). A temporary per-call `getenv` in the getc hook made it ~2x slower
  for a while: newlib's getenv scans the whole environment.

## Memory budget, PSP-1000 (2026-10-17, in progress)
- `MOTORACER_MEM_STATS=1` on the PSP also logs newlib's `mallinfo`, the guest-stack and host-stack
  high-water marks, and `MOTORACER_GE_STATS` the converted-texture count / size.
- Measured in a race (PPSSPP): ELF 9.28 MB (text 7.57: game code 3.82, other 1.17, SDL3 1.00,
  libstdc++ 0.86; bss 1.69: SDL's PSP renderer `DisplayList` 1 MB — unused, comes with SDL's video
  subsystem — and our 512 KB GE list), malloc 27.2 MB: guest block 24.06 MB (stack 1 MB, image
  3.2 MB, 0.8 MB unused gap, heap 18 MB of which the race uses 10.5 MB, VirtualAlloc backing 1 MB of
  which the CRT commits 256 KB), host stack 2 MB, textures / SDL / buffers ~1 MB.
- Guest stack: 29 KB used of 1 MB; host stack (the recompiled code): 3 KB of 2 MB. Both now 256 KB,
  VirtualAlloc backing 512 KB: malloc 27.2 → 24.2 MB.
- A PSP-1000 has ~24 MB for the program; with the 9.3 MB ELF that leaves ~14 MB for malloc: still
  ~10 MB over. Remaining levers: the guest heap (18 MB; a race peaks at 10.5 MB — needs measuring
  over other tracks / modes), the game's 640x480 surfaces (4 x 600 KB in the heap during a race),
  8-bit textures stored as 32-bit GE copies, the unused 1 MB SDL renderer array.
- Heap peaks (PSP profile, 23 opponents): Speed Bay race 10.5 MB, Dirt Arena 12.8 MB (owner test):
  the heap cannot go below ~14 MB. `MOTORACER_MEM_STATS` now also lists live heap bytes by owner
  (first game-code return address on the guest stack, past the CRT's malloc). A race: the model
  loader `0x401160` 2.4 MB (track / bike geometry), two 10,000-entry arrays of `0x419020`
  (2 x 507 KB), ~1 MB of texture surfaces (`CreateSurface` from `0x41b0f0`; each also has a
  converted copy on the host), many small object blocks. The 640x480 menu surfaces are freed during
  races.
- Estimate for the PSP-1000 with a 14 MB heap: ELF 9.3 + guest block ~19 + host (textures, SDL,
  stacks) ~2-4 → ~31 MB of ~24 MB. Closing ~7 MB needs deeper changes: textures shared between the
  guest surface and the GE (no second copy), the unused SDL renderer array (1 MB), the x86 `.text`
  / `.reloc` of the image (~0.9 MB, but jump tables and self-modified code live in it), possibly
  fewer opponents (less model / object data).
- Two dead ends: (1) choosing the heap size by probing `malloc`: the PSP's newlib does not check its
  partition's end, huge requests "succeed" with memory the program does not own, and PPSSPP crashed
  later; (2) moving the VirtualAlloc arena into the unused gap below the engine tables: the CRT's
  4 MB small-block-heap reservation then covered heap addresses, and the CRT decides by address
  range whether a pointer is its own — free() broke at the title screen. Reservations must be
  distinct address space.

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
