# Hot functions: per-function profile (2026-10-16)

Owner request: what would it take to make the expensive game functions native (faster on ARM / the
PSP)? First step: find out how concentrated the game's CPU time is.

## Tooling
- Counting build (CMake `MOTORACER_COUNT_INSNS=ON`, see 21-psp-feasibility.md): the translator emits
  `IC(<function entry>)` before every instruction; `recomp.h` counts in total and per function
  (`recomp_icf[entry - 0x400000]`, 8 MB, counting builds only). Self counts: a function's own
  instructions, not its callees'.
- `MOTORACER_ICOUNT_DUMP=<dir>`: every 5 s the runtime writes `<dir>/ic_<ms>.txt` (`frames <n>`, then
  `<entry> <count>` lines, cumulative).
- `tools/icprof.py <dump A> <dump B> [--gen <gen dir>]`: ranks the functions over the window between
  two dumps (per frame, %, cumulative %, static size).
- `tools/fasm.sh <entry> [gen dir]`: the x86 listing of one translated function.
- Not counted: the runtime (Win32 layer, Direct3D translation, audio) — host code.

## Profile: Speed Bay, PSP profile (480×270, 30 fps, detail 1, 7 opponents)
Racing (903 frames, 1.49 M x86 instructions per frame, 365 functions executed):

| # | Function | K/frame | % | cum % | size | Role |
|---|---|---|---|---|---|---|
| 1 | `0x4af570` | 361 | 24.2 | 24.2 | 639 | build Direct3D vertices: loops over the polygons of the render list, writes 32-byte TLVERTEX records into the vertex buffer (`[0x624afc]+4`); gouraud colours from the shading table `0x5d43b0` and fog colour `0x709370`; textured vertices via `0x4af420`; render states via `0x419d60` |
| 2 | `0x49b7f0` | 222 | 14.9 | 39.1 | 370 | per-face loop of a model: 4 vertex indices (`+0x2c..+0x32`, vertices 28 bytes), depth compares (cull / sort key), calls `0x49c590` |
| 3 | `0x4af420` | 153 | 10.3 | 49.4 | 102 | write one textured TLVERTEX (x, y from ints, z, rhw ×`[0x4d1fd0]`, colour from packed intensity, specular / fog alpha, uv) |
| 4 | `0x49c110` | 102 | 6.8 | 56.2 | 344 | polygon routine (calls `0x49c590`, `0x49ca70`) |
| 5 | `0x49b2c0` | 98 | 6.6 | 62.8 | 142 | vertex transform: 3×4 matrix × vertex, 1/z, projection to integer screen x/y (near-plane test), 12-byte in / 28-byte out |
| 6 | `0x497080` | 71 | 4.8 | 67.5 | 878 | clipping (285 x87 instructions, 16 `_ftol` calls) |
| 7 | `0x49c590` | 59 | 4.0 | 71.5 | 303 | polygon setup (calls clip `0x497080`, `0x497e40`, `0x499460`) |
| 8 | `0x419d60` | 45 | 3.0 | 74.5 | 111 | write an execute-buffer instruction (opcode 8 = STATERENDER, texture handle) |
| 9 | `0x4b3048` | 28 | 1.9 | 76.4 | 16 | CRT `_ftol` (fnstcw / fldcw truncate / fistp), 525 call sites |
| 10 | `0x422b90` | 16 | 1.1 | 77.5 | 26 | |
| 11 | `0x49ca70` | 16 | 1.1 | 78.6 | 290 | polygon routine, like `0x49c590` |
| 12 | `0x4b8800` | 15 | 1.0 | 79.6 | 599 | |
50% of the instructions are in the top 4 functions, 75% in the top 9, 90% in the top 37.

Countdown / start (150 frames, 2.9 M per frame): the same functions, more concentrated — top 3 = 52%,
top 6 = 75% (`0x4af570` 19%, `0x49b7f0` 17%, `0x49c110` 16%, `0x4af420` 10%, `0x49b2c0` 10%). Track
loading (not time-critical) is dominated by other code (`0x4309e0`, `0x42f690`, `0x41c050`).

The game logic (bike physics, AI, camera, sound) is the remaining ~20%, spread thinly.

## Groups
- **A. Direct3D emission** (`0x4af570`, `0x4af420`, `0x419d60`): ~37% racing. Integer and table work;
  the output (execute buffer bytes) is deterministic, so a native version can be checked byte for byte.
  The runtime then parses those buffers again (host cost, not in the counts). On the PSP a native
  version would write GE vertices directly and skip both steps.
- **B. Geometry** (`0x49b2c0` transform, `0x49b7f0` faces, `0x49c110` / `0x49c590` / `0x49ca70`
  polygons, `0x497080` / `0x497e40` clipping, `_ftol`): ~40%. Float work; compared with a tolerance.
  The transform is a textbook VFPU case (3×4 matrix × vector, reciprocal).
- **`_ftol`**: trivial; could be recognised by the translator and replaced by an inline conversion.

## What native versions would take
1. Reverse each function fully (data structures: render list, polygon / face records, the 28-byte
   transformed vertex, matrices, clip planes). Group A ~1–2 days, group B ~3–5 days.
2. A translator table mapping a guest entry to a native function (like the hooks), keeping the
   translated original selectable for testing.
3. Verification: run both on the same recorded inputs from real races and compare outputs and memory
   writes (exact for A, tolerance for B), like `tools/recomp/tests/difftest.py` does per instruction;
   then E2E races.
4. PSP: VFPU for the transform and clipping, direct GE output for group A.

Expected effect (estimate, not measured): translated code needs several host instructions per x86
instruction (flags, memory access through the guest base, registers in a global struct); a native
version of these loops ~3–4× fewer. With ~77% of the instructions in groups A and B, game code would
take roughly 0.4–0.5× its current time. On macOS this is ~1–2 ms per frame and not noticeable; it
matters for the PSP. The translator speed pass (21-psp-feasibility.md, step 2) applies to all code and
should be measured first.
