# Phase 2 — x86 → C Translator

## Usage
```sh
.venv/bin/python -I tools/recomp/recomp_main.py extracted/app/game.EXE build/gen   # ~4 s
# compile check (what CMake will do in phase 3)
clang -std=c11 -O1 -c -Iruntime/include -Ibuild/gen build/gen/code_000.c ...
```
Output (`build/gen/`, gitignored — derived from the game binary, never committed):
- `code_NNN.c` — 65 files × 64 functions, one C function `f_XXXXXXXX(void)` per guest function, each instruction preceded by a disassembly comment.
- `recomp_funcs.h` — prototypes. `imports.h` — 278 import shims `imp_DLL_Name` the runtime must provide.
- `dispatch_table.c` — sorted `{va, fn}` table for indirect calls.

## Status (GOG game.EXE)
| | |
|---|---|
| Functions translated | 4,106 / 4,106 |
| Unsupported instructions | **0** (139 distinct mnemonics) |
| SMC operand reads emitted | 46 (= the 46 patch sites in `05-self-modifying-code.md`) |
| clang `-O1 -Wall` | 0 errors, 0 warnings, ~5 s on 12 cores (26 MB of C) |
| Differential test | **2,405 instruction forms × 16 random trials, 0 mismatches** |

## Execution model
- **Memory**: guest address space = one 4 GB host reservation `M`; guest pointers are `uint32_t` offsets; PE image lives at its original VAs (no relocation needed). Accessors `RD8/16/32/64`, `WR*`, `RDF32/64/80`.
- **Registers**: global `X86Regs R` (`eax…edi`, `df`, `fs_base`). 8/16-bit sub-registers are masked reads/writes of the 32-bit field.
- **Arithmetic flags**: C locals `cf zf sf of pf af` per function, zero-initialised. Every flag-setting instruction computes all its flags; clang dead-code-eliminates the unused ones. Assumption: flags never live across `call`/`ret` (true for MSVC-compiled code; `pushfd`/`popfd`/`lahf`/`sahf` pack/unpack the locals). `df` *is* global because it is architectural state that survives calls.
- **Calls**: `call` pushes the real return VA then calls `f_target()`; `ret n` just does `esp += 4+n` and returns — the popped value is ignored, which keeps `_chkstk`/`_alloca`-style return-address shuffling working.
- **Tail jumps** into another function: `f_target(); return;`.
- **Imports**: `call [IAT]` → direct `imp_DLL_Name()`; register-cached import pointers (`mov esi,[IAT]; call esi`) go through `call_indirect`, which must recognise the magic values the runtime writes into the IAT.
- **Jump tables**: `switch (RD32(table + idx*4)) { case target_va: goto L_…; }` — switching on the loaded target VA rather than the index stays correct even if the index bound was guessed wrong; misses trap.
- **Indirect calls/jumps** (vtables, function pointers): `call_indirect(va)` → binary search in `dispatch_table.c`.
- **x87**: global `X87State F` with 8 `double`s + `top`. Decoded from raw opcode bytes (D8–DF + ModRM), not from capstone mnemonics, to avoid the AT&T `fsub/fsubr` reversal trap in the DC/DE encodings. `double` is exact because the MSVC CRT runs the FPU at 53-bit precision. Rounding control is honoured by `fistp`/`frndint` (`F_ROUND`).
- **MMX**: separate `F.mm[8]` (not aliased onto the x87 stack). CPUID reports no MMX, so MMX paths should stay dormant.
- **SMC**: any instruction whose immediate/displacement bytes are written by a `mov [abs], reg` gets a runtime load from guest memory instead of a constant.
- **Traps**: `int n`, `in/out`, divide errors, undecoded branch targets call runtime hooks (`x86_int`, `x86_in`, `x86_div_fault`, `x86_unimpl`) and return.
- **CPUID**: identifies as a Pentium (family 5) without MMX; `cli/out 0x22/in 0x23` (Cyrix detection at `0x4402c0`) goes to `x86_in/x86_out`.

## Differential tester (`tools/recomp/tests/difftest.py`)
- Collects one-or-two instances of every (mnemonic, operand shape, opcode) from the live code, plus synthetic `cmp/test + setcc/jcc` over all 16 conditions.
- Each case is a tiny guest function: `push [FLAGS_IN]; popfd` (random incoming flags) → optional x87 preload (7–8 random doubles via `fld`) → **instruction** → x87 dump (`fnstsw`, 8× `fstp`) → `pushfd; ret`.
- The same bytes run in **Unicorn 2.1.4** (reference) and through the translator (compiled into a dylib, driven via ctypes). Compared: all GPRs, defined flags (architecturally undefined flags masked per mnemonic), every byte of memory written (Unicorn write hook vs. C pristine-copy diff), x87 TOP/condition codes/stack values (relative tolerance 1e-12).
- **Mutation-checked**: planting a wrong `sub` carry produced 67 failing cases; planting a reversed `fsubrp` produced 4. The harness detects real bugs.
- Run: `.venv/bin/python -I tools/recomp/tests/difftest.py extracted/app/game.EXE build/difftest --trials 16 --per-form 2` (~15 s).

### Bugs it found (fixed)
1. `RDF80` accepted 80-bit *unnormals* (exponent ≠ 0, integer bit clear); real x87 treats them as invalid → NaN.
2. `fprem` didn't report quotient bits in C0/C3/C1.
3. `FXAM` reported "empty" because the tag word isn't tracked → empty-detection dropped (the game only uses `fxam` for NaN/Inf/zero classification).

### Harness lessons
- Unicorn silently *stops* on `#DE` (divide overflow) without raising; detect via EIP ≠ end address.
- Reading empty x87 registers gives NaN in hardware but garbage in our model — preload the stack fully.
- `IF` must be set on both sides, and the C1/C2 bits after arithmetic ops are undefined.

## Known gaps / assumptions (revisit if behaviour is wrong)
- Not covered by difftest: control flow inside whole functions (call/ret/tail calls/switches), SMC slots, `fs:` accesses, `int/in/out/cpuid/rdtsc` — these are exercised at runtime instead.
- x87 precision-control bits ignored (always double). If DirectDraw/D3D drops the FPU to 24-bit, results may differ in the last bits.
- x87 tag word not tracked; stack overflow/underflow not detected.
- C++ exceptions / `longjmp` (non-local control transfer) are not supported yet — `RaiseException` will be fatal in the runtime.
- Performance not tuned: registers live in a global struct and every guest memory access is an aliasing `memcpy`. Fine for a 1997 game on an M3; optimise later (register caching in locals).
