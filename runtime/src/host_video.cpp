// Presents guest frame buffers in the SDL window (Metal-backed SDL_Renderer).
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "host_video.h"
#include "runtime.h"

using namespace rt;

namespace host {

extern SDL_Window *sdl_window;

namespace {
SDL_Renderer *renderer = nullptr;
SDL_Texture *texture = nullptr;
int tex_w = 0, tex_h = 0;
SDL_PixelFormat tex_fmt = SDL_PIXELFORMAT_UNKNOWN;
std::vector<uint32_t> convert_buf;
uint64_t frames = 0;

bool ensure(int w, int h, SDL_PixelFormat fmt) {
    if (!sdl_window) {
        sdl_window = SDL_CreateWindow("Moto Racer", 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!sdl_window) fatal("SDL_CreateWindow failed: %s", SDL_GetError());
    }
    if (!renderer) {
        renderer = SDL_CreateRenderer(sdl_window, nullptr);
        if (!renderer) fatal("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_SetRenderVSync(renderer, 1);
        log("renderer: %s\n", SDL_GetRendererName(renderer));
    }
    if (!texture || w != tex_w || h != tex_h || fmt != tex_fmt) {
        if (texture) SDL_DestroyTexture(texture);
        texture = SDL_CreateTexture(renderer, fmt, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!texture) fatal("SDL_CreateTexture failed: %s", SDL_GetError());
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        SDL_SetRenderLogicalPresentation(renderer, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        tex_w = w;
        tex_h = h;
        tex_fmt = fmt;
        log("present: %dx%d %s\n", w, h, SDL_GetPixelFormatName(fmt));
    }
    return true;
}

// Debug aid: MOTORACER_SNAPSHOT_DIR=<dir> saves every 60th presented frame as BMP.
void maybe_snapshot(const void *px, int w, int h, int pitch, SDL_PixelFormat fmt) {
    static const char *dir = getenv("MOTORACER_SNAPSHOT_DIR");
    if (!dir || frames % 60) return;
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, fmt, const_cast<void *>(px), pitch);
    if (!s) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06llu.bmp", dir, static_cast<unsigned long long>(frames));
    SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
}

void draw() {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
    frames++;
}
}  // namespace

void present_rgb16(const uint8_t *px, int w, int h, int pitch, bool rgb555) {
    ensure(w, h, rgb555 ? SDL_PIXELFORMAT_XRGB1555 : SDL_PIXELFORMAT_RGB565);
    SDL_UpdateTexture(texture, nullptr, px, pitch);
    maybe_snapshot(px, w, h, pitch, tex_fmt);
    draw();
}

void present_pal8(const uint8_t *px, int w, int h, int pitch, const uint32_t *pal_xrgb) {
    ensure(w, h, SDL_PIXELFORMAT_XRGB8888);
    convert_buf.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) convert_buf[static_cast<size_t>(y) * w + x] = pal_xrgb[px[y * pitch + x]];
    SDL_UpdateTexture(texture, nullptr, convert_buf.data(), w * 4);
    maybe_snapshot(convert_buf.data(), w, h, w * 4, tex_fmt);
    draw();
}

uint64_t frame_count() { return frames; }

}  // namespace host
