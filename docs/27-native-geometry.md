# Native geometry functions (2026-10-17)

Owner request ("do all" after the PSP profile, 26-psp-profile.md): native versions of the geometry
pipeline's hottest functions. `runtime/src/native_geom.cpp`, hooked through `NATIVE_FUNCS` in
`translate.py` (`f_<va>` calls the native function; it returns 0 to run the translated body
`f_<va>_t`). Same results as the translated code: registers, stack pointer, x87 status / control
words and guest memory, floating point in `fpu_t` with the x87 code's operation order.

## Functions
| VA | What | Native |
|---|---|---|
| `0x49b2c0` | vertex transform: 3x4 matrix (x / y columns scaled by the viewport), 1/z, screen x/y (fistp, caller's rounding mode) into the transformed-vertex pool `0x6d20d8` (28 bytes per vertex, used count `[0x68b014]`) | whole function |
| `0x49b7f0` | face visibility of an object (64-byte face records, 4 vertex indexes at +0x2c): reference vertex (nearest / farthest) vs near plane `[0x625774]`, screen outcodes, 2D cross product or 3D normal · reference vertex > 1, clip flag; visible faces → `0x49c590` | whole function |
| `0x49c110` | the same with a farthest-vertex test; faces with a "special" entry (`[0x4f9300 + 824 * face+0x36]`) → `0x49ca70` | whole function (shares the code) |
| `0x49c590` | polygon setup: depth bucket (`_ftol(z * [0x4d1f9c])`, 0..0x1f3e), min / max bucket, material remap (jump table `0x49ca54` by `[0x4dc098]`), 24-byte render node pushed on the bucket list `[0x655d38 + bucket*4]` | fast path; faces to clip (flags with clipping on) stay translated |

| `0x497080` | triangle clipping (Sutherland–Hodgman) against x ≤ xmax+1, y ≤ ymax+1, x > xmin, y > ymin; clip vertices of 28 bytes (screen x/y, light as ints; u/z, v/z, 1/z floats) in two swapping 8-entry buffers `0x60d2d0` / `0x60d1e8`; new vertices: limit, ints `_ftol(v + (n−v)·d/e)`, floats `(n−v)·t + v`; final u, v divided back by 1/z | whole function (2026-10-17) |

`0x49ca70` (special polygons, from `0x49c110`, ~730 calls per frame): fast path native like
`0x49c590` (bucket, min / max, material override: 0 or 0x2a for flags 9, node with the texture
`[0x4f9304 + 4·(id·206 + variant)]` and the variant); faces to clip stay translated.

## Floating-point contraction
The first native clipper differed from the translated code in the last bit of some floats (23 of
10.7 M checks): clang fuses a single expression `(n - v) * t + v` into one fused multiply-add (one
rounding), while the translated code computes it in separate statements (two roundings). The native
files are now compiled with `-ffp-contract=off` (CMake `set_source_files_properties`): 0 mismatches.

## Details found on the way
- `0x49b2c0` sets the x87 control word with `and 0xfcff; or 0xc`: precision → single, and two
  exception masks; the rounding mode stays the caller's, so its fistp rounds to nearest in the race.
- `0x49c110` checks the clip rectangle before the visibility test (also for faces rejected by the
  outcodes) and leaves the last bound it compared in edx; callers such as `0x485bb3` return right
  after calling `0x49c590`, so eax / ecx / edx are reproduced on every exit path.
- Objects packed in the exe's data have face records and source vertices at any address (seen
  `0x58a51b`, `0x5872ca`): those are read with unaligned loads; everything else (vertex pool,
  transformed vertices, matrices, globals, stack) with aligned loads (`check` logs misaligned ones).
- Build bug found: `native_render.cpp` (and the first `native_geom.cpp`) defined
  `RECOMP_GENERATED` before including `recomp.h`, but `runtime.h` had already included it, so the
  local memory base (`MEM_LOCAL`) never took effect: global `M` reloaded after every store,
  unaligned loads everywhere. The macro block is now `runtime/include/recomp_mem_local.h`, included
  by `recomp.h` for generated code and by the native files after `runtime.h`. Before this fix the
  native face loop on the PSP was no faster than the translated one.

## Checking
- `MOTORACER_NATIVE=check`: transform and polygon setup run natively, the state is restored, the
  translated version runs, and registers / FPU words / output memory are compared. The face loops
  run natively as a dry run that records the calls it would make; the translated loop runs for real
  with `0x49c590` / `0x49ca70` recording their calls at entry; call lists (target, arguments, ecx,
  eax, edx, esp), registers and the face counter are compared.
- Desktop races (30 fps, steering): 1.2–1.3 M checks per run, 0 mismatches.
- `MOTORACER_GEOM_TIME=1`: time per function (inclusive, native or translated), logged every 5 s.
  Not for `0x49c590` (~100 k calls per 5 s: the clock reads would dominate).

## PSP (PPSSPP, race, per call, inclusive of callees)
| Function | translated | native |
|---|---|---|
| transform `0x49b2c0` | 16.9 µs | 10.8 µs (−36%) |
| faces `0x49b7f0` (+ polygon setup) | 67 µs | 54 µs (−19%) |
| faces `0x49c110` (+ polygon setup) | 66 µs | 50 µs (−24%) |
| clip `0x497080` | 10.8 µs | 7.8 µs (−28%) |
Translated, these took ~1.2 s per 5 s (~25% of the race). Race frames in the last six 5-s windows:
1,299 → 1,379 (+6%); with the GE state cache (26-psp-profile.md) 1,238 → ~1,380 since the quick
wins. What remains inside the face loops is mostly the clip path (`0x497080` and its callers).
