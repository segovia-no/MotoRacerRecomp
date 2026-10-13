// The single SDL_GPU (Metal) device: owns the window swapchain and is shared by the frame
// presenter (software path) and the Direct3D backend.
#pragma once
#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace host::gpu {

SDL_GPUDevice *device();   // creates the device and claims the game window on first use

// Part of the game frame to show, in game pixels (640x480 space). The game's 16/9 mode draws
// a letterboxed 640x360 band; presenting only that band gives true widescreen.
struct Crop { uint32_t x = 0, y = 0, w = 640, h = 480; };

// Blits `tex` (w x h, a `scale`-times image of the game frame) into the window, showing `crop`
// letterboxed to its own aspect, and submits `cmd`. Consumes `cmd`.
void present(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *tex, uint32_t w, uint32_t h, const Crop &crop);
// Same, for a texture that holds only the game-frame rectangle `placed` (e.g. the 640x360 band
// of the game's 16/9 mode, which it blits to y=60 of the 640x480 screen).
void present_placed(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *tex, uint32_t w, uint32_t h, const Crop &placed,
                    const Crop &crop);

// Uploads an RGBA8 frame and presents it (software renderer path).
void present_rgba(const uint32_t *px, uint32_t w, uint32_t h, const Crop &crop);

// Widescreen detection shared by both renderers: feed one observation per check.
// Returns the crop to use for the current frame.
Crop widescreen_vote(bool bars_black, uint32_t game_w, uint32_t game_h);

void set_game_size(uint32_t w, uint32_t h);   // game frame size behind a scaled texture

// Window coordinates -> game frame coordinates (0..game_w-1), using the last letterbox.
bool window_to_game(float wx, float wy, int *gx, int *gy);

// Render scale for Direct3D for a game frame of `game_h` rows (from settings, or for "auto" the
// smallest multiple whose height covers the display, capped so the target stays <= 8192).
uint32_t render_scale(uint32_t game_w, uint32_t game_h);

}  // namespace host::gpu
