# Native render-list code (2026-10-16)

Owner request: native versions of the expensive game functions (22-hot-functions.md), mainly for
the PSP. First target: Direct3D vertex building, ~37% of the game's instructions in a race.

## The render-list flush `0x4af570` (reversed)
Called once per frame (from `0x49add9`). Works on the execute-buffer object `[0x624afc]`
(`+4` vertex write pointer, `+8` instruction write pointer; methods `0x41a3f0` begin, `0x41ad80` room
check, `0x41a310` end / Execute, `0x41dde0` / `0x41e000` / `0x41e1b0` / `0x41e2b0` / `0x41e380` /
`0x41e730` render states, `0x419d60` / `0x419ee0` primitives).
- Polygon lists by depth: `[0x655d38 + 4·bucket]`, bucket `0x1f3f` → 0 (or the other way when
  `[0x53bce4]` and `[0x4dc1b8]`), each list cleared after drawing. List node (`ebx`): `+0` polygon
  record, `+8` next, `+0xc` the model's transformed vertices (28 bytes each), `+0x10` 16-bit colour
  table, `+0x14` texture variant (−1 none).
- Polygon record (`esi`): `+2` colour index, `+4` uv pairs (2 floats per vertex), `+0x2c` vertex
  indexes (u16; `+0x34` = 0xffff → triangle), `+0x36` texture id (< 0x2d), `+0x38` flags (0x10: two
  vertices, a line), `+0x3a` type, `+0x3c` colour, `+0x3f` alpha.
- Transformed vertex (28 bytes, from `0x49b2c0`): `+0` x, `+4` y, `+8` z (floats, camera space), `+0xc`
  lighting (bits 10–17 intensity, 13–17 shade row, bit 31 / bit 30 special colours), `+0x10` / `+0x14`
  screen x / y (int), `+0x18` rhw (float).
- Output: Direct3D TLVERTEX (32 bytes: sx, sy, sz, rhw, colour, specular, tu, tv) at the vertex
  pointer; screen z = `z · [0x4d1fd0]` (double) when `[0x53bce4]` and not `[0x4dc1cc]` (raw z), else 1.0.
- Per polygon: texture lookup `[0x4f9550 + 4·(id·206 + variant)]` (−1 = none → untextured), colour
  `[0x709370]` (from the 16-bit colour table converted with the pixel-format masks at `0x4ee9b8..d8`,
  or `+0x3c`), alpha forced to 0x80 for some types, render states, vertices, then one or two triangles
  (`0x419d60`) or a line (`0x419ee0`).
- Untextured vertex loop `0x4af89b–0x4af9a3`: x/y from the screen ints, colour components through the
  shade table `0x5d43b0 + 256·row` (row = lighting bits 13–17), colour and specular, z; rhw and uv are
  not written.
- Textured loop `0x4af9af–0x4af9fc`: `0x4af420(vertex, uv, index)` per vertex: x, y, z, rhw, uv; grey
  intensity colour (lighting bits 10–17, halved when `[0x53bb38]`) only for the first vertex unless
  `[0x4dc1d8]` = 1; bit 31: white specular + vertex colour; bit 30: white (or opaque white).
- `0x419d60(first, shape, texture id, flags, variant)` (thiscall, `ret 0x14`): if the texture differs
  from `[0x4e4f3c]` (or that is −100): STATERENDER (op 8, TEXTUREHANDLE = `[0x53c538 + 4·slot]` or 0)
  and a new TRIANGLE instruction (op 3, header address in `[0x53bcf0]`); then one D3DTRIANGLE
  (indexes first+1/+2 with flags 0x300, first+2/+3 with 0x1f, else first+1/+2 with 0x700) and the
  header's count + 1. Returns 0 when the buffer is full (`[0x53bcec]`).
- Debug: `[0x4dc1e0]` ≠ 0 makes the flush busy-wait that many ms (timeGetTime loop) once.

## Native versions (`runtime/src/native_render.cpp`)
- The two vertex loops: jump hooks at `0x4af89b` / `0x4af9af` (`native_flat_vertices`,
  `native_tex_vertices`) run them natively and continue at `0x4af9fe`, leaving the registers and stack
  slots the following code reads as the translated loops would.
- Guest memory through a local base pointer (`MEM_LOCAL`, like the generated code), and the globals
  the loops read (colour, z mode, execute-buffer object) read once per loop: the first version used
  the global `M` and re-read them per vertex and was *slower* than the translated code.
- `NATIVE_FUNCS` in `translate.py` replaces a whole function (the translated body stays as
  `f_<va>_t`, `f_<va>` calls the native one, which can fall back). Tried for `0x419d60`: no gain
  (below), removed; the table is kept for later candidates.
- Floating point exactly as the translated x87 code (`fpu_t`: double, float in PSP-profile builds).
- `MOTORACER_NATIVE=0`: translated code only; `MOTORACER_NATIVE=check`: run both on the same input
  and log differences. Loops: native first, results kept, memory and stack undone, the translated loop
  runs, compared at `0x4af9fe` (vertex bytes, write pointer, eax/ecx/edx/esi/edi/ebp, stack slots).
  Check mode also counts the game thread's instructions and cycles for both versions on the same
  inputs (`rt::thread_counts`, macOS) and logs the saving per call.

## Verification and measurements
- Check mode, Speed Bay races (30 fps, with and without steering): 3–4.4 million checks per run,
  0 mismatches (vertex loops and, while it existed, the triangle emitter).
- Same inputs, check mode (PSP profile, 23 opponents, 2,355 frames incl. menus):
  - vertex loops: 1,343,043 calls, native saves 610 instructions and 113 cycles per call (~1,100
    calls per race frame → ~0.67 M instructions and ~0.12 M cycles per frame, ~14% / ~4% of the game
    thread's ~4.7 M instructions / ~3.2 M cycles);
  - triangle emitter `0x419d60`: 1,656,957 calls, 8 instructions and 0.4 cycles per call — nothing to
    gain after the translator speed pass (23-translator-speed.md); removed.
- A/B races (native on / off, alternating) could not resolve a few percent: race-to-race variation is
  ~10%. The same-input measurement above is the reliable one.
- On the M3 the translated instructions run at a high IPC, so cycles drop much less than
  instructions; on the PSP's in-order CPU the instruction count matters more.
- Conclusion: after the translator improvements, hand-written C only pays where it changes the work
  (hoisting, fewer loads); straight transcriptions of x86 loops gain little. For the PSP the larger
  win is skipping Direct3D entirely (writing GE vertices in these loops instead of TLVERTEX records
  that the runtime then translates again).

## Note (2026-10-17)
The local memory base described above (`MEM_LOCAL`) was not in effect in this file until
2026-10-17: `runtime.h` includes `recomp.h` before the file's own `#define RECOMP_GENERATED`. Fixed
with `runtime/include/recomp_mem_local.h` (27-native-geometry.md).

## Empty depth buckets (2026-10-17)
Per-address instruction counts of a race (counting build, natives on) showed that more than half of
the flush's own instructions (~150 k of 277 k x86 per frame) went into walking all ~8,000 depth
buckets (`head[b] = 0; b += step; until b == end`) to draw ~600 polygons. `native_bucket_skip`
(GAME_HOOK at `0x4afd00`, the advance block) clears the current bucket and moves `b` to the last empty
bucket before the next non-empty one; the translated block takes the final step as before (same
memory and registers). Not with the abort flag `[esp+0x30]` or a pending resume (`[esp+0x44]`,
`[esp+0x48]` = 1). PPSSPP race: +10% frames.

## One polygon natively (2026-10-17)
`native_flush_polygon` (jump hook at `0x4af6cb`, the start of a node) does a whole polygon: counters,
the room check `0x41ad80` (inlined: `[0x53bdf8] + 4 >= [0x53bcf4]` or instruction pointer + 0x70
`>=` `[0x53bcec]`), textured or not (type 0 / 5 / 0xc, texture id ≥ 0x2e, or no handle in
`[0x4f9550 + 4·(id·206 + variant)]`), the colour (16-bit colour table through the pixel-format masks
for untextured polygons, else `+0x3c`), half light for types 0x15 / 0x20 / 0x27, alpha 0x80 for
0x2c / 0x3e / 0xc (0x44 per player state), the shape (line / triangle / quad), the vertex loops (the
native ones), then the render states — `0x41e2b0` (`[0x53be0c]`), `0x41e380` (`[0x4e4f14]`),
`0x41e000` (types 0x44 / 0x45), `0x41e1b0` (textures with flags 0x12 and render flag 1), else
`0x41dde0` — the texture-variant cache `[0x4f9620 + id·824]`, the render flags `[esp+0x24]` (the
texture-state reset `0x41e730` = `[0x4e4f3c] = -100` inlined), the line / triangle switch, and the
emitters `0x419ee0` / `0x419d60` (a quad's second triangle from index − 3). The render-state methods
and emitters stay translated (guest calls). Exits (translator: jump hooks can now have several
targets): next node `0x4afcd3`, no room `0x4afcd8`, buffer full `0x4afd3e`.
- Check mode: the native polygon runs first, its results are kept, memory is restored and the
  translated code runs; compared at the three exits (execute-buffer object, vertex and instruction
  areas, the flush's stack frame, polygon record, node, the globals involved). Desktop race: 0
  mismatches. Owner-tested on desktop and PSP.
- PPSSPP: render-heavy screens (~350 draws per frame) 335 → 488 frames per 5 s (+46%); race windows
  within run-to-run noise.

## Execute-buffer methods natively (2026-10-17)
- `0x419d60` triangle / `0x419ee0` line emitter: when the texture changed (`[0x4e4f3c]` ≠ id, or
  −100), a STATERENDER of the texture handle (`[0x53c538 + 4·[0x4f9550 + 4·(id·206 + variant)]]`
  for render flag 1 and id < 0x2d, else 0) and a new TRIANGLE (op 3, size 8) / LINE (op 2, size 4)
  header whose address goes to `[0x53bcf0]`; then the header's count + 1 and the primitive
  (triangle flags 0x300 / 0x1f / 0x700 by shape). 0 when the buffer has no room (`[0x53bcec]`).
- `0x41dde0(enable, additive)`: alpha blending — states 0x13 / 0x14 (5 / 6 or 6 / 6 with additive,
  `[0x4dc1cc]`; else 2 / 3), 0x1b and (unless the player flag `[0x53c770 + 404·[0x53be30]]`) 0x21 =
  `[0x4e4f10]`, 0xf = 0, 0x15 = `[0x53bc10]`; disabling: 0x1b / 0x21 = 0, 0xf, 0x15; nothing when
  already off (`[0x4e4f2c]`).
- `0x41e2b0(on)`: states 0x11 / 0x12 = 2 / 1, cached in `[0x4e4f34]`; `0x41e380(value, force)`: state
  0x1a, cached in `[0x4e4f38]`. All reset the texture state to −100.
- The flush calls them directly; NATIVE_FUNCS wrappers for the other callers (check mode per call).
- Check-mode bug found on the way: the flush check did not snapshot the open TRIANGLE header's
  count, which sits before the write pointer — in check mode it was incremented twice (native, then
  translated) and the screen showed garbage triangles (normal mode was not affected).
- PPSSPP race: +4% with the emitters.
