# Moto Racer Recomp

Play **Moto Racer** (Delphine Software, 1997) natively on modern Macs, Linux PCs and the **Steam Deck**: in widescreen, at up to 4K, with a gamepad.

This is a *static recompilation*: the original Windows game executable from your GOG copy is translated to C on your machine and linked against a small runtime that re-implements the Windows, DirectDraw/Direct3D, DirectSound and CD-audio parts the game needs. No emulator, no Wine: the game runs as a native Apple Silicon or x86-64 Linux program.

> **You need your own copy of the game.** Buy *Moto Racer* on [GOG](https://www.gog.com/game/moto_racer) and download the offline installer (`setup_moto_racer_2.1.0.11.exe`). This repository contains no game code or assets; everything is generated from your installer on your own machine.

## Features
- Native macOS app (Apple Silicon) or Linux / Steam Deck program, built locally from your GOG installer with one command.
- The game's Direct3D renderer on the GPU (Metal) — or its original software renderer (Options › D3D: OFF).
- **True widescreen**: the game's own 16/9 mode at native 1280×720, 1920×1080, 2560×1440 or 3840×2160, or 16:10 at 1280×800 (Steam Deck), 1920×1200 or 2560×1600, chosen in the game's Options › SCREEN MODE.
- Race HUD scaled to the resolution, longer draw distance and smoother opponent level of detail.
- **Gamepads** (Xbox, PlayStation, Switch Pro, 8BitDo, …): analog throttle/brake on the triggers, menu navigation with the d-pad/stick, a ready-made layout.
- CD music from the GOG audio tracks, sound effects, working volume sliders.
- Good defaults on first start: fullscreen, 16/9 mode at a native resolution for your display (16:10 on 16:10 screens), JOYSTICK controls on a Steam Deck.

## Quick start
1. **Prerequisites** (once): an Apple Silicon Mac with macOS 12 or later,
   - Xcode Command Line Tools: `xcode-select --install`
   - [Homebrew](https://brew.sh)
2. **Get the code and run the setup** with the path to your GOG installer:
   ```sh
   git clone https://github.com/segovia-no/MotoRacerRecomp.git
   cd MotoRacerRecomp
   ./setup.sh ~/Downloads/setup_moto_racer_2.1.0.11.exe
   ```
   The script installs the missing tools (`cmake`, `sdl3`, `innoextract`) with Homebrew, unpacks the installer to `~/Library/Application Support/MotoRacerRecomp/game`, builds the game (about a minute the first time) and puts **MotoRacer.app** in `~/Applications`.
3. **Play**: open *MotoRacer* from Launchpad or Spotlight, or `open ~/Applications/MotoRacer.app`.

To update after a `git pull`, run `./setup.sh` again (no argument needed); your game files, saves and settings are kept.

## Steam Deck / Linux
1. In **Desktop Mode**, download the GOG offline installer to `~/Downloads` and open **Konsole**:
   ```sh
   git clone https://github.com/segovia-no/MotoRacerRecomp.git
   cd MotoRacerRecomp
   ./setup.sh ~/Downloads/setup_moto_racer_2.1.0.11.exe
   ```
   No sudo or developer mode is needed. The script:
   - builds inside Valve's Steam Runtime SDK container with `podman` (preinstalled on SteamOS);
   - unpacks the installer in a small Debian container;
   - installs to `~/.local/share/MotoRacerRecomp`, adds a menu entry and adds *Moto Racer* to your Steam library.

   The first run downloads about 2 GB of build images and takes a few minutes.
2. Go back to **Game Mode** and start *Moto Racer* from your library (under Non-Steam).
   - The built-in controls work as a gamepad.
   - The game starts in 16/9 MODE at the Deck's native **1280×800**.
   - Docked to a TV? Choose e.g. 1920×1080 in Options › SCREEN MODE.

Other Linux PCs (x86-64): same command. It needs `podman` or `docker`; the binary runs on any distribution with glibc 2.29 or later.

## Controls
**Keyboard** (as in the original; change them in Options › CONTROLLER › config): arrows to steer, accelerate and brake, **A**/**Z** gear up/down, **Space** turbo/stunts, **Esc** pause.

**Gamepad** (Xbox 360 layout; PlayStation and Switch pads map by button position):

| Action | Button |
|---|---|
| Accelerate / brake (analog) | **RT** / **LT** |
| Steer | left stick |
| Gear up / down | **RB** / **LB** |
| Turbo / stunts / wheelie | **A** |
| Pause | **Start** |
| Menus | d-pad or left stick, **A** select, **B** back |

A gamepad connected on first start sets the game's CONTROLLER option to JOYSTICK with this layout. Plugged in later? Choose Options › CONTROLLER › JOYSTICK and the layout is applied (it never overwrites bindings you made yourself).

**Window**: **Alt+Enter** (also **Cmd+F** on macOS) toggles fullscreen; **Cmd+Q** quits on macOS.

## Settings
Most settings are the game's own, in **Options**:
- **SCREEN MODE** — with 16/9 MODE on: 16:9 1280×720 … 3840×2160 and 16:10 1280×800 … 2560×1600 (rendered natively); off: 4:3 modes up to 1920×1440.
- **16/9 MODE** — widescreen.
- **D3D** — OFF (original software renderer, original resolutions), ON, FILTERED (smooth textures).
- **DETAIL LEVELS**, opponents, volumes, controller.

Port options live in the `[Recomp]` section of `motoracer.ini` in the game folder (`~/Library/Application Support/MotoRacerRecomp/game` on macOS, `~/.local/share/MotoRacerRecomp/game` on Linux):

| Setting | Default | Meaning |
|---|---|---|
| `Fullscreen` | `1` (from setup) | start in fullscreen |
| `WindowWidth`, `WindowHeight` | `1280`, `960` | initial window size |
| `VSync` | `1` | sync to the display |
| `Filter` | `linear` | scaling filter for the picture: `linear` or `nearest` |
| `IntegerScale` | `0` | `1` = only whole-number scaling (sharp pixels, may add borders) |
| `DrawDistance` | `2` | track sections drawn ahead, opponent draw distance and detail, as a multiple of the original (`1` = original) |
| `HudScale` | `auto` | race HUD size in large modes: `auto` (width ÷ 640), a factor such as `2`, or `1` (off) |
| `Widescreen` | `auto` | show the game's 16/9 band as a true 16:9 picture; `off` keeps the letterbox |
| `CustomModes` | `1` | offer the extra resolutions in SCREEN MODE |
| `RenderScale` | `auto` | Direct3D supersampling: `auto` upscales the game's original modes to the display, larger modes render natively; or `1`…`4` |
| `ControllerPreset` | `1` | apply the Xbox layout described above |
| `ControllerMenus` | `1` | navigate the menus with a gamepad |
| `TriggerMode` | `auto` | triggers as analog throttle/brake in JOYSTICK mode, as buttons 9/10 otherwise; or `axis` / `buttons` |
| `Renderer` | `d3d` | `software` forces the original renderer everywhere (the in-game D3D option is usually enough) |

## Troubleshooting
- **Log**: `~/Library/Logs/MotoRacerRecomp/motoracer.log` on macOS, `~/.local/state/MotoRacerRecomp/motoracer.log` on Linux (the previous run is kept as `motoracer.log.1`). Please attach it to bug reports.
- **Reinstall the game files**: `./setup.sh path/to/installer.exe` again.
- **Reset settings**: delete the `[Recomp]` section of `motoracer.ini` and `recomp_state.txt` in the game folder; the game's own options are in `data/CONFIG.BPT`.
- **Uninstall**: delete `~/Applications/MotoRacer.app` and `~/Library/Application Support/MotoRacerRecomp` (macOS), or `~/.local/share/MotoRacerRecomp` and `~/.local/share/applications/motoracer-recomp.desktop` (Linux).
- The build only accepts the GOG `game.EXE` (checked by hash), because the recompiler's analysis is specific to that binary.

## Building by hand
```sh
brew install cmake sdl3 innoextract
python3 -m venv .venv && .venv/bin/pip install -r tools/recomp/requirements.txt   # + unicorn for the tests
tools/install_game.sh ~/Downloads/setup_moto_racer_2.1.0.11.exe
cmake -S . -B build/cmake -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang \
      -DGAME_EXE="$HOME/Library/Application Support/MotoRacerRecomp/game/game.EXE"
cmake --build build/cmake -j
build/cmake/motoracer                          # run directly, or:
cmake --build build/cmake --target app         # -> build/cmake/MotoRacer.app
```
On Linux without containers: install cmake, a C/C++ compiler, Python 3 and innoextract from your distribution, and add `-DMOTORACER_BUNDLED_SDL=ON` if it has no SDL3 3.2+ (or see `tools/linux/sdk_build.sh` for the container build).

Game files are found in this order: `--game-dir DIR`, `$MOTORACER_GAME_DIR`, `<data dir>/MotoRacerRecomp/game` (`~/Library/Application Support` on macOS, `$XDG_DATA_HOME` or `~/.local/share` on Linux), `./extracted/app`.

Developer aids: `--trace` logs every Windows API call; environment overrides `MOTORACER_SNAPSHOT_DIR` / `MOTORACER_SNAPSHOT_EVERY` (save frames as BMP), `MOTORACER_RENDERER`, `MOTORACER_RENDER_SCALE`, `MOTORACER_DRAW_DISTANCE`, `MOTORACER_D3D_DEBUG=1`, `MOTORACER_GAME_DIR`. Unattended end-to-end tests drive the menus with scripted keys: `tools/e2e.sh`, `tools/e2e_race.sh` ([docs/13-e2e-testing.md](docs/13-e2e-testing.md)). The translator is checked instruction by instruction against the Unicorn emulator: `.venv/bin/python tools/recomp/tests/difftest.py <game.EXE> <work dir>`. After editing `runtime/shaders/`, regenerate the compiled shaders with `tools/build_shaders.sh` (needs `glslang` and `spirv-cross`).

## How it works
1. `tools/recomp/` finds every function in `game.EXE` (recursive descent, relocations, MSVC switch tables and exception-handling patterns) and translates each x86 instruction to C — integer operations with exact flags, the x87 FPU, MMX and the software rasterizer's self-modifying code.
2. `runtime/` provides the guest address space, Win32 (KERNEL32/USER32 with MFC-compatible window management, GDI, registry, files), C++/SEH exceptions, DirectDraw, Direct3D (execute buffers → SDL_GPU/Metal), DirectSound, an emulated CD-audio drive backed by the GOG audio tracks, joysticks via SDL gamepads, and presentation.
3. A few game-specific fixes are applied at translation time or through small hooks — e.g. relocating the 3D engine's 480-row tables so it can render above 480 lines, the widescreen mode list, HUD scaling and draw distance.

The reverse-engineering notes and design decisions are in [docs/](docs/00-overview.md).

## Roadmap
See [docs/12-roadmap.md](docs/12-roadmap.md) and [docs/15-ports-plan.md](docs/15-ports-plan.md): the Windows port, 21:9, and hand-decompiling game modules into readable C++.

## Distribution and legal
Only this source repository may be shared. Everything a build produces — the generated C in `build/cmake/gen/`, the `motoracer` binary and `MotoRacer.app` — contains code translated from your `game.EXE` and must **not** be redistributed. There are no prebuilt releases: every player builds from their own GOG copy.

The recompiler, runtime and documentation are released under the [MIT License](LICENSE); the license does not cover Moto Racer itself or anything generated from it.

This is an unofficial fan project for preservation and interoperability. It is not affiliated with or endorsed by Delphine Software, the current rights holders of Moto Racer, or GOG. "Moto Racer" and related names are trademarks of their respective owners.
