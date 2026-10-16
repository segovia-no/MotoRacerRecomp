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
| VirtualAlloc block | 4 MB | purpose not yet known |
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
| VirtualAlloc block | 4 MB | ? (if mostly unused: commit only what is used) |
| Guest stack | 2 MB | ~0.5 MB |
| Display lists, audio buffers, host stacks | ~1 MB | ~1 MB |
| **Total** | **~27–28 MB** | **~21–23 MB** |
Over budget as is; it fits only after the code-size and VirtualAlloc work, with little margin. On a
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

## Verdict
Possible, but hard on a PSP-1000: memory fits only after shrinking the translated code and checking the
4 MB VirtualAlloc block, and the CPU needs the translator and VFPU work to be playable. A PSP-2000/3000
(64 MB) removes the memory problem. Distribution would follow the project's model: the player builds the
EBOOT from their own GOG copy.

## Suggested steps
1. Desktop "PSP profile" build: x87 as float, 480×272 band, compact 32-bit-style guest layout, the
   game's lowest settings; measure memory high-water and guest instructions per frame.
2. Translator size/speed pass (flag liveness, register locals); re-measure code size and frame time.
3. Minimal PSP build: platform adapter + GE backend, boot to the title screen in PPSSPP, then on hardware.
4. Profile on hardware; VFPU replacements for the hottest functions; audio.
