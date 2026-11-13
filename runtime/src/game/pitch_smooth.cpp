// Smooth bike pitch on slopes, for the view only (GitHub issue #11; Recomp menu "SMOOTH BIKE PITCH",
// PitchSmoothing; not on the PSP). The game pitches a bike from a front and a back point put on the
// road (0x469c60, 0x46bf80), and the road's height is sampled per track sector, which does not join
// smoothly: on a steady climb the pitch saw-tooths by 2-3 degrees every few frames (measured: sin of
// the pitch 0.273 0.283 0.320 0.292 0.286 0.335 ... at 60 fps), which the cockpit camera shows as a
// shake. After the physics step and before the camera update (0x458620) the player's orientation
// matrix (+0xbc), its lean frame (+0x2e55) and forward vector (+0x198) are rotated about their right
// axis to a filtered pitch; at the end of the frame and before the next physics step the game's own
// values are put back,
// so the simulation never sees them (each block only if it still holds the view's value: a race restart
// in between puts the bike on the grid, which must stay). The filter is an alpha-beta tracker (position and rate, gains from
// a time constant and the frame time): steady slopes and steady pitch changes pass without lag, sudden
// changes are spread over about the time constant.
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "game/settings.h"
#include "runtime.h"

using namespace rt;

namespace {

#ifndef __PSP__
constexpr uint32_t PLAYER = 0x508834, BIKES = 0x581218, BIKE_SIZE = 0x3209;   // bike(i) = BIKES + i * size
constexpr uint32_t FRAME_MS = 0x4e980c;
constexpr uint32_t MATRIX = 0xbc, LEAN_FRAME = 0x2e55, FORWARD = 0x198;      // rows: right, up, forward
constexpr double RESET_PITCH = 0.5;   // a jump this large (respawn, crash): taken at once
// the shown pitch stays this close to the game's: the seams' steps (2-3 degrees) are smoothed, real
// slope changes pass, and the tracker cannot overshoot on crests / landings (unclamped: up to 13
// degrees measured). (The height was filtered too at first: its per-frame steps follow the uneven
// frame times on steep slopes, smooth in time, and filtering it gained nothing.)
constexpr double MAX_PITCH_GAP = 0.05;

double tau_ms() {   // MOTORACER_PITCH_TAU=<ms> (dev tuning)
    static const double t = getenv("MOTORACER_PITCH_TAU") ? atof(getenv("MOTORACER_PITCH_TAU")) : 120.0;
    return t > 1 ? t : 120.0;
}

// alpha-beta tracker: x follows the measurement, v its rate; critically damped gains from tau
struct Tracker {
    bool init = false;
    double x = 0, v = 0;
    double update(double m, double dt, double reset, double gap) {
        if (!init || std::fabs(m - x) > reset || dt <= 0) {
            init = true, x = m, v = 0;
            return x;
        }
        const double a = 1.0 - std::exp(-dt / tau_ms()), b = a * a / (2.0 - a);
        const double predicted = x + v * dt, err = m - predicted;
        x = predicted + a * err;
        v += b * err / dt;
        if (x > m + gap) x = m + gap, v = std::fmin(v, 0.0);   // held at the limit: no rate away from it
        if (x < m - gap) x = m - gap, v = std::fmax(v, 0.0);
        return x;
    }
};

// One swapped block: the game's bytes and the ones written for the view. A block is put back only if
// it still holds what was written: when the game itself set it meanwhile (a race restart puts the bike
// on the grid between the view and the next physics step), the game's new value stays.
struct Block {
    uint32_t offset = 0, size = 0;
    uint8_t game[48] = {}, view[48] = {};
};
struct Saved {
    bool on = false;
    uint32_t bike = 0;
    Block blocks[3];
} saved;
Tracker pitch_track;
uint32_t tracked_bike = 0;

float rdf(uint32_t a) { uint32_t v = RD32(a); float f; memcpy(&f, &v, 4); return f; }
void wrf(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); WR32(a, v); }

struct V3 { double x, y, z; };
V3 rd3(uint32_t a) { return {rdf(a), rdf(a + 4), rdf(a + 8)}; }
void wr3(uint32_t a, V3 v) { wrf(a, static_cast<float>(v.x)); wrf(a + 4, static_cast<float>(v.y)); wrf(a + 8, static_cast<float>(v.z)); }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 rotate(V3 v, V3 axis, double angle) {   // Rodrigues, axis normalized
    const double c = std::cos(angle), s = std::sin(angle), k = dot(axis, v) * (1 - c);
    const V3 x = cross(axis, v);
    return {v.x * c + x.x * s + axis.x * k, v.y * c + x.y * s + axis.y * k, v.z * c + x.z * s + axis.z * k};
}
double pitch_of(V3 forward) {
    const double len = std::sqrt(dot(forward, forward));
    return len > 1e-6 ? std::asin(std::fmax(-1.0, std::fmin(1.0, forward.y / len))) : 0;
}

// Rotates a frame's up / forward rows about its right row so that its forward pitch becomes target.
void pitch_frame(uint32_t m, double target) {
    V3 right = rd3(m), up = rd3(m + 0xc), forward = rd3(m + 0x18);
    const double rl = std::sqrt(dot(right, right));
    if (rl < 1e-6) return;
    right = {right.x / rl, right.y / rl, right.z / rl};
    const double delta = target - pitch_of(forward);
    // the sign that brings the pitch towards the target (the frame's handedness)
    const V3 a = rotate(forward, right, delta), b = rotate(forward, right, -delta);
    const double angle = std::fabs(pitch_of(a) - target) <= std::fabs(pitch_of(b) - target) ? delta : -delta;
    wr3(m + 0xc, rotate(up, right, angle));
    wr3(m + 0x18, rotate(forward, right, angle));
}

uint32_t player_bike() {
    const uint32_t p = RD32(PLAYER);
    return p < 64 ? BIKES + BIKE_SIZE * p : 0;
}

void restore() {
    if (!saved.on) return;
    saved.on = false;
    for (const Block &k : saved.blocks) {
        bool ours = true;
        for (uint32_t i = 0; i < k.size && ours; i++) ours = RD8(saved.bike + k.offset + i) == k.view[i];
        if (ours)
            for (uint32_t i = 0; i < k.size; i++) WR8(saved.bike + k.offset + i, k.game[i]);
    }
}
#endif

}  // namespace

// GAME_HOOKs at the end of the race frame (0x47da8f, before the frame limiter) and before the physics
// step (0x47d7c5, for frames that left early): the game's own values back.
extern "C" void game_hook_pitch_sim(void) {
#ifndef __PSP__
    restore();
#endif
}

namespace game { void dev_warp(); }   // dev_warp.cpp

// GAME_HOOK before the camera update (0x47d8bb): the player's pitch low-passed for the view.
extern "C" void game_hook_pitch_view(void) {
#ifndef __PSP__
    game::dev_warp();
    if (saved.on || !settings::get().pitch_smoothing) return;   // paused: still swapped from the last frame
    const uint32_t b = player_bike();
    if (!b) return;
    const double pitch = pitch_of(rd3(b + MATRIX + 0x18));
    const double dt = std::fmin(200.0, std::fmax(0.0, static_cast<double>(RD32(FRAME_MS))));
    if (b != tracked_bike) pitch_track.init = false, tracked_bike = b;
    const double smoothed = pitch_track.update(pitch, dt, RESET_PITCH, MAX_PITCH_GAP);
    saved.on = true;
    saved.bike = b;
    const uint32_t layout[3][2] = {{MATRIX, 48}, {LEAN_FRAME, 48}, {FORWARD, 12}};
    for (int k = 0; k < 3; k++) {
        Block &bl = saved.blocks[k];
        bl.offset = layout[k][0], bl.size = layout[k][1];
        for (uint32_t i = 0; i < bl.size; i++) bl.game[i] = RD8(b + bl.offset + i);
    }
    const double lean_target = pitch_of(rd3(b + LEAN_FRAME + 0x18)) + (smoothed - pitch);   // same offset
    pitch_frame(b + MATRIX, smoothed);
    pitch_frame(b + LEAN_FRAME, lean_target);
    V3 f = rd3(b + MATRIX + 0x18);
    const double fl = std::sqrt(dot(f, f));
    if (fl > 1e-6) wr3(b + FORWARD, {f.x / fl, f.y / fl, f.z / fl});
    for (Block &bl : saved.blocks)
        for (uint32_t i = 0; i < bl.size; i++) bl.view[i] = RD8(b + bl.offset + i);
    static const bool trace = getenv("MOTORACER_PITCH_LOG") != nullptr;   // dev: raw / shown pitch per frame
    if (trace)
        log("pitch: dt %u raw %.4f shown %.4f\n", RD32(FRAME_MS), pitch, pitch_of(rd3(b + MATRIX + 0x18)));
#endif
}
