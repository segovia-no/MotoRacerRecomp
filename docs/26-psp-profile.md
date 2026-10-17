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
