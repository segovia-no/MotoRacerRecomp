# Roadmap (owner's list, 2026-10-13)

## Quality of life
- [ ] **In-game options menu** for host settings (render resolution/scale, renderer, fullscreen, filter, vsync) — an overlay drawn by the runtime, so no game code changes.
- [x] **Navigate the main menu with a controller** — the menus only read keyboard/mouse; map d-pad/stick/A/B to arrow keys, Enter, Escape while in menus.
- [x] **Triggers for accelerate/brake** — analog via the Y axis in the game's analog joystick mode, buttons 9/10 in JOYPAD mode (`TriggerMode=auto`).
- [ ] ~~Triggers as buttons for accelerate/brake~~ (superseded) — the game's control setup binds *buttons*, not an analog axis; expose RT/LT as extra joystick buttons (threshold) in addition to the Y axis.

## Features
- [x] **Widescreen (first step)** — the game has its own **16/9 MODE** (Options menu): a letterboxed 640x360 band with a *wider* field of view and the HUD moved into the band. `Widescreen=auto` detects the pure-black bands (software: every frame on the guest frame; D3D: every 10th frame via a downscaled band readback) and presents only the band, filling a 16:9 window — at RenderScale 4 that is 2560x1440. The game keeps the D3D viewport at 640x480 and draws full-height geometry, painting the bars over it, so detection is pixel-based.
- [ ] **Custom resolutions / native 16:9 rendering** — build on the above: render the band directly at the window's resolution instead of a multiple of 640x480; later, adjust the projection for arbitrary aspect ratios.
- [x] **In-game volume sliders** — Options shows MASTER/WAVE/CD AUDIO VOLUME "Not available" because the WINMM mixer API reports no device; wire it to our music/SFX gains.
- [ ] **Linux / Steam Deck port** — runtime is SDL3 + C/C++; needs an x86-64 stack-switch trampoline (arm64 only today), Vulkan/SPIR-V shaders for SDL_GPU (MSL only today), packaging (AppImage/Flatpak), Steam Deck controller defaults.
- [ ] **Windows port** — same as Linux plus D3D12/Vulkan shader formats for SDL_GPU and a Win32 build; the guest Win32 layer stays emulated (no native API passthrough).

## In progress
- [x] Direct3D stage 3: high-resolution 3D (see `11-d3d-plan.md`).

