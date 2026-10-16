# Main menu: Reverse and Pocket modes

Owner report (2026-10-16): after finishing the game a Reverse mode toggle appears in the main menu. It
cannot be reached or toggled with keyboard or gamepad (mouse only), and with Reverse on, pressing Solo
switched Reverse off instead of opening the Solo menu. The owner suspected another hidden toggle.

## The two toggles
| Toggle | Unlocked by | Mode flag | Menu item |
|---|---|---|---|
| Reverse mode (tracks run backwards) | `ReverseAvailable` (`0x4dc040`) or cheat **CESREVER** (`0x4dc050`) | `0x4dc068` | `[0x53f4e8]`: 5, or 4 if Pocket is not unlocked |
| Pocket mode (pocket bikes) | `PocketAvailable` (`0x4dc03c`) or cheat **CTEKCOP** (`0x4dc054`) | `0x4dc078` | `[0x53f848]` = 4 |

- `ReverseAvailable` / `PocketAvailable` are read with the championship data (`0x428e3e`); finishing
  championships sets them (`0x458172`…, which also shows the unlock pages 0x11 / 0x10).
- Cheat table `0x4f1250`, entries {code, flag, value}; the codes are stored reversed: `CESREVER`
  (REVERSE) → `0x4dc050`, `CTEKCOP` (POCKET) → `0x4dc054`, `CDNALSI` (ISLAND) → `0x4dc058`.
- The track select page labels them (`0x44db75`: "Reverse" ↔ `0x4dc068`, "Pocket" ↔ `0x4dc078`).

## Main menu page (3)
- Init `0x4422c0` registers the items with `0x40bef0` (args: sprite, highlight sprite, nav mask, …):
  0 Solo `[0x504890]` (nav up/down/right/left = –/2/1/–), 1 Link `[0x504898]` (–/3/–/0), 2 Options
  `[0x5049ac]` (0/–/3/–), 3 Exit `[0x5049a4]` (1/–/–/2, Esc shortcut 0x1b), then Pocket (first argument
  `[0x5049e4]`) and Reverse (`[0x5049ec]`), both with nav mask `0xffffffff`.
- Per frame `0x442790`: draws the ON / OFF art (`0x5049f0`…`0x5049fc`; one toggle alone is centred),
  reads the activated item (`0x40c1b0`); Solo / Link start a short sound timer (`0x53f78c`) before acting
  (switch `0x442ddc`: 0 Solo → page 2, 1 Link → page 11?, 2 Options → page 0xb, 3 Exit → page 4), and
  `activated == [0x53f848]` toggles Pocket, `activated == [0x53f4e8]` toggles Reverse (`0x442e51`).

## Causes
- The toggles have no nav links and no item links to them, so the keyboard / gamepad cursor never
  reaches them.
- After a mouse click on a toggle the cursor (menu `+0x10`) stays on it, and arrows cannot move it
  away (no links). Enter / gamepad A, meant for Solo, activates the toggle again → Reverse goes off.

## Fix
`game_hook_main_menu_button_nav` before the six registration calls (`0x442522`, `0x44254d`,
`0x442572`, `0x44259d`, `0x4425f4`, `0x442640`) rewrites each nav mask from the unlock flags, laid out
as on screen (Solo | Link, Reverse | Pocket or one centred toggle, Options | Exit): Solo ↓ Reverse,
Link ↓ Pocket, Reverse ↔ Pocket, toggles ↓ Options / Exit, Options ↑ Reverse, Exit ↑ Pocket; with no
toggles the game's own links are kept.

Dev aid: `MOTORACER_UNLOCK=1|reverse|pocket` sets the unlock flags when the main menu opens.

## Verified (E2E, keyboard only, cursor / flags traced)
- Both unlocked: Solo ↓ Reverse (Enter → on) → Pocket (Enter → on) ↑ Link ← Solo, Enter → Solo
  activated with both modes still on.
- Reverse only (centred, item 4): Link → Solo ↓ Reverse (Enter → on) ↓ Options ↑ Reverse ↑ Solo,
  Enter → SELECT MODE page, Reverse still on.
- Race with both on: Speed Bay runs backwards with pocket bikes; no errors.
