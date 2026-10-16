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

## The "Recomp" button image (2026-10-16)
- Menu art is drawn by the game's CPU blitter into the back buffer (only five 256×256 Direct3D
  textures are uploaded on the Options page), so the label is in the sprite's bitmap.
- Sprite object (page object table `0x504880`, Info = `[0x504b28]`): `+8` width, `+0xc` height
  (Info 121×40), `+0x30` packed data, `+0x34` its size, `+0x2c` the unpacked pixels (0 until first used).
  A second image at `+0x8c` (Accept: same size; its role is unconfirmed).
- `0x43eb00` (thiscall sprite) unpacks: data starts with "LEZ1" (LZ: 32-bit flag words, literal bytes,
  16-bit back-references with a 10-bit distance and a 6-bit length + 3), giving a TGA (18-byte header,
  colour-map length at +5, width/height at +0xc/+0xe, BGR palette, 8-bit pixels). The palette is turned
  into 16-bit with the shift bytes at `0x4ee9b8`… (RGB565 here) and the pixels into a w×h RGB565 buffer;
  black (0) is transparent. The button registration `0x40bef0` calls it once and keeps the result at `+0x2c`.
- Unpacked buttons seen: Play Solo / Play Link cards 222×159, Accept / Exit / Start / Continue 120×40,
  Options 172×56, config 52×30, Info / Data disk 121×40. Labels: black italic geometric sans, with no
  R or m in any label, and they change with the game language. So the label is drawn rather than
  pieced together from other buttons.
- Hook `0x40bf5e` (`game_hook_button_unpacked`: eax = pixels, ebx = sprite), Info only:
  1. the old label = pixels dark against their row (luminance < 60, or 35 below the brightest pixel
     3–4 px left/right) in rows 4–23, grown by one pixel; each run is filled by interpolating the oval
     colours on both sides (the strokes are thin, so this hides well; the dark shadow below is
     untouched);
  2. "Recomp" from a small stroked vector font (stems, arcs) in the runtime: cap 17, x-height 12,
     descender 5, stroke 2.5 px, italic shear 0.45, baseline 22.5 (measured against Info and Accept),
     3×3 supersampled, ink = the average of the old label's darkest pixels.
  Nothing of the game's art is shipped; the image is made from the player's own copy at run time.
- Verified (E2E): the Options page shows "Recomp" in place of "Info", matching Accept's style.

## Open
- The header banner still reads "OPTIONS".
- Mouse and gamepad not yet tested on the page (the game's menu handles both).
