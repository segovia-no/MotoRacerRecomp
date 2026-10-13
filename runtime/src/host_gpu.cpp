#include "host_gpu.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#include "runtime.h"
#include "settings.h"

using namespace rt;

namespace host {
SDL_Window *ensure_window();
}

namespace host::gpu {

namespace {
SDL_GPUDevice *dev = nullptr;
SDL_Window *win = nullptr;
SDL_GPUTexture *frame_tex = nullptr;
uint32_t frame_w = 0, frame_h = 0;
SDL_GPUTransferBuffer *frame_tb = nullptr;
// last letterbox (window points) and game frame size, for mouse mapping
float lb_x = 0, lb_y = 0, lb_w = 0, lb_h = 0;
uint32_t game_w = 640, game_h = 480;
}  // namespace

SDL_GPUDevice *device() {
    if (dev) return dev;
    win = ensure_window();
    dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    if (!dev) fatal("SDL_CreateGPUDevice failed: %s", SDL_GetError());
    if (!SDL_ClaimWindowForGPUDevice(dev, win)) fatal("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
    if (!settings::get().vsync && SDL_WindowSupportsGPUPresentMode(dev, win, SDL_GPU_PRESENTMODE_IMMEDIATE))
        mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
    SDL_SetGPUSwapchainParameters(dev, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
    log("gpu: %s device, window claimed\n", SDL_GetGPUDeviceDriver(dev));
    return dev;
}

uint32_t render_scale() {
    const auto &cfg = settings::get();
    if (cfg.render_scale > 0) return static_cast<uint32_t>(std::clamp(cfg.render_scale, 1, 8));
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(ensure_window(), &pw, &ph);
    // "auto": smallest scale whose 4:3 image covers the window height
    SDL_DisplayID d = SDL_GetDisplayForWindow(ensure_window());
    const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : nullptr;
    float density = SDL_GetWindowPixelDensity(ensure_window());
    int target_h = std::max(ph, m ? static_cast<int>(m->h * (density > 0 ? density : 1.f)) : ph);
    return static_cast<uint32_t>(std::clamp((target_h + 479) / 480, 1, 4));
}

void present(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *tex, uint32_t w, uint32_t h) {
    device();
    SDL_GPUTexture *swap = nullptr;
    uint32_t sw = 0, sh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &swap, &sw, &sh) || !swap) {
        SDL_SubmitGPUCommandBuffer(cmd);   // minimised / not ready: drop the frame
        return;
    }
    const auto &cfg = settings::get();
    // 4:3 letterbox in swapchain pixels; optional whole-number scale of the game frame.
    float aspect = 4.f / 3.f;
    float dw = static_cast<float>(sw), dh = dw / aspect;
    if (dh > sh) { dh = static_cast<float>(sh); dw = dh * aspect; }
    if (cfg.integer_scale) {
        uint32_t base_w = game_w ? game_w : 640;
        float k = std::floor(dw / base_w);
        if (k >= 1) { dw = k * base_w; dh = dw / aspect; }
    }
    float dx = std::floor((sw - dw) / 2), dy = std::floor((sh - dh) / 2);
    SDL_GPUBlitInfo bi{};
    bi.source.texture = tex;
    bi.source.w = w;
    bi.source.h = h;
    bi.destination.texture = swap;
    bi.destination.x = static_cast<uint32_t>(dx);
    bi.destination.y = static_cast<uint32_t>(dy);
    bi.destination.w = static_cast<uint32_t>(dw);
    bi.destination.h = static_cast<uint32_t>(dh);
    bi.load_op = SDL_GPU_LOADOP_CLEAR;
    bi.clear_color = {0, 0, 0, 1};
    bi.filter = cfg.nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cmd, &bi);
    SDL_SubmitGPUCommandBuffer(cmd);
    // remember letterbox in window points for mouse mapping
    int ww = 0, wh = 0;
    SDL_GetWindowSize(win, &ww, &wh);
    float px_per_pt = ww ? static_cast<float>(sw) / ww : 1.f;
    lb_x = dx / px_per_pt;
    lb_y = dy / px_per_pt;
    lb_w = dw / px_per_pt;
    lb_h = dh / px_per_pt;
}

void present_rgba(const uint32_t *px, uint32_t w, uint32_t h) {
    device();
    game_w = w;
    game_h = h;
    if (!frame_tex || frame_w != w || frame_h != h) {
        if (frame_tex) SDL_ReleaseGPUTexture(dev, frame_tex);
        if (frame_tb) SDL_ReleaseGPUTransferBuffer(dev, frame_tb);
        SDL_GPUTextureCreateInfo ci{};
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ci.width = w;
        ci.height = h;
        ci.layer_count_or_depth = 1;
        ci.num_levels = 1;
        frame_tex = SDL_CreateGPUTexture(dev, &ci);
        SDL_GPUTransferBufferCreateInfo tci{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, w * h * 4, 0};
        frame_tb = SDL_CreateGPUTransferBuffer(dev, &tci);
        frame_w = w;
        frame_h = h;
        log("present: %ux%u frames\n", w, h);
    }
    memcpy(SDL_MapGPUTransferBuffer(dev, frame_tb, true), px, static_cast<size_t>(w) * h * 4);
    SDL_UnmapGPUTransferBuffer(dev, frame_tb);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{frame_tb, 0, w, h};
    SDL_GPUTextureRegion dst{};
    dst.texture = frame_tex;
    dst.w = w;
    dst.h = h;
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, true);
    SDL_EndGPUCopyPass(copy);
    present(cmd, frame_tex, w, h);
}

bool window_to_game(float wx, float wy, int *gx, int *gy) {
    if (lb_w <= 0 || lb_h <= 0) { *gx = static_cast<int>(wx); *gy = static_cast<int>(wy); return true; }
    float u = (wx - lb_x) / lb_w, v = (wy - lb_y) / lb_h;
    *gx = std::clamp(static_cast<int>(u * game_w), 0, static_cast<int>(game_w) - 1);
    *gy = std::clamp(static_cast<int>(v * game_h), 0, static_cast<int>(game_h) - 1);
    return true;
}

}  // namespace host::gpu
