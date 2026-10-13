# Strategy & Decisions

## Decisions (2026-10-13, confirmed with project owner)
| # | Decision | Choice |
|---|---|---|
| D1 | Approach | **Static recompilation x86 → C, then hybrid**: hand-rewrite functions into readable C++ over time |
| D2 | First renderer | **Software renderer** (16-bit DirectDraw) first; Direct3D 5 path later |
| D3 | Multiplayer | **Stubbed** (DirectPlay returns "no providers"); revisit later |
| D4 | Repo policy | **Public**. No game code/assets/generated code in git. User supplies `game.EXE`; C is generated at build time |
| D5 | Host | macOS arm64 first (Apple M3 Pro dev box) |
| D6 | Baseline binary | **GOG release only** (`setup_moto_racer_2.1.0.11.exe` → `game.EXE` SHA-256 `b2f92109…cb2187`). Purchasable today; original CD effectively lost. Build verifies this hash and refuses others. |

## Defaults chosen (not yet challenged)
- Recompiler: **Python 3 + capstone**, emits C. (845 KB code, Python fast enough; easy to iterate.)
- Runtime/shim: **C++20**, **SDL3** for window/input/audio/timer; Metal later via SDL_GPU for the D3D path.
- Build: CMake + clang.

## Architecture

```
game.EXE ──► recomp (tools/recomp) ──► build/gen/*.c  (one C fn per x86 function)
                                            │
runtime/ (C++)                              ▼
  memory.cpp   guest 4 GB address space, PE sections mapped at original VAs
  cpu.h        CPU state: regs, flags (lazy), x87 stack, MMX
  dispatch.cpp guest VA → host fn table (indirect calls / vtables / callbacks)
  win32/       KERNEL32/USER32/GDI32/ADVAPI32/WINMM shims (stdcall, args on guest stack)
  dx/          DirectDraw/D3D/DSound/DInput/DPlay COM objects living in guest memory,
               vtables filled with "host thunk" addresses
  sdl/         SDL3 backend
```

### Key mechanisms
- **Guest memory**: reserve 4 GB host VM (`mmap` with `MAP_NORESERVE`), `MEM(a) = base + a`. Pointers stay 32-bit; struct layouts unchanged. Host-side allocations for the guest (HeapAlloc, VirtualAlloc, COM objects) come from a guest-space allocator.
- **Calls**: guest stack in guest memory; `call` pushes return VA, calls host fn; `ret` pops. Each generated fn is `void f_XXXXXXXX(Cpu*)`.
- **Indirect calls/jumps**: lookup in dispatch table; miss = fatal log with VA (finds undiscovered functions).
- **Imports**: IAT filled with magic VAs (e.g. `0xF000xxxx`); dispatch maps them to shim fns.
- **Jump tables**: discovered via `jmp [tbl + r*4]` + relocation entries pointing into tables.
- **Function discovery**: entry point + call targets + relocations pointing into `.text` (vtables, callbacks) + exported tables. Do not trust Hopper boundaries blindly (see SMC doc: Hopper mis-split `0x4af3f0`).
- **x87**: 8-entry `double` stack + control/status word; `fistp` honors rounding mode (MSVC `_ftol` sets truncation).
- **Flags**: per-instruction lazy flags (store op/result/operands), materialized when consumed; peephole `cmp/test + jcc`.
- **SMC**: operand-slot table → runtime loads (see `05-self-modifying-code.md`).
- **SEH / fs:[0]**: provide a TEB in guest memory; implement `RtlUnwind`/`RaiseException` minimally.
- **MFC/CRT**: translated like everything else at first; replace with native versions later where cheaper.

## Phases
1. **Loader + function discovery** — ✅ done (see `06-function-discovery.md`): 4,106 functions, 97.1 % of .text, remaining gaps verified dead.
2. **Translator** — x86 → C for integer ISA, x87, MMX; compile all of `.text` cleanly.
3. **Runtime boot** — CRT startup → `WinMain` reaches window creation with KERNEL32/USER32 shims.
4. **DirectDraw software path** — first frame on screen via SDL3.
5. **Input + timing + DSound + CD audio (WAV)** — playable.
6. **D3D5 path** on Metal/SDL_GPU, resolution upgrades.
7. **Hybrid**: hand-decompile game modules (bank loader, menu, physics…) into readable C++.
