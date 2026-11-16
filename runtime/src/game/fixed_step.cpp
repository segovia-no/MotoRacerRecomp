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
// Interpolation (step 2; MOTORACER_INTERP=0 turns it off): the draw only reads the game's state, so each
// draw shows it blended between the last two logic steps (one step, 33 ms, behind): listed fields of
// the bikes and the cameras (positions, orientations, lean, rider angles, the view) are copied at every
// logic frame's draw; before a draw each one that changed gets prev + (curr - prev) * alpha (angles the
// short way round), alpha = the time since the step / 33.3 ms; after the draw the current values go
// back. A bike that jumped (respawn, warp) and a camera cut are not blended.
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
// ---------- interpolation between logic steps ----------
// Only listed fields are blended: the guest heap lives at 0x40000000, so a heap pointer also reads as a
// float of 2..8, and blending "anything float-like" turned a bike's model pointer into garbage (crash in
// node_matrices).
constexpr uint32_t BIKES = 0x581218, BIKE_SIZE = 0x3209, BIKE_COUNT = 0x509bac;
constexpr uint32_t CAMERAS = 0x5161c0, CAMERA_SIZE = 0x238, CAMERA_COUNT = 16, CAMERA_POS = 0x98;
enum Kind : uint8_t { FLOAT, ANGLE, INT_ANGLE };   // ANGLE: float, wraps at 2 pi; INT_ANGLE: sine-table units
struct Field { uint16_t off, count; Kind kind; };
const Field kBikeFields[] = {
    {0x30, 3, FLOAT}, {0x3c, 3, FLOAT},          // position (two copies)
    {0xbc, 9, FLOAT},                            // orientation rows (right, up, forward)
    {0x18c, 3, FLOAT}, {0x198, 3, FLOAT},        // heading, forward vector
    {0x1b4, 3, FLOAT}, {0x338, 3, FLOAT},        // front / back points
    {0x2e1d, 2, FLOAT},                          // lean, previous lean
    {0x2e3d, 3, ANGLE}, {0x2e49, 3, ANGLE},        // rider / body angles
    {0x2e55, 12, FLOAT},                         // frame from the back to the front point
    {0x2e8d, 2, FLOAT},                          // roll lag, bob
    {0x2ea1, 1, ANGLE},                           // lean angle
    {0x2ea9, 18, FLOAT},                         // contact points
    {0x307d, 1, FLOAT}, {0x30b5, 3, FLOAT}, {0x30dd, 1, FLOAT}, {0x30ed, 12, FLOAT},   // crash
};
// camera record (0x238 bytes; dumped at two steps of a race): +0x10..+0x1c four angles in sine-table
// units, +0x68 the view rows (3 x 3), +0x98 the position, +0xa4 the point looked at, +0xc0 a slowly
// changing float, +0xc8..+0x114 a point and the view's planes (normal + distance); the rest constant
const Field kCameraFields[] = {
    {0x10, 4, INT_ANGLE}, {0x68, 9, FLOAT}, {CAMERA_POS, 3, FLOAT}, {0xa4, 3, FLOAT}, {0xc0, 1, FLOAT},
    {0xc8, 20, FLOAT},
};

struct Interp {
    std::vector<uint32_t> addr;            // the blended dwords (fixed at the step that made the copies)
    std::vector<uint8_t> kind;
    std::vector<uint32_t> prev, curr;
    std::vector<uint32_t> group;           // per dword: its bike (0..) or 1000 + camera
    bool have_prev = false;
    std::vector<uint32_t> written;         // indices blended for this draw (restored after it)
    double step_time = 0;
} ip;

bool plausible_float(uint32_t u) {
    if ((u & 0x7fffffff) == 0) return true;
    const uint32_t e = (u >> 23) & 0xff;
    return e >= 100 && e <= 170;
}

void interp_layout(std::vector<uint32_t> &addr, std::vector<uint8_t> &kind, std::vector<uint32_t> &group) {
    uint32_t n = RD32(BIKE_COUNT);
    if (n > 40) n = 40;
    static const char *only = getenv("MOTORACER_INTERP");   // dev: "bikes" / "camera" only
    const bool bikes = !(only && !strcmp(only, "camera")), cams = !(only && !strcmp(only, "bikes"));
    for (uint32_t b = 1; bikes && b <= n; b++)
        for (const Field &f : kBikeFields)
            for (uint32_t i = 0; i < f.count; i++)
                addr.push_back(BIKES + b * BIKE_SIZE + f.off + i * 4), kind.push_back(f.kind), group.push_back(b);
    for (uint32_t c = 0; cams && c < CAMERA_COUNT; c++)
        for (const Field &f : kCameraFields)
            for (uint32_t i = 0; i < f.count; i++)
                addr.push_back(CAMERAS + c * CAMERA_SIZE + f.off + i * 4), kind.push_back(f.kind), group.push_back(1000 + c);
}

void interp_capture(double now) {   // at a logic frame's draw: the state this step produced
    std::vector<uint32_t> addr, group;
    std::vector<uint8_t> kind;
    interp_layout(addr, kind, group);
    ip.have_prev = addr == ip.addr;   // a new race / another bike count: no previous step to blend from
    ip.prev.swap(ip.curr);
    ip.curr.resize(addr.size());
    for (size_t k = 0; k < addr.size(); k++) ip.curr[k] = RD32(addr[k]);
    ip.addr = std::move(addr), ip.kind = std::move(kind), ip.group = std::move(group);
    ip.step_time = now;
}

void interp_apply(double alpha) {
    ip.written.clear();
    if (!ip.have_prev || ip.prev.size() != ip.curr.size() || alpha >= 1.0) return;
    alpha = std::max(0.0, alpha);
    auto f = [](uint32_t u) { float x; memcpy(&x, &u, 4); return static_cast<double>(x); };
    // a bike or camera that jumped this step (respawn, warp, camera cut) is shown as it is
    auto jumped = [&](uint32_t addr0, double limit) {
        const auto it = std::find(ip.addr.begin(), ip.addr.end(), addr0);
        if (it == ip.addr.end()) return false;
        const size_t i = static_cast<size_t>(it - ip.addr.begin());
        double d2 = 0;
        for (int c = 0; c < 3; c++) { const double d = f(ip.curr[i + c]) - f(ip.prev[i + c]); d2 += d * d; }
        return !(d2 < limit * limit);
    };
    uint32_t last_group = ~0u;
    bool skip = false;
    for (size_t k = 0; k < ip.addr.size(); k++) {
        if (ip.group[k] != last_group) {
            last_group = ip.group[k];
            skip = last_group < 1000 ? jumped(BIKES + last_group * BIKE_SIZE + 0x30, 1500)
                                     : jumped(CAMERAS + (last_group - 1000) * CAMERA_SIZE + CAMERA_POS, 3000);
        }
        const uint32_t p = ip.prev[k], c = ip.curr[k], a = ip.addr[k];
        if (skip || p == c || RD32(a) != c) continue;
        if (ip.kind[k] == INT_ANGLE) {
            const int32_t d = static_cast<int32_t>(c - p);
            if (d > 0x2000 || d < -0x2000) continue;   // a wrap or a cut: as it is
            WR32(a, p + static_cast<uint32_t>(static_cast<int32_t>(std::lround(d * alpha))));
            ip.written.push_back(static_cast<uint32_t>(k));
            continue;
        }
        if (!plausible_float(p) || !plausible_float(c)) continue;
        const double x0 = f(p), x1 = f(c);
        double d = x1 - x0;
        if (ip.kind[k] == ANGLE && std::fabs(d) > M_PI) d -= std::copysign(2 * M_PI, d);   // the short way round
        WRF32(a, static_cast<float>(x0 + d * alpha));
        ip.written.push_back(static_cast<uint32_t>(k));
    }
}

void interp_restore() {
    for (uint32_t k : ip.written) WR32(ip.addr[k], ip.curr[k]);
    ip.written.clear();
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
    static const bool interp = [] {
        const char *e = getenv("MOTORACER_INTERP");
        return !(e && *e == '0');
    }();
    if (interp) {
        if (!st.this_draw_only) interp_capture(now);
        interp_apply((now - ip.step_time) / TICK_MS);
    }
    static const bool trace = getenv("MOTORACER_INTERP_LOG") != nullptr;   // dev: per draw, what is shown
    if (trace)
        log("I %.2f %c a %.3f cam %.2f %.2f %.2f bike %.2f %.2f\n", now, st.this_draw_only ? 'd' : 'L',
            (now - ip.step_time) / TICK_MS, RDF32(CAMERAS + 0x98), RDF32(CAMERAS + 0x9c), RDF32(CAMERAS + 0xa0),
            RDF32(BIKES + BIKE_SIZE * RD32(0x508834) + 0x30), RDF32(BIKES + BIKE_SIZE * RD32(0x508834) + 0x38));
    st.saved_f = RD32(FRAME_F), st.swapped = true;
    // in hundredths of a second, as the draw gets it: the bike loop 0x47df40 has scaled the frame time
    // by [0x4d1970] (0.1) for everything after it (in ms, the race-end place number spun 10 times as fast)
    WRF32(FRAME_F, static_cast<float>(real * RDF32(0x4d1970)));
#endif
}

extern "C" void game_hook_draw_end(void) {
#ifndef __PSP__
    if (st.swapped) WR32(FRAME_F, st.saved_f), st.swapped = false;
    interp_restore();
    draw_diff_end(now_ms());
    st.this_draw_only = false;
#endif
}
