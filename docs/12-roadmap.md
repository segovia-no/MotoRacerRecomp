# Roadmap (owner's list, 2026-10-13)

## Quality of life
- [ ] **In-game options menu** for host settings (render resolution/scale, renderer, fullscreen, filter, vsync) — an overlay drawn by the runtime, so no game code changes.
- [ ] **Navigate the main menu with a controller** — the menus only read keyboard/mouse; map d-pad/stick/A/B to arrow keys, Enter, Escape while in menus.
- [ ] **Triggers as buttons for accelerate/brake** — the game's control setup binds *buttons*, not an analog axis; expose RT/LT as extra joystick buttons (threshold) in addition to the Y axis.

## Features
- [ ] **Widescreen** — the camera/projection is built for 4:3; needs changes in the game's projection code (hybrid decompilation) plus HUD placement.
- [ ] **Linux / Steam Deck port** — runtime is SDL3 + C/C++; needs an x86-64 stack-switch trampoline (arm64 only today), Vulkan/SPIR-V shaders for SDL_GPU (MSL only today), packaging (AppImage/Flatpak), Steam Deck controller defaults.
- [ ] **Windows port** — same as Linux plus D3D12/Vulkan shader formats for SDL_GPU and a Win32 build; the guest Win32 layer stays emulated (no native API passthrough).

## In progress
- [ ] Direct3D stage 3: high-resolution 3D (see `11-d3d-plan.md`).
