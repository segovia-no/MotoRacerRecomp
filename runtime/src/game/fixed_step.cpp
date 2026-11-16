// Fixed 30 Hz race logic (desktop; MOTORACER_FIXED_STEP=0 turns it off; docs/notes/16-frame-rate.md).
//
// The game's race frame (0x47d660) runs its logic once per drawn frame with a variable timestep, and
// several of its rules act per frame (takeoffs, grip, lean, walls...): the game behaves differently at
// every frame rate. The owner's reference is the game's own 30 fps (its limiter's default; the PSP's
// pace). Here the logic runs at exactly 30 steps a second with the frame times of a 30 fps race
// (33 / 33 / 34 ms), whatever the display's rate, and the frames in between only draw:
//   - jump hook 0x47d790 (the race is on: past the clock 0x4120e0, the in-race windows 0x4126a0 and the
//     replay checks 0x4121e0, which still run every frame): no step due -> straight to the draw call at
//     0x47da4e (the logic, the race-end gate 0x412a50 and the commentator 0x407420 are skipped); a step
//     due -> the frame times become the 30 fps tick and the frame runs as the game wrote it;
//   - hooks 0x47da4e / 0x47da5d around the draw ([vtable + 0x20](frame time, -2)): the draw gets the
//     real time since the last draw (in hundredths of a second, as the bike loop 0x47df40 leaves the
//     frame time for the rest of the frame), so what it animates by time (the race-end place number,
//     0x47b7a0) runs at real speed.
// Replays play their recorded frame times; their steps are paced by those times.
// Step 1 of the owner's plan: nothing is interpolated yet, so the motion is drawn at 30 fps.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "runtime.h"

using namespace rt;

namespace game { void pitch_redraw(); }   // pitch_smooth.cpp: the smoothed view again for a draw-only frame

namespace {

#ifndef __PSP__
constexpr uint32_t FRAME_MS = 0x4e980c, FRAME_F = 0x4e9808, SMOOTHED = 0x4dc150, REPLAY = 0x4e9834;
constexpr double TICK_MS = 1000.0 / 30;

bool enabled() {
    static const bool on = [] {
        const char *e = getenv("MOTORACER_FIXED_STEP");
        const bool v = !(e && *e == '0');
        log("fixed step: race logic at %s\n", v ? "30 Hz, drawing every frame" : "the frame rate (off)");
        return v;
    }();
    return on;
}

double now_ms() { return static_cast<double>(SDL_GetPerformanceCounter()) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency()); }

struct State {
    double last_call = 0, last_step = 0, last_draw = 0;   // host times (ms)
    double acc = 0;                                       // real time not yet covered by logic steps
    bool drew = false;          // the last logic frame reached the draw (draw-only frames need one)
    uint64_t steps = 0;         // logic steps since the tick pattern started
    uint64_t emitted = 0;       // ms of frame time given to them (steps * 33.33, rounded)
    float smoothed = 0;         // the smoothed frame time 0x4dc150 as a 30 fps race has it
    bool smoothed_init = false;
    uint32_t saved_f = 0;       // the game's float frame time around the draw
    bool swapped = false;
    uint64_t draw_only = 0, logic = 0;
    bool this_draw_only = false;   // the current frame skipped the logic
} st;

// MOTORACER_DRAW_DIFF=1 (dev): what the draw call itself changes in the game's static data (.data),
// on draw-only frames; every 5 s the small runs of dwords changed in most of them, with their values
// (state the draw advances per call, which a draw-only frame advances too often).
constexpr uint32_t DATA_LO = 0x4db000, DATA_HI = 0x70c568;
struct DrawDiff {
    bool on = false;
    std::vector<uint8_t> before;
    std::vector<uint32_t> count;   // per dword: draw-only frames that changed it
    uint32_t frames = 0;
    double next_dump = 0;
} dd;

void draw_diff_begin() {
    if (!dd.on || !st.this_draw_only) return;
    if (dd.before.empty()) dd.before.resize(DATA_HI - DATA_LO), dd.count.assign((DATA_HI - DATA_LO) / 4, 0);
    for (uint32_t a = DATA_LO; a + 4 <= DATA_HI; a += 4) memcpy(&dd.before[a - DATA_LO], &M[a], 4);
}

void draw_diff_end(double now) {
    if (!dd.on || !st.this_draw_only || dd.before.empty()) return;
    dd.frames++;
    for (uint32_t i = 0; i < dd.count.size(); i++)
        if (memcmp(&dd.before[i * 4], &M[DATA_LO + i * 4], 4)) dd.count[i]++;
    if (now < dd.next_dump) return;
    dd.next_dump = now + 5000;
    if (dd.frames >= 20) {
        log("draw diff: %u draw-only frames\n", dd.frames);
        int lines = 0;
        for (uint32_t i = 0; i < dd.count.size() && lines < 60;) {
            if (dd.count[i] * 2 < dd.frames) { i++; continue; }
            uint32_t j = i;
            while (j < dd.count.size() && dd.count[j] * 2 >= dd.frames) j++;
            const uint32_t a = DATA_LO + i * 4, n = j - i;
            if (n <= 6) {
                for (uint32_t k = i; k < j; k++) {
                    const uint32_t ad = DATA_LO + k * 4, v = RD32(ad);
                    float f;
                    memcpy(&f, &v, 4);
                    log("draw diff:   %08x  %u/%u  = %08x (%g)\n", ad, dd.count[k], dd.frames, v, static_cast<double>(f));
                }
            } else {
                log("draw diff:   %08x..%08x  %u dwords\n", a, a + n * 4, n);
            }
            lines++;
            i = j;
        }
    }
    dd.frames = 0;
    std::fill(dd.count.begin(), dd.count.end(), 0);
}

void set_frame_ms(uint32_t ms) {
    WR32(FRAME_MS, ms);
    WRF32(FRAME_F, static_cast<float>(ms));
    if (!st.smoothed_init) st.smoothed = RDF32(SMOOTHED), st.smoothed_init = true;
    st.smoothed += (static_cast<float>(ms) - st.smoothed) * 0.125f;   // the game's own average ([0x4d0380])
    WRF32(SMOOTHED, st.smoothed);
}
#endif

}  // namespace

// Jump hook at 0x47d790: 1 = draw only this frame (continue at 0x47da4e).
extern "C" int game_hook_step_gate(void) {
#ifndef __PSP__
    if (!enabled()) return 0;
    const double now = now_ms();
    const double elapsed = st.last_call > 0 ? now - st.last_call : 1e9;
    st.last_call = now;
    const bool replay = RD32(REPLAY) == 1;
    // a replay steps at its recorded frame times (the last one played stands for the next)
    const double period = replay ? std::max(1.0, static_cast<double>(RD32(FRAME_MS))) : TICK_MS;
    if (elapsed > 250) st.acc = period, st.drew = false;   // after loading / a pause: no catch-up burst
    else st.acc += elapsed;
    if (st.acc < period && st.drew) {
        st.draw_only++;
        st.this_draw_only = true;
        game::pitch_redraw();
        return 1;
    }
    // a logic step
    st.this_draw_only = false;
    st.acc = std::max(0.0, st.acc - period);
    if (st.acc > period) st.acc = 0;   // more than a step behind: dropped (no burst of steps)
    st.drew = false;
    st.logic++;
    const double since = st.last_step > 0 ? now - st.last_step : TICK_MS;
    st.last_step = now;
    if (!replay) {
        if (since > 3 * TICK_MS) {
            // the host is slower than 30 fps: the game's variable timestep, as the original does
            set_frame_ms(static_cast<uint32_t>(std::clamp(std::lround(since), 1L, 200L)));
            st.steps = st.emitted = 0;
        } else {
            st.steps++;
            const uint64_t target = (st.steps * 1000 + 15) / 30;   // 33, 67, 100, ...: 33 / 34 / 33
            const uint32_t ms = static_cast<uint32_t>(target - st.emitted);
            st.emitted = target;
            if (st.steps >= 3000) st.steps = st.emitted = 0;   // every 100 s, exact multiple
            set_frame_ms(ms);
        }
    }
    static uint64_t next_log = 0;
    if (st.logic >= next_log) {
        next_log = st.logic + 1800;   // once a minute of logic
        log("fixed step: %llu logic steps, %llu draw-only frames\n", static_cast<unsigned long long>(st.logic),
            static_cast<unsigned long long>(st.draw_only));
    }
#endif
    return 0;
}

// GAME_HOOKs at 0x47da4e (before the draw call pushes the frame time) and 0x47da5d (after it).
extern "C" void game_hook_draw_begin(void) {
#ifndef __PSP__
    if (!enabled()) return;
    static const bool diff = (dd.on = getenv("MOTORACER_DRAW_DIFF") != nullptr);
    (void)diff;
    draw_diff_begin();
    const double now = now_ms();
    const double real = st.last_draw > 0 ? std::clamp(now - st.last_draw, 1.0, 200.0) : TICK_MS;
    st.last_draw = now;
    st.drew = true;
    st.saved_f = RD32(FRAME_F), st.swapped = true;
    // in hundredths of a second, as the draw gets it: the bike loop 0x47df40 has scaled the frame time
    // by [0x4d1970] (0.1) for everything after it (in ms, the race-end place number spun 10 times as fast)
    WRF32(FRAME_F, static_cast<float>(real * RDF32(0x4d1970)));
#endif
}

extern "C" void game_hook_draw_end(void) {
#ifndef __PSP__
    if (st.swapped) WR32(FRAME_F, st.saved_f), st.swapped = false;
    draw_diff_end(now_ms());
    st.this_draw_only = false;
#endif
}
