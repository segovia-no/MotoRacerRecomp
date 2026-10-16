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

## Plan
- Case 21 (`0x444983`): open the recomp page instead of page 13.
- Recomp page: a runtime version of the Options per-frame function that calls the game's own
  functions (background, font `0x4903d0(0xc)`, `0x493ee0`, `0x40bd60`, dispatch via `0x40c1b0`) with
  the port's strings, so it looks like Options. `guest_call_stdcall`/`cdecl` exist (dispatch.cpp); a
  thiscall variant sets ecx.
- Button art: the "Info" label is baked into the button texture; it needs a "Recomp" replacement.
- Open: signatures of `0x493ee0` and `0x40bd60`; how the background, header ("OPTIONS" banner) and
  buttons are drawn; the button texture.
