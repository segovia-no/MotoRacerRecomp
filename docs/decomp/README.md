# Decompilation notes

What the native rewrites (`runtime/src/native_*.cpp`) and the analysis behind them found out about
`game.EXE` (Moto Racer, Delphine 1998), kept here for a future decompilation. Each rewrite was
checked against the translated x86 at run time (`MOTORACER_NATIVE=check`), so the semantics below
are verified on real races (Speed Bay, Dirt Arena), not guessed.

- [functions.md](functions.md) — functions by address: calling convention, arguments, what they do.
- [structs.md](structs.md) — record layouts: bikes, track sectors, chunks, polygon records,
  vertices, render nodes, matrices, cameras, fonts.
- [globals.md](globals.md) — global variables and tables by address.

Conventions used in these notes:

- Addresses are the image's virtual addresses (base 0x400000). `[x]` = the dword at x.
- Calling conventions are MSVC 5's: `thiscall` (ecx = this, callee pops), `stdcall` (callee pops),
  `cdecl` (caller pops). "ret N" is the callee's `ret N`.
- Floats are 32-bit unless noted; the x87 code keeps intermediates in extended precision and rounds
  where it stores a dword, which matters for bit-exact results (the native code reproduces that).
- Matrices ("4x3"): 12 floats, three rows of 3 (rotation) then the translation at +0x24; a point
  transforms as `o = M * v + t` with `o.x = m6*v.z + m3*v.y + m0*v.x + m9` (see 0x410010).
- Angles index a 16384-entry sine table at 0x5fcb80: `i = _ftol(angle * 4096 / (pi/2))`,
  `sin = T[i & 0x3fff]`, `cos = T[(i - 0x1000) & 0x3fff]`.
- After a call, MSVC code never reads ecx / edx; several functions leave meaningful values there
  only by accident — noted where the native versions had to reproduce them.
