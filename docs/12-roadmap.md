# Roadmap (owner's list, 2026-10-13)

## Quality of life
- [ ] **In-game options menu** for host settings (render resolution/scale, renderer, fullscreen, filter, vsync) — an overlay drawn by the runtime, so no game code changes.
- [x] **Navigate the main menu with a controller** — the menus only read keyboard/mouse; map d-pad/stick/A/B to arrow keys, Enter, Escape while in menus.
- [x] **Triggers for accelerate/brake** — analog via the Y axis in the game's analog joystick mode, buttons 9/10 in JOYPAD mode (`TriggerMode=auto`).
- [x] **Draw distance / LOD** — `DrawDistance=2`: 20 track sections ahead instead of 10, opponent bikes drawn twice as far, bike models/sprites switching 6× further out. See 14-draw-distance.md.
- [ ] ~~Triggers as buttons for accelerate/brake~~ (superseded) — the game's control setup binds *buttons*, not an analog axis; expose RT/LT as extra joystick buttons (threshold) in addition to the Y axis.

## Known issues
- [x] **Software renderer: main-menu background animation too fast** (owner, 2026-10-14) — the software main menu presents with a full-screen Blt offscreen → primary every loop instead of a Flip; the Blt returned at once, so the loop (and its per-loop checker animation) ran ~4000 times/s. Full-screen Blts to the primary now present immediately and are paced to 60 Hz (`pace_frame` in ddraw.cpp). Measured 897 per 15 s; owner-confirmed.

## Features
- [x] **Widescreen (first step)** — the game has its own **16/9 MODE** (Options menu): a letterboxed 640x360 band with a *wider* field of view and the HUD moved into the band. `Widescreen=auto` detects the pure-black bands (software: every frame on the guest frame; D3D: every 10th frame via a downscaled band readback) and presents only the band, filling a 16:9 window — at RenderScale 4 that is 2560x1440. The game keeps the D3D viewport at 640x480 and draws full-height geometry, painting the bars over it, so detection is pixel-based.
- [x] **Custom resolutions in the game's own Options › SCREEN MODE** (D3D renderer): 4:3 up to 1920×1440, and with 16/9 MODE on 1280×720 / 1920×1080 / 2560×1440 / 3840×2160 rendered natively; remembered across restarts. See 11-d3d-plan.md.
- [x] **HUD scaling at high resolutions** — race HUD enlarged by screen width / 640 about its screen edges (`HudScale=auto|factor`). See 11-d3d-plan.md.
- [ ] **Custom resolutions / native 16:9 rendering** — build on the above: render the band directly at the window's resolution instead of a multiple of 640x480; later, adjust the projection for arbitrary aspect ratios.
- [x] **In-game volume sliders** — Options shows MASTER/WAVE/CD AUDIO VOLUME "Not available" because the WINMM mixer API reports no device; wire it to our music/SFX gains.
- [x] **Linux / Steam Deck port**: verified on a Steam Deck 2026-10-14; Steam Input template still set by hand — runtime is SDL3 + C/C++; needs an x86-64 stack-switch trampoline (arm64 only today), Vulkan/SPIR-V shaders for SDL_GPU (MSL only today), packaging (AppImage/Flatpak), Steam Deck controller defaults. Plan: 15-ports-plan.md.
- [ ] **Windows port** — same as Linux plus D3D12/Vulkan shader formats for SDL_GPU and a Win32 build; the guest Win32 layer stays emulated (no native API passthrough). Plan: 15-ports-plan.md.

## In progress
- [x] Direct3D stage 3: high-resolution 3D (see `11-d3d-plan.md`).

