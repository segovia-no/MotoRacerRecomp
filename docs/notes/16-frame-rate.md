# Frame rate: the game's timing and higher frame rates

Investigation 2026-10-16 (owner asked for custom and unlimited frame rates).

## How the game times frames
- **Limiter** `0x459fd0`, called once per race frame from `0x47da8f`: busy-waits on `timeGetTime` until
  1000/N ms have passed since the last frame, N = `0x4dc200`. It then returns the average frame rate of
  the last ≤ 24 frames (history at `0x5809f8`). That value (`0x5809f4`) only feeds debug text
  (`0x483f14`, `0x484167`).
- **`-FrameRateMax<n>`** (parsed at `0x414c4f`): values 21..98 set N; **0 turns the limiter off**; other
  values are ignored. Without the argument N = 30 (initial data). GOG's `motoracer.ini` passes
  `-FrameRateMax60`.
- **Race clock / frame time** `0x4120e0`, every race frame:
  - `0x50bbc8` = `timeGetTime()` (via `0x459fa0`/`0x459fc0`, which keep it in `0x4e9cfc`);
  - frame time `dt` = new − old in whole ms → int `0x4e980c`, float `0x4e9808`, clamped to 200 ms;
    **0 ms becomes 30 ms**;
  - smoothed frame time `0x4dc150` += (dt − smoothed) · 0.125 (per-frame exponential average).
  - Replays: when `0x4e9834` = 1 the recorded frame times are used instead (`0x4117c2`; the recorder at
    `0x4578a0` stores the int `0x4e980c`).
- The float frame time has ~34 readers: race motion is scaled by the real frame time (variable timestep).

## Measured
- Player speed: dword at `0x581330 + 12809 · player` (player index `0x508834`), km/h (mph if
  `0x4f0dd0` = 1; the speedometer reads it at `0x484cad`).
- E2E, throttle held on Speed Bay, 60 fps (GOG cap, vsync) vs unlimited (62–82 fps): speed after 2 s
  107 vs 112 km/h, after 4 s 170 vs 172 km/h (difference = sampling phase). **Acceleration does not
  depend on the frame rate.**
- Unlimited on an M3 Pro reached only 62–82 fps at 1920×1080. `sample`: ~70% of the main thread waits
  for the GPU. In 16:9 modes `present_blt` calls `flush(After::Nothing)`, which waits for the command
  buffer to complete (`SDL_WaitForGPUFences`) every frame, so CPU and GPU never overlap.
- Owner, watching the unlimited run on a 60 Hz display: **the camera shook vertically and motion was
  choppy.**

## Frame-rate assumptions found in the game
- Bike code `0x46a880` / `0x46ac1b` (an object at esi, field `+0x2e1d`, clamped to ±1.04 rad; likely
  the lean angle): `atan(−x / smoothed_dt)` and `x / max(smoothed_dt, 20 ms) · k`.
  - It divides by the **smoothed** frame time while motion uses the raw one. With uneven frame times
    (no vsync, 62–82 fps) the two differ every frame, which is a likely cause of the shake.
  - The **20 ms floor** (the 2nd path, when `+0x2e29` = 0) means that above 50 fps the result shrinks
    with the frame time if `x` is a per-frame change: 83% at 60 fps (GOG's default), 35% at 144 fps.
    Not yet confirmed what `x` is.
- `timeGetTime` has 1 ms resolution: at 144 fps frame times alternate 6/7 ms (±7%).

## FrameRate setting (committed 2026-10-16)
`FrameRate=game|display|<n>|unlimited` in `[Recomp]`, also in the in-game Recomp page (without
unlimited). The game's busy-wait limiter is always off (`-FrameRateMax0`; its value is kept as
`game_frame_cap`, 30 without the argument), and the runtime paces frames (`limit_frame_rate` in
host_video.cpp, `SDL_DelayPrecise`), so the rate can change while playing. With vsync on and a target at
or above the display's refresh rate, vsync alone paces (pacing as well gave 59.4 instead of 60 fps).
Frames stay ≥ 1 ms apart (dt = 0 would become 30 ms). `unlimited` turns vsync off (not recommended:
uneven frame times). Dev aids: `MOTORACER_FRAMERATE`, `MOTORACER_VSYNC`, `MOTORACER_FPS_LOG=1`.
Race at the default (60, vsync): 59.5–60.0 fps, same as with the game's own limiter.

## GPU wait removed (2026-10-16)
- `flush()` now waits for the GPU only for a readback (`After::Download`). `present_blt` renders, then
  presents in a second command buffer, and the CPU goes on with the next frame; SDL releases buffers once
  the GPU is done with them (the `After::Present` path never waited).
- Unlimited on the Mac (60 Hz display, windowed): 62–82 → 73–94 fps. The main thread now waits in
  `CAMetalLayer nextDrawable` (~90%); the same ~80 fps at a 640×480 render target, so the remaining limit
  is presentation on a 60 Hz display (drawables come back from the compositor), not GPU work.
- 60 fps races unchanged (E2E: menus, loading, race, HUD correct). The log names the present mode
  (`gpu: ... window claimed, vsync|no vsync`).

## Bike lean fix (2026-10-16)
- Function `0x469c60` (per bike, esi = bike object; `+0x2e1d` = lean, clamped ±1.04 rad) has three lean
  paths: A (`+4`, `+8` and a per-bike pointer set): `atan(−x / smoothed · min(speed, 50) · k)` smoothed
  over frames; C (pointer unset): x / a speed term; B (`0x46ac1b`, flags set, not replaying):
  `x / max(smoothed, 20 ms) · k'` (only `smoothed` when `+0x2e29` is set). Replays take `0x46acf3`.
- Traced in races: only **path B** runs, for the player's bike (esi `0x584421`).
- `x` = a per-frame change: in the same held turn it levels off at 0.75 at 30 fps and 0.40 at 60 fps.
  `0x4e9808` holds the frame time in hundredths of a second (1.6 at 60 fps), `0x4dc150` in ms.
- Because of the 20 ms floor the lean shrinks above 50 fps: at 60 fps (GOG) it is ~85% of the 30 fps
  lean, at ~68 fps measured −0.58 vs −0.68 at 60, at 144 fps it would be ~40% of the 60 fps lean.
- Fix (`game_hook_lean` at `0x46ac41`, before the division): the divisor `[esp+0x10]` becomes
  `max(t, min(20, 1.2·t))`, t = smoothed frame time: the floor scales as it acts at 60 fps, so 60 fps and
  below are unchanged and faster rates lean as at 60 fps.
- Tried first: dividing by this frame's own time (`0x4e980c`). Lean size was right but frame-to-frame
  change rose 0.006 → 0.022 at 60 fps (0.19 with uneven frame times): `x` does not belong to the same
  frame's time. The smoothed time is steadier; even frame pacing keeps both equal.
- Verified (E2E, same held turn, mean lean / mean frame-to-frame change over 350 ms): 60 fps original
  −0.678 / 0.006, fixed −0.677 / 0.006; ~68 fps (paced, no vsync) original −0.577 / 0.007, fixed
  −0.690 / 0.005.

## Open
1. Even frame pacing; test on a high-refresh display (120/144 Hz).

## Stutter above the display's refresh rate (owner report, 2026-10-16)
- Owner's setup: BenQ PD3205U at 60 Hz, `FrameRate=120`, `VSync=0` → very noticeable hiccups.
- Measured (E2E, frame time `0x4e980c` per race frame):
  - 60 fps + vsync: 541 × 16 ms, 1026 × 17 ms, 16 × 18 ms, plus 5–6 frames of 33–34 ms per ~27 s;
  - 120 fps, no vsync, 60 Hz screen: anything from 8 to 35 ms (8/9 runs broken by 13–28 ms frames).
    A 60 Hz panel shows 60 pictures a second; extra frames are shown at uneven ages (plus tearing).
- Fix: the runtime caps the frame rate at the display's refresh rate (`host::display_refresh_rate`,
  logged once: "N fps is above the display's M Hz, capped"); the Recomp page only offers ORIGINAL,
  DISPLAY (M HZ) and rates below M, and shows "M (DISPLAY MAX)" for a higher ini value.
- The occasional 33 ms frame at 60 Hz + vsync is a missed refresh while waiting for a drawable (frame
  breakdown: game 2.7–5.5 ms, submission ~1 ms, `present` 26–30 ms instead of ~12). Not linked to
  pipeline creation (≤ 0.4 ms) or texture uploads; 3 frames in flight instead of SDL's 2 changed nothing
  (6 vs 4 slow frames), so it is the macOS window compositor (runs were windowed). Open.
- Replays store the integer frame time per frame (`0x558aa8`: dt in the top 8 bits, inputs, stick X/Y)
  and are re-simulated from it, so live races must keep integer frame times; sub-millisecond frame
  times would make replays drift.

## Bike pitch steps on slopes (GitHub issue #11, 2026-11-09, owner-confirmed)
- Owner: bikes on slopes do not pitch smoothly, most visible in the cockpit view; suspected a fixed
  physics rate. Not the case: the game runs a variable timestep (above).
- Logged per frame (player bike `0x581218 + 0x3209 · [0x508834]`; `+0xd8` = sin of the pitch, the
  forward row of the orientation `+0xbc`): on a steady climb at 60 fps the pitch saw-tooths,
  0.273 0.283 0.320 | 0.292 0.286 0.335 | 0.322 0.287 0.348 …, 2–3.5 degrees every few frames, with the
  height difference of the front / back wheel points (`+0x1b8` / `+0x33c`). The pitch is the angle between
  those points, each put on the floor of its own track sector (`0x469c60`, `0x46bf80`, floor `0x40c8b0`),
  and the sector floors do not join smoothly: a point crossing a seam (~50 units a frame) steps the pitch.
  Same in the original at 30 fps; the chase camera hides it, the cockpit camera rides on it.
  (`+0x2db8` reads 1 on the ground in these logs, unlike its note in structs.md.)
- Fix (`pitch_smooth.cpp`, `PitchSmoothing`, Recomp menu "SMOOTH BIKE PITCH", on; desktop only): race
  frame GAME_HOOKs before the camera update (`0x47d8bb`; `0x458620` is the camera update), at the frame's
  end (`0x47da8f`, after the draw `[vtable + 0x20]` = `0x47ff90`, before the limiter) and before the
  physics step (`0x47d7c5`). For the view the player's orientation `+0xbc`, lean frame `+0x2e55` and
  forward vector `+0x198` are rotated about their right row to a filtered pitch, then put back, so the
  simulation never sees it. Filter: alpha-beta tracker (critically damped gains from τ = 120 ms and the
  frame time, `MOTORACER_PITCH_TAU` dev), the shown pitch held within 0.05 rad (3 degrees) of the game's,
  jumps above 0.5 rad (respawn) taken at once. Measured (owner's drives, jaggedness = change of the
  per-frame step, p90): raw 0.0163, shown 0.0015 rad.
- Wrong turns: a plain low-pass (τ 70 ms; "improved but not enough"); without the 3-degree cap the tracker
  overshot crests / landings by up to 13 degrees; filtering the height as well gained nothing (its
  per-frame steps follow the uneven frame times on steep slopes, smooth in time); restoring the saved
  values unconditionally at the next physics step put the bike back where it had been after a race
  restart (wrong orientation, once dropped from the sky): a block is now restored only if it still holds
  the value written for the view, and the restore also runs at the frame's end.
- Regression (2026-11-09): race with F6 and a crash / respawn, native check mode (0 mismatches),
  `PitchSmoothing=0`, translator difftest (0 failing), macOS release module and PSP builds.
