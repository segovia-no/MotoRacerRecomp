// Software-renderer presentation: converts guest frames to RGBA8 and hands them to the shared
// GPU presenter (host_gpu.cpp), which letterboxes them into the window.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "game_hooks.h"
#include "host_gpu.h"
#include "host_video.h"
#include "runtime.h"
#include "settings.h"

using namespace rt;

namespace host {

SDL_Window *ensure_window();   // host_window.cpp

namespace {
std::vector<uint32_t> rgba;
uint64_t frames = 0;

// Debug aid: MOTORACER_SNAPSHOT_DIR=<dir> saves every 60th (MOTORACER_SNAPSHOT_EVERY) presented frame.
void maybe_snapshot(const uint32_t *px, int w, int h) {
    static const char *dir = getenv("MOTORACER_SNAPSHOT_DIR");
    static const uint64_t every = getenv("MOTORACER_SNAPSHOT_EVERY") ? std::max(1, atoi(getenv("MOTORACER_SNAPSHOT_EVERY"))) : 60;
    if (!dir || frames % every) return;
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, const_cast<uint32_t *>(px), w * 4);
    if (!s) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06llu.bmp", dir, static_cast<unsigned long long>(frames));
    SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
}

// The game's 16/9 mode: pure black bands of 60 rows above and below a 640x360 picture.
bool bars_black(int w, int h) {
    int band = (h - w * 9 / 16) / 2;
    if (band <= 0) return false;
    for (int y = 0; y < band; y++)
        for (int x = 0; x < w; x++)
            if ((rgba[static_cast<size_t>(y) * w + x] & 0xFFFFFF) || (rgba[static_cast<size_t>(h - 1 - y) * w + x] & 0xFFFFFF))
                return false;
    // and the picture itself must not be black (screen fades)
    const uint32_t *mid = &rgba[static_cast<size_t>(h / 2) * w];
    for (int x = 0; x < w; x++) if (mid[x] & 0xFFFFFF) return true;
    return false;
}

// FrameRate setting: the runtime's frame limiter (the game's own busy-wait limiter is always off,
// see settings.cpp; FrameRate=game paces to its -FrameRateMax value). Also keeps game frames at
// least 1 ms apart: the game measures frame time with timeGetTime in whole milliseconds and treats
// 0 ms as 30 ms (0x412164), which would speed up a fast race.
void limit_frame_rate() {
    static uint64_t next = 0, last_ms = 0, count = 0, count_since = 0;
    static const bool fps_log = getenv("MOTORACER_FPS_LOG") != nullptr;
    static int display_rate = 0;
    if (!display_rate) {
        SDL_DisplayID d = SDL_GetDisplayForWindow(ensure_window());
        const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : nullptr;
        display_rate = m && m->refresh_rate > 0 ? static_cast<int>(m->refresh_rate + 0.5f) : 60;
        log("frame rate: display refresh rate %d Hz\n", display_rate);
    }
    int rate = settings::get().frame_rate;
    if (rate == 0) rate = settings::get().game_frame_cap ? settings::get().game_frame_cap : -1;
    if (rate == -2) rate = display_rate;
    // With vsync at or below the target rate, vsync paces; sleeping as well would now and then
    // miss a refresh (measured 59.4 instead of 60 fps).
    if (settings::get().vsync && rate >= display_rate) rate = -1;
    if (rate > 0) {
        uint64_t period = 1000000000ull / static_cast<uint64_t>(rate), now = SDL_GetTicksNS();
        if (next > now && next - now <= period) {
            SDL_DelayPrecise(next - now);
            next += period;
        } else {
            next = now + period;   // first frame, or slower than the limit
        }
    }
    while (SDL_GetTicks() == last_ms) SDL_DelayPrecise(100000);
    last_ms = SDL_GetTicks();
    if (fps_log) {
        uint64_t now = SDL_GetTicks();
        if (!count_since) count_since = now;
        if (++count, now - count_since >= 2000) {
            log("fps: %.1f\n", count * 1000.0 / static_cast<double>(now - count_since));
            count = 0;
            count_since = now;
        }
    }
}

void submit(int w, int h) {
    maybe_snapshot(rgba.data(), w, h);
    gpu::Crop crop = gpu::widescreen_vote(bars_black(w, h), static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    gpu::present_rgba(rgba.data(), static_cast<uint32_t>(w), static_cast<uint32_t>(h), crop);
    frames++;
    game::frame_tick();
    limit_frame_rate();
}
}  // namespace

void present_rgb16(const uint8_t *px, int w, int h, int pitch, bool rgb555) {
    rgba.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++) {
        const uint16_t *row = reinterpret_cast<const uint16_t *>(px + static_cast<size_t>(y) * pitch);
        uint32_t *out = &rgba[static_cast<size_t>(y) * w];
        for (int x = 0; x < w; x++) {
            uint32_t p = row[x], r, g, b;
            if (rgb555) { r = (p >> 10) & 31; g = (p >> 5) & 31; b = p & 31; g = g << 3 | g >> 2; }
            else { r = (p >> 11) & 31; g = (p >> 5) & 63; b = p & 31; g = g << 2 | g >> 4; }
            r = r << 3 | r >> 2;
            b = b << 3 | b >> 2;
            out[x] = r | g << 8 | b << 16 | 0xFF000000u;
        }
    }
    submit(w, h);
}

void present_pal8(const uint8_t *px, int w, int h, int pitch, const uint32_t *pal_xrgb) {
    rgba.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = pal_xrgb[px[y * pitch + x]];   // 0xAARRGGBB -> RGBA bytes
            rgba[static_cast<size_t>(y) * w + x] = ((c >> 16) & 255) | (c & 0xFF00) | ((c & 255) << 16) | 0xFF000000u;
        }
    submit(w, h);
}

uint64_t frame_count() { return frames; }
void count_frame() {
    frames++;
    game::frame_tick();
    limit_frame_rate();
}

bool window_to_game(float wx, float wy, int *gx, int *gy) { return gpu::window_to_game(wx, wy, gx, gy); }

}  // namespace host
