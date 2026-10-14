# Phase 3 — Runtime Boot

## Build & run
```sh
python3 -m venv .venv && .venv/bin/pip install pefile capstone unicorn   # once
brew install sdl3 cmake                                                   # once
cmake -S . -B build/cmake -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang
cmake --build build/cmake -j 12
build/cmake/motoracer [--game-dir extracted/app] [--trace] [-- game args]
```
- CMake runs `tools/recomp/recomp_main.py` on `GAME_EXE` (default `extracted/app/game.EXE`) to produce `build/cmake/gen/` (64 code files + `dispatch_table.c` + `imports.c`). Nothing generated is committed.
- Without `--`, the game gets the GOG launcher's default arguments (`-DoRealLock -OpaqueBlackLevel3 -TextureSquareOnly -Pal16 -CorrectRatioTextures -NoDither -TextureVideoMem268435456 -FrameRateMax60`).
- `--trace` logs every import call: `[return VA] DLL!Name(args…)`.

## Runtime layout (`runtime/`)
| File | Role |
|---|---|
| `include/recomp.h` | CPU model used by generated code (regs, x87, memory accessors) |
| `include/runtime.h` | Host API: address-space layout, heap, thunks, `guest_call_*`, `STDCALL()` shim macro |
| `src/main.cpp` | Args, SDL init, loads image, switches to a 512 MB host stack **on the main thread** (Cocoa needs it) via `plat::Execution` |
| `src/memory.cpp` | 4 GB guest reservation (address space, not RAM; first 64 KB inaccessible), commits on use, best-fit heap arena, VirtualAlloc arena |
| `src/platform/` | host OS adapters (virtual memory, big stack, crash reports, files, folders): see 15-ports-plan.md |
| `src/gpu/shaders.*`, `shaders/` | GLSL shaders compiled to SPIR-V + MSL (`tools/build_shaders.sh`), one backend adapter per format |
| `src/loader.cpp` | Verifies GOG `game.EXE` (size + PE timestamp `0x35A4A64F`), maps sections, fills all 284 IAT slots with thunks, TEB/PEB/stack |
| `src/dispatch.cpp` | `call_indirect`, thunk registry, traps, crash handler (reports *guest* address + host backtrace whose `f_XXXXXXXX` frames are guest VAs) |
| `src/x86_helpers.c` | x87 conversions, string ops, CPUID |
| `src/host_window.cpp` | SDL window for the game's main window, SDL scancode → VK / PC scancode, `GetKeyState` |
| `src/win32/*.cpp` | KERNEL32, USER32 (+dialogs), GDI32, ADVAPI32, WINMM, small DLLs |

Imports not implemented yet are weak stubs (generated `imports.c`) that stop with `unimplemented import DLL!Name (called from 0x…)` — the next thing to implement is always obvious.

## Guest address space
| Range | Use |
|---|---|
| `0x00000000–0x0000FFFF` | inaccessible (null-pointer traps) |
| `0x00100000–0x002FFFFF` | main thread stack |
| `0x00400000–0x0072FFFF` | game.EXE image |
| `0x01000000–0x3FFFFFFF` | HeapAlloc / GlobalAlloc / LocalAlloc |
| `0x40000000–0x6FFFFFFF` | VirtualAlloc (CRT small-block heap lives here) |
| `0x7FFDE000` / `0x7FFDF000` | TEB / PEB (`fs:` base = TEB) |
| `0xFFC00000–0xFFEFFFFF` | thunks: fake code addresses for host functions (IAT entries, built-in window procs, later COM vtables) |

## Shim ABI
At shim entry `[esp]` is the return address and `arg(i)` = `[esp+4+4i]`. `STDCALL(DLL, Name, N)` pops return address + N args and puts the result in EAX; `CDECL` pops only the return address (`wsprintfA`). Shims call back into guest code with `guest_call_stdcall/cdecl` (window procs, hooks, dialog procs).

## Win32 emulation notes
- **Windows 98** identity (`GetVersion` = `0xC0000A04`, platform 1).
- **Paths**: guest sees `C:\MOTORACER` (install) and `D:\` (CD-ROM, label `MOTORACER`); both map to the game dir; case-insensitive lookup. INI files under `C:\WINDOWS` go to `<game dir>/windows/`. Registry persists to `<game dir>/registry.txt`.
- **USER32** is a small window manager built around MFC 4.2's needs:
  - `CreateWindowExA` calls the WH_CBT hook with `HCBT_CREATEWND` first (MFC subclasses every window there via `SetWindowLong(GWL_WNDPROC)` + `SetProp`), then `WM_GETMINMAXINFO`, `WM_NCCREATE`, `WM_CREATE`.
  - Showing a top-level window sends `WM_SHOWWINDOW`, `WM_ACTIVATEAPP`, `WM_NCACTIVATE`, `WM_ACTIVATE`, `WM_SETFOCUS`, `WM_SIZE`, `WM_MOVE`.
  - Message queue + timers; `GetMessage`/`PeekMessage` pump SDL events (keys → `WM_KEYDOWN/UP` with real PC scan codes in lParam, window close → `WM_CLOSE`).
  - Built-in classes (`BUTTON`, `STATIC`, `#32770`, …) use host thunks for `DefWindowProc`/`DefDlgProc`.
  - Dialogs: `DLGTEMPLATE` parsed, controls created as child windows, `WM_INITDIALOG` sent. Nothing is drawn.
- **GDI** returns plausible tokens (desktop reported as 1024×768×16).

## What the game does at boot (observed)
1. CRT init (heap via VirtualAlloc small-block heap, TLS, environment, locale).
2. C++ static constructors: global `CWinApp` subclass at `0x508838` (vtable `0x4d0420`) — the game is an **MFC 4.2** application.
3. `InitInstance` at `0x414080`:
   - reads `HKLM\SOFTWARE\DelphineSoft\MotoRacer\CurrentVersion\Moto` values `DataPath`, `SavingPath`, `Language` (`SWEDISH/SPANISH/ITALIAN/GERMAN/FRENCH` → index 1–5 in `[0x4dbce8]`, else 0 = English), `InstallType` (default 3), `Version`; and `HKLM\…\Windows\CurrentVersion\ProductId`. Missing keys are tolerated: the DataPath/SavingPath success checks at `0x414315`/`0x41431c` are dead `je` to the next instruction (patched out).
   - registers class `"DirectDraw"` (wndproc `0x432980`) and creates the popup main window `"Moto Racer - © 1997"` (style `0x80CA0000`).
   - creates modeless MFC dialog 231 `"Moto"` (two STATIC lines — a loading/status box).
   - polls joysticks (`joyGetDevCapsA` ×8) and the mixer.
   - calls `DirectDrawEnumerateA` from `0x475150` → **phase 4**.

## Packaging (added after phase 5)
- `runtime/src/settings.*`: reads `<game dir>/motoracer.ini`; `[Settings] Params=` (GOG launcher) is the game command line; `[Recomp]` holds window size, fullscreen, filter, integer scale, vsync.
- Game folder search: `--game-dir`, `$MOTORACER_GAME_DIR`, `~/Library/Application Support/MotoRacerRecomp/game`, `./extracted/app`; error dialog if none.
- `tools/install_game.sh <setup.exe>`: innoextract into the Application Support folder.
- `cmake --build build/cmake --target app` → `tools/make_app.sh`: `MotoRacer.app` with bundled SDL3 (`@rpath`), icon generated from the user's `goggame-*.ico`, ad-hoc signature.
- Hotkeys: Cmd+F / Alt+Enter fullscreen, Cmd+Q quit; Cmd combinations are not forwarded to the game.
