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
- Superseded 2026-11-13 ("Same game at any frame rate" below): the lean's input itself depends on the
  frame rate through the per-frame grip; this divisor restored only the lean's size, and was not applied
  by the native `0x469c60` (commit 20a6896 on).

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

## Same game at any frame rate (2026-11-13, owner's goal; reference 60 fps, 30 fps since 2026-11-16)
Owner: "the objective is making the game consistent on any fps value, respecting the original
developers intention. exclude psp from this"; reference rate chosen by the owner: 60 fps (GOG).
Found while chasing GitHub issue #14 on the owner's Steam Deck (native Linux build): at 125–250 fps
animations ran fast, the bike leaned "very very fast", opponents stopped in the middle of the track.

### Method
- `MOTORACER_DISPLAY_HZ=144` (dev, `host_video.cpp`): the display refresh rate the runtime assumes, so
  `MOTORACER_FRAMERATE=display MOTORACER_VSYNC=0` paces a steady 144 fps on a 60 Hz screen.
- `MOTORACER_FRAME_TRACE=1|2` (dev, `game/dev_trace.cpp`): race motion every 500 ms (frame times,
  player lean, opponents' sectors / speeds, wall hits), opponents that stop ("stall", 1.5 s in one
  sector) or fall; `=2` adds per-frame lines of the player bike (lean, roll lag, heading, steering, the
  native lean's inputs).
- Reference runs need `MOTORACER_VSYNC=0` with an explicit `MOTORACER_FRAMERATE`: on the Deck with its
  screen idle the compositor throttled vsync'd windows to 20 fps (50 ms frames), which invalidated a
  first round of "60 fps" runs.
- Steering test: no opponents (`MOTORACER_OPPONENTS=0`), throttle, a short LEFT tap on Speed Bay's
  start straight; runs aligned on the steering value crossing −50; compared: facing (forward row of
  `+0xbc`), travel direction `+0x18c`, lean `+0x2e1d`, the native lean's yaw.

### What depends on the frame rate (the original game)
- Acceleration does not (speed curves equal at 30 / 60 / 144).
- The steering input (`0x466240`: rate `+0x10c`, steering `+0x110`) and the turn it applies
  (`0x46661a`: yaw = factor · turn rate · dt · steering) scale with dt: equal at all rates.
- Grip (`0x48d620`, physics object at bike `+0x24`, on the ground): the travel direction `+0x168`
  (= bike `+0x18c`) becomes g · itself + (1 − g) · facing every frame, g = 0.85..0.9 by speed. The lag
  of the travel direction behind the facing therefore shrinks with the frame rate: 0.51 / 0.43 / 0.31 /
  0.15 rad 250 ms after the same tap at 20 / 30 / 60 / 144 fps.
- The player's lean (`0x469c60`, path at `0x46ac1b`) = yaw (twice that lag) / max(smoothed frame time,
  20 ms) · k (without the floor for analog steering, `+0x2e29`): 0.39 / 0.53 / 0.53 / 0.22 at the same
  rates. Above 60 fps the lean shrank and died within 0.5 s; the 2026-10-16 fix (divisor 1.2 t) only
  restored its size, and since commit 20a6896 (native `0x469c60`) it was not even applied (natives run
  by default and did not call `game_hook_lean`).
- Roll and bounce (`0x466a80`, every bike): the drawn roll `+0x2ea1` moves 1 / [model − 0x18] of the way
  to its target every frame (the model is drawn turned by the lag `+0x2e8d`); the landing bounce damps
  per frame (`+0x2e9d *= [model + 0x24]`), its phase advances per frame (`+0x2e99 += +0x2e9d /
  [model + 0x2c]`, drawn as `+0x2e91` = sin · amplitude), its speed `+0x2e95` steps by clamp(dt, 3, 5)
  hundredths per frame.
- Per frame as well: AI speed blend (`0x46b120`: speed = speed · b + L · (1 − b)); an AI bike behind a
  slower one it follows (`0x46cd20`, `0x46d13e`: speed = target = 0.95 · (e + (o − e) / 32)); wall hits
  (AI / second player ×0.5, `0x46bf80` / `0x45e940`; player ×0.75, `0x4692b3`); a crashed rider
  sliding (×0.9, `0x4683e0`); the AI lean smoothing (×0.5, `0x46c729`); the player's lean target offset
  `+0x2e39` (±0.01, `0x45d610`) and the dust emitter's offset `+0x188` (+2 / −1, `0x467546`; reset to 1
  or 5 with random offsets at `0x467237`, `0x466d70`).
- Opponents: one opponent alone drives identically at 30, 60 and 144 fps (sector and speed equal at every
  check to 40 s), so the AI's own driving does not depend on the frame rate. Stops in the middle of the
  track are queues behind crashed opponents, at 60 fps too (first suspected: pairs braking each other in
  0x46cd20; with falls logged, every stall followed a crash). With 6 opponents, mean progress after
  42 s: 637 / 641 sectors (two 60 fps runs), 631 (144 original), 638 (144 fixed).

### Fix (`game/frame_rate.cpp`, GAME_HOOKs in translate.py plus the same calls in the natives)
(As first made, with 60 fps as the reference; since 2026-11-16 the reference is 30 fps, below.)
Each per-frame step is applied as n 60 fps steps, n = frame time / 16.67 ms, from the game's integer
frame time `0x4e980c` (replays record and replay it, so they stay deterministic): factors become
factor^n, affine steps x ← a x + b their n-fold closed form, increments n-fold. Frames of 16 and 17 ms
keep the game's own arithmetic (60 fps unchanged); slower frame rates are corrected as well. With the
grip fixed, the lean divisor keeps its 60 fps value (20 ms, or 16.67 ms with analog steering).
The dirt bikes' lean (`0x46a893`) divides the yaw by the smoothed frame time without a floor: with the
grip fixed it uses one 60 fps frame. `MOTORACER_FPS_FIX=0` (dev) turns everything off, a mask of the
`FIX_` bits picks single fixes.

### Verified (Deck, Speed Bay)
- Steering tap, 150 / 250 ms after the tap, yaw and lean: 60 fps original 0.20 / 0.20, −0.30 / −0.30;
  144 fixed 0.19 / 0.19, −0.28 / −0.29; 30 fixed 0.20 / 0.22, −0.30 / −0.34; 144 original 0.12 / 0.08,
  −0.18 / −0.12.
- 240 fps fixed: 0.18 / 0.18, −0.27 / −0.28 (original 0.08 / 0.04, −0.11 / −0.06). The rest of the
  difference to 60 fps is the input: a 60 ms tap is held for whole frames (67 ms at 60 fps) and the
  steering grows with the square of the hold time.
- Uncapped (~600 fps, frames of 1–2 ms): fixed 0.15 / 0.15, −0.23 / −0.22 (original 0.04 / 0.01,
  −0.06 / −0.02): still short; not offered by the Recomp menu.
- Dirt Arena (dirt bikes), lean every 100 ms after the tap: 60 fps −0.21 −0.32 −0.25 −0.15 −0.08;
  144 fixed −0.16 −0.27 −0.22 −0.13 −0.07; 144 with the grip fix but not the dirt lean −0.36 −0.59 −0.50;
  144 original −0.26 −0.32 −0.15 −0.04.
- Translated code only (`MOTORACER_NATIVE=0`) gives the same curves as the natives (both tracks), and
  native check mode at 200 ms frames (n = 12 in every fix) found 0 mismatches.
- With no player input the race left for the game's demo after ~44 s (the trace stops: replay).

### Reference 30 fps; takeoffs (2026-11-16, owner)
- Owner: on Dirt Arena the bikes "jump most of the time on slopes" on the PSP (the PSP port, which runs
  at 30 fps or less), almost never at 120 fps; the PSP feel is the faithful one. The game's own limiter
  defaults to 30 fps (N = 30 without `-FrameRateMax`; GOG's ini passes 60). The frame-rate fixes were not
  the cause: same takeoffs with them on and off.
- Measured, player's bike on Dirt Arena (`MOTORACER_FRAME_TRACE=1`, owner driving): 120 fps 4.1 takeoffs
  a minute, 5 % of the time in the air (fixes on or off alike); 30 fps 15.5 a minute, 22 %. Opponents
  (23, scripted runs, ~45 s): 33 takeoffs at 60 fps, 41 at 120 with or without the fixes.
- Takeoff (player, `0x468ca0`): the bike keeps contact, and is put back on the road (`pos.y` = floor),
  while its height is at most a tolerance above the floor (`0x469427`: 32 units, 8 when the function's
  argument is set); else `+0x2db8` = 0. `0x466660`: without contact the counter `+0xf0` grows by 2.592
  ([`0x4d0f20`]) every frame, contact resets it to 0; the physics `0x48d620` (physics object = bike
  `+0x24`, so `+0xcc` = bike `+0xf0`) takes its air branch (gravity, no grip) when it is above 2.592
  ([`0x4d1c18`]), i.e. on the second frame without contact. A frame's step lifts the bike off a crest or
  a slope's edge in proportion to its length, so at 120 fps it stays under the tolerance and is put back
  on the road every 8 ms. On the ground the travel direction's vertical part is the facing's (`0x48da83`).
- Fix: the reference is 30 fps (`REF_MS` = 33.3; frames of 33 / 34 ms keep the game's arithmetic, the
  lean divisor is one 30 fps frame, the bounce step's dt clamp uses 3.33); `FIX_GROUND` (GAME_HOOKs
  `0x46943c`, tolerance × n, rounded, at least 1; `0x466688`, the counter's step × n). Owner, 120 fps:
  21.3 takeoffs a minute, 23 % in the air; "the jumps feel super great".
- Open: owner: movement at 120 fps with these fixes is "erratic, jerky" (more small hops: 21 takeoffs a
  minute against 15.5). Next (owner's go): a fixed 30 Hz logic step with the drawing interpolated, which
  would make every per-frame rule the original's by construction. Opponents' takeoffs (`+0x2e85`, their
  own path) are not scaled.
- `dev_trace.cpp`: the bike loop started one record late (`0x584421`) and its last pass read a stale
  record past the bikes, which could have +8 set (taken for a second player: garbage speed in earlier
  traces); bikes are records 1..count, the player is found by its index `[0x508834]`.

