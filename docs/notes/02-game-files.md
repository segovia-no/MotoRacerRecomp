# Game Files & Formats

Root: `extracted/app/`. Total ~443 MB (mostly WAV CD tracks + BKF banks).

## Audio
- `02.wav`..`13.wav` — CD audio tracks (GOG rip). Played via MCI → GOG `winmm.dll` proxy.
- `data/SFX.BKF` — sound effects bank (6.5 MB).

## BKF — "bank file" archives (status: header identified, compression TBD)
Example `GENERIC.BKF`:
```
13 00 00 00            u32 entry count? (0x13 = 19)
"DSI2.GEN\0..."        entry name, then fields...
...
"END.GEN"
"DSI3"...
```
- Entry names like `DSI2.GEN`, `END.GEN` → named chunks. Payload appears compressed (high-entropy).
- Loader code: `D:\Dsed\mr320\screen3d\bankw.Cpp` — reverse this to decode.
- Per-track banks `ILE01..ILE11.BKF` (~5 MB each; "île" = island → track sets), bikes `KTM*`, `SPEED*`, menus `MENU*.BKF` (per language), `FONTES.BKF`, `PAGES.BKF`, `PODIUM.BKF`, `DEMO.BKF`, `DESIGN.BKF`.

## BPT — small parameter/table files (status: encrypted/compressed, TBD)
- `CHAMP*.BPT` (championships), `MOTO*.BPT` (bike stats), `PKT*.BPT` (pocket bikes), `LIST*.BPT`, `CONFIG.BPT`, `FONTS.BPT`.
- First u32 sometimes small (e.g. `CONFIG.BPT` starts `03 00 00 00`), rest high-entropy → likely XOR/obfuscated.

## TRK — track geometry/path (status: plain binary)
- Magic `"TRK\0"`, then u32 `5`, u32 count (`0x314`), followed by float32 vectors (`0x3f800000` = 1.0 visible). Likely spline/checkpoint data.
- `data/ile01/ile01.trk`, `ile03/Track03.trk`, `ile06/Track06.trk`, `ile09/bercy.trk`, ...

## DEM — demo playlist (plain text)
- `data/demo/EA.dem`, `BMG.dem`: `NbItems: N` then `DemoN:\t<TRACK_NAME>\t<duration ms>\t<bitmap path .lez>`.
- `.lez` → another compressed image format (TBD).

## Misc
- `data/config.txt` → `-us` (language code).
- `data/bestscor.dat` — high scores (293 KB?! probably includes ghost/replay data, TBD).
- `data/delphine.err` — error log written by game.
