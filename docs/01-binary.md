# game.EXE — Binary Facts

## Header
- PE32, i386, GUI subsystem. ImageBase `0x400000`, entry `0x4b6420`.
- Linker version 4.20 → **MSVC 4.2** toolchain (1996–97 era).
- Relocations present (`.reloc`, 95 KB) → image could be rebased; also gives a full list of absolute pointers in code/data (useful for recomp: distinguishes pointers from constants).

## Segments (from REA/Hopper)
| Segment | Start | End | Size |
|---|---|---|---|
| .text | 0x401000 | 0x4cf548 | 845,128 |
| .rdata | 0x4d0000 | 0x4dad20 | 44,320 |
| .data | 0x4db000 | 0x70c568 | 2,299,240 (mostly BSS) |
| .idata | 0x70d000 | 0x70edac | 7,596 |
| .rsrc | 0x70f000 | 0x716748 | 30,536 |
| .reloc | 0x717000 | 0x72e622 | 95,778 |

## Statically linked libraries (inferred from imports/strings)
- **MSVC CRT** (heap, locale, stdio, `RtlUnwind`, SEH).
- **MFC 4.x** (static): menus, dialogs, `CTL3D32`, `WINSPOOL`, `comdlg32`, hook/property APIs. Most of the 9219 procedures are likely CRT/MFC, not game code. → identify with FLIRT-style signatures and replace, don't translate.

## Imports (summary)
- `DDRAW`: `DirectDrawCreate`, `DirectDrawEnumerateA` (Direct3D reached via `QueryInterface` on DirectDraw — DX5 era, `Dsi_d3d.cpp`).
- `DSOUND`: `DirectSoundCreate`.
- `DINPUT.DLL`: loaded dynamically (`LoadLibrary` + `DirectInputCreate`).
- `DPLAYX`: ordinals 1, 2 (DirectPlay — network/modem multiplayer).
- `WINMM`: MCI (CD audio), joystick (`joyGetPosEx`), mixer, `timeGetTime`.
- 3DFX Glide: referenced by `-3DFX` flag; loaded dynamically (TBD: confirm DLL name).
- Standard KERNEL32/USER32/GDI32/ADVAPI32 (registry), COMCTL32.

## Resources
- 13 cursors, 1 bitmap, icon group, 7 dialogs (French LANGID 1036), 2 string tables, version info.

## GOG additions (not original)
- `winmm.dll` (2010) — proxy that redirects CD-audio MCI calls to `02.wav`..`13.wav` (CD tracks 2–13).
- `goggame-*.dll`, `GameuxInstallHelper.dll` — GOG Galaxy / Windows Game Explorer, irrelevant.
- `motoracer.ini` — launcher settings; passes `-DoRealLock -OpaqueBlackLevel3 -TextureSquareOnly -Pal16 -CorrectRatioTextures -NoDither -TextureVideoMem268435456 -FrameRateMax60`.
