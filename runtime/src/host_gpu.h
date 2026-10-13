// The single SDL_GPU (Metal) device: owns the window swapchain and is shared by the frame
// presenter (software path) and the Direct3D backend.
#pragma once
#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace host::gpu {

SDL_GPUDevice *device();   // creates the device and claims the game window on first use

// Blits `tex` (w x h, the game's 4:3 frame) into the window, letterboxed per settings, and
// submits `cmd` (acquired by the caller, may already contain work). Consumes `cmd`.
void present(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *tex, uint32_t w, uint32_t h);

// Uploads an RGBA8 frame and presents it (software renderer path).
void present_rgba(const uint32_t *px, uint32_t w, uint32_t h);

// Window coordinates -> game frame coordinates (0..game_w-1), using the last letterbox.
bool window_to_game(float wx, float wy, int *gx, int *gy);

// Render scale for Direct3D (from settings, or derived from the window size for "auto").
uint32_t render_scale();

}  // namespace host::gpu
