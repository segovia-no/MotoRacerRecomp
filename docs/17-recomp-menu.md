# Recomp settings menu (in the game's Options page)

Goal (owner, 2026-10-16): a "Recomp" submenu inside Options, in the game's own look, for the port's
settings (frame rate, filtering, HUD scale, …) instead of editing `motoracer.ini`.

## Menu pages
- Page table `0x4dbb38`, 16 bytes per page: +0 active flag, +4 per-frame function, +8 init function,
  +0xc exit function (called by the page switch). `0x409eb0(page)` (thiscall on the page manager
  `[0x4dc0c4]`) switches page. Labels and button art come from the language files `MENU*.BKF`, not
  from `game.EXE`.
- Pages seen: 11 Options (`0x443200` per frame, `0x443000` init), 12 controller config (`0x444f30`),
  13 **DIAGNOSTIC** (`0x4459d0`/`0x4458a0`), 23 **PLUGIN / DATA DISK** (`0x4514e0`, loads
  `Data\list%02d.spt`, `Data\champ%c%02d.spt`, track photos), 3 = target of Accept.

## Options page (11)
- Init `0x443000` places the four buttons (`0x43fa40` ×4).
- Per frame `0x443200`: background/buttons, then for each row the label in font object
  `0x4903d0(0xc)` (colour `0x4f0de8` when the row is selected, `0x4f0dd4` otherwise) via `0x493ee0`
  with text from the string table (`[0x53b64c]`, …), and the value as a selectable item via
  `0x40bd60` on the menu object `[0x4dc0c4]` (x = 300; items numbered in call order). Cursor row:
  `0x40c0c0`; activated item: `0x40c1b0` (−1 = none).
- Dispatch `jmp [esi*4 + 0x444aac]` at `0x44450d`, items 0..22:
  0 DIFFICULTY (`0x4dc060`), 1 CONTROLLER (`0x4e90d0`), **2 config button** (→ page 12), 3 DETAIL,
  4 `0x4dbfdc` toggle, 5 D3D, 6–8 voices/sounds/music (`0x408f30`, `0x408b30`, `0x409410`),
  9 SCREEN MODE, 10 16/9 MODE (`0x504e80`), 11 WAVE OUTPUT (`0x409570`), 12–17 volume −/+,
  18–19 sessions refresh −/+ (`0x4dc044`, 0..30), **20 Accept** (input re-init, → page 3),
  **21 Info** (`0x444983`, → page 13), **22 Data disk** (`0x44498e`, → page 23).
- Keyboard: DOWN walks the rows and ends on Accept; LEFT from Accept selects Info.

## Which button to take over
- **Data disk** works (E2E): PLUGIN / DATA DISK, a track-set selector (8 tracks per screen, arrows,
  Cancel / Accept). Keep it.
- **Info** opens DIAGNOSTIC: OS, CPU, memory, DirectDraw/Direct3D, sound and joystick lines. In the
  port they only show the emulated values (Windows 95, Direct3D HAL, 256 MB video board). Owner's choice:
  **Info becomes "Recomp"**.

## Implementation (2026-10-16)
- Translator: `GAME_JUMP_HOOKS` emit `if (hook()) goto target;` (target in the same function):
  - `0x444983` Info → `game_hook_options_info`: opens the Recomp page (no page switch), cursor on row 0;
  - `0x44329a` first row → `game_hook_options_rows`: draws our rows and the Back button, continues at
    `0x44448f` (after the game's buttons), so background, header, menu handling and the common tail
    (`0x4449a2`) stay the game's;
  - `0x444504` dispatch → `game_hook_options_action`: our items; Back (the Accept sprite) closes.
  - `0x443000` (page init) → `game_hook_options_init`: the page starts closed.
- `runtime/src/recomp_menu.cpp`: rows at y = 100 + 20·i, label x = 50 (`0x493ee0`), value item x = 300
  (`0x40bd60`, nav mask up = i−1, down = i+1), Back = `0x40bef0` with the Accept sprite (item n).
  Selected row colour `0x4f0de8`. Enter / click cycles a value; Escape (gamepad B) = Back. The cursor
  (menu `+0x10`) goes to row 0 on open and back to Info (21) on close. Guest calls use
  `guest_call_stdcall` / the new `guest_call_thiscall`; registers are restored after them.
- Rows: FRAME RATE (original, display, 30…240), VSYNC (live), SCALING FILTER (smooth/sharp), HUD SCALE
  (auto, original, 1.5…4), DRAW DISTANCE (next race), WIDESCREEN CROP, GAMEPAD IN MENUS.
  Each change is saved at once: `settings::save` rewrites the key in `motoracer.ini [Recomp]` (adds the
  section if missing; CRLF like GOG's file; temp file + rename).
- Verified (E2E): Info opens the page in the game's font; DOWN/UP move the highlight; Enter changes
  values (filter, vsync switched live, logged); Back and Escape return to Options with Info selected;
  the ini gets `[Recomp] Filter=nearest VSync=0`.

## Open
- The button still reads "Info" (label baked into its texture): needs a "Recomp" image.
- The header banner still reads "OPTIONS".
- Mouse and gamepad not yet tested on the page (the game's menu handles both).
