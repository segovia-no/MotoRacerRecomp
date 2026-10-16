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

## Prototype (not committed)
`FrameRate=game|display|<n>|unlimited` in `[Recomp]`: removes the game's cap (`-FrameRateMax0`),
paces frames in the runtime (`limit_frame_rate` in host_video.cpp, `SDL_DelayPrecise`), keeps frames ≥ 1 ms
apart (dt = 0 would become 30 ms), `unlimited` turns vsync off. `MOTORACER_FRAMERATE` and
`MOTORACER_FPS_LOG=1` are dev aids.

## GPU wait removed (2026-10-16)
- `flush()` now waits for the GPU only for a readback (`After::Download`). `present_blt` renders, then
  presents in a second command buffer, and the CPU goes on with the next frame; SDL releases buffers once
  the GPU is done with them (the `After::Present` path never waited).
- Unlimited on the Mac (60 Hz display, windowed): 62–82 → 73–94 fps. The main thread now waits in
  `CAMetalLayer nextDrawable` (~90%); the same ~80 fps at a 640×480 render target, so the remaining limit
  is presentation on a 60 Hz display (drawables come back from the compositor), not GPU work.
- 60 fps races unchanged (E2E: menus, loading, race, HUD correct). The log names the present mode
  (`gpu: ... window claimed, vsync|no vsync`).

## Open
1. Find what `x` is in `0x46a880`/`0x46ac1b`; correct the smoothed-dt use and the 20 ms floor for high
   frame rates (replays take another path there).
2. Even frame pacing; test on a high-refresh display (120/144 Hz).
