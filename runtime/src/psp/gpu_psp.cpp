// host::gpu on the PSP (docs/25-psp-port.md): the software frame presenter draws the CPU-made
// frame (menus: 640x480) as a scaled texture with the GE; there is no SDL_GPU device.
#include <malloc.h>
#include <pspgu.h>

#include <cstdlib>
#include <cstring>

#include "../host_gpu.h"
#include "ge.h"

namespace host::gpu {

namespace {

struct SpriteVertex {
    float u, v;
    float x, y, z;
};

uint16_t *frame16 = nullptr;   // the frame in GE 5650, stride = frame width (16-byte aligned)
uint32_t frame16_px = 0;

// Draws the w x h 5650 image (stride w) into the visible 480x272 window of the draw buffer, keeping
// its aspect ratio; strips of at most 512 texels (the GE's texture limit).
void draw_frame(const uint16_t *img, uint32_t w, uint32_t h, uint32_t stride) {
    using namespace psp_ge;
    float scale = static_cast<float>(SCREEN_H) / static_cast<float>(h);
    if (w * scale > SCREEN_W) scale = static_cast<float>(SCREEN_W) / static_cast<float>(w);
    float dw = w * scale, dh = h * scale;
    float x0 = (SCREEN_W - dw) / 2, y0 = VIEW_Y0 + (SCREEN_H - dh) / 2;
    begin();
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuClearColor(0);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_5650, 0, 0, 0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    uint32_t th = 1;
    while (th < h) th <<= 1;
    for (uint32_t sx = 0; sx < w; sx += 512) {
        uint32_t sw = w - sx < 512 ? w - sx : 512, tw = 1;
        while (tw < sw) tw <<= 1;
        sceGuTexImage(0, static_cast<int>(tw), static_cast<int>(th), static_cast<int>(stride), img + sx);
        sceGuTexFlush();
        auto *v = static_cast<SpriteVertex *>(list_memory(2 * sizeof(SpriteVertex)));
        v[0] = {0.f, 0.f, x0 + sx * scale, y0, 0.f};
        v[1] = {static_cast<float>(sw), static_cast<float>(h), x0 + (sx + sw) * scale, y0 + dh, 0.f};
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, v);
    }
    sceGuDisable(GU_TEXTURE_2D);
    show();
}

}  // namespace

SDL_GPUDevice *device() { return nullptr; }
void apply_vsync() {}

void present(SDL_GPUCommandBuffer *, SDL_GPUTexture *, uint32_t, uint32_t, const Crop &) {}
void present_placed(SDL_GPUCommandBuffer *, SDL_GPUTexture *, uint32_t, uint32_t, const Crop &, const Crop &) {}

void present_rgba(const uint32_t *px, uint32_t w, uint32_t h, const Crop &) {
    if (w > 1024 || h > 512) return;
    if (frame16_px < w * h) {
        free(frame16);
        frame16 = static_cast<uint16_t *>(memalign(64, w * h * 2));
        frame16_px = w * h;
    }
    for (uint32_t i = 0; i < w * h; i++) {   // RGBA8 (R in the low byte) -> 5650 (R in the low bits)
        uint32_t p = px[i];
        frame16[i] = static_cast<uint16_t>(((p >> 3) & 0x1F) | ((p >> 10) & 0x3F) << 5 | ((p >> 19) & 0x1F) << 11);
    }
    draw_frame(frame16, w, h, w);
}

void present_rgb16(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch, bool rgb555) {
    if (w > 1024 || h > 512) return;
    const uint32_t stride = (w + 7) & ~7u;
    if (frame16_px < stride * h) {
        free(frame16);
        frame16 = static_cast<uint16_t *>(memalign(64, stride * h * 2));
        frame16_px = stride * h;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint16_t *row = reinterpret_cast<const uint16_t *>(px + static_cast<size_t>(y) * pitch);
        uint16_t *out = frame16 + y * stride;
        if (rgb555)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t p = row[x];
                out[x] = static_cast<uint16_t>(((p >> 10) & 31) | ((p >> 5) & 31) << 6 | (p & 31) << 11);
            }
        else
            for (uint32_t x = 0; x < w; x++) out[x] = psp_ge::swap_rb565(row[x]);
    }
    draw_frame(frame16, w, h, stride);
}

Crop widescreen_vote(bool, uint32_t game_w, uint32_t game_h) { return Crop{0, 0, game_w, game_h}; }
void set_game_size(uint32_t, uint32_t) {}
bool window_to_game(float, float, int *, int *) { return false; }
uint32_t render_scale(uint32_t, uint32_t) { return 1; }

}  // namespace host::gpu
