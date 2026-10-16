# Resource usage audit (2026-10-16)

Owner request: compare the recomp's resource use with the original game's requirements and find what
is worth optimising.

## Original requirements (1997)
Windows 95 + DirectX 3, Pentium 90–133 MHz (sources differ; 133 recommended for smooth play), 16 MB RAM,
2 MB video card (4 MB for 3D acceleration with patch 3.22, per its readme), 2× CD-ROM, ~5 MB on disk
(the music played from the CD). Sources: elisoftware.org (EA 1997 release), oldgames.sk, gamepressure.

## Measured (M3 Pro, macOS, 60 fps + vsync, windowed 1280×1377 pt on a 5K display at 2×)
E2E session (menus, then a Speed Bay race), sampled every 2 s:

| | Menus (640×480, rendered 4× = 2560×1920) | Race (2560×1440 16/9 mode) |
|---|---|---|
| CPU, process (% of one core) | 20–26% | 18–28% |
| Memory, resident | ~140 MB | ~230 MB |
| Physical footprint (incl. GPU) | | 475 MB |
| GPU utilisation (whole GPU, incl. desktop) | 20–23% | 23–26% |
| macOS energy impact | 17–19 | 16–19 |

- Per race frame (earlier trace, docs/16): game code incl. the Direct3D translation 2.7–5.5 ms,
  GPU submission ~1 ms, the rest waiting for vsync.
- `sample`: the process mostly waits (vsync / drawables). Hot code in races: the game's own 3D pipeline
  (`0x49b…` transform/clip, `0x4af…` execute buffers), spread over many functions; Metal state encoding.
  In menus: `flush()` (CPU-side changed-pixel check of the 2D frame) ~8% of the main thread, `memmove`,
  the game's sprite blitter `0x43ede0`.
- Footprint breakdown (`footprint`, race): 232 MB GPU allocations, 83 MB IOSurface (the window's
  drawables, 3 × 2560×2754 px), ~43 MB guest memory (VM_ALLOCATE), ~70 MB heap, code/libraries. 4 GB of
  address space is reserved for the guest, but only touched pages count.
- Our GPU objects in the race (temporary stat): 32 textures = 6.7 MB; render target + depth 2560×1440 =
  28 MB; `lowres` + `overlay` at the game frame size 2560×1440 = 28 MB (only needed for CPU-drawn pixels,
  i.e. menus and readbacks); menus: render target + depth 2560×1920 = 37.5 MB. The rest of the 232 MB is
  driver / SDL_GPU allocations (per-frame vertex, index and transfer buffers are created and released
  every flush).
- Disk: game files 439 MB (mostly the CD audio tracks as WAV), app 8.3 MB, build folder ~91 MB.

## Assessment
Nothing is severe: a quarter of one core and a quarter of the GPU at 60 fps, with the GPU work coming
from rendering at 2560×1440 instead of 640×480. Memory is dominated by graphics buffers sized for modern
resolutions. Steam Deck (1280×800, 60 Hz) runs with smaller targets; owner reported it flawless.

## Optimisation candidates (by value)
1. **Drawable size on HiDPI screens** — fullscreen on a 5K display at 2× uses 5120×2880 drawables (~59 MB
   each, three of them) and the present pass writes ~3.5 GB/s while the game renders 2560×1440. Matching
   the drawable to the rendered size (or not requesting high pixel density when the frame is smaller than
   the window in points) would save ~100+ MB and most of the present cost. Mac-specific.
2. **`lowres` / `overlay` only when needed** — allocate them on first use (menus, readbacks): −28 MB at
   2560×1440, −63 MB at 3840×2160.
3. **Reuse GPU buffers** — one persistent, cycled vertex/index/transfer buffer set instead of creating
   three buffers per flush: less driver work and fragmentation.
4. **Menus' changed-pixel check** — compare rows with `memcmp` before per-pixel work: menus only, small.
Not worth it: the game's own code (3–5 ms per frame at 1440p on M3; well within budget).
