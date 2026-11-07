# Architecture

1. `recompiler/` finds every function in `game.EXE` (recursive descent, relocations, MSVC switch tables and exception-handling patterns) and translates each x86 instruction to C — integer operations with exact flags, the x87 FPU, MMX and the software rasterizer's self-modifying code.
2. `runtime/` provides the guest address space, Win32 (KERNEL32/USER32 with MFC-compatible window management, GDI, registry, files), C++/SEH exceptions, DirectDraw, Direct3D (execute buffers → SDL_GPU/Metal), DirectSound, an emulated CD-audio drive backed by the GOG audio tracks, joysticks via SDL gamepads, and presentation.
3. A few game-specific fixes are applied at translation time or through small hooks — e.g. relocating the 3D engine's 480-row tables so it can render above 480 lines, the widescreen mode list, HUD scaling and draw distance. Hot game functions also have native C++ rewrites (`runtime/src/game/natives/`), checked against the translated code.

Release packages carry the recompiler and a C compiler, so the game code is built on the player's computer from their own copy ([notes/28-easy-install.md](../notes/28-easy-install.md)). The PSP version uses the same recompiler and runtime with a GE (PSP GPU) backend ([psp.md](psp.md)).

## Repository layout

| Folder | What is there |
|---|---|
| `recompiler/` | The translator (`recomp_main.py`, `recomp/`), the release builds' module builder (`build_gamecode.py`), the differential tests (`tests/`) |
| `runtime/src/core/` | Program entry, PE loader, guest memory, exceptions, dispatch, profiler |
| `runtime/src/win32/`, `dx/`, `gpu/` | Windows API, DirectDraw / Direct3D / DirectSound, the SDL_GPU shaders |
| `runtime/src/host/` | SDL window, audio, video and presentation |
| `runtime/src/game/` | Moto Racer specifics: hooks, the Recomp menu, settings, the first-run installer; `natives/`: native rewrites of game functions |
| `runtime/src/platform/`, `psp/` | OS adapters (macOS, Linux, Windows, PSP) and the PSP's GE backend |
| `runtime/include/` | Headers shared with the generated code |
| `scripts/setup/` | Setup helpers (installer unpacking, Linux container build, Windows toolchain, the PSP build) |
| `scripts/psp/`, `dev/`, `release/` | PSP packaging and tests, development tools (E2E tests, profiling, disassembly), release kits |
| `packaging/` | Per-platform packaging (macOS app bundle, Windows manifest, Linux icon) |
| `docs/guides/`, `dev/`, `notes/`, `decomp/` | Player guides, developer docs, dated technical notes, decompilation notes |
| `site/` | The project website (and its image generator in `site/tools/`) |
| `setup.sh`, `setup.cmd`, `setup.ps1` | One-step builds from the GOG installer (`--psp`: the PSP version) |


## Further reading
- [docs/README.md](../README.md): index of the technical notes (reverse engineering, translator, runtime, ports)
- [notes/04-strategy.md](../notes/04-strategy.md): the recompilation strategy and decisions
- [decomp/](../decomp/README.md): what the native rewrites found out about the game's code
- Roadmap: [notes/12-roadmap.md](../notes/12-roadmap.md), [notes/15-ports-plan.md](../notes/15-ports-plan.md) — 21:9, CI builds, and hand-decompiling game modules into readable C++.
