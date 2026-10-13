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

## Docs index

- [01-binary.md](01-binary.md) — PE layout, toolchain, imports, statically-linked libraries
- [02-game-files.md](02-game-files.md) — data directory, file formats (BKF, BPT, TRK, DEM)
- [03-subsystems.md](03-subsystems.md) — engine subsystems, source module names, command-line flags
- [04-strategy.md](04-strategy.md) — recompilation strategy, decisions, architecture, phases
- [05-self-modifying-code.md](05-self-modifying-code.md) — SMC in rasterizer span loops and how recomp handles it

## Analysis tooling

- REA MCP (Hopper provider) on `game.EXE`: 9219 procedures, 2587 strings.
  - Evidence: `binary_overview` → `ev_9f7e453d…bfae61813`; PE resources → `ev_7be955af…cb841afe`.
- Python venv at `.venv/` (pefile, capstone) for scripted analysis.
