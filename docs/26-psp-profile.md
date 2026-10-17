# PSP performance profile (2026-10-17)

Owner request: identify the hot functions that limit the PSP's frame rate.

## Method
- Guest x86 instructions per function: the counting build (`build/pspprof`, `MOTORACER_COUNT_INSNS`,
  float x87) in the PSP profile (480x270, 30 fps, 23 opponents, detail 5), `MOTORACER_ICOUNT_DUMP`,
  753 race frames (22-hot-functions.md). The guest instruction stream is the same on every host.
- MIPS cost: each guest function's compiled MIPS size (`psp-nm -S` on the PSP build's objects) divided
  by its x86 size gives an expansion factor; x86 instructions per frame × expansion ≈ MIPS
  instructions per frame (static ratio as a proxy for the dynamic one).
- Library / runtime calls per function: `psp-objdump -dr`, R_MIPS_26 relocations to non-guest
  symbols. Script: build/e2e/tmp/pspest.py (scratch).
- Runtime share on the PSP: `MOTORACER_GE_STATS` (Direct3D Execute = parsing + GE backend 8–13% of
  the frame time in PPSSPP, 25-psp-port.md).

## Result: 1.32 M x86 → ~4.24 M MIPS instructions per race frame (game code; native loops excluded)
| # | Function | est. MIPS / frame | share | x86 / frame | expansion | role |
|---|---|---|---|---|---|---|
| 1 | `0x4af570` | 798 K | 18.8% | 293 K | 2.7 | render-list flush: per-polygon state / texture / triangle logic (its vertex loops are native) |
| 2 | `0x49b7f0` | 597 K | 14.1% | 200 K | 3.0 | per-face loop (cull, depth keys) |
| 3 | `0x49b2c0` | 283 K | 6.7% | 89 K | 3.2 | vertex transform (3x4 matrix, 1/z, projection) |
| 4 | `0x49c110` | 277 K | 6.5% | 92 K | 3.0 | polygon routine |
| 5 | `0x497080` | 245 K | 5.8% | 66 K | 3.7 | clipping (12 `rep movs` → runtime calls) |
| 6 | `0x49c590` | 169 K | 4.0% | 54 K | 3.2 | polygon setup |
| 7 | `0x4b3048` | 129 K | 3.0% | 28 K | 4.6 | CRT `_ftol` (calls F_TO_I64) |
| 8 | `0x419d60` | 109 K | 2.6% | 42 K | 2.6 | triangle → execute buffer |
| 9 | `0x4b8800` | 80 K | 1.9% | 15 K | 5.3 | CRT printf core (HUD strings each frame; 79 parity calls) |
| 10 | `0x4b10c0` | 63 K | 1.5% | 11 K | 5.9 | dot product + offset (point vs plane), ~830 calls / frame |
| 11 | `0x40c7e0` | 59 K | 1.4% | 14 K | 4.2 | (64-bit division: `__divdi3` / `__moddi3`) |
Top 6 = 56%, top 30 = 80%. Plus the runtime: Direct3D Execute (parsing the execute buffers + the GE
backend) 8–13% of the frame.

## MIPS-specific overheads (whole game code, call sites)
| Call | Sites | Cause | Fix |
|---|---|---|---|
| `__paritysi2` | 4,226 | the PF flag (`PARITY`, x87 compare idioms, printf): no popcount on MIPS | inline bit trick (~6 instructions) |
| `__truncdfsf2` | 1,028 | x87 double loads (`fld/fmul qword [const]`) converted to float in software each time | constants in read-only data → float literals at translation time |
| `REP_MOVS` / `REP_STOS` / `REP_SCAS` | 995 / 236 / 438 | runtime helpers + full register spill / reload around each | inline forward-copy fast path on the locals |
| `__divdi3` / `__moddi3` | 337 / 144 | `div` / `idiv` always divide a 64-bit edx:eax in software | 32-bit fast path when edx is the sign / zero extension |
| `__extendsfdf2` | 190 | x87 stores to doubles | (needed) |

## Candidates, by expected gain
1. **Render path** (rows 1, 8 + Execute ≈ 30%): a native render-list flush that emits GE commands
   directly (states, textures, triangles), skipping execute buffers and their parsing. Largest
   single item; needs the rest of 0x4af570 reversed (state methods of the execute-buffer object).
2. **Geometry** (rows 2–6 ≈ 37%): native transform / clipping / face loops, VFPU for the transform
   and plane tests (`0x4b10c0`).
3. **Translator quick wins** (each a few % at most, all functions): inline parity, float literals for
   double constants, inline `rep movs`, 32-bit division fast path, inline `_ftol`.
4. **CRT printf** (1.9%): route the guest's sprintf to a host implementation.

## Done: translator quick wins (2026-10-17)
- Parity inline on MIPS (bit trick instead of the libgcc call).
- x87 loads of constants in `.rdata` (read-only: characteristics 0x40000040) become literals: 1,455
  sites (no memory access; on the PSP no software double→float conversion each time).
- `rep movs` / `stos` (and single movs / stos) inline on the cached registers: memcpy / memset for
  forward non-overlapping cases, the x86 element order otherwise; 1,033 sites, no runtime call and no
  register spills. (`scas` / `cmps` still call the runtime: they set flags.)
- `div` / `idiv` 32-bit fast path when edx is the zero / sign extension (no 64-bit software division).
- MSVC `_ftol` (0x4b3048) inlined at all 525 call sites (`FTOL`: one trunc.w.s for the int32 range).
- `F_TO_I32` (fistp) on the PSP: the FPU's cvt / floor / ceil / trunc.w.s with the x87 rounding
  mode, no double arithmetic; F_TO_I16 / I64 in fpu_t.
- difftest 0 failures; desktop and PPSSPP races normal. MIPS game code 3.93 → 4.06 MB (inlined copy
  loops). PPSSPP race: 168–266 frames per 5 s (34–53 fps), was 111–186 (22–36 fps).


## Done: Direct3D backend per-polygon overheads (2026-10-17)
Race in PPSSPP, same scripted run (deterministic: identical runs give identical counts), presented
frames per 5 s in the last six windows:
| Build | frames | per window |
|---|---|---|
| quick wins (baseline) | 1,238 | 124 200 204 252 256 202 |
| + GE state cache | 1,315 (+6%) | 135 211 218 235 266 250 |
| + no histograms / timers | 1,341 (+2%) | 132 217 223 244 270 255 |
- GE state cache (`runtime/src/psp/d3d_ge.cpp`, `apply_states`): the game sends 3–6 render states
  with every polygon, mostly unchanged; the backend sent ~20 GE commands per draw (incl. a texture
  flush). It now keeps the last values sent and sends only changes; the texture image / flush only
  when the texture (or its converted contents) changes. `psp_ge::touch_state()` invalidates the
  cache when other code changes GE state (the CPU-frame presenter). GE draw time in the race
  ~800 → ~600 ms per 5 s.
- The execute-buffer parser updated `std::map` histograms (opcodes, every render state's value) for
  the 10-s "D3D stats" log: only with `MOTORACER_D3D_STATS=1` now (all platforms). The per-draw timer (two
  clock reads) only with `MOTORACER_GE_STATS=1`. Parsing time ~540 → ~450 ms per 5 s.
- Batching consecutive draws with identical state into one GE draw was tried: no gain (the state
  cache already removes the redundant commands; draws are ~4.5 triangles), not kept.
- Snapshots: menus, race HUD, transparency and tunnels unchanged.

## Profile after the native geometry (2026-10-17)
Translated game code in a race: ~0.99 M x86 instructions per frame (natives not counted). Top:
render-list flush `0x4af570` 28% (more than half of it the empty-bucket walk, since skipped:
24-native-render.md), CRT `_output` `0x4b8800` 5% (+ helpers `0x4b91b0` / `0x4b9240` ~2.7%),
triangle emitter `0x419d60` 4%, polygon setup `0x49c590` 2.7% (clip path), `0x49a160` 2%.
`MOTORACER_PRINTF_STATS=1` (sprintf / vsprintf entry hooks) lists the formats: per race frame ~11
`"SURFACE %s BLIT DATA(%3d,...) --> SCREEN(...)"` (an MFC `CString::Format` debug trace in the blit
function `0x478990`, used only when a blit fails) and the HUD's `%02d:%02d;%02d`, `%d<%d`, `%3d`.
- Native `_output` (`native_load.cpp`, 2026-10-17): c s d i u o x X %% with flags / width / precision
  / h / l through the host snprintf (NULL %s → "(null)" as MSVC), written into string streams that
  have room; everything else translated. Check mode: 0 mismatches. PPSSPP race +~2%.

## Direct drawing from the flush: tried, no gain (2026-10-17)
- Experiment: in the native flush (PSP), render states went straight to the device's state array,
  vertices and triangles to the GE backend (batched while the state did not change), instructions
  written by translated methods (begin's initial states, `0x41e000` / `0x41e1b0`) were applied and
  taken out of the buffer; the execute buffer then only carried an empty PROCESSVERTICES. The
  "execute" time of `MOTORACER_GE_STATS` fell from ~1,100 to ~5 ms per 5 s and draw calls by ~25%.
- Same-build A/B (`MOTORACER_GE_DIRECT=0`): race windows 2,027 (off) vs 1,994 (on), continue screen
  379 vs 382 frames per 5 s — no gain in PPSSPP (a first version was slower: it rescanned the pending
  batch for its lowest index on every polygon). Reverted; it could still save GE command overhead
  on a real PSP (not measurable here).
- Lesson: the "execute" share in `MOTORACER_GE_STATS` was mostly its own timers (two clock reads
  per draw call, ~65,000 draw calls per 5 s); parsing the execute buffers is cheap. Judge PSP
  changes by frame counts with the stats timers on in both runs, not by those timings.

## Translated long tail (2026-10-17)
Profile (counting build, desktop race, natives not counted): ~0.534 M x86 instructions per frame.
Top: polygon setup `0x49c590` 5.2% (clip path), `0x49a160` 3.8%, `0x40c7e0` 3.4% (337 calls per
frame), MFC `CString::FormatV` `0x4c3b8b` 3.1%, matrix multiply `0x403a80` 3.0% (its translation
is already tight), `0x46cd20`, `0x4b10c0` (13 instructions, ~1,060 calls per frame), `0x485e50`.
- `0x422b90` (polygon record constructor: five 0xea60 words, -1, two zero words; returns this,
  ecx = this + 0x24): native, `native_geom.cpp`.
- `CString::FormatV` `0x4c3b8b` sizes its buffer before vsprintf by walking the format: per
  conversion max(width, estimate) — strings their length (min 1; `lstrlenA`, wide `%S` / `%ls` a
  word count), integers and `%p` max(32, width + precision), floats max(128, ...), `%c` 2, `#` +2,
  clipped to the precision for c / s — and 1 or 2 (lead byte) per other character. Its callers in a
  race: the blit function `0x478990` formats `"SURFACE %s BLIT DATA(...) --> SCREEN(...)"` for
  every blit (~11 per frame) although it only logs it (to `data\delphine.err`, through `0x4767f0` →
  `0x472800`, which opens, appends and closes the file per message, up to 1 MB) when the blit
  fails. Skipping the format would change that error text, so the estimate loop is native instead:
  jump hook at the loop head `0x4c3bb2` → `0x4c3e79` (`native_load.cpp`), with the loop's exit
  registers and locals (edi = the NUL, esi = past the arguments used, ebx = last precision, eax /
  ecx / edx as the last `_tcsinc` leaves them, `[ebp-0xc]` = the estimate). It reads the CRT's own
  ctype (`[0x4f36f8]`) and mbctype (`0x4f2f01`) tables; a multibyte locale (`[0x4f3904]` > 1) or a
  non-ASCII byte in a digit test (`_ismbcdigit`'s MBCS path) runs the translated loop. The hook
  also sits on the loop's back edge, so a loop entered translated continues natively from there.
- Check mode: 45,000 FormatV estimates compared in a desktop race, 0 mismatches (32.6 M checks in
  all). PPSSPP: race windows 1,717 vs 1,665 frames (same build, hook off by a temporary switch),
  +3%, about the size of the scene differences between runs.
- Check-mode crash found on the way: the check regions `{[0x53bcf0], 4}` / `{[PRIM_HEADER], 4}`
  (the open TRIANGLE instruction's count) are read through a pointer that is NULL while no header
  is open (state methods called while textures load at the race selection) → guest memory fault
  at address 0 inside the snapshot. `native::take` now gives such a region no bytes. Since those
  regions were added (native flush, 24-native-render.md) check-mode runs stopped at the race
  selection screen (~1.5 M checks; the 60-s runs ended before it showed), so the checks reported
  since then covered menus and the bike selection; earlier counts (docs 24 / 27) included races.
  Lesson: a check-mode run needs a contact sheet showing the race (`MOTORACER_SNAPSHOT_EVERY=300`).
- Track sector search `0x40c7e0` (thiscall: this = {sectors, count}; start index, point, float
  *out; ~340 calls per race frame): from the start index it walks the 252-byte sectors, testing the
  point against each sector's plane (+0x40) with `0x4b10c0` (dot(normal, point) + d, which also
  counts its calls in `[0x709378]`): negative → steps back (wrapping) to the first sector with a
  distance >= 0; else steps forward (`(i + 1) % count`) and returns the last sector before a
  negative distance; *out = the last distance. Native with the plane test inline (`native_geom.cpp`):
  ~32 K x86 instructions per frame (0x40c7e0 + most of 0x4b10c0's ~1,060 calls), ~6% of the
  translated code. Check mode: +2.6 M checks in a race, 0 mismatches. PPSSPP race windows 1,686 vs
  1,665 frames (+1%, within the scene differences between runs).
- Not worth a native on the PSP: `0x49a160` (3D frame start) is mostly a loop over the viewport's
  rows (span-list heads `0x623bd8`, pool `0x60d3d0`): its share in the desktop profile comes from
  the desktop resolution; at 272 rows it is ~0.5%. The desktop counting profiles overstate
  per-row / per-pixel loops for the PSP (`MOTORACER_PSP_PROFILE=1` gives the PSP's 480x360 mode).

## Where a PSP race frame goes: sampling profiler (2026-10-17)
`MOTORACER_PROF=1` (`runtime/src/prof.cpp`): the main thread keeps its current region in one byte
(`prof_region`, a store at each boundary, no clock reads): Win32 / COM shims (`STDCALL`, `CDECL`,
`COM_METHOD`), Direct3D Execute, present, waits (frame pacing, `GetMessage`, GE sync), and every
native hook — the translator wraps each native call in `PROF_IN("<name>")` / `PROF_OUT()`, a region
per name registered at the first call; everything else is translated game code. Translated code that
a native calls back (guest calls) counts as that native. A timer counts the current region ~5,000
times per second (`plat::Execution::start_sampler`: a kernel alarm on the PSP, a thread elsewhere);
every 5 s the log lists shares and ms per frame. SDL's audio thread flags its mixing from the first
stream callback of a device buffer to the post-mix callback; on the PSP's single core those samples
count as "audio mixing".
- Pitfalls met: a guest exception (`RaiseException` → `nonlocal_jump` longjmp) skipped the shims'
  restores, so all later game code counted as "Win32 shims" (`nonlocal_jump` now resets the region);
  an audio callback that set the shared region and blocked on a lock restored a stale one onto the
  main thread (only the main thread writes the region now); `sceKernelGetThreadId` in the alarm
  handler does not name the interrupted thread.

PPSSPP, Speed Bay, 23 opponents, first race window (17.7 ms per frame, 56 fps):
| Region | ms / frame | share |
|---|---|---|
| translated game code | 5.2 | 29% |
| audio mixing (SDL audio thread) | 3.1 | 18% |
| D3D Execute (execute-buffer parsing + GE backend) | 2.6 | 15% |
| native render-list flush polygon (incl. its guest calls) | 2.3 | 13% |
| native geometry: faces 1.5, faces2 0.7, clip 0.5, transform 0.5, polygon setup 0.2, special 0.1 | 3.5 | 20% |
| sector search 0.3, Win32 / COM shims 0.3, `_output` 0.1, present 0.1 | 0.8 | 5% |
Later race windows: audio 12–14%, game code 29–32%. The time-out screen (race frozen behind
CONTINUE, 12.7 ms): Execute 23%, flush 21%, faces 15%, game code 13%, audio 5%.
- Audio is the largest single item that is not game logic: every DirectSound buffer is an SDL stream
  of its own (float stereo at the buffer's rate, the engines' pitch through the frequency ratio), so
  SDL resamples and converts each of them (~24 engine sounds) on the audio thread. `docs/25`'s "no
  measurable cost" dates from ~25 fps, when it was a smaller share.
- Main menu (software-drawn, ~50 fps): present 58% (the 640x480 frame converted for the GE), game code 40%.

## Translated code in the PSP's screen mode (2026-10-17)
Counting build with `MOTORACER_PSP_PROFILE=1` (480x360 mode, 480x270 band), race: 0.442 M x86
instructions per frame (0.534 M at the desktop resolution). Top: polygon setup `0x49c590` 5.4% (clip
path), matrix multiply `0x403a80` 3.6%, `0x4232e0` 3.2%, `0x46cd20` 3.1%, `0x46b120` 2.7%,
`0x485e50` 2.6%, `0x4b0f60` 2.6%, `0x40c960` 2.5%, `0x47ecd0` 2.4%, `0x469c60` 2.3%; 21 functions
make half of it, 48 three quarters. By 16 KB of code: `0x40c000` 15% (track / collision helpers),
`0x46c000` 9%, `0x468000` 9%, `0x4b0000` 8% (plane / vector helpers), `0x420000` 7%.

## Own DirectSound mixer (2026-10-17)
`dx/dsound.cpp` now mixes itself: buffers that have been played are voices of one mixer, which
fills a single SDL stream (float stereo at the device's rate) on SDL's audio thread — each voice
stepped at its frequency in 16.16 fixed point (SetFrequency, the engines' pitch) with linear
interpolation between frames, DirectSound volume / pan / SFX gain as before; non-looping voices stop
at their end. SDL now resamples nothing for the effects (the CD music stream is already in the
device format). Positions advance by what was mixed (no SDL read-ahead), SetCurrentPosition / Stop no
longer need to drain a stream. The game's buffers: 16-bit mono, 8,000–22,050 Hz, up to ~15 playing.
- PPSSPP race (profiler): audio mixing 3.1 → 1.1 ms per frame (18% → 7%); first race window 17.7 →
  15.8 ms per frame, the next 16.5 → 15.5.
- Same mixer on every platform (one code path); linear interpolation instead of SDL's windowed-sinc
  resampler, as 1990s software mixers did.
