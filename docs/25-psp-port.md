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
  (`MOTORACER_NO_AUDIO=1`): 120–186 / 114–132 frames per 5 s — no measurable cost in PPSSPP at the
  time (~25 fps). At ~55 fps the sampling profiler puts SDL's mixing at 12–18% of a race frame
  (26-psp-profile.md); with the own DirectSound mixer (one SDL stream) 6–7%.
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

## Perspective in the GE (2026-10-17)
- Textured triangles whose 1/w varies are drawn in the GE's 3D mode, which interpolates texture
  coordinates perspective-correctly in hardware: no CPU subdivision (`MOTORACER_GE_3D=0` restores
  the subdivision path). Vertex = (x·w, y·w, w), w = 1/rhw; the projection set in `psp_ge`
  (column-major, P.x = (2/480,0,0,0), P.y = (0,−2/272,0,0), P.z = (−1,1,1,1), P.w = (0,0,−2·W0,0))
  maps it back to pixel (x, y) with depth 65535·W0/w for the viewport (centre 2048, 480x272) and
  depth range 65535..0.
- The 3D mode cannot produce the game's depth (linear in distance: sz = camera z / 65536), so every
  draw now uses 65535·min(1, S0/sz), S0 = half the previous frame's smallest sz (precision where
  the geometry is), and the 3D draws W0 = S0/k with k = sz·rhw measured on the previous frame's
  textured vertices — the same depth. Depth comes from sz, not rhw: the game's untextured vertex loop
  never writes rhw (the first version used rhw and the bike in the bike selection had parts at
  wrong depths). Draws with a vertex nearer than S0 (one frame after a sudden approach) and 2D pieces
  (constant rhw, w = 1 would be clipped at the near plane) stay in through mode.
- PPSSPP race: no subdivided triangles (was ~230 k per 5 s), GE backend draw time per draw
  ~12-14 → ~8 µs, race frames +2.8%. Owner-tested: the bike selection (races: my snapshots only).

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

### PSP-1000 again (2026-11-05, owner: "optimize the game to fit in its memory")
- `MOTORACER_PSP_1000=ON` (CMake): EBOOT without `MEMSIZE 1`, so the program gets the 24 MB user
  partition on every model — PPSSPP enforces it — and a 14 MB guest heap (`RECOMP_GUEST_HEAP_MB`).
- Budget, measured at start in that mode: the C heap's block has **13.88 MB** of room (`memory:` log
  line: an `sbrk` probe in 64 KB steps, given back at once; newlib's malloc cannot be probed). The
  program image (9.1 MB: text 7.19, bss 1.69) is loaded below it, so every byte cut from the image is
  heap room. Start: guest block 22.81 MB + ~1.5 MB other host allocations → ~10.4 MB over.
- Step 1, layout: the relocated engine tables moved from 0x800010 to 0x760010 (same address on
  desktop, where [0x740000, 0x800000) is free too), the guest stack (128 KB; uses 29 KB) moved into
  the former gap above the PEB, nothing below the image any more (`GUEST_LO` = 0x400000), host stack
  128 KB. Guest block 22.81 → 17.94 MB with the 14 MB heap (18.0 → 13.94 in the 64 MB build's
  terms: −0.87 MB layout, −4 MB heap). PPSSPP race (64 MB build) and desktop race OK.
- Image breakdown: game code 3.95 MB, runtime + newlib + pspsdk ~3.0, SDL3 1.05, libstdc++ 0.84;
  bss: SDL's PSP renderer `DisplayList` 1 MB (unused: the GE backend draws), our GE list 512 KB.
- Step 2, no SDL on the PSP: `runtime/src/psp/sdl_psp.cpp` defines the 47 SDL3 functions the
  runtime calls, on sce APIs (SDL's headers stay for the types): `SDL_RunApp` (module info, HOME exit
  callback), ticks / delays, a fixed 480x272 window, an event queue fed by `SDL_PushEvent` and by
  polling sceCtrl as one gamepad with SDL's mapping, audio (one PSP channel + thread per bound stream,
  pulling through the stream's get callback; f32 / s16 stereo 44.1 kHz in, s16 out), and
  `SDL_SaveBMP` for snapshots. Text 7.19 → 5.71 MB, bss 1.69 → 0.59 MB; C heap room on a PSP-1000
  13.88 → 16.44 MB. PPSSPP race OK (64 MB build), pad detected, both audio streams start;
  owner-tested in PPSSPP (input, sound): "worked pretty great".
  Still needed: ~19 MB (guest block 17.94 + ~1.1 MB other) → ~2.6 MB to go.
- `MOTORACER_MUTE=1`: audio runs at volume 0 (desktop and PSP); the test scripts use it instead of
  `MOTORACER_NO_AUDIO`, so the audio path is still exercised.
- Measured again during a race (memory stats now also tick on GE-presented frames: they used to run
  only on CPU frames, i.e. menus): host allocations 26.96 MB = guest block 21.94 (64 MB build) + 5.0
  MB, of which GE texture copies 3.49 MB (2.12 MB from 8-bit textures expanded to 8888). So the
  PSP-1000 gap was ~6.5 MB, not 2.6.
- `-Os` for all game code: 3.78 → 3.56 MB only (−0.22); race speed about the same. Kept `-O2`.
- Step 3, zero-copy 8-bit textures: the GE reads them as T8 + a 256-entry 8888 CLUT straight from
  the guest surface (16-byte aligned start, pitch 16n; the guest block is now `memalign(64)`), colour
  key as CLUT alpha. Host allocations in a race 26.96 → 24.85 MB; race 306–366 presents per 5 s
  (205–247 before; runs vary).
- Mute fix: `MOTORACER_MUTE` is read when audio starts (the PSP's environment comes from
  psp_env.txt in main(), after static initialisers); the log says "audio: muted".
- Step 4, in-place 16-bit textures: the texture cache converts the guest pixels to the GE's order in
  place (565 R/B swap, 4444 nibble order, 1555 swap + alpha / colour key) and `sync_to_cpu` (Lock,
  Blt, GetDC) converts them back before the game touches them; marks are kept by memory address
  (`DuplicateSurface` shares pixel memory) and dropped when a surface is destroyed. Only a colour-keyed
  565 texture (lossy as 5551) keeps a copy. Host allocations in a race 24.85 → 23.49 MB, no texture
  copies left; picture and race speed unchanged.
- Tally for the PSP-1000: guest block 17.94 + other host allocations 1.55 = 19.49 MB vs 16.44 MB
  room → ~3 MB to go.
- `MOTORACER_MALLOC_TRACE` (CMake, PSP dev): `--wrap` malloc / calloc / realloc / memalign / free and
  log live blocks by call site with `MOTORACER_MEM_STATS` (resolve with `psp-addr2line`). In a race
  besides the guest block: 590 KB in ~7,000 small C++ objects, the snapshot buffer 272 KB (test runs
  only), the GE staging buffer 272 KB, the CPU-frame buffer 195 KB, host stack 128 KB, log buffer 64 KB.
- Step 5, no iostreams: `rt::read_lines` / `read_file` / `write_file` (`runtime/src/textio.cpp`, stdio)
  replace ifstream / ofstream / stringstream in settings, recomp_state, registry, private-profile ini
  and the PE loader (all platforms; installer.cpp, desktop-only, keeps them). libstdc++'s locale /
  stream code drops out: text 5.71 → 5.03 MB, rodata 240 → 113 KB.
- Trims: log buffer 64 → 4 KB (PSP), host stack 128 → 64 KB (peaks ~17 KB), VirtualAlloc backing
  512 → 256 KB (what the CRT commits; more fails loudly), audio thread stacks 64 → 32 KB.
- Looked at and left: `-Os` game code (−0.22 MB), the fallback bodies of natived functions (49 KB),
  a larger C heap via `PSP_HEAP_SIZE_KB` (no effect: the 0.75 MB the heap does not take are separate
  pieces, the largest 0.5 MB).
- Step 6, the x86 `.text` as guest heap: the translated code never executes it and reads only jump
  tables, their byte index tables, the self-modifying rasterizer (48 KB) and a few data operands.
  `tools/psp/gen_text_keep.py` lists those ranges from the discovery analysis and the instructions'
  memory operands (indexed ones up to the next function, at most 1 KB — the CRT's memcpy tables are
  found that way; a first version that kept 4 bytes per operand died with "jump table miss at
  0x4bb326") into `runtime/src/text_keep.inc` (62 KB in 61 ranges); `donate_text_to_heap` (compact
  layout, after `load_image`; `MOTORACER_TEXT_HEAP=0` off) gives the other 764 KB in 61 pieces to the
  guest heap. PPSSPP race OK.
- Step 7, PSP-1000 fit: guest heap 12.5 MB (`RECOMP_GUEST_HEAP_KB=12800`) + the 764 KB from `.text`
  ≈ 13.25 MB for races that peak at 12.6–12.8 MB; guest block 16.19 MB in 17.31 MB of room. The GE
  staging buffer and the CPU-frame buffer (512 KB) come from the user partition's leftover piece
  (`psp_ge::outside_heap`, `sceKernelAllocPartitionMemory`), not the C heap. The PE loader reads the
  sections straight into guest memory (it held the whole 1.1 MB file, more than the heap had left).
  An out-of-memory `new` now ends with "out of host memory" in the log instead of silently.
- **Result (2026-11-05): the `MOTORACER_PSP_1000=ON` EBOOT runs in PPSSPP within the 24 MB partition**
  — intro, menus, a race to the time-out screen (muted E2E). Margin is small: `MOTORACER_MEM_STATS`
  (its per-block owner map) runs out of host memory in this build. Still to check: Dirt Arena (the
  12.8 MB heap peak), other modes, longer play, a real PSP-1000.
- Owner test of that build: (1) quitting a race crashed with "guest heap exhausted (request 614432)"
  — in use 12.2 MB, free 1.05 MB, largest free 524 KB: the menus allocate their 640x480 surfaces
  (600 KB each) while the race data is still there; the 64 MB build peaks at 12.8 MB there, and the
  `.text` pieces are too small for 600 KB blocks. (2) The menu's breaking-glass effect turned red:
  `IDirect3DTexture::Load` copied pixels without `sync_to_cpu`, so an in-place converted texture
  was copied and converted again (R/B swapped).
- Fixes: `Tex_Load` syncs both surfaces; the guest heap places blocks ≥ 256 KB top-down (compact
  layout, guest heap only — doing it in the VirtualAlloc arena too put the CRT's 4 MB reservation
  past the backed part at once); guest heap 13 MB (`RECOMP_GUEST_HEAP_KB=13312`); guest stack 64 KB;
  the layout gaps after the PEB and after the relocated tables also go to the heap (920 KB donated
  in all); the GE display list 512 → 256 KB (a race frame peaks at ~67 KB, no early flushes); the
  snapshot buffer borrows the GE staging buffer; "guest heap exhausted" reports in use / free /
  largest free; `MOTORACER_MEM_STATS=2` for the per-block owner map (level 1 is cheap); the
  memstats page count no longer reads past the guest block (it crashed the PSP-1000 build).
- PSP-1000 build after the fixes (PPSSPP, muted E2E): race, pause, Quit Race, Top 5 screen; heap
  peak 12.8 MB; host allocations 17.25 of 17.56 MB.
- Test runs are muted (`tools/psp/e2e_psp.sh`, `tools/e2e.sh`; `WITH_SOUND=1` for sound).

### Crash at the end of a Dirt Arena race (owner, 2026-11-05)
- "guest memory 0x014a2000+0x10000 is outside the host block": the CRT's small-block heap commits
  ~264 KB of its VirtualAlloc reservation by then; the earlier trim of `VALLOC_BACKED` to 256 KB was
  wrong. Back to 512 KB; memstats logs the highest commit ("highest commit", 256 KB on Speed Bay).
- Paying for it: the GE list 256 → 128 KB, host stack 40 KB. The first build then ran out of host
  memory while a race loaded: the PSP backend kept every vertex of a frame in one vector
  (`frame_vertices`, ~10,000 vertices in a race) whose doubling briefly needed twice that. Draws only
  use the vertices of the latest PROCESSVERTICES, so the list now restarts in `add_vertices`: host
  C++ allocations 555 → 331 KB and no load spike (C-heap high-water = in use).
- Dev aids: `MOTORACER_PSP_HEAP_KB` (CMake, PSP-1000 guest heap, default 13312); the malloc tracer
  (8,192 slots now) also lists small blocks by size and dumps itself on "out of host memory".
- PSP-1000 build: race, Quit Race, Top 5 (muted E2E); heap peak 12.8 MB; host 17.25 of 17.69 MB.
- Owner-tested (PPSSPP, PSP-1000 build): full Dirt Arena race to the results, quitting races, the
  menu glass, Circle for stunts — "that worked".

### Circle paused races (owner, 2026-11-05)
- Cause: the PPSSPP staging copies the Mac's game folder, so CONTROLLER = KEYBOARD and recomp_state's
  `Defaults=1` came along; the first-run switch to JOYSTICK never ran. With KEYBOARD the game does
  not poll the pad, so the runtime's menu mapping stayed active in races and Circle (B) became
  Escape = pause.
- Fix: `HostInfo::pad_only()` (true on the PSP) sets CONTROLLER = JOYSTICK at every start
  (`game_hook_input_loaded`), and on the PSP the PSP layout also replaces the desktop's Xbox preset
  (throttle on triggers the PSP lacks), not only the factory bindings; the player's own bindings stay.

### Hardware test package (2026-11-06)
- `tools/psp/package_hw.sh [game dir]` → `build/psp-hw/PSP/GAME/MOTORACER/`: `EBOOT.PBP` from the
  stripped ELF (5.3 MB; the build's own EBOOT carries 45 MB of debug info) with `PARAM.SFO`
  `MEMSIZE=2` (pspdev's "limited memory": the PSP-1000's 24 MB on every model, as tested in PPSSPP),
  plus the game files in `game/`. Copy `PSP/` to the memory stick root; needs custom firmware. The log is
  `ms0:/PSP/GAME/MOTORACER/motoracer.log`. Checked in PPSSPP from a plain folder (PPSSPP treats a path
  with `PSP/GAME/` as its own memory stick).

### First real PSP-1000 (ARK CFW) test (2026-11-06)
- Black screen, then the PSP switched itself off. The log (closed and reopened per line for the first
  300 lines: the memory stick only updates a file's size when it is closed) stopped after the `.text`
  donation, before "entering the game": in `run_on_large_stack`, which moved `$sp` into a malloc'd
  block (`call_on_stack`) — fine in PPSSPP, fatal on hardware. Same free memory as PPSSPP (17.69 MB).
- Fix: the PSP runs the game on its own thread (`sceKernelCreateThread`, 40 KB kernel stack, VFPU);
  the main thread waits. The frame-buffer block is reserved before that thread takes kernel memory.
  `MOTORACER_TRACE=1` (psp_env.txt) logs every API call, like `--trace`.
- Second hardware run: stopped after the frame-buffer block, before the game thread's first line. The
  thread filled the unused bottom of its stack with a marker byte for the high-water mark; the kernel
  keeps a guard word there. Now nothing writes to the stack: the mark scans for the kernel's 0xFF fill
  (from 64 bytes up); "game thread started" is logged first.
- Third hardware run: menus, CD music and sound work; it powered off on entering the bike selection
  (last line: the detail-table hook, then the bike model loads). Not reproducible in PPSSPP, with or
  without the CD-audio WAVs. Ruled out: unaligned accesses — `MOTORACER_CHECK_ALIGN` (CMake, dev)
  makes every "proven aligned" access (`RD32A` & co., translator and native code) check its address;
  no fault from boot through the bike selection and a race (`PSP_BUILD=build/psp-align
  tools/psp/e2e_psp.sh ...` runs another build's EBOOT). Found: a new PSP thread starts with FCR31 =
  0x00000e00, i.e. FPU traps enabled for invalid operation, division by zero and overflow. x86 runs
  with the x87 exceptions masked and the game divides by zero / makes NaNs freely; on the Allegrex
  that traps (PPSSPP never does). `psp/fpu.h` `psp_fpu::quiet()` clears the enables
  (pspSdkDisableFPUExceptions) in main, the game thread and the audio threads; the game thread logs
  the old value.
- Same run: crackling audio, slow menus. CD audio seeked and read 4 KB from the memory stick on the
  audio thread for every 23 ms block (a seek drops the stdio buffer): now a seek only when playback
  jumps, and a 32 KB file buffer. PSP audio blocks 1024 → 2048 samples (46 ms to mix the next one).
  The log is now synced (closed / reopened) for the first 2000 lines.
- Fourth hardware run (owner): no crash, the game plays normally — the FPU traps were the cause.

### CD audio as MP3 (2026-11-06)
- The 12 CD-audio WAVs are 345 MB on the memory stick and 176 KB/s of reads competing with the
  game's loads. `tools/psp/cd_mp3.sh <game dir> <out>` converts them with ffmpeg (libmp3lame, CBR
  128 kbps, 44.1 kHz stereo, no ID3 / Xing header so the decoder starts at byte 0): 31 MB, cached in
  `build/psp-mp3`. `cdaudio.txt` lists `NN.mp3 <CD frames>`: the virtual disc keeps the WAV lengths
  (the game reads the table of contents; past the decoded end it plays silence).
- `package_hw.sh` ships the MP3s instead of the WAVs (package 444 → 131 MB); `e2e_psp.sh`
  `WITH_MUSIC=1` stages them too (`WITH_MUSIC=wav` for the WAVs).
- Playback: `winmm_cd.cpp` reads a track through `host::TrackDecoder` — the WAV file, or
  `host::open_mp3_track` (`psp/mp3_psp.cpp`; other hosts return nullptr and play the WAVs). The PSP
  version uses the firmware's sceMp3 (Media Engine decoding, the main CPU only copies PCM):
  `sceUtilityLoadModule` avcodec + mp3 on the first track, 16 KB stream + 9 KB PCM buffers per open
  track, file data through libc (`open` / `read`: relative paths use libc's working directory, which
  `sceIoOpen` does not know). A seek inside a track restarts at the MP3 frame and drops the rest.
  PPSSPP: the modules take 16 KB of kernel memory (180 → 164 KB free); the log line
  "CD: MP3 decoder ready (kernel free A -> B KB)" shows the real cost on hardware.
- The log is now written under a mutex (the audio thread logs too; the PSP reopens the file per line).
- A folder that still has NN.wav plays the WAVs (they are looked for first): replace the whole
  `PSP/GAME/MOTORACER` folder on the memory stick.

- Hardware run with MP3 (owner): music worked but crackled more than the WAVs, and the PSP powered
  off ~1 s into the race load — when the audio thread wrote its first log line for the new track
  (the log is closed / reopened per line) while the game was loading files. Real decoder cost: the
  same 16 KB as PPSSPP. Fixes: other threads never write the log file (`rt::log_queued`, written by
  the game thread at the next presented frame or before a FATAL); the MP3 file is read ahead in two
  16 KB chunks with `sceIoReadAsync` (sceMp3 takes its stream data by memcpy, so the audio thread
  never waits for the memory stick; before, sceMp3 asked for small pieces and every block did a
  seek + read); the buffers are allocated once.
- Profiler: `prof: audio: N blocks, L late (channel ran dry), slowest X ms to produce (block 46.4
  ms)` every 5 s (the wait in `sceAudioOutputPannedBlocking` under 1 ms = the block came late). PPSSPP
  with MP3: 0 late after startup, at most 5 ms to produce a block.
- Hardware profile (owner, menus, MOTORACER_PROF): 56–67 ms per frame (15–18 fps); game code ~40%,
  present ~22%, waiting for the GE ~24%, audio mixing ~10%. Bike / track select 77–80 ms (game code
  55%, D3D Execute 22%). PPSSPP is ~3x faster on the same screens.

- Next hardware run: the race loads and plays; the crackle stayed, at a very steady rate. Cause: one
  output buffer per channel — `sceAudioOutputPannedBlocking` returns once the block is queued and the
  hardware reads it by DMA while it plays, so mixing the next block into the same memory overwrote
  samples still playing (PPSSPP copies them at submit). Now two 64-byte aligned blocks alternate,
  with a data-cache writeback before each output.

- Hardware race profile (owner, before the double buffering; audio never late): 62–85 ms per
  frame (12–16 fps): game code ~21 ms (30%), "audio mixing" 12–17 ms (17–22%), D3D Execute 9–15 ms
  + native render-list flush 6–9 ms, "present" ~6 ms, native geometry ~6 ms. Three corrections:
  - Profiler: the audio share counted the whole time an audio thread was between its stream
    callback and the post-mix, including its waits for the Media Engine (MP3 decoding) and the memory
    stick, while the game ran. Now `prof::audio_cpu(+1/-1)` brackets only CPU work (and drops around
    `sceMp3Decode` / `sceIoWaitAsync` / `sceIoRead`). PPSSPP race: 9.6% → under 0.5%.
  - DirectSound mixer: integer (16.16 stepping, Q14 interpolation, Q15 gains, int32 accumulation,
    16-bit output stream instead of float; also on desktop). Silent voices only advance.
  - Log: every line closed and reopened the file on the memory stick (and `_IOLBF` wrote each line),
    milliseconds per line — the profiler's ~15 lines per 5 s inside the present (its ~6 ms), hundreds
    of `CreateSurface` lines per load screen. Now the first 300 lines are synced each, later the log
    is fully buffered and synced at most once per second (by a line or every presented frame) and
    before a FATAL.

- Next hardware race (owner): crackle gone, no crashes, ~60–68 ms per frame. Game code ~24 ms, D3D
  Execute ~12, render-list flush ~8, audio ~7 (true CPU), native geometry ~8, present ~1 (the log fix).
  PPSSPP on the same race: 9.6 ms per frame, game code only 1.5 ms (16%) — on hardware the translated
  code is ~16x slower, the native loops 2.4–6x: PPSSPP has no cache model, and ~4 MB of translated
  MIPS code against a 16 KB instruction cache. PPSSPP's ranking is not the hardware's.
- Audio: the DirectSound mixer runs at 22050 Hz on the PSP (the game's sounds are 5–22 kHz) and
  `sdl_psp` upsamples a half-rate stream x2 (linear); inaudible voices only advance (O(1)).
- `MOTORACER_PROF_FUNCS` (CMake, profiling build): every translated function stores its guest address
  in `prof_fn` on entry and restores the caller's on return (`PROF_FN`, a cleanup variable); the
  sampler histograms it while the region is translated code, and `prof: hot guest functions (%)`
  lists the top 24 every 5 s. Render sub-regions: `ge: vertex conversion`, `ge: draw (vertices out)`,
  `ge: texture + state`. `PROFILE_BUILD=build/psp-pf tools/psp/package_hw.sh` adds
  `PSP/GAME/MOTORACER_PROF/` with that EBOOT, the profiler on and `MOTORACER_GAME_DIR` pointing at
  `MOTORACER/game` (configured with `MOTORACER_PSP_HEAP_KB=13056`: the EBOOT grows 5.1 → 5.3 MB).

### Hardware profile with guest functions, and what followed (2026-11-06)
- Race (~60 ms per frame on a PSP-1000): game code ~25 ms, of which the top 24 guest functions ~15 ms
  (bike AI / physics `0x46b120`, `0x469c60`, `0x46bf80`, `0x461aa0`, `0x46cd20`, the render-list flush's
  translated part `0x4af570`, matrix multiply `0x403a80`, ...); after the finish line (AI stopped)
  game code drops to ~10 ms. Render path ~29 ms (flush 7.5, GE draw 5.3, faces 5, Execute 3.2, vertex
  conversion 2.3, state 1.7, transform / clip / setup ~3).
- Menus (~60 ms): the sprite blitter `0x43ede0` 25% and the tiled background `0x43f0f0` 12% of the
  frame; the CPU downscale of the 640x480 frame ~16 ms. Both functions are native now
  (`native_menu.cpp`, check mode: 0 mismatches).
- GE: textured 3D-mode draws are indexed (`MOTORACER_GE_INDEXED`, default on): each PROCESSVERTICES
  batch is converted once into display-list memory (w = 1/rhw, x*w, y*w, w), each draw adds 16-bit
  indices; texture size and the 1/16-texel nudge go to `sceGuTexScale` / `sceGuTexOffset`, Direct3D
  culling to the GE (`sceGuFrontFace` + `GU_CULL_FACE`). ~65% of the race's draws; same pictures as
  the per-triangle path; PPSSPP draw time −20%. The batch arrays fill the 128 KB list sooner: about
  one early flush per frame (harmless: the GE runs a list only after it is closed anyway).
  `psp_ge::reserve` keeps a draw's index and vertex memory in one list.
- Menu frames (640x480, larger than the screen) go to the screen through the GE instead of the CPU
  downscale (`psp_ge::draw_halved_565`, `MOTORACER_GE_HALVE=0` for the CPU path): the GE halves the
  picture into a VRAM scratch buffer behind the depth buffer with its bilinear filter sampling between
  four texels (an exact 2x2 average, so the 1-pixel checker transparency still blends), the 320x240
  result comes back to RAM where the CPU swaps red / blue (two pixels per word), and the GE scales it
  to 363x272. PPSSPP: the menu's CPU-frame upload 5–6 ms → 0.6–1.3 ms per frame. The download of a
  GE frame for CPU drawing (mixed 3D / 2D menus) uses aligned stores and copies repeated rows; the
  colour clear stores two pixels per word.
- Hot race functions, native in `native_race_a..d.cpp` (written in parallel in four worktrees, each
  verified with `MOTORACER_NATIVE=check` in desktop races, double and single-precision x87 builds, then
  all together: 21 natives, 0 mismatches; a `MOTORACER_CHECK_ALIGN` race with all of them on: no
  unaligned access):
  - A: `0x410010` matrix · vector + t, `0x40fd40` rotation matrix, `0x46c970` track window,
    `0x46bdc0` look-at frame, `0x46cd20` AI target pick (check_abi: its caller reloads eax/ecx/edx);
  - B: `0x48e180` gear ratio, `0x45b4e0` angle blend, `0x467930` bike speed (calls
    `game_hook_ai_speed` at the same point; its check replays the hook), `0x466a80` bike lean;
  - C: `0x403a80` matrix multiply, `0x490220` angle (fpatan), `0x4b0f60` segment / plane,
    `0x40c960` point on a sector plane, `0x461aa0` bike contact points, `0x46bf80` bike movement step
    (its check restores .data/.bss, the stack below the call and rand()'s seed; fields derived from
    `0x467930`'s hook are masked);
  - D: `0x4b3400` strncmp, `0x492b10` text size, `0x485e50` quad into the render list, `0x493490`
    text draw.
  - Left translated: `0x469c60` and `0x46b120` (bike physics, ~800–1,000 x86 instructions each, already
    register-cached by the translator; little to gain for the risk), `0x493820` (SEH / COM plumbing
    around the now-native text functions), `0x45f290` (2,400 instructions, 0.7%).
  - Bike records are 0x3209 bytes, so they sit at odd addresses: native code reads their fields with
    unaligned-safe loads (a plain `lw` there traps on the Allegrex).
  - Parallel desktop test runs share the game folder (best scores, settings); `MOTORACER_LOG_DIR`
    (macOS) keeps their logs apart.

- Hardware run with all of the above (owner): races ~55–65 ms (was 60–68): translated game code
  25 → ~9 ms, but the new natives take ~13 ms (inclusive of their translated callees) — the bike code
  went 25 → ~22 ms only. It is bound by data memory (24 bike records of 0x3209 bytes, ~300 KB, against a
  16 KB data cache), not by instruction count. Menus got slower (GE wait ~37 ms) and showed a black
  bar: the halving left a 320x240 scissor in place for pass 2 (fixed); the GE's tiny texture cache
  made wide sprites over unswizzled RAM textures slow — the old menu path's ~17 ms "wait" too: sprites
  are now drawn in 32-texel strips. The native menu copies used newlib memmove, which copies byte by
  byte when source and destination are 2 bytes out of word phase: `copy_px` copies words, or funnels
  aligned word loads.
- `PROFILE_ENV="MOTORACER_OPPONENTS=7"` (package_hw.sh) runs the profiling app with 7 opponents: the
  hardware cost of the bike AI per opponent.

### AI level of detail (2026-11-06)
- Setting `AiLod` (`[Recomp]`, PSP default on, desktop off; dev `MOTORACER_AI_LOD=0|1|"R1,R2,P_MID,P_FAR"`).
  The per-frame bike loop `0x47df40` updates each racing opponent with `0x45d540(..., dt)` (dt = frame
  time in hundredths, `[0x4e9808]`; for AI bikes → `0x46bf80`: AI + physics). Jump hook at `0x47e189`:
  an opponent farther than R1 (3000) from the player is updated once per P_MID (13 hundredths, ~7.5 Hz)
  up to R2 (20000) and once per P_FAR (25, ~4 Hz) beyond; skipped frames' time accumulates and the next
  update gets it (the frame-time global holds the sum for that call; `game_hook_ai_lod_after` at
  `0x47e1bb` restores it). Near the player every frame, unchanged.
- Skipping an opponent's first updates on the grid crashed the race: each bike needs 60 normal
  updates after the race setup (`game_hook_opponents` resets the counters) before it can be skipped.
- PPSSPP race (23 opponents): 85–92% of opponent updates skipped once the field spreads out; frame
  14.3 → 10.6 ms, `0x46bf80` 1.16 → 0.11 ms.
- First hardware test (owner): bikes, the player's too, jumped "a lot of meters". The player was taken
  to be slot 0, but the player's bike is the one with `+8` nonzero, in any slot: the player's own updates
  were skipped and distances measured from an opponent. Now the flagged bike is never skipped and is the
  reference; and only bikes beyond drawing range are thinned (R1 = 40000 > 2 x LenBPlane = 36000 at
  detail 5; one update per 10 hundredths), so nothing you see or touch moves in larger steps.
  PPSSPP race: 15–85% of opponent updates skipped (the field bunched / spread), frame 14.3 → 9.8 ms.
- The menus' black band at the bottom (and stray lines) on hardware: two colour buffers, so the next
  frame was drawn into the buffer still being scanned out until the vertical blank (the menus start a
  frame with a clear). Now three colour buffers (`COLOR_BUFFERS`), depth and scratch after them (VRAM
  1.33 MB).
- `MOTORACER_BIKE_LOD_SCALE=<s>` (dev): the bike model / sprite thresholds × s (0.5: sprites from 1500).
- `MOTORACER_LOG_SYNC=1` (dev): the PSP log is synced every line (crash hunting; slow).
- The desktop tests' game folder: `data/bestscor.dat` also stores Time Attack and the lap count; test
  runs left Time Attack on (no opponents). `package_hw.sh` ships the original file; desktop checks of
  AI code should use a private game copy (`MOTORACER_GAME_DIR`).

### Third hardware profile and what followed (2026-11-06)
- Owner: black band gone, menus much faster, race physics fine, still slow; red lines remain bottom
  left during the menus' "window break" transition (not in PPSSPP). Race ~55–60 ms per frame: game code
  (translated) ~9.5 ms, `native_flush_polygon` ~8, GE draw ~6, `native_hot_46bf80` ~5.5, faces ~3.7,
  D3D Execute ~2.8, audio mixing ~2.6, vertex conversion ~2.3, texture + state ~1.8. AI level of detail
  skipped 10–30% of opponent updates. PPSSPP race: ~600 GE draws (~1,460 triangles) per frame.
- Red lines: the CPU-drawn menu frame (and converted textures, CLUTs) went to the GE from cached RAM
  with the data cache written back only in `finish()`, but the GE runs a direct list while it is built
  (the stall address moves), so it could read stale lines. `psp_ge::writeback` now writes the range back
  before the draw that reads it (`draw_image`, `draw_frame`, every texture conversion).
- pspgu's `sceGuDrawArray` / `sceGuGetMemory` move the stall address with a kernel call each time
  (~1,200 per race frame). `psp_ge::draw_array` and `list_memory` write the same list words themselves
  (pspgu's `gu_list`) and move the stall address every 4 KB of list.
- Translated code converted guest doubles (`fld` / `fstp qword`) with libgcc's soft-float
  `__truncdfsf2` / `__extendsfdf2` (no double FPU): ~430 call sites, many in the bike code. `recomp.h`
  (`RECOMP_FPU_FLOAT`) converts with integer operations instead, bit-identical (tested on every float
  and 400 M doubles incl. ties and carries); zero, subnormals, inf, NaN outside the fast path.
  `-fno-math-errno` on the PSP: `sqrtf` is the `sqrt.s` instruction.
- Audio: at most the 10 loudest voices are mixed on the PSP (`MOTORACER_MIX_VOICES`, 0 = all; desktop
  all); a race plays ~24 engine voices, most distant.

### CPU: draw distance (2026-11-06)
- The package copied the desktop config's `DrawDistance=2`. PPSSPP, Speed Bay race with the
  profiler: 11.7 ms per frame at x2, 7.6 at x1 (−35%). The PSP default is now 1 (`settings.h`) and
  `package_hw.sh` writes `DrawDistance=1` into the packaged ini (the recomp menu can still raise it).
- `PROF=1 tools/psp/package_hw.sh` adds `psp_env.txt` with `MOTORACER_PROF=1`: the hardware log then
  has the sampling profile every 5 s (PPSSPP does not model the PSP's caches or memory timing).

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
