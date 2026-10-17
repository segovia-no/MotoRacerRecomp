// Software-renderer presentation: converts guest frames to RGBA8 and hands them to the shared
// GPU presenter (host_gpu.cpp), which letterboxes them into the window.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#ifdef __APPLE__
#include <dlfcn.h>
#endif

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
}  // namespace

int display_refresh_rate() {
    static int rate = 0;
    if (!rate) {
        SDL_DisplayID d = SDL_GetDisplayForWindow(ensure_window());
        const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : nullptr;
        rate = m && m->refresh_rate > 0 ? static_cast<int>(m->refresh_rate + 0.5f) : 60;
        log("frame rate: display refresh rate %d Hz\n", rate);
    }
    return rate;
}

namespace {

// MOTORACER_FRAME_WORK=1 (dev, translator benchmarks): per frame, the time spent outside frame
// pacing (game code + runtime + submission) and, on macOS, the instructions / cycles the game thread
// executed in it (thread_selfcounts: exported by libsystem_kernel, private; Apple Silicon counters,
// independent of the core type the scheduler picked, unlike wall time). Logged every 2 s.
struct ThreadCounts { uint64_t insns = 0, cycles = 0; };
ThreadCounts thread_counts() {
    ThreadCounts c;
#ifdef __APPLE__
    using Fn = int (*)(int, void *, size_t);
    static const Fn fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "thread_selfcounts"));
    uint64_t v[2] = {0, 0};
    if (fn && fn(1, v, sizeof v) == 0) { c.insns = v[0]; c.cycles = v[1]; }
#endif
    return c;
}

void log_frame_work(bool entering) {
    static const bool on = getenv("MOTORACER_FRAME_WORK") != nullptr;
    if (!on) return;
    static uint64_t left = 0, work = 0, frames = 0, since = 0, insns = 0, cycles = 0;
    static ThreadCounts at_left;
    uint64_t now = SDL_GetTicksNS();
    if (!entering) { left = now; at_left = thread_counts(); return; }
    if (left) {
        ThreadCounts c = thread_counts();
        work += now - left;
        insns += c.insns - at_left.insns;
        cycles += c.cycles - at_left.cycles;
        frames++;
    }
    if (!since) since = now;
    if (now - since >= 2000000000ull && frames) {
        double n = static_cast<double>(frames);
        log("frame work: %.3f ms, %.3f M instructions, %.3f M cycles per frame (%llu frames)\n",
            work / 1e6 / n, insns / 1e6 / n, cycles / 1e6 / n, static_cast<unsigned long long>(frames));
        work = frames = insns = cycles = 0;
        since = now;
    }
}

void limit_frame_rate() {
    log_frame_work(true);
    struct Leave { ~Leave() { log_frame_work(false); } } leave;
    static uint64_t next = 0, last_ms = 0, count = 0, count_since = 0;
    static const bool fps_log = getenv("MOTORACER_FPS_LOG") != nullptr;
    int display_rate = display_refresh_rate();
    int rate = settings::get().frame_rate;
    if (rate == 0) rate = settings::get().game_frame_cap ? settings::get().game_frame_cap : -1;
    if (rate == -2) rate = display_rate;
    // More frames than the display shows only make motion uneven (each refresh shows a frame of a
    // different age; measured on a 60 Hz screen at 120: frame times 8..27 ms). Cap at the refresh rate.
    if (rate > display_rate) {
        static int warned = 0;
        if (warned != rate) {
            warned = rate;
            log("frame rate: %d fps is above the display's %d Hz, capped\n", rate, display_rate);
        }
        rate = display_rate;
    }
    // With vsync at or below the target rate, vsync paces; sleeping as well would now and then
    // miss a refresh (measured 59.4 instead of 60 fps).
    if (settings::get().vsync && rate >= display_rate) rate = -1;
    if (rate > 0) {
        uint64_t period = 1000000000ull / static_cast<uint64_t>(rate), now = SDL_GetTicksNS();
        if (next > now && next - now <= period) {
            prof::Scope prof_wait(prof::WAIT);
            SDL_DelayPrecise(next - now);
            next += period;
        } else {
            next = now + period;   // first frame, or slower than the limit
        }
    }
    while (SDL_GetTicks() == last_ms) {
        prof::Scope prof_wait(prof::WAIT);
        SDL_DelayPrecise(100000);
    }
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
#ifdef __PSP__
    gpu::present_rgb16(px, static_cast<uint32_t>(w), static_cast<uint32_t>(h), static_cast<uint32_t>(pitch), rgb555);
    frames++;
    game::frame_tick();
    limit_frame_rate();
    return;
#endif
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

namespace rt {
void thread_counts(uint64_t *insns, uint64_t *cycles) {
    host::ThreadCounts c = host::thread_counts();
    *insns = c.insns;
    *cycles = c.cycles;
}
}  // namespace rt
