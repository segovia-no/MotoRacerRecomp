#include "host_gpu.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#include "gpu/shaders.h"
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
// last letterbox (window points) and the shown crop of the game frame, for mouse mapping
float lb_x = 0, lb_y = 0, lb_w = 0, lb_h = 0;
uint32_t game_w = 640, game_h = 480;
Crop shown;
int ws_score = 0;   // >0: recent checks saw black letterbox bars
}  // namespace

SDL_GPUDevice *device() {
    if (dev) return dev;
    win = ensure_window();
    dev = SDL_CreateGPUDevice(gpu_shaders::formats(), false, nullptr);
    if (!dev) fatal("SDL_CreateGPUDevice failed: %s", SDL_GetError());
    if (!SDL_ClaimWindowForGPUDevice(dev, win)) fatal("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
    if (!settings::get().vsync && SDL_WindowSupportsGPUPresentMode(dev, win, SDL_GPU_PRESENTMODE_IMMEDIATE))
        mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
    SDL_SetGPUSwapchainParameters(dev, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
    log("gpu: %s device, window claimed\n", SDL_GetGPUDeviceDriver(dev));
    return dev;
}

uint32_t render_scale(uint32_t gw, uint32_t gh) {
    const auto &cfg = settings::get();
    gw = gw ? gw : 640;
    gh = gh ? gh : 480;
    uint32_t max_scale = std::max<uint32_t>(1, 8192 / std::max(gw, gh));
    if (cfg.render_scale > 0) return std::clamp<uint32_t>(static_cast<uint32_t>(cfg.render_scale), 1, max_scale);
    // "auto" renders the SCREEN MODEs above the game's own 640x480 natively.
    if (gw > 640) return 1;
    // "auto": smallest scale whose image covers the display height (in pixels)
    SDL_Window *w = ensure_window();
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    SDL_DisplayID d = SDL_GetDisplayForWindow(w);
    const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : nullptr;
    // SDL_DisplayMode sizes are in points; pixel_density converts them to pixels.
    float density = m && m->pixel_density > 0 ? m->pixel_density : 1.f;
    int target_h = std::max(ph, m ? static_cast<int>(m->h * density) : ph);
    uint32_t scale = (static_cast<uint32_t>(target_h) + gh - 1) / gh;
    scale = std::clamp<uint32_t>(scale, 1, std::min<uint32_t>(4, max_scale));
    log("gpu: render scale %u for %ux%u (window %dx%d px, display %dx%d x%.2f)\n", scale, gw, gh, pw, ph,
        m ? m->w : 0, m ? m->h : 0, density);
    return scale;
}

Crop widescreen_vote(bool bars_black, uint32_t gw, uint32_t gh) {
    Crop full{0, 0, gw, gh};
    if (!settings::get().widescreen || gh * 4 != gw * 3) { ws_score = 0; return full; }
    ws_score = bars_black ? std::min(ws_score + 1, 3) : 0;
    if (ws_score < 2) return full;
    uint32_t band = gw * 9 / 16;   // 640 -> 360
    return Crop{0, (gh - band) / 2, gw, band};
}

void present_placed(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *tex, uint32_t w, uint32_t h, const Crop &placed,
                    const Crop &crop) {
    device();
    SDL_GPUTexture *swap = nullptr;
    uint32_t sw = 0, sh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &swap, &sw, &sh) || !swap) {
        SDL_SubmitGPUCommandBuffer(cmd);   // minimised / not ready: drop the frame
        return;
    }
    const auto &cfg = settings::get();
    shown = crop;
    // Letterbox the crop (game pixels) to its own aspect in swapchain pixels.
    float aspect = static_cast<float>(crop.w) / static_cast<float>(crop.h ? crop.h : 1);
    float dw = static_cast<float>(sw), dh = dw / aspect;
    if (dh > sh) { dh = static_cast<float>(sh); dw = dh * aspect; }
    if (cfg.integer_scale) {
        float k = std::floor(dw / (crop.w ? crop.w : 640));
        if (k >= 1) { dw = k * crop.w; dh = dw / aspect; }
    }
    float dx = std::floor((sw - dw) / 2), dy = std::floor((sh - dh) / 2);
    // Where the texture's game rectangle lands inside that letterbox.
    float k = dw / static_cast<float>(crop.w ? crop.w : 1);
    SDL_GPUBlitInfo bi{};
    bi.source.texture = tex;
    bi.source.w = w;
    bi.source.h = h;
    bi.destination.texture = swap;
    bi.destination.x = static_cast<uint32_t>(std::max(0.f, dx + (static_cast<float>(placed.x) - crop.x) * k));
    bi.destination.y = static_cast<uint32_t>(std::max(0.f, dy + (static_cast<float>(placed.y) - crop.y) * k));
    bi.destination.w = static_cast<uint32_t>(placed.w * k);
    bi.destination.h = static_cast<uint32_t>(placed.h * k);
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

void present(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *tex, uint32_t w, uint32_t h, const Crop &crop) {
    // `tex` holds the whole game frame; blit only the crop, as a texture placed at the crop.
    float sx = static_cast<float>(w) / (game_w ? game_w : 640), sy = static_cast<float>(h) / (game_h ? game_h : 480);
    if (crop.x == 0 && crop.y == 0 && crop.w == game_w && crop.h == game_h) {
        present_placed(cmd, tex, w, h, crop, crop);
        return;
    }
    // Cropped: copy the region into a temporary view by blitting through present_placed with
    // a source sub-rectangle.
    device();
    SDL_GPUTexture *swap = nullptr;
    uint32_t sw = 0, sh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &swap, &sw, &sh) || !swap) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }
    const auto &cfg = settings::get();
    shown = crop;
    float aspect = static_cast<float>(crop.w) / static_cast<float>(crop.h ? crop.h : 1);
    float dw = static_cast<float>(sw), dh = dw / aspect;
    if (dh > sh) { dh = static_cast<float>(sh); dw = dh * aspect; }
    if (cfg.integer_scale) {
        float k = std::floor(dw / (crop.w ? crop.w : 640));
        if (k >= 1) { dw = k * crop.w; dh = dw / aspect; }
    }
    float dx = std::floor((sw - dw) / 2), dy = std::floor((sh - dh) / 2);
    SDL_GPUBlitInfo bi{};
    bi.source.texture = tex;
    bi.source.x = static_cast<uint32_t>(crop.x * sx);
    bi.source.y = static_cast<uint32_t>(crop.y * sy);
    bi.source.w = static_cast<uint32_t>(crop.w * sx);
    bi.source.h = static_cast<uint32_t>(crop.h * sy);
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
    int ww = 0, wh = 0;
    SDL_GetWindowSize(win, &ww, &wh);
    float px_per_pt = ww ? static_cast<float>(sw) / ww : 1.f;
    lb_x = dx / px_per_pt;
    lb_y = dy / px_per_pt;
    lb_w = dw / px_per_pt;
    lb_h = dh / px_per_pt;
}

void set_game_size(uint32_t w, uint32_t h) {
    game_w = w;
    game_h = h;
}

void present_rgba(const uint32_t *px, uint32_t w, uint32_t h, const Crop &crop) {
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
    present(cmd, frame_tex, w, h, crop);
}

bool window_to_game(float wx, float wy, int *gx, int *gy) {
    if (lb_w <= 0 || lb_h <= 0) { *gx = static_cast<int>(wx); *gy = static_cast<int>(wy); return true; }
    float u = (wx - lb_x) / lb_w, v = (wy - lb_y) / lb_h;
    *gx = std::clamp(static_cast<int>(shown.x + u * shown.w), 0, static_cast<int>(game_w) - 1);
    *gy = std::clamp(static_cast<int>(shown.y + v * shown.h), 0, static_cast<int>(game_h) - 1);
    return true;
}

}  // namespace host::gpu
