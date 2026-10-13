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

## Gamepad (WINMM joystick API) — `runtime/src/win32/winmm_joy.cpp`
- The game polls `joyGetPosEx` with flags `0x483` (X, Y, buttons, centered) for ids 0..7, after `joyGetDevCapsA`. Code at `0x418a3f`.
- It classifies a stick as **digital** if its resting X/Y read exactly `0`, `0x8000` or `0xFFFF`; we report centre `0x7FFF` so pads are analog.
- Mapping: X = left stick X; Y = left stick Y − right trigger + left trigger (RT = up/accelerate, LT = down/brake); buttons 1–8 = A, B, X, Y, LB, RB, Back, Start; POV = d-pad. 4000 dead zone on sticks.
- Any SDL-recognised controller (Xbox, PlayStation, Switch Pro, 8BitDo…), hot-plug supported; connected pads take ids 0..n.

## Mixer volumes, triggers, controller menus (2026-10-13)
- **Volumes:** `winmm_mixer.cpp` exposes one mixer: Speakers/Master (control 1 → SDL device gain), Wave (2 → DirectSound mix), CD Audio (3 → music samples). The game reads the controls at startup, then restores its saved levels via `mixerSetControlDetails` (`0x470d22`: `MulDiv(vol, max-min, scaleMax) + min`). Configs saved while the mixer reported "Not available" contain 0 for all three → silence until raised in Options (seen once during development).
- **Audio drives animation timing:** with the device gain at 0 the 3D menu animations ran much too fast — the game paces some animations from audio progress. Keep this in mind when touching audio.
- **Triggers:** buttons 9 (RT) / 10 (LT) past half travel; the pad reports 10 buttons. They no longer affect the Y axis. D-pad steers.
- **Controller menus:** D-pad/left stick → arrow keys (auto-repeat 400/120 ms), A → Enter, B → Escape, Start → Escape always; suppressed while the game polled the joystick in the last 300 ms (races). `ControllerMenus=0` disables it.

### How the game reads the joystick in a race (`0x411b30`)
- `0x431190(n)` returns the action bound to joystick input `n`: `-2/-3` = X left/right, `-4/-5` = Y up/down (corrected 2026-10-14 from the factory and owner bindings), `0..k` = buttons. Actions set bits in a control mask (1, 2, 4, 8, 0x10, 0x20) and some invert the axis value (`0xFFFF - v`) so the analog strength always points the same way.
- If the global `0x4dc11c == 2` (analog joystick mode) X/Y stay analog (X clamped per steering state); otherwise both are forced to centre `0x8000` (digital JOYPAD mode).
- Therefore analog throttle/brake = triggers on the Y axis, bound in the game's config to "Y up"/"Y down".
- The config screen takes whichever input it notices first, so triggers must not be buttons *and* axis at once. `TriggerMode=auto` (default) reads the mode global: 2 → triggers add `LT − RT` to Y; otherwise → buttons 9/10. `axis` / `buttons` force one.

**Confirmed in play (2026-10-13):** `0x4dc11c` = 1 for the digital JOYPAD mode, 2 for the analog joystick mode. In mode 2 the game's control setup registers the triggers as "Y up"/"Y down" and races get analog throttle/brake (owner-confirmed).

## Control bindings and the Xbox preset (2026-10-14)
- Bindings: 7 × `{keyboard VK, joystick input}` at `0x53eae0`, in the order of the controls screen
  (Options › CONTROLLER › config): Accelerate, Brake, Turn Left, Turn Right, Higher Gear, Lower Gear,
  Turbo/Stunts. Factory: keys ↑ ↓ ← → A Z Space; joystick 0, 1, −2, −3, 2, 3, none.
- Options keys: `InputDevice` → `0x4dc11c` (CONTROLLER: 1 when no joystick / keyboard, 2 = JOYSTICK,
  4 = joypad type), `JoyType` → `0x4dc120`. Startup input init `0x47310b` → `0x4311c0` initialises the
  joystick (`0x4189c0`) for mode 2 and falls back to 1 if none answers. The options are saved to
  `data/CONFIG.BPT` when the game exits normally (also without visiting Options).
- Runtime (`game_hooks.cpp`, `ControllerPreset=1`):
  - first run (no `Defaults=1` in recomp_state.txt) with a gamepad connected: hook `0x42b37d` (right
    after `InputDevice` is read, before the startup input init) sets CONTROLLER = JOYSTICK;
  - whenever CONTROLLER is JOYSTICK and the joystick bindings are still the factory ones, they become
    an Xbox 360 layout: Accelerate = Y up (RT, analog), Brake = Y down (LT), steering = left stick X,
    Higher Gear = RB (button 5), Lower Gear = LB (4), Turbo/Stunts = A (0). Start already pauses
    (sent as Escape). Custom bindings are never overwritten.
- SDL's gamepad database maps Xbox, PlayStation, Switch Pro, 8BitDo and most pads to the same
  positional layout, so one preset fits all of them (on Nintendo-layout pads the bottom face
  button, labelled B, is "A").
