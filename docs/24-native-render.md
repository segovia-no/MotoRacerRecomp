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
