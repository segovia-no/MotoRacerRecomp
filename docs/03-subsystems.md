# Engine Subsystems

## Original source modules (from assert/debug paths in binary)
Build root `D:\Dsed\mr320\` ("mr320" = Moto Racer 3.20):
- `animDecor.cpp` — animated scenery
- `event.cpp` — events
- `frame3d.cpp` — 3D frame
- `player.cpp` — player/bike
- `Dsi_d3d.cpp` — Direct3D backend
- `screen3d/screen3d.cpp`, `screen3d/dd.cpp` (DirectDraw), `screen3d/bankw.Cpp` (bank loader)
- `menu/menu.cpp`, `menu/menuTools.cpp`, `menu/bitmap.cpp`, `menu/pageMainMenu.cpp`, `menu/pageProtocol.cpp` (network protocol page)

## Rendering paths
1. **Software** renderer → 16-bit DirectDraw surface (`-DoRealLock`, palettes `-Pal16`/`-NoPal256`, MMX optional `-NoMMX`).
2. **Direct3D** (DX5, `-D3D`, `-ZBuffer`, `-Alpha` = ARGB4444 textures, texture size limits).
3. **3DFX Glide** (`-3DFX`).
4. PowerVR special-casing (`-PowerVr`, `-NoPowerVr`).

## Command-line flags (complete list from strings)
Rendering: `-D3D -NoD3D -3DFX -ZBuffer -InfiniteZBuffer -ScanOtReverse -Alpha -Pal16 -NoPal256 -NoDither -NoGouraud -NoFlick -TextureSquareOnly -CorrectRatioTextures -NoTextureSizeMax256 -Min/MaxTextureWidth/Height<n> -TextureVideoMem<n> -TotalVideoMem<n> -OpaqueBlackLevel<1..3> -PowerVr -NoPowerVr -NoAGP -OnlyPrimaryDisplay -NoDuplicateSurfaces`
DirectDraw: `-DoRealLock -AskDirectDraw -BlitTimeOut<n> -DontWaitBlits -NoBlitAsync`
System: `-NoSound -NoMMX -NoCpuDetect -ThreadHighest -ThreadTimeCritical -FrameRateMax<n> -SnapShot`
Game: `-NbComp<n>` (AI count), `-Modem`, `-DataV2`
Undocumented (TBD): `-BMG -HON -T5P -DKb -PKb -XOb -XWb` (likely input/keyboard/joystick or publisher modes; BMG = publisher demo set matching `BMG.dem`).

## Input
- DirectInput (dynamic), WinMM joystick (`joyGetPosEx`), keyboard.

## Audio
- DirectSound for SFX; MCI CD audio for music.

## Network
- DirectPlay (DPLAYX ordinals) — modem/IPX/TCP multiplayer.
