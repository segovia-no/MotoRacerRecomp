# Moto Racer Recomp

A static recompilation of **Moto Racer** (Delphine Software, 1997; "Polygons 3.22" build from the GOG release) that runs natively on macOS (Apple Silicon). The original Win32 x86 executable is translated to C at build time and linked against a small runtime that re-implements the Windows, DirectDraw, DirectSound and CD-audio APIs the game uses on top of SDL3.

**Status:** playable — intro, menus (mouse, keyboard, gamepad), races (keyboard, gamepad with analog triggers), CD music, sound effects and volume sliders. The game's Direct3D path renders on the GPU (Metal); the game's own Options › SCREEN MODE offers resolutions up to 3840×2160 with true widescreen from its 16/9 mode, the race HUD is scaled to match, and draw distance / opponent detail are extended. See [docs/00-overview.md](docs/00-overview.md).

> This repository contains **no game code or assets**. You need your own copy of Moto Racer from [GOG](https://www.gog.com/game/moto_racer). The C code is generated locally from your `game.EXE` and never committed.

## Requirements
- macOS 12+ on Apple Silicon (arm64)
- Homebrew: `brew install cmake sdl3 innoextract`
- Python 3 (for the recompiler; Pillow is only needed for the E2E contact sheets)
- The GOG installer `setup_moto_racer_2.1.0.11.exe`

## Setup
```sh
# 1. Python tools for the recompiler
python3 -m venv .venv && .venv/bin/pip install pefile capstone unicorn

# 2. Game files -> ~/Library/Application Support/MotoRacerRecomp/game
tools/install_game.sh /path/to/setup_moto_racer_2.1.0.11.exe

# 3. Build (recompiles game.EXE to C, then compiles everything)
cmake -S . -B build/cmake -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang \
      -DGAME_EXE="$HOME/Library/Application Support/MotoRacerRecomp/game/game.EXE"
cmake --build build/cmake -j

# 4a. Run directly
build/cmake/motoracer
# 4b. or build a double-clickable app
cmake --build build/cmake --target app      # -> build/cmake/MotoRacer.app
```
The build only accepts the GOG `game.EXE` (checked by hash/timestamp) because the recompiler's analysis is specific to that binary.

Game files are found in this order: `--game-dir DIR`, `$MOTORACER_GAME_DIR`, `~/Library/Application Support/MotoRacerRecomp/game`, `./extracted/app`.

## Controls
- Keyboard and mouse as in the original game.
- Gamepads (Xbox, PlayStation, Switch Pro, 8BitDo, …) appear as an analog joystick: left stick steers, right trigger accelerates, left trigger brakes, face/shoulder buttons are joystick buttons 1–8. Configure them in the game's control options.
- **Cmd+F** or **Alt+Enter**: toggle fullscreen. **Cmd+Q**: quit.

## Options
`motoracer.ini` in the game folder (the same file GOG's launcher uses):
```ini
[Settings]
Params=-DoRealLock ... -FrameRateMax60   ; game command line (GOG default)

[Recomp]
Fullscreen=0
WindowWidth=1280
WindowHeight=960
Filter=linear        ; or nearest
IntegerScale=0       ; 1 = whole-number scaling only
VSync=1
Renderer=d3d         ; d3d (default). In game, Options > D3D: OFF = original software renderer, ON / FILTERED = GPU
RenderScale=auto     ; auto: original modes upscaled to the display, larger modes native; or 1..4
CustomModes=1        ; d3d: SCREEN MODE offers 800x600..1920x1440, or 1280x720..3840x2160 with 16/9 MODE on
DrawDistance=2       ; track and opponent draw distance / bike detail, multiple of the original (1 = original)
HudScale=auto        ; race HUD size in large modes: auto (width/640), a factor like 2, or 1 (off)
Widescreen=auto      ; show the game's own 16/9 MODE band at 16:9; off to disable
ControllerMenus=1    ; navigate menus with a gamepad
TriggerMode=auto     ; analog triggers in the game's joystick mode, buttons 9/10 in joypad mode
```
Debugging: `--trace` logs every Windows API call; the log is `~/Library/Logs/MotoRacerRecomp/motoracer.log`. Environment overrides for development: `MOTORACER_SNAPSHOT_DIR=<dir>` (save frames as BMP, `MOTORACER_SNAPSHOT_EVERY=<n>`), `MOTORACER_RENDERER=software|d3d`, `MOTORACER_RENDER_SCALE=<n>`, `MOTORACER_DRAW_DISTANCE=<x>`, `MOTORACER_D3D_DEBUG=1`, `MOTORACER_GAME_DIR=<dir>`.
Unattended testing: `tools/e2e.sh` / `tools/e2e_race.sh` drive the menus with scripted keys and build a contact sheet — see [docs/13-e2e-testing.md](docs/13-e2e-testing.md).

## How it works
1. `tools/recomp/` finds every function in `game.EXE` (recursive descent + relocations, MSVC switch/EH patterns) and translates each x86 instruction to C — integer ops with exact flags, x87 FPU, MMX, self-modifying rasterizer operand slots.
2. The translation is checked instruction-by-instruction against the Unicorn emulator (`tools/recomp/tests/difftest.py`).
3. `runtime/` provides the guest address space, Win32 (KERNEL32/USER32 with MFC-compatible window management, GDI, registry, files), C++/SEH exceptions, DirectDraw, DirectSound, an emulated CD-audio drive backed by the GOG WAV tracks, joysticks, and SDL3/Metal presentation.

Design notes and reverse-engineering findings live in [docs/](docs/).

## Distribution
Only this source repository may be shared. Everything produced by a build — `build/cmake/gen/*.c`, the `motoracer` binary and `MotoRacer.app` — contains code translated from your `game.EXE` and must **not** be redistributed (no prebuilt releases). Each player installs their own GOG copy and builds locally.

## Legal
This is an unofficial fan project for preservation and interoperability. It is not affiliated with or endorsed by Delphine Software, the current rights holders of Moto Racer, or GOG. "Moto Racer" and related names are trademarks of their respective owners. The repository contains no code, data or assets from the game; you must own a legitimate copy to build and play.

## Roadmap
See [docs/12-roadmap.md](docs/12-roadmap.md). Next up: Linux / Steam Deck and Windows ports, other aspect ratios (21:9, 16:10), and hand-decompiling game modules into readable C++ (hybrid approach).
