# Moto Racer Recomp

A static recompilation of **Moto Racer** (Delphine Software, 1997; "Polygons 3.22" build from the GOG release) that runs natively on macOS (Apple Silicon). The original Win32 x86 executable is translated to C at build time and linked against a small runtime that re-implements the Windows, DirectDraw, DirectSound and CD-audio APIs the game uses on top of SDL3.

**Status:** playable — intro, menus (mouse/keyboard), races (keyboard/gamepad), CD music and sound effects, at the original 640×480 software-rendered resolution. See [docs/00-overview.md](docs/00-overview.md).

> This repository contains **no game code or assets**. You need your own copy of Moto Racer from [GOG](https://www.gog.com/game/moto_racer). The C code is generated locally from your `game.EXE` and never committed.

## Requirements
- macOS 12+ on Apple Silicon (arm64)
- Homebrew: `brew install cmake sdl3 innoextract`
- Python 3 (for the recompiler)
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
```
Debugging: `--trace` logs every Windows API call; `MOTORACER_SNAPSHOT_DIR=<dir>` saves every 60th frame as BMP.

## How it works
1. `tools/recomp/` finds every function in `game.EXE` (recursive descent + relocations, MSVC switch/EH patterns) and translates each x86 instruction to C — integer ops with exact flags, x87 FPU, MMX, self-modifying rasterizer operand slots.
2. The translation is checked instruction-by-instruction against the Unicorn emulator (`tools/recomp/tests/difftest.py`).
3. `runtime/` provides the guest address space, Win32 (KERNEL32/USER32 with MFC-compatible window management, GDI, registry, files), C++/SEH exceptions, DirectDraw, DirectSound, an emulated CD-audio drive backed by the GOG WAV tracks, joysticks, and SDL3/Metal presentation.

Design notes and reverse-engineering findings live in [docs/](docs/).

## Roadmap
- Direct3D renderer path on Metal for higher resolutions
- Hand-decompiling game modules into readable C++ (hybrid approach)
