# 13 — End-to-end (E2E) testing

Unattended game runs that drive the real menus with scripted keys and capture frames. This is the
standard way to verify anything visible (renderer, menus, resolutions) before asking the owner to
test. Each run opens a game window for its duration.

## Pieces
- `MOTORACER_AUTOKEYS` (runtime, `host_window.cpp`): a script of waits and key presses injected as
  WM_KEY* messages. Tokens: a number waits that many ms; `ENTER ESC UP DOWN LEFT RIGHT SPACE` or a
  single letter/digit taps a key (held 80 ms); `+KEY` / `-KEY` press / release.
- `MOTORACER_SNAPSHOT_DIR` (runtime): BMP snapshots — `gpu_*.bmp` every 120 Direct3D frames at full
  render-target resolution, `frame_*.bmp` every 60 software-renderer frames.
- `MOTORACER_SNAPSHOT_EVERY=<frames>`: GPU snapshot interval (default 120); e.g. 10 to follow an
  animation such as the race-start banner.
- `EXTRA_KEYS="5000 +UP"` (tools/e2e_race.sh) holds the throttle after the start — needs Options >
  CONTROLLER = KEYBOARD (UP accelerates).
- `MOTORACER_DRAW_DISTANCE=<x>` overrides DrawDistance for A/B runs.
- `MOTORACER_MODES="1024x600,960x720"` (runtime, `ddraw.cpp`): replaces the 16-bit display modes
  above 640×480 offered to the game — for bisecting resolution bugs.
- `MOTORACER_D3D_DEBUG=1`: triangle classification / execute-buffer statistics in the log.
- `tools/e2e.sh <name> "<keys>" [seconds] [ENV=value ...]`: runs the game with the above, copies
  the log to `build/e2e/<name>.log`, writes a contact sheet `build/e2e/<name>.png`
  (`SHEET_GLOB` picks frames, default `gpu_*.bmp`). Sheets need Pillow (system `python3` has it).
- `tools/e2e_race.sh <name> <screen-mode-presses> [seconds] [ENV=value ...]`: canned path to a
  Single Race on Speed Bay, optionally cycling Options › SCREEN MODE first; `TOGGLE_169=1` also
  toggles 16/9 MODE.

## Menu navigation facts (keyboard)
- Intro: `ESC`, title: `ESC`, then `ENTER`, `ENTER` (name entry) → main menu. ~4 s before the first key.
- Menus have no initial selection: the first arrow key selects an item, then `ENTER`. A bare
  `ENTER` does nothing.
- Main menu: `LEFT ENTER` = Play Solo; `DOWN DOWN ENTER` = Options.
- Select mode: `UP UP ENTER` = Single Race (`LEFT` lands on Back).
- Select race: `UP UP UP ENTER` = Continue (Speed Bay). Choose bike: `DOWN DOWN ENTER` = Start.
- Options: rows are DIFFICULTY, CONTROLLER, DETAIL LEVELS, OPPONENTS, D3D, VOICES, SOUNDS, MUSIC,
  SCREEN MODE, 16/9 MODE, ... — 9 × `DOWN` reaches SCREEN MODE. `ENTER` cycles a value (`LEFT`/
  `RIGHT` do nothing). From SCREEN MODE, 7 × `DOWN` reaches Accept.
- SCREEN MODE lists (runtime-owned): 4:3 = 320×240, 512×384, 640×480, 800×600, 1024×768, 1280×960,
  1600×1200, 1920×1440; 16/9 MODE on = 1920×1080 → 2560×1440 → 3840×2160 → 320×180 → 512×288 →
  640×360 → 1280×720 (wraps). The 16/9 MODE row is right below SCREEN MODE; `ENTER` toggles it.
- 300 ms between rows and 1.5 s between screens is enough.
- SCREEN MODE persists between runs (the game saves its size class, the runtime the exact mode —
  see 11-d3d-plan.md), so count SCREEN MODE presses from the saved mode. With `MOTORACER_MODES`
  set to a list without the saved mode, the game starts from 640×480.
- Option values changed by a run stay changed — check DIFFICULTY / CONTROLLER / SOUNDS afterwards.

## Example
```
SHEET_GLOB="gpu_00[23]*.bmp" tools/e2e_race.sh race1024 1 50 MOTORACER_D3D_DEBUG=1 MOTORACER_MODES=1024x768
# -> build/e2e/race1024.png, build/e2e/race1024.log
```
