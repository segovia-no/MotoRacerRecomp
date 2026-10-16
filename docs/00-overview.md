# Moto Racer Recomp — Overview & Findings Log

Living index of what we know. Each topic gets its own file; this one links them.

## Source artifact

| Item | Value |
|---|---|
| Installer | `setup_moto_racer_2.1.0.11.exe` (GOG, Inno Setup 5.5.0 unicode) |
| Extraction | `brew install innoextract && innoextract --gog -d extracted setup_moto_racer_2.1.0.11.exe` |
| Game binary | `extracted/app/game.EXE` |
| SHA-256 | `b2f921098f9ba59b076b3dd0bbd013e40b06b8481651b4a7a774757c95cb2187` |
| Version | "Polygons version 3.22" patch, Delphine Software, PE timestamp 1998-07-09 11:15:27 UTC |
| Launcher | `extracted/app/MotoRacer.exe` — GOG/Delphine config launcher, not the game |

Game files are **not** committed (see `.gitignore`). Anyone reproducing must extract their own copy.

## Status
**Playable on macOS (Apple Silicon)** as of 2026-10-13: intro, menus (mouse, keyboard, gamepad), races (keyboard, gamepad with analog triggers), CD music, sound effects, volume sliders, clean quit. Direct3D path on Metal by default (render scale up to 4×), the game's 16/9 mode shown as true widescreen, and SCREEN MODE options from 800×600 to 2048×1536. Software renderer still available (`Renderer=software`).
Not yet: Linux / Steam Deck, Windows, multiplayer (stubbed by decision), GDI text on surfaces.

## Docs index

- [01-binary.md](01-binary.md) — PE layout, toolchain, imports, statically-linked libraries
- [02-game-files.md](02-game-files.md) — data directory, file formats (BKF, BPT, TRK, DEM)
- [03-subsystems.md](03-subsystems.md) — engine subsystems, source module names, command-line flags
- [04-strategy.md](04-strategy.md) — recompilation strategy, decisions, architecture, phases
- [05-self-modifying-code.md](05-self-modifying-code.md) — SMC in rasterizer span loops and how recomp handles it
- [06-function-discovery.md](06-function-discovery.md) — phase 1: our own function/jump-table discovery, coverage, MSVC patterns
- [07-translator.md](07-translator.md) — phase 2: x86→C execution model, differential tester vs Unicorn, known gaps
- [08-runtime.md](08-runtime.md) — phase 3: build/run, runtime layout, address space, Win32/MFC emulation, observed boot sequence
- [09-ddraw-and-eh.md](09-ddraw-and-eh.md) — phase 4: COM, DirectDraw software path, presentation, C++ exceptions via SEH + setjmp/longjmp
- [10-input-audio.md](10-input-audio.md) — phase 5: CD audio (MCI) emulation, DirectSound, keyboard/mouse; speedometer style (F7 / gamepad Back)
- [11-d3d-plan.md](11-d3d-plan.md) — phase 6 plan: game uses Direct3D v1 execute buffers; Metal backend + resolution scaling
- [12-roadmap.md](12-roadmap.md) — owner roadmap: QoL (options menu, controller menus, trigger buttons), widescreen, Linux/Steam Deck, Windows
- [14-draw-distance.md](14-draw-distance.md) — track sections drawn ahead, per-detail-level table (bike LOD, culling), DrawDistance
- [16-frame-rate.md](16-frame-rate.md) — game frame timing (limiter, frame time, replays), higher frame rates: measurements and engine assumptions
- [17-recomp-menu.md](17-recomp-menu.md) — Options page internals (pages, items, dispatch), Info → Recomp settings submenu plan
- [18-main-menu-modes.md](18-main-menu-modes.md) — Reverse / Pocket mode toggles (unlocks, cheat codes), keyboard/gamepad navigation fix
- [19-engine-sounds.md](19-engine-sounds.md) — race sound systems (bike engines rpm model, positional emitters), F11 developer sound menu, why opponents hum
- [20-resource-usage.md](20-resource-usage.md) — resource audit vs 1997 requirements (CPU, memory, GPU, disk) and optimisation candidates
- [21-psp-feasibility.md](21-psp-feasibility.md) — PSP homebrew port analysis (PSP-1000 memory budget, CPU estimate, GE/audio plan)
- [13-e2e-testing.md](13-e2e-testing.md) — unattended E2E runs: scripted keys, snapshots, contact sheets, menu navigation facts
- [15-ports-plan.md](15-ports-plan.md) — Linux / Steam Deck / Windows ports: plan, platform adapters (P0 done), shader pipeline, risks

## Analysis tooling

- REA MCP (Hopper provider) on `game.EXE`: 9219 procedures, 2587 strings.
  - Evidence: `binary_overview` → `ev_9f7e453d…bfae61813`; PE resources → `ev_7be955af…cb841afe`.
- Python venv at `.venv/` (pefile, capstone) for scripted analysis.
