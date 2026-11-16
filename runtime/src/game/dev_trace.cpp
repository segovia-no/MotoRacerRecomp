// MOTORACER_FRAME_TRACE=1 (dev, desktop; frame-rate bugs): race motion in the log, from the pitch view
// hook (0x47d8bb, after the physics step, every race frame, not in replays).
//   every 500 ms of race time: frames and their frame times (min / max ms), the player's lean (mean,
//     peak |lean| and how far it travelled per second: sum of |change| / time) and speed (+0x118, km/h), and
//     per opponent its track sector / speed (+0x118);
//   the player's time in the air (0x48d620's air branch) and takeoffs since the start, the same for
//   all opponents together (+0x2e85);
//   "stall": an active opponent that has not left its track sector for 1.5 s of race time while not
//     fallen, and "moving" when it does again.
// Race time adds up the game's own frame times (0x4e980c), so a pause stops it.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "runtime.h"

using namespace rt;

namespace game {

float dbg_lean_ev, dbg_lean_div, dbg_lean_a, dbg_lean_b;   // the native lean 0x469c60 (player path)

void dev_trace() {
#ifndef __PSP__
    static const int on = getenv("MOTORACER_FRAME_TRACE") ? atoi(getenv("MOTORACER_FRAME_TRACE")) : 0;
    if (!on || RD32(0x4dc008)) return;   // replay
    constexpr uint32_t BIKES = 0x581218, SIZE = 0x3209, COUNT = 0x509bac, FRAME_MS = 0x4e980c, SMOOTHED = 0x4dc150;
    constexpr int N = 32;
    auto rdf = [](uint32_t a) { uint32_t v = RD32(a); float f; memcpy(&f, &v, 4); return f; };
    static double clock = 0, window = 0, lean_sum = 0, lean_travel = 0, last_lean = 0, lean_peak = 0, speed_sum = 0;
    static bool started = false;   // stalls count once the player has left its start sector
    static uint32_t start_sector = ~0u;
    static double air_ms = 0;   // the player's time in the air and takeoffs since the start (whole race)
    static uint32_t takeoffs = 0;
    static bool was_air = false;
    static double ai_air_ms = 0;   // all opponents together
    static uint32_t ai_takeoffs = 0;
    static uint32_t frames = 0, dt_min = 1000, dt_max = 0, wall_hits = 0, spans_max = 0, verts_max = 0, nodes_max = 0, recs_max = 0;
    static uint32_t sector[N];
    static double since[N];
    static bool stalled[N];
    const uint32_t dt = RD32(FRAME_MS), count = RD32(COUNT);
    if (dt > 200) return;
    if (dt == 0 || count == 0 || count > N) return;
    clock += dt, window += dt, frames++;
    spans_max = std::max(spans_max, RD32(0x65e1d4));   // span pool entries used by the last frame (8000)
    verts_max = std::max(verts_max, RD32(0x68b014));   // transformed vertices of the last frame (pool: 8000)
    nodes_max = std::max(nodes_max, RD32(0x655d14));   // render nodes (limit 0x1d4b)
    recs_max = std::max(recs_max, RD32(0x65e1c8));     // clipped triangle records 0x62ebf0
    dt_min = dt < dt_min ? dt : dt_min, dt_max = dt > dt_max ? dt : dt_max;
    // bikes are records 1..count (record 0 is unused); a record past them can hold a stale +8, so the
    // player is found by its index
    const uint32_t player = RD32(0x508834);
    for (uint32_t i = 0; i <= count && i < N; i++) {
        const uint32_t b = BIKES + i * SIZE;
        if (i == player) {
            const double lean = rdf(b + 0x2e1d);
            lean_sum += lean, lean_travel += std::fabs(lean - last_lean), last_lean = lean;
            lean_peak = std::fmax(lean_peak, std::fabs(lean));
            const int32_t speed = static_cast<int32_t>(RD32(b + 0x118));   // km/h
            speed_sum += speed;
            if (start_sector == ~0u) start_sector = RD32(b + 0x31c1);
            started = started || RD32(b + 0x31c1) != start_sector;
            // in the air: the physics (0x48d620, physics object = bike + 0x24) takes its air branch when
            // [+0xcc] > [0x4d1c18] (bike + 0xf0: the height above the ground)
            const bool air = rdf(b + 0xf0) > RDF64(0x4d1c18);
            if (started && air) air_ms += dt;
            if (started && air && !was_air) takeoffs++;
            was_air = air;
            if (on >= 2) {
                log("L %.0f %u %.4f %d %.4f %08x %.4f %08x %.4f %.4f\n", clock, dt, lean, static_cast<int32_t>(RD32(b - 0x18)),
                    rdf(b + 0x2e8d), RD32(b + 0x118), std::atan2(rdf(b + 0x18c), rdf(b + 0x194)), RD32(b - 0x20),
                    rdf(b + 0x2e91), rdf(b + 0x2ea1));
                log("T %.0f %.4f %.4f %.4f %.4f %.4f %.4f %.5f %.3f %.4f %.4f\n", clock, rdf(b + 0x110), rdf(b + 0x10c), rdf(b + 0xf8),
                    rdf(b + 0x124), std::atan2(rdf(b + 0xd4), rdf(b + 0xdc)), rdf(b + 0x100), dbg_lean_ev, dbg_lean_div,
                    dbg_lean_a, dbg_lean_b);
            }
            continue;
        }
        if (!RD32(b + 0x68)) continue;
        wall_hits += RD32(b + 0x2dac) & 1;
        {   // opponents in the air (+0x2e85) and takeoffs since the start
            static bool ai_was_air[N];
            const bool air = RD32(b + 0x2e85) != 0;
            if (started && air) ai_air_ms += dt;
            if (started && air && !ai_was_air[i]) ai_takeoffs++;
            ai_was_air[i] = air;
        }
        const uint32_t s = RD32(b + 0x31c1);
        const bool fallen = RD32(b + 0x30c1) != 0;
        static bool was_fallen[N];
        if (fallen != was_fallen[i]) {
            was_fallen[i] = fallen;
            if (fallen && started)
                log("trace: t=%.1f bike %u fell at sector %u, speed %.1f, pos %.0f %.0f %.0f\n", clock / 1000, i, s,
                    rdf(b + 0xec), rdf(b + 0x30), rdf(b + 0x34), rdf(b + 0x38));
        }
        if (s != sector[i] || fallen || !started || clock < since[i]) {
            if (stalled[i] && !fallen)
                log("trace: t=%.1f bike %u moving again after %.1f s (sector %u)\n", clock / 1000, i,
                    (clock - since[i]) / 1000, s);
            sector[i] = s, since[i] = clock, stalled[i] = false;
        } else if (stalled[i] && on >= 2) {   // per frame while stuck
            log("S %u %.0f %u pos %.0f %.0f %.0f spd %.1f tgt %.1f ai %d air %u gnd %u fall %u tumble %.2f 2dac %x hd %.3f %.3f %.3f\n",
                i, clock, s, rdf(b + 0x30), rdf(b + 0x34), rdf(b + 0x38), rdf(b + 0xec), rdf(b + 0x1a4),
                static_cast<int32_t>(RD32(b + 0x118)), RD32(b + 0x2e85), RD32(b + 0x2db8), RD32(b + 0x30c1),
                rdf(b + 0x307d), RD32(b + 0x2dac), rdf(b + 0x18c), rdf(b + 0x190), rdf(b + 0x194));
            if (stalled[i]) {   // its neighbours on the track: index, sector, +0x114, +0x31d9
                char nb[256];
                int n = snprintf(nb, sizeof nb, "N %u own114 %d d9 %d:", i, static_cast<int32_t>(RD32(b + 0x114)),
                                 static_cast<int32_t>(RD32(b + 0x31d9)));
                for (uint32_t j = 0; j < count && n < 220; j++) {
                    const uint32_t o = BIKES + j * SIZE;
                    const int32_t d = static_cast<int32_t>(RD32(o + 0x31c1)) - static_cast<int32_t>(s);
                    if (j != i && d >= -12 && d <= 12)
                        n += snprintf(nb + n, sizeof nb - n, " %u@%d/%d/%d", j, d, static_cast<int32_t>(RD32(o + 0x114)),
                                      static_cast<int32_t>(RD32(o + 0x31d9)));
                }
                log("%s\n", nb);
            }
        }
        if (s == sector[i] && started && !fallen && !stalled[i] && clock - since[i] > 1500) {
            stalled[i] = true;
            log("trace: t=%.1f bike %u stall at sector %u, ai speed %d, pos %.0f %.0f %.0f\n", clock / 1000, i, s,
                static_cast<int32_t>(RD32(b + 0x118)), rdf(b + 0x30), rdf(b + 0x34), rdf(b + 0x38));
        }
    }
    if (window < 500) return;
    char ai[512];
    int len = 0;
    for (uint32_t i = 0; i <= count && i < N && len < 440; i++) {
        const uint32_t b = BIKES + i * SIZE;
        if (i == player || !RD32(b + 0x68)) continue;
        len += snprintf(ai + len, sizeof ai - len, " %u:%u/%d", i, RD32(b + 0x31c1), static_cast<int32_t>(RD32(b + 0x118)));
    }
    ai[len] = 0;
    {
        static int once = 0;
        if (once++ < 3) {
            const uint32_t p = RD32(0x508834);
            log("trace: player index %u, speedometer %d, bike0 +8 %u +0x68 %u, 0x581218 +8 %u +0x68 %u +0x118 %d\n", p,
                static_cast<int32_t>(RD32(0x581330 + 12809 * p)), RD32(BIKES + 8), RD32(BIKES + 0x68), RD32(0x581218 + 8),
                RD32(0x581218 + 0x68), static_cast<int32_t>(RD32(0x581218 + 0x118)));
        }
    }
    log("trace: t=%.1f %u frames dt %u..%u sm %.2f | lean %.3f peak %.3f travel %.2f/s speed %.0f | air %.0f ms takeoffs %u | ai air %.0f ms takeoffs %u | wall hits %u | spans %u rows %u verts %u nodes %u recs %u | ai%s\n",
        clock / 1000, frames, dt_min, dt_max, rdf(SMOOTHED), lean_sum / frames, lean_peak, lean_travel / window * 1000,
        speed_sum / frames, air_ms, takeoffs, ai_air_ms, ai_takeoffs, wall_hits, spans_max, RD32(0x68b008), verts_max, nodes_max, recs_max, ai);
    window = lean_sum = lean_travel = lean_peak = speed_sum = 0, frames = wall_hits = spans_max = verts_max = nodes_max = recs_max = 0, dt_min = 1000, dt_max = 0;
#endif
}

}  // namespace game
