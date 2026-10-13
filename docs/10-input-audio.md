# Phase 5 — Input and Audio

## CD audio (MCI `cdaudio`) — `runtime/src/win32/winmm_cd.cpp`
**Problem found:** in the menu the game spun forever trying to open the CD audio device: ~590k `mciSendCommandA(MCI_OPEN)` failures, 197k `MessageBoxA` calls, and `data/delphine.err` grew to 1 MB. With no CD device it never got past this (the "CD check").

**What the game does with the CD** (from the MCI log):
1. `MCI_OPEN` type id 516 (`MCI_DEVTYPE_CD_AUDIO`), element `"c:"` — the drive letter comes from its install path (empty registry `DataPath` → current drive), flags `TYPE|TYPE_ID|ELEMENT|SHAREABLE|WAIT`.
2. `MCI_STATUS` mode, number of tracks (13), track type (`0x4001` → `MCI_CDA_TRACK_AUDIO`), length of the track it wants.
3. `MCI_PLAY` with `NOTIFY|FROM|TO` (track 13 jingle, then track 3 for the menu), then polls mode.

**Emulation** (same idea as GOG's `winmm.dll` wrapper — no game code is patched):
- Virtual disc answers on any drive letter. Layout in CD frames (75/s): 150-frame lead-in, track 1 = data (nominal 25 min), 150-frame pregap, tracks 2–13 from `02.wav`…`13.wav` (44.1 kHz stereo 16-bit; 2352 bytes = 1 frame, so positions map exactly to file offsets).
- Time formats ms / MSF / TMSF; status items length, position (+start), track count, mode, media present, time format, ready, current track, track type.
- Play from/to across track boundaries, stop/pause/resume/seek/close; `MM_MCINOTIFY` (`SUCCESSFUL`, `SUPERSEDED`, `ABORTED`) posted from the message pump.
- Every MCI error, open and play is logged; the first 40 status queries too.

Result: menu music plays (confirmed by project owner); the retry loop is gone.

## Host audio — `runtime/src/host_audio.cpp`
One SDL3 playback device (44.1 kHz S16 stereo requested). Each sound source is an `SDL_AudioStream` bound to the device (SDL mixes) and fed by a pull callback on SDL's audio thread.

## DirectSound — `runtime/src/dx/dsound.cpp`
- `IDirectSound` + `IDirectSoundBuffer` (21 methods). 3D sound QI is refused.
- Secondary buffer samples live in guest memory; `Lock` returns guest pointers (with wrap-around second region).
- A buffer gets its own SDL stream on first `Play`: 8/16-bit mono/stereo → float stereo, DirectSound volume (hundredths of dB) and pan applied per sample, `SetFrequency` → stream frequency ratio (engine pitch).
- Looping, `GetStatus`, `GetCurrentPosition` (write cursor = play + ~10 ms), `DuplicateSoundBuffer` (shares sample memory, independent play state).
- Thread safety: one mutex for buffer state; release stops under the lock but destroys the stream outside it (SDL waits for an in-flight callback that takes the same lock).
- Observed: ~20 sound-effect buffers, 16-bit mono, 5–22 kHz, flags `CTRLFREQUENCY|CTRLPAN|CTRLVOLUME`.

## Input
- **Keyboard:** SDL scancodes → VK codes + PC set-1 scan codes in `WM_KEYDOWN/UP` lParam; `GetKeyState`. Works in menus (confirmed).
- **Mouse:** SDL motion/buttons → `WM_MOUSEMOVE`, `WM_L/RBUTTONDOWN/UP` with `MK_*` flags, coordinates converted to the 640×480 frame buffer through the renderer's logical presentation (letterbox + Retina aware); `GetCursorPos` uses the same coordinates.
- **DirectInput:** not loaded in the menus. `LoadLibrary("DINPUT.DLL")` + `GetProcAddress("DirectInputCreateA")` will need a real implementation if races use it.
- **Joystick:** `joyGetDevCapsA` reports unplugged. TODO: SDL gamepad → WINMM joystick API.

## Validation session (2026-10-13)
Owner played through menus (mouse + keyboard) and a race (keyboard): sound effects, music and visuals correct. Session log: 0 `FATAL`, 0 unimplemented imports, clean exit (code 0). Only repeated events were the per-screen DirectDraw re-initialisation (`SetDisplayMode(640,480,16)` + refused `IDirect3D` QI).
