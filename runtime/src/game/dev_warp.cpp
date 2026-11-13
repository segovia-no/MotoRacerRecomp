// MOTORACER_WARP=<k> (dev, tests of the race end; desktop): the player's bike is carried along the track
// to k sectors before the line every lap, so that a scripted run (hold the throttle) finishes a race in
// well under a minute — e.g. a championship race to the podium (GitHub issue #12). Called from the
// pitch view hook (0x47d8bb, after the physics step, before the camera update).
//
// The bike is moved STEP sectors a frame (MOTORACER_WARP_STEP, default 10) rather than at once: the
// race's lap / checkpoint tracking only counts a lap the bike went forward through (a single jump to
// the line counted nothing). Each step moves the bike's points and turns its frames as the track turns,
// onto the centre line. Only while riding: a fallen rider has points of its own, and a bike carried
// without them fell from the sky for half a minute. Not during replays (they replay the carried race).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "runtime.h"

using namespace rt;

namespace {

#ifndef __PSP__
constexpr uint32_t PLAYER = 0x508834, BIKES = 0x581218, BIKE_SIZE = 0x3209;   // bike(i) = BIKES + i * size
constexpr uint32_t FRAME_MS = 0x4e980c, REPLAY = 0x4dc008;
constexpr uint32_t TRACK_SECTORS = 0x52c108, TRACK_COUNT = 0x52c10c, SECTOR = 252;   // sector +0: centre
constexpr uint32_t MATRIX = 0xbc, LEAN_FRAME = 0x2e55, HEADING = 0x18c, FORWARD = 0x198, POSITION = 0x3c;
constexpr uint32_t BIKE_SECTOR = 0x31c1;

struct V3 { double x, y, z; };
float rdf(uint32_t a) { uint32_t v = RD32(a); float f; memcpy(&f, &v, 4); return f; }
void wrf(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); WR32(a, v); }
V3 rd3(uint32_t a) { return {rdf(a), rdf(a + 4), rdf(a + 8)}; }
void wr3(uint32_t a, V3 v) { wrf(a, static_cast<float>(v.x)); wrf(a + 4, static_cast<float>(v.y)); wrf(a + 8, static_cast<float>(v.z)); }
double dist(V3 a, V3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
V3 rotate_y(V3 v, double angle) {   // about +y; turns atan2(x, z) by +angle
    const double c = std::cos(angle), s = std::sin(angle);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

double track_yaw(uint32_t sectors, uint32_t count, uint32_t s) {
    const V3 c0 = rd3(sectors + s * SECTOR), c1 = rd3(sectors + ((s + 1) % count) * SECTOR);
    return std::atan2(c1.x - c0.x, c1.z - c0.z);
}

// Moves the bike from sector from to sector to: onto the centre line, at its height above the sector
// centre, turned as the track turns.
void carry(uint32_t b, uint32_t sectors, uint32_t count, uint32_t from, uint32_t to) {
    const V3 c0 = rd3(sectors + from * SECTOR), c1 = rd3(sectors + to * SECTOR);
    const double angle = std::remainder(track_yaw(sectors, count, to) - track_yaw(sectors, count, from), 2 * M_PI);
    const V3 old = rd3(b + POSITION), dest{c1.x, c1.y + (old.y - c0.y), c1.z};
    auto point = [&](uint32_t a) {
        const V3 p = rd3(a);
        if (dist(p, old) > 3000) return;   // not a point of this bike
        const V3 r = rotate_y({p.x - old.x, p.y - old.y, p.z - old.z}, angle);
        wr3(a, {dest.x + r.x, dest.y + r.y, dest.z + r.z});
    };
    // position (+0x30, +0x3c), previous position, front / back points, ground points
    for (uint32_t p : {0x30u, POSITION, 0x344u, 0x1b4u, 0x338u, 0x30b5u}) point(b + p);
    for (uint32_t i = 0; i < 6; i++) point(b + 0x2ea9 + i * 12);
    for (uint32_t i = 0; i < 4; i++) point(b + 0x30ed + i * 12);
    for (uint32_t m : {MATRIX, LEAN_FRAME})
        for (uint32_t r = 0; r < 3; r++) wr3(b + m + r * 12, rotate_y(rd3(b + m + r * 12), angle));
    wr3(b + HEADING, rotate_y(rd3(b + HEADING), angle));
    wr3(b + FORWARD, rotate_y(rd3(b + FORWARD), angle));
    WR32(b + BIKE_SECTOR, to);
}

// Riding: not fallen (+0x30c1 and +0x319d are 1 until the rider is put back on the track), not
// tumbling, not airborne, moving.
bool riding(uint32_t b) {
    return RD32(b + 0x30c1) == 0 && RD32(b + 0x319d) == 0 && rdf(b + 0x307d) <= 0 && RD32(b + 0x2e85) == 0 &&
           rdf(b + 0xec) > 2;
}
#endif

}  // namespace

namespace game {

void dev_warp() {
#ifndef __PSP__
    static const int k = getenv("MOTORACER_WARP") ? atoi(getenv("MOTORACER_WARP")) : 0;
    static const uint32_t step = getenv("MOTORACER_WARP_STEP") ? std::max(1, atoi(getenv("MOTORACER_WARP_STEP"))) : 10;
    static double ridden = 0;   // ms of riding in the first half of the lap
    static bool carrying = false;
    static uint32_t frames = 0, last_bike = 0;
    if (k <= 0 || RD32(REPLAY)) return;
    const uint32_t p = RD32(PLAYER);
    if (p >= 64) return;
    const uint32_t b = BIKES + BIKE_SIZE * p;
    const uint32_t sectors = RD32(TRACK_SECTORS), count = RD32(TRACK_COUNT), cur = RD32(b + BIKE_SECTOR);
    if (!sectors || count < 8 || static_cast<uint32_t>(k) >= count || cur >= count) return;
    const uint32_t goal = count - k;
    if (b != last_bike) last_bike = b, ridden = 0, carrying = false;
    if (!riding(b)) { ridden = 0, carrying = false; return; }
    if (!carrying) {   // 3 s of riding past the line (or since the start), then carry
        if (cur >= count / 2) { ridden = 0; return; }
        ridden += std::fmin(200.0, static_cast<double>(RD32(FRAME_MS)));
        if (ridden < 3000) return;
        carrying = true, frames = 0;
        log("warp: %u sectors, bike at sector %u: carried to %u\n", count, cur, goal);
    }
    if (cur >= goal || ++frames > 2000) {
        carrying = false, ridden = 0;
        return;
    }
    carry(b, sectors, count, cur, std::min(goal, cur + step));
#endif
}

}  // namespace game
