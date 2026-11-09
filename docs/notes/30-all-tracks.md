# All tracks in one list (GitHub issue #9, 2026-11-09)

Request: Fun Fair and Sea of Sand (the "twonewislands" data disk) next to the original tracks,
without switching the data disk in the options.

## The game's data disks
- Options › Data disk (page 23, `0x4514e0`) chooses a track set; the active one is `[0x4dc048]`
  (0 = `Data\list.spt`, n = `Data\list%02d.spt`, with `champ%c%02d.spt` championships). GOG ships two:
  | Set | Tracks (folder) |
  |---|---|
  | PLUGIN (0) | Speed Bay (ile01), Snow Ride (ile05), Rock Forest (ile03), Lost Ruins (ile04), West Way (ile02), Great Wall (ile06), Red City (ile08), Dirt Arena (ile09) |
  | TWONEWISLANDS (1) | Fun Fair (ile10), Sea of Sand (ile11), then the PLUGIN tracks without Rock Forest / Lost Ruins |
- The list files are the game's encrypted `.BPT` scripts (`.spt` names are mapped to `.BPT`). Reading
  one: object ctor `0x428c70` (0x134 bytes), open `0x4246a0(path, 1)` (thiscall; `0x4246c0` opens
  for writing), parse `0x428d60(0, 0, 0)`, close `0x4246e0`, dtor `0x428d10`. The parse sets
  `ListName` → `0x504eb8` (0x20), `PocketAvailable` → `0x4dc03c`, `ReverseAvailable` → `0x4dc040`,
  `NumberIslande` → `[0x51d298]`, and frees / allocates the records `[0x4dc088]`: 0x130 bytes each,
  name at +4 (0x20), folder at +0x24 (0x104). `[0x4dc08c]` is the selected track's index.
- Loads of the active list: startup `0x472d6e`, options reload `0x433218` (after the write-back
  `0x4291f0` at `0x433183`, which stores the unlock flags and the list in `LIST.BPT`). Page 23 reads
  each set itself to show its tiles (`0x451642`, `0x451a47`).
- About 40 places use the count `[0x51d298]` and loop over it; the single-race track picker cycles
  through the whole list. Best times (`Data\bestscor.dat`, `0x4646c1`) are keyed by the track name
  (+ pocket / reverse suffixes and laps), so added tracks get their own records.

## Merge (`runtime/src/game/tracks.cpp`, setting `AllTracks`, Recomp menu "ALL TRACKS", on by default)
- GAME_HOOK after the active list loads at startup (`0x472d73`): with the setting on and disk 0,
  the original records and the parse's other globals are saved, `Data\list01.spt` is read with the
  game's own functions (guest calls; skipped when `Data\LIST01.BPT` is missing), its records whose
  folder the original list lacks are appended in a new game-heap array (`0x4c5381`; the disk-1 array
  freed with `0x4c53c1`), and the globals restored. A selection past the end goes back to track 0.
- Around the write-back (`0x433183` / `0x433188`) the count is cut back to the original eight, so
  `LIST.BPT` never stores the merged list. Turning the setting off cuts the count the same way.
- Championships and the data disk page are untouched; with disk 1 active nothing is merged.
- Not merged at the other read of the active list (`0x433218`): that is how the game saves it at exit.
  It writes `Data\list.Spt` (plain text: `ListName`, `PocketAvailable`, `ReverseAvailable`,
  `NumberIslande`, then per track name, folder, ON / OFF, laps) at `0x433183`, switches the script reader
  to sources (`0x4331bb`: `[0x4e714c]` = 0, `[0x4e7148]` = 1; normal is 1 / 0) so that reading list.Spt
  compiles it into LIST.BPT, switches back (`0x433239`) and deletes list.Spt (`0x433252`). The first
  version also merged there: reading list01 in that mode truncated LIST01.BPT, asked for the missing
  list01.spt, and the game's error path looped until the guest heap was exhausted (owner: froze on Exit;
  LIST.BPT and LIST01.BPT left empty, restored from the GOG files). Now the merge runs only after the
  startup read, refuses while the source mode is on, and its own read forces the .BPT mode.
- Verified (E2E, macOS): "all tracks: 8 + 2 from data disk 1: FUN_FAIR SEA_OF_SAND"; the single-race
  picker steps Speed Bay ← Sea of Sand ← Fun Fair (maps, photos, lap counts); a race on Fun Fair
  loads `data\ile10` and runs.
- Dev aid: the E2E key script's `MOUSE:x,y` hover moves the menu cursor (used to reach Continue on
  the track picker, whose keyboard focus is hard to script).
