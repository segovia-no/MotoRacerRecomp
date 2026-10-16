# PSP port: feasibility (2026-10-16)

Owner question: could the recomp run as PSP homebrew, targeting the PSP-1000 (32 MB RAM)?

## Target
- PSP-1000: Allegrex (MIPS32 R4000-class, in-order, single issue) at 222 / 333 MHz; FPU **single
  precision only** (doubles are software); VFPU (128 × 32-bit registers, vector/matrix ops); 32 MB RAM
  (user programs get ~24 MB, the kernel keeps the rest; commonly cited figure, to be confirmed on
  hardware); GE graphics chip with 2 MB eDRAM; 480×272 screen; Media Engine (second Allegrex, mainly
  audio/video decoding); no MMU (no virtual memory, no paging). PSP-2000/3000: 64 MB.
- Toolchain: pspdev (GCC, binutils, newlib, PSPSDK); C/C++ homebrew is common. PPSSPP emulates the PSP
  for development.

## Measured on the desktop build
| | Value | Note |
|---|---|---|
| Original `.text` | 845 KB | x86 |
| Translated game code, compiled | 4.0 MB | arm64 (fixed-width RISC like MIPS), 4.7× the x86 size |
| Runtime (Win32/MFC layer, DirectX translation, audio, …) | 0.5 MB | without SDL |
| Original image (code + data + bss) | 3.3 MB | `.data` incl. bss 2.3 MB |
| Guest heap in a race at 2560×1440 | 38 MB | screen surfaces 9.6 + 9.6 + 7.2 MB |
| Guest heap without the screen surfaces | ~12 MB | textures (16-bit 256×256 = 128 KB each), models, track, sounds; at 480×272 the surfaces are ~0.25 MB each |
| VirtualAlloc block | 4 MB reserved, 0.19 MB used | the CRT small-block heap (see step 1 below) |
| x87 instructions | 8.6% of all (22,430 of 261,481) | only 1,382 with 64-bit operands: the game computes in single precision |
| Game CPU time per race frame | 3–5 ms on one M3 core | incl. the Direct3D translation; the rest of a frame is waiting for vsync |

The translator currently emulates the x87 stack in `double` (`double st[8]` in recomp.h), which would be
software floating point on the PSP.

## Memory budget, PSP-1000 (~24 MB for the program)
| Item | Today | Achievable |
|---|---|---|
| Translated code | ~4 MB | ~2.5–3 MB (smaller translation: skip unused flag computations, keep guest registers in locals) |
| Runtime + libstdc++/newlib | ~1–1.5 MB | ~1 MB |
| Guest image | 3.3 MB | 3.3 MB |
| Guest heap (race, 480×272) | ~12 MB | ~11–12 MB |
| VirtualAlloc block | 0.2 MB | 0.2 MB (4 MB is only reserved; 3 × 64 KB committed) |
| Guest stack | 2 MB | ~0.5 MB |
| Display lists, audio buffers, host stacks | ~1 MB | ~1 MB |
| **Total** | **~23–24 MB** | **~19–21 MB** |
(Corrected after step 1: the guest heap peaks at 11.8 MB with 7 opponents, and the VirtualAlloc block is
a reservation.) Right at the limit as is; it fits after the code-size work, with a few MB of margin. On a
PSP-2000/3000 (64 MB) it fits comfortably. Textures need no second copy: the game keeps them as 16-bit
surfaces in its own memory, and the GE can texture from main RAM in 5650/5551/4444 formats.

The original's "16 MB RAM" was on Windows 95 with virtual memory (paging to disk), so its committed
memory could exceed its working set; the PSP has no paging, so everything must be resident.

## CPU
- A Pentium 133 runs ~133 M x86 instructions/s; the Allegrex 333 M MIPS instructions/s. The translation
  turns one x86 instruction into ~4–5 MIPS instructions on average (code size ratio), i.e. ~70–80 M
  x86-equivalent instructions/s: about half a Pentium 133.
- The game itself does all transform, lighting and clipping on the CPU (it hands Direct3D
  screen-space vertices). Expect roughly **10–15 fps** from a straight port at 333 MHz, before tuning.
- Ways up:
  1. translator: liveness of flags (most `cf/zf/sf/of` computations are never read), guest registers in
     C locals within a function, cheaper memory access on a 32-bit host;
  2. x87 as `float` (FPU single precision, no soft-float);
  3. replace the hottest game functions (3D transform / clip, execute-buffer building: `0x49b…`,
     `0x4af…`) with native VFPU code, verified against the translated originals;
  4. the game's own settings: DETAIL LEVELS, fewer OPPONENTS (24 → 6–12), draw distance 1×;
  5. audio mixing on the Media Engine (advanced).
  With 1–4, 20–30 fps looks reachable; this needs measuring on hardware or PPSSPP.

## Graphics, audio, input
- **Graphics:** the game's Direct3D execute buffers carry pre-transformed vertices (TLVERTEX:
  screen x/y/z, rhw, colour, uv) — the GE draws these directly in "2D transform" mode with 16-bit
  textures, z-buffer and the blend modes the game uses (SRCALPHA / INVSRCALPHA). Render natively at
  480×272 with the game's 16/9 mode (a 480-wide band is 480×270; the 16:10 hook already sets band
  heights). VRAM: two 480×272 16-bit colour buffers (stride 512) + 16-bit depth ≈ 0.8 MB, ~1.2 MB left for
  hot textures. The CPU-drawn 640×480 menus become a texture scaled to 363×272.
- **Audio:** DirectSound buffers (8/16-bit, 5–22 kHz, pitch for engines) mixed in software to a PSP
  audio channel; CD music streamed from the Memory Stick (WAV as today, or converted to MP3/ATRAC3 for
  the hardware decoder to save ~350 MB).
- **Input:** buttons and the analog nub map onto the existing gamepad layer (nub = steering, R/L =
  throttle/brake or face buttons).
- **Win32/MFC layer, exceptions, file I/O:** portable C/C++ already; needs a PSP platform adapter
  (files, threads, timers) and a 32-bit host build (the runtime currently assumes a 64-bit host with a 4 GB
  guest reservation; on the PSP the guest gets one compact block, e.g. 0x100000–0x1700000).

## Step 1: desktop PSP profile (2026-10-16)
Build: `cmake -S . -B build/pspprof -DMOTORACER_FPU_FLOAT=ON -DMOTORACER_COUNT_INSNS=ON …` (plus the usual
compiler and `GAME_EXE` options). Dev switches:
- `MOTORACER_FPU_FLOAT` (CMake, defines `RECOMP_FPU_FLOAT`): the x87 stack is `float` instead of `double`
  (`fpu_t`, `FM(sin)` → `sinf`, … in recomp.h / x86_helpers.c), as the PSP FPU would run it.
- `MOTORACER_COUNT_INSNS` (CMake, defines `RECOMP_COUNT_INSNS`): every translated instruction starts with
  `IC()`, a counter; `MOTORACER_ICOUNT=1` logs executed x86 instructions per frame (2 s averages).
  Both are off by default (`IC()` compiles to nothing).
- `MOTORACER_PSP_PROFILE=1`: adds a 480×360 entry to the 16:9 list and selects it with 16/9 MODE on (a
  480×270 band, the PSP screen); not saved to recomp_state.txt.
- `MOTORACER_MEM_STATS=1`: logs guest heap / VirtualAlloc use every 5 s and each VirtualAlloc call.
- `MOTORACER_OPPONENTS=<0..23>`: opponent count for the race (hook at `0x47b5af`, see below).
- `MOTORACER_EXTRA_ARGS`: appended to the game's command line.
- `tools/e2e_race.sh`: `DETAIL_PRESSES=<n>` sets DETAIL LEVELS through the Options menu.

Findings:
- **Float x87 works:** menus, race, physics and AI look and play the same in single precision (several
  E2E races, 30 fps, 480×270).
- **Opponent count:** Single Race setup (`0x47b542`) sets `0x542874` to 23, to 1 when the OPPONENTS
  option (`0x4dc094`) is off, or to 11 when `0x4dbfdc` is set. `-NbComp<n>` (stored at `0x4dbf78`) is only
  read by another start path (`0x40b924`), so it does nothing for Single Race.
- **DETAIL LEVELS must be set through the menu:** writing the global (`0x4dc098`) directly gave white
  bikes, untextured scenery and black HUD boxes; set from Options, detail 1 renders correctly.
- **VirtualAlloc:** one `MEM_RESERVE` of 4 MB from `0x4bcd18` (the MSVC CRT small-block heap) plus three
  64 KB commits; 0.19 MB of it holds non-zero data in a race.

Measured (Speed Bay, 480×270, 30 fps, detail 1, x86 instructions executed by the game per frame):
| | 23 opponents | 7 opponents | 0 opponents |
|---|---|---|---|
| Grid / start (all bikes close, in view) | 3.4 M | 2.0 M | 1.8 M |
| Racing | 1.45–1.65 M | 1.3–1.45 M | 1.2–1.3 M |
| Guest heap, race (peak) | 11.7 MB (12.6) | 11.0 MB (11.8) | 10.7 MB (11.5) |
Detail 1 vs detail 5 changes the counts by less than 5%. Track loading peaks at ~8 M per frame
(not time-critical). Menus: heap ~7 MB (peak 9.5), the 640×480 screens are 600 KB surfaces.

CPU estimate for the Allegrex at 333 MHz: ~4–5 MIPS instructions per x86 instruction (static ratio) and
an effective 0.5–0.7 instructions per cycle (in-order, 16 KB I-cache against ~4 MB of code, memory
stalls) give ~170–230 M instructions/s, i.e. ~35–45 M x86 instructions/s. Game code alone then runs a
race at **~25–35 fps with 7 opponents** (1.3 M) and the start at ~17–23 fps; with 23 opponents the start
drops to ~10–13 fps. The runtime (Direct3D translation to GE lists, audio mixing) comes on top. Better
than the first estimate (10–15 fps), because the game executes far fewer instructions per frame than
the frame times on the desktop suggested.

## Verdict
Possible on a PSP-1000. Memory fits (~23–24 MB today, ~20 MB after shrinking the translated code). With
fewer opponents (e.g. 7–11) a 30 fps target is within reach of the translator work (step 2) plus VFPU
replacements for the hottest functions; with the full 23 the start would stutter. A PSP-2000/3000
(64 MB) removes the memory problem. Distribution would follow the project's model: the player builds the
EBOOT from their own GOG copy.

## Suggested steps
1. ~~Desktop "PSP profile" build: x87 as float, 480×272 band, the game's lowest settings; measure memory
   high-water and guest instructions per frame.~~ Done (above). The compact 32-bit guest layout is left
   for step 3.
2. Translator size/speed pass (flag liveness, register locals); re-measure code size and frame time.
3. Minimal PSP build: platform adapter + GE backend, boot to the title screen in PPSSPP, then on hardware.
4. Profile on hardware; VFPU replacements for the hottest functions; audio.
