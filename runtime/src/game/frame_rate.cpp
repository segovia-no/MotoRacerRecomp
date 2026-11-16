// Frame-rate independence of per-frame game logic (docs/notes/16-frame-rate.md; desktop only).
//
// The game runs a variable timestep (motion scales with the frame time), but some of its logic acts
// once per frame: at 120 fps it acts 4 times as often per second as at 30. The reference is 30 fps, the
// game's own frame rate (its limiter defaults to 30 without -FrameRateMax; GOG's launcher passes 60) and
// the PSP's: the owner found the PSP port's feel the faithful one, and at 120 fps the bikes jumped a
// quarter as often (Dirt Arena: 15.5 takeoffs a minute at 30 fps, 4.1 at 120). Each fix makes a frame
// do what 30 fps frames do over the same time; 30 fps frames (33 / 34 ms) stay exactly as the game
// computes them. Times come from the game's integer frame time (0x4e980c), which replays
// record and play back, so replays stay deterministic.
//
// MOTORACER_FPS_FIX (dev, A/B tests): 0 turns the fixes off, a mask of FIX_ bits picks some.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "runtime.h"

using namespace rt;

namespace {

#ifndef __PSP__
constexpr uint32_t FRAME_MS = 0x4e980c;
constexpr double REF_MS = 1000.0 / 30;

// MOTORACER_FPS_FIX (dev): unset = all fixes, 0 = none, else a mask of the FIX_ bits.
enum : uint32_t {
    FIX_WALL = 1, FIX_SPEED_BLEND = 2, FIX_FOLLOW = 4, FIX_SLIDE = 8, FIX_LEAN_DIV = 16, FIX_LEAN_SMOOTH = 32,
    FIX_BOUNCE = 64, FIX_ROLL = 128, FIX_OFFSET = 256, FIX_COUNTER = 512, FIX_GRIP = 1024, FIX_DIRT_LEAN = 2048,
    FIX_GROUND = 4096,
};
constexpr uint32_t FIX_DEFAULT = ~0u;   // all
uint32_t fix_mask() {
    static const uint32_t m = getenv("MOTORACER_FPS_FIX") ? static_cast<uint32_t>(strtoul(getenv("MOTORACER_FPS_FIX"), nullptr, 0)) : FIX_DEFAULT;
    return m;
}

// This frame's time in ms if it is not a 30 fps frame (and fix `bit` is on), else 0. Slower frame rates
// are corrected as well (measured after the same steering tap: lean 0.39 at 20 fps, 0.53 at 30 and 60,
// 0.22 at 144; the travel direction's lag behind the facing 0.51 / 0.43 / 0.31 / 0.15 rad).
uint32_t odd_frame_ms(uint32_t bit) {
    const uint32_t dt = RD32(FRAME_MS);
    if (!(fix_mask() & bit) || dt == 33 || dt == 34 || dt == 0 || dt > 200) return 0;
    return dt;
}

// The same, as a number of 30 fps frames (0: a 30 fps frame, use the game's own step).
double odd_frames(uint32_t bit) { return odd_frame_ms(bit) / REF_MS; }

// x <- a x + b applied n times (n need not be whole): a^n x + b (1 - a^n) / (1 - a).
double affine_steps(double x, double a, double b, double n) {
    const double an = std::pow(a, n);
    return an * x + (a == 1 ? b * n : b * (1 - an) / (1 - a));
}

struct Follow { uint32_t bike = 0; float speed = 0, other = 0; } follow;

float rdf(uint32_t a) { uint32_t v = RD32(a); float f; memcpy(&f, &v, 4); return f; }
void wrf(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); WR32(a, v); }
#endif

}  // namespace

namespace game {

// Wall hit (0x46bf80, after the move against the track): a bike that hit the track's edge has its
// target speed +0x1a4 and speed +0xec halved ([0x4d1018] = 0.5), every frame it is in contact. A bike
// scraping a wall hits it every frame, so at 144 fps its speed went 0.5^144 a second instead of 0.5^60:
// opponents stopped against walls for seconds (measured on Speed Bay, never at 60 fps). Applied to
// the halved values: x * 0.5 is exact in floats, so x * 0.5^(dt / 33.3) is computed from x.
void wall_hit(uint32_t bike) {
#ifndef __PSP__
    const uint32_t dt = odd_frame_ms(FIX_WALL);
    if (!dt) return;
    const double f = 2.0 * std::pow(0.5, dt / REF_MS);
    for (uint32_t off : {0x1a4u, 0xecu}) wrf(bike + off, static_cast<float>(rdf(bike + off) * f));
#else
    (void)bike;
#endif
}

// The player's wall contact (0x468ca0, 0x4692b3): road speed +0x124 *= 0.75 every frame of contact.
void player_wall(uint32_t bike) {
#ifndef __PSP__
    const double n = odd_frames(FIX_WALL);
    if (n > 0) wrf(bike + 0x124, static_cast<float>(rdf(bike + 0x124) * std::pow(0.75, n - 1)));
#else
    (void)bike;
#endif
}


// AI speed blend (0x46b120): speed = speed * b + L * (1 - b) every frame, b = [+0x31d1] * 0.005 (the
// blend after a hit, decaying with time) or 0.01, L the speed the bike's target velocity asks for. Over
// n 30 fps frames towards the same L that is b^n.
float speed_blend(float b) {
#ifndef __PSP__
    const double n = odd_frames(FIX_SPEED_BLEND);
    if (n > 0 && b > 0) return static_cast<float>(std::pow(static_cast<double>(b), n));
#endif
    return b;
}

// AI bike behind a slower one it follows (0x46cd20, 0x46d13e): its speed +0xec and target +0x1a4 become
// 0.95 * (e + (o - e) / 32) every frame, e = its speed, o = the other bike's. A pair following each other
// lose speed with every frame, so at 144 fps they stopped for seconds (measured: a pair on Speed Bay,
// speed and target 0.3 for 2 s; never at 60 fps). Over n 30 fps frames: the same step n times. The
// game's own one-step values stay on the x87 stack for the check that follows (|e - new| > 11.1: the
// bike brakes hard, +0x2dc8 = 2).
void follow_begin(uint32_t bike, float speed, float other) {
#ifndef __PSP__
    follow = {bike, speed, other};
#else
    (void)bike, (void)speed, (void)other;
#endif
}

void follow_end(uint32_t bike) {
#ifndef __PSP__
    const double n = odd_frames(FIX_FOLLOW);
    if (n <= 0 || bike != follow.bike) return;
    follow.bike = 0;
    const double k = rdf(0x4d1158), keep = 0.95;   // 1/32; the 0.95 is a double ([0x4d1310])
    const float v = static_cast<float>(affine_steps(follow.speed, keep * (1 - k), keep * k * follow.other, n));
    wrf(bike + 0xec, v);
    wrf(bike + 0x1a4, v);
#else
    (void)bike;
#endif
}

// Crashed rider sliding (0x4683e0, while +0x30c1 is set): every frame an AI bike's speed +0xec and
// target +0x1a4 (0x468bfc), or a human player's road speed +0x124 and +0x180 (0x468c22), are multiplied
// by 0.9 ([0x4d11b8]): at 144 fps a fallen rider stopped in less than half the distance. Applied to the
// stored products: x * 0.9^n = (x * 0.9) * 0.9^(n - 1).
namespace {
#ifndef __PSP__
uint32_t slide_player = 0;   // the player branch ran for this bike this frame (+0x180 still to scale)
#endif
}

void crash_slide(uint32_t bike, std::initializer_list<uint32_t> fields) {
#ifndef __PSP__
    const double n = odd_frames(FIX_SLIDE);
    if (n <= 0) return;
    const double f = std::pow(0.9, n - 1);
    for (uint32_t off : fields) wrf(bike + off, static_cast<float>(rdf(bike + off) * f));
#else
    (void)bike, (void)fields;
#endif
}

// Player lean (0x469c60, the player's path): the yaw between the look frame and the bike, twice the lag
// of the travel direction behind the facing (see grip), divided by the smoothed frame time but at least
// 20 ms unless +0x2e29 (analog steering), times a constant. The lag depends on the frame rate through the
// per-frame grip, the divisor through the frame time: lean after one steering tap was 0.39 at 20 fps,
// 0.53 at 30 and 60, 0.22 at 144. With the grip fixed the lag is the 30 fps lag at any frame rate, so
// the divisor keeps its 30 fps value: one 30 fps frame (above the 20 ms floor, so keyboard and analog
// alike). `div` is the game's divisor; the result is used instead.
float lean_divisor(float div, bool analog) {
#ifndef __PSP__
    if (odd_frames(FIX_LEAN_DIV) > 0 && (fix_mask() & FIX_GRIP)) return static_cast<float>(std::max(REF_MS, analog ? 0.0 : 20.0));
#else
    (void)analog;
#endif
    return div;
}

// Dirt bikes' lean (0x469c60, 0x46a893): the yaw divided by the smoothed frame time [0x4dc150]
// (no floor); for the same reason the 30 fps frame time is used. 0 = the game's own.
float dirt_lean_time() {
#ifndef __PSP__
    if (odd_frames(FIX_DIRT_LEAN) > 0) return static_cast<float>(REF_MS);
#endif
    return 0.f;
}

// Lean smoothing (0x46bf80 after 0x469c60): lean = prev + (raw - prev) * 0.5 every frame
// ([0x4d0f08]), prev = +0x2e21. Over n 30 fps frames: prev + (raw - prev) * (1 - 0.5^n); raw = 2 lean - prev.
void lean_smooth(uint32_t bike) {
#ifndef __PSP__
    const double n = odd_frames(FIX_LEAN_SMOOTH);
    if (n <= 0) return;
    const double prev = rdf(bike + 0x2e21), raw = 2.0 * rdf(bike + 0x2e1d) - prev;
    wrf(bike + 0x2e1d, static_cast<float>(prev + (raw - prev) * (1 - std::pow(0.5, n))));
#else
    (void)bike;
#endif
}

// Bike roll and landing bounce (0x466a80, every bike, every frame; m = the bike's model record,
// 0x5092b0 + 292 * model). Three per-frame steps, each made n 30 fps frames long:
//  - bounce: speed +0x2e9d *= [m + 0x24] (damping), landing speed +0x2e95 += clamp(dt, 3, 5) * 5.184
//    towards 0 (dt in 1/100 s clamped to 30..50 ms: a 30 fps frame counts as 33 ms), phase
//    +0x2e99 += +0x2e9d / [m + 0x2c]; the bike and rider bob by sin(phase) * speed (+0x2e91), at 144 fps
//    2.4 times as fast;
//  - roll: +0x2ea1 moves 1 / [m - 0x18] of the way to its target angle; the bike model is drawn turned
//    by the lag (+0x2e8d, clamped): at 144 fps it lagged less than half as much and swung back quickly.
void bounce_damp(uint32_t bike) {   // after the +0x2e9d and +0x2e95 stores
#ifndef __PSP__
    const double n = odd_frames(FIX_BOUNCE);
    if (n <= 0) return;
    const uint32_t m = 0x5092b0 + 292 * RD32(bike + 0x78);
    const double d = rdf(m + 0x24);
    if (d != 0 && std::isfinite(d)) wrf(bike + 0x2e9d, static_cast<float>(rdf(bike + 0x2e9d) / d * std::pow(d, n)));
    float dt = rdf(0x581014);
    dt = dt > 5.f ? 5.f : dt < 3.f ? 3.f : dt;
    const double k = RDF64(0x4d1128), ref = std::clamp(REF_MS / 10, 3.0, 5.0) * k;   // a 30 fps frame's step
    wrf(bike + 0x2e95, static_cast<float>(rdf(bike + 0x2e95) - dt * k + ref * n));
#else
    (void)bike;
#endif
}

void bounce_phase(uint32_t bike) {   // after the +0x2e99 store
#ifndef __PSP__
    const double n = odd_frames(FIX_BOUNCE);
    if (n <= 0) return;
    const double dc = rdf(0x5092b0 + 292 * RD32(bike + 0x78) + 0x2c);
    if (dc != 0) wrf(bike + 0x2e99, static_cast<float>(rdf(bike + 0x2e99) + (n - 1) * rdf(bike + 0x2e9d) / dc));
#else
    (void)bike;
#endif
}

void roll_follow(uint32_t bike, float diff) {   // after the +0x2ea1 store; diff = target - old angle
#ifndef __PSP__
    const double n = odd_frames(FIX_ROLL);
    if (n <= 0) return;
    const double m = rdf(0x5092b0 + 292 * RD32(bike + 0x78) - 0x18);
    if (!(m >= 1)) return;
    const double old = rdf(bike + 0x2ea1) - diff / m;
    wrf(bike + 0x2ea1, static_cast<float>(old + diff * (1 - std::pow(1 - 1 / m, n))));
#else
    (void)bike, (void)diff;
#endif
}

// Player lean target offset +0x2e39 (0x45d580): 0.01 ([0x4d0ed8]) closer to 0 every frame.
namespace {
#ifndef __PSP__
struct { uint32_t bike = 0; float value = 0; } offset, counter;
#endif
}
void offset_begin(uint32_t bike) {
#ifndef __PSP__
    offset = {bike, rdf(bike + 0x2e39)};
#else
    (void)bike;
#endif
}
void offset_end(uint32_t bike) {
#ifndef __PSP__
    const double n = odd_frames(FIX_OFFSET);
    if (n <= 0 || offset.bike != bike) return;
    offset.bike = 0;
    const double v = offset.value, step = RDF64(0x4d0ed8) * n;
    if (v > 0) wrf(bike + 0x2e39, static_cast<float>(std::max(0.0, v - step)));
    else if (v < 0) wrf(bike + 0x2e39, static_cast<float>(std::min(0.0, v + step)));
#else
    (void)bike;
#endif
}

// Player counter +0x188 (0x466d70; 1..12): +2 or -1 every frame.
void counter_begin(uint32_t bike) {
#ifndef __PSP__
    counter = {bike, rdf(bike + 0x188)};
#else
    (void)bike;
#endif
}
void counter_end(uint32_t bike) {   // after the step, before the clamps
#ifndef __PSP__
    const double n = odd_frames(FIX_COUNTER);
    if (n <= 0 || counter.bike != bike) return;
    counter.bike = 0;
    wrf(bike + 0x188, static_cast<float>(counter.value + (rdf(bike + 0x188) - counter.value) * n));
#else
    (void)bike;
#endif
}

// Player grip (physics 0x48d620, on the ground): the direction of travel +0x168 (= the bike's +0x18c)
// becomes g * itself + (1 - g) * the bike's facing every frame, g = 0.85..0.9 by speed. At 144 fps the
// travel direction caught up with the facing 2.4 times as fast: turns ended abruptly and the lean (the
// angle between the two) died within half a second where 60 fps keeps it for over a second. Over n
// 30 fps frames: g^n.
float grip(float g) {
#ifndef __PSP__
    const double n = odd_frames(FIX_GRIP);
    if (n > 0 && g > 0) return static_cast<float>(std::pow(static_cast<double>(g), n));
#endif
    return g;
}

// Player's ground contact and takeoff (0x468ca0, 0x466660): the bike keeps contact, and is put back on
// the road, while its height is at most a tolerance above the road (32 units, 8 when the function's
// argument is set: 0x469427); otherwise contact is lost, and a counter +0xf0 grows by 2.592 every frame
// without contact (0x466660; contact resets it); the physics (0x48d620) flies the bike once it exceeds
// 2.592, i.e. on the second frame without contact. A frame's step lifts the bike off a crest or a
// slope's edge in proportion to its length: at 120 fps a step lifts it a quarter as much, under the
// tolerance, and the bike is put back on the road every 8 ms: on Dirt Arena it took off 4.1 times a
// minute at 120 fps against 15.5 at 30 (the jumps the owner knows from the PSP). The tolerance and the
// counter's step are scaled by n 30 fps frames.
uint32_t ground_tolerance(uint32_t tol) {
#ifndef __PSP__
    const double n = odd_frames(FIX_GROUND);
    if (n > 0) return static_cast<uint32_t>(std::max(1L, std::lround(tol * n)));
#endif
    return tol;
}

void air_counter(uint32_t bike) {   // after the +0xf0 += 2.592 store
#ifndef __PSP__
    const double n = odd_frames(FIX_GROUND);
    if (n <= 0) return;
    const double step = RDF64(0x4d0f20);
    wrf(bike + 0xf0, static_cast<float>(rdf(bike + 0xf0) - step + step * n));
#else
    (void)bike;
#endif
}

}  // namespace game

// GAME_HOOK at 0x46943c (0x468ca0; eax = the contact tolerance, about to go to [esp+0x10]) and at
// 0x466688 (0x466660, after the air counter's store; ebx = the bike).
extern "C" void game_hook_ground_tolerance(void) { R.eax = game::ground_tolerance(R.eax); }
extern "C" void game_hook_air_counter(void) { game::air_counter(R.ebx); }

// GAME_HOOKs around the dirt bikes' fdiv by [0x4dc150] (0x46a893 .. 0x46a899): the 30 fps frame time
// for that one instruction.
namespace {
#ifndef __PSP__
uint32_t dirt_saved = 0;
bool dirt_swapped = false;
#endif
}
extern "C" void game_hook_dirt_lean_begin(void) {
#ifndef __PSP__
    const float t = game::dirt_lean_time();
    if (t > 0) {
        dirt_saved = RD32(0x4dc150), dirt_swapped = true;
        uint32_t v;
        memcpy(&v, &t, 4);
        WR32(0x4dc150, v);
    }
#endif
}
extern "C" void game_hook_dirt_lean_end(void) {
#ifndef __PSP__
    if (dirt_swapped) WR32(0x4dc150, dirt_saved), dirt_swapped = false;
#endif
}

// GAME_HOOK at 0x48da21 (g at [esp+0x10]).
extern "C" void game_hook_grip(void) {
    uint32_t v = RD32(R.esp + 0x10);
    float g;
    memcpy(&g, &v, 4);
    g = game::grip(g);
    memcpy(&v, &g, 4);
    WR32(R.esp + 0x10, v);
}

// GAME_HOOKs in the bike roll 0x466a80 (esi = the bike): 0x466aed (bounce damping), 0x466b2f (phase),
// 0x466cf6 (roll; [esp+0xc] = target - old angle).
extern "C" void game_hook_bounce_damp(void) { game::bounce_damp(R.esi); }
extern "C" void game_hook_bounce_phase(void) { game::bounce_phase(R.esi); }
extern "C" void game_hook_roll_follow(void) {
    uint32_t v = RD32(R.esp + 0xc);
    float d;
    memcpy(&d, &v, 4);
    game::roll_follow(R.esi, d);
}
// GAME_HOOKs 0x45d610 / 0x45d676 (lean target offset) and 0x467524 / 0x467562 (counter +0x188).
extern "C" void game_hook_offset_begin(void) { game::offset_begin(R.esi); }
extern "C" void game_hook_offset_end(void) { game::offset_end(R.esi); }
extern "C" void game_hook_counter_begin(void) { game::counter_begin(R.esi); }
extern "C" void game_hook_counter_end(void) { game::counter_end(R.esi); }

// GAME_HOOK at 0x46c73f (bike step, after the smoothed lean store, before +0x2e21 takes it).
extern "C" void game_hook_lean_smooth(void) { game::lean_smooth(R.esi); }

// GAME_HOOKs in the crash slide 0x4683e0 (esi = the bike): 0x468c20 at the end of the AI branch,
// 0x468c39 after the player's +0x124 store (read by the engine code right after), and 0x468c7d where
// all branches meet (the player's +0x180 store has happened).
extern "C" void game_hook_slide_ai(void) { game::crash_slide(R.esi, {0xec, 0x1a4}); }
extern "C" void game_hook_slide_player(void) {
    game::crash_slide(R.esi, {0x124});
#ifndef __PSP__
    game::slide_player = R.esi;
#endif
}
extern "C" void game_hook_slide_end(void) {
#ifndef __PSP__
    if (game::slide_player == R.esi) game::crash_slide(R.esi, {0x180});
    game::slide_player = 0;
#endif
}

// GAME_HOOKs at 0x46c084 (bike step 0x46bf80) and 0x45ea84 (the second human player's step 0x45e940),
// right after both halvings; esi = the bike.
extern "C" void game_hook_wall_hit(void) { game::wall_hit(R.esi); }
// GAME_HOOK at 0x4692d5 (the player's wall contact, after the +0x124 store; esi = the bike).
extern "C" void game_hook_player_wall(void) { game::player_wall(R.esi); }

// GAME_HOOK at 0x46b82c (the blend factor is at [esp+0x10]; esi = the bike).
extern "C" void game_hook_speed_blend(void) {
    uint32_t v = RD32(R.esp + 0x10);
    float b;
    memcpy(&b, &v, 4);
    b = game::speed_blend(b);
    memcpy(&v, &b, 4);
    WR32(R.esp + 0x10, v);
}

// GAME_HOOKs at 0x46d13e (before the ease; the other bike's speed at [esp+0x14]) and 0x46d184 (after
// both stores).
extern "C" void game_hook_follow_begin(void) {
    uint32_t v = RD32(R.esp + 0x14);
    float o;
    memcpy(&o, &v, 4);
    v = RD32(R.esi + 0xec);
    float e;
    memcpy(&e, &v, 4);
    game::follow_begin(R.esi, e, o);
}
extern "C" void game_hook_follow_end(void) { game::follow_end(R.esi); }
