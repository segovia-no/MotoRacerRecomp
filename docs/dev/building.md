# Building from source

For development, or a system without a package. Each script installs what it needs and builds the game from your GOG installer.

## With the setup scripts
**macOS** (Xcode Command Line Tools: `xcode-select --install`, and [Homebrew](https://brew.sh)):
```sh
git clone https://github.com/segovia-no/MotoRacerRecomp.git
cd MotoRacerRecomp
./setup.sh ~/Downloads/setup_moto_racer_2.1.0.11.exe
```
It installs `cmake`, `sdl3` and `innoextract` with Homebrew, unpacks the installer to `~/Library/Application Support/MotoRacerRecomp/game`, builds the game and puts **MotoRacer.app** in `~/Applications`.

**Steam Deck / Linux**: the same commands in Konsole (Desktop Mode); no sudo or developer mode. It builds inside Valve's Steam Runtime SDK container with `podman` (preinstalled on SteamOS; other distributions: `podman` or `docker`), installs to `~/.local/share/MotoRacerRecomp`, adds a menu entry and, if Steam is running, a Steam library entry. The first run downloads about 2 GB of build images.

**Windows**: clone the repository (or **Code › Download ZIP**), put the GOG installer in Downloads and double-click **`setup.cmd`** (or drop the installer onto it). Python 3 is installed for your user with winget if missing; llvm-mingw, innoextract, CMake and Ninja are downloaded into the project's `build` folder (pinned, checksum-verified). The game goes to `%LOCALAPPDATA%\MotoRacerRecomp` with a Start menu entry.

**PSP**: `./setup.sh --psp` (macOS / Linux) or `setup.cmd --psp` (Windows, builds in WSL): downloads the pspdev toolchain into `build/pspdev`, builds `build/psp` and packages `build/psp-hw/PSP` ([psp-build.md](../guides/psp-build.md)).

To update after a `git pull`, run the setup again (no argument needed). Saves, best times and settings are kept.

## By hand
```sh
brew install cmake sdl3 innoextract
python3 -m venv .venv && .venv/bin/pip install -r recompiler/requirements.txt   # + unicorn for the tests
scripts/setup/install_game.sh ~/Downloads/setup_moto_racer_2.1.0.11.exe
cmake -S . -B build/cmake -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang \
      -DGAME_EXE="$HOME/Library/Application Support/MotoRacerRecomp/game/game.EXE"
cmake --build build/cmake -j
build/cmake/motoracer                          # run directly, or:
cmake --build build/cmake --target app         # -> build/cmake/MotoRacer.app
```
On Linux without containers: install cmake, a C/C++ compiler, Python 3 and innoextract from your distribution, and add `-DMOTORACER_BUNDLED_SDL=ON` if it has no SDL3 3.2+ (or see `scripts/setup/linux/sdk_build.sh` for the container build).

The PSP build by hand, testing in PPSSPP and on hardware, profiling: [psp.md](psp.md).

Game files are found in this order: `--game-dir DIR`, `$MOTORACER_GAME_DIR`, `<data dir>/MotoRacerRecomp/game` (`~/Library/Application Support` on macOS, `$XDG_DATA_HOME` or `~/.local/share` on Linux), `./extracted/app`.

## Developer aids
- `--trace` logs every Windows API call; environment overrides `MOTORACER_SNAPSHOT_DIR` / `MOTORACER_SNAPSHOT_EVERY` (save frames as BMP), `MOTORACER_RENDERER`, `MOTORACER_RENDER_SCALE`, `MOTORACER_DRAW_DISTANCE`, `MOTORACER_D3D_DEBUG=1`, `MOTORACER_GAME_DIR`.
- Unattended end-to-end tests drive the menus with scripted keys: `scripts/dev/e2e.sh`, `scripts/dev/e2e_race.sh` ([notes/13-e2e-testing.md](../notes/13-e2e-testing.md)); PSP in PPSSPP: `scripts/psp/e2e_psp.sh`.
- Native rewrites of game functions are checked against the translated code: `MOTORACER_NATIVE=check` ([notes/24-native-render.md](../notes/24-native-render.md)).
- The translator is checked instruction by instruction against the Unicorn emulator: `.venv/bin/python recompiler/tests/difftest.py <game.EXE> <work dir>`.
- After editing `runtime/shaders/`, regenerate the compiled shaders with `scripts/dev/build_shaders.sh` (needs `glslang` and `spirv-cross`).

How the pieces fit together: [architecture.md](architecture.md).
