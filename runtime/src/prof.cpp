// Sampling profiler for the PSP's frame time (MOTORACER_PROF=1, docs/26-psp-profile.md).
//
// The main thread marks the region it is in with one store (prof_region): Win32 / COM shims,
// Direct3D Execute, present, waits, and each native hook (generated code: PROF_IN / PROF_OUT
// around the call, a region per hook name); everything else is translated game code. A timer
// (plat::Execution::start_sampler: an alarm on the PSP, a thread elsewhere) counts the current
// region ~5,000 times per second; every 5 s the shares and the milliseconds per frame are logged.
// No clock reads on the measured paths (MOTORACER_GE_STATS-style timers inflated what they timed).
// Translated code called from inside a native hook (guest calls) counts as that hook. Only the main
// thread marks regions. SDL's audio thread flags its mixing (from the first stream callback of a
// buffer to the post-mix callback): on the PSP's single core that time counts as "audio mixing";
// elsewhere it runs in parallel and is ignored.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include <SDL3/SDL_timer.h>

#include "platform/platform.h"
#include "runtime.h"

extern "C" volatile uint8_t prof_region = 0;
extern "C" volatile uint32_t prof_fn = 0;   // RECOMP_PROF_FUNCS: the running translated function
extern "C" uint8_t native_off[256] = {};

namespace {
const char *names[256] = {"game code (translated)", "Win32 / COM shims", "D3D Execute", "present", "wait",
                          "audio mixing (audio thread)"};
uint32_t next_id = prof::FIRST_NATIVE;
volatile uint32_t samples[256];
bool on = false;

// guest functions sampled while in translated code (RECOMP_PROF_FUNCS): open addressing, filled
// from the timer, read and cleared by the game thread every 5 s
struct FnCount { uint32_t va, n; };
constexpr uint32_t FN_SLOTS = 2048;
FnCount fn_counts[FN_SLOTS];

volatile bool audio_busy = false;
volatile int audio_cpu_count = 0;   // PSP: audio threads running on the CPU (not blocked in a wait)

void tick() {
#ifdef __PSP__   // one core: while the audio thread mixes, the game is not running
    if (audio_cpu_count > 0) {
        samples[prof::AUDIO]++;
        return;
    }
#endif
    samples[prof_region]++;
#ifdef RECOMP_PROF_FUNCS
    if (prof_region == prof::GAME && prof_fn) {
        const uint32_t va = prof_fn;
        for (uint32_t i = (va * 2654435761u) >> 21, k = 0; k < 16; i = (i + 1) & (FN_SLOTS - 1), k++) {
            if (fn_counts[i].va == va) { fn_counts[i].n++; break; }
            if (!fn_counts[i].va) { fn_counts[i].va = va; fn_counts[i].n = 1; break; }
        }
    }
#endif
}
}  // namespace

extern "C" uint8_t prof_register(const char *name) {
    if (next_id >= 255) {
        names[255] = "other native hooks";
        return 255;
    }
    names[next_id] = name;
    // MOTORACER_NATIVE_OFF: a comma-separated list of native hook names to skip
    if (const char *off = getenv("MOTORACER_NATIVE_OFF")) {
        const std::string list = std::string(",") + off + ",";
        if (list.find(std::string(",") + name + ",") != std::string::npos) {
            native_off[next_id] = 1;
            rt::log("native: %s off (MOTORACER_NATIVE_OFF)\n", name);
        }
    }
    return static_cast<uint8_t>(next_id++);
}

namespace prof {

void audio_mixing(bool busy) { audio_busy = busy; }
void audio_cpu(int delta) { audio_cpu_count = audio_cpu_count + delta; }

volatile uint32_t audio_blocks = 0, audio_late = 0, audio_worst_us = 0, audio_budget_us = 0;
void audio_block(uint32_t produce_us, uint32_t wait_us, uint32_t budget_us) {
    if (!on) return;
    audio_blocks = audio_blocks + 1;
    if (wait_us < 1000) audio_late = audio_late + 1;
    if (produce_us > audio_worst_us) audio_worst_us = produce_us;
    audio_budget_us = budget_us;
}

void start() {
    const char *e = getenv("MOTORACER_PROF");
    if (!e || !*e || *e == '0') return;
    on = plat::host().execution.start_sampler(200, tick);
    rt::log("prof: sampler %s\n", on ? "started (every 200 us)" : "not available");
}

void frame() {
    rt::log_flush_queued();   // other threads' log lines (not profiling, just a regular game-thread point)
    if (!on) return;
    static uint32_t frames = 0;
    static uint64_t since = 0;
    const uint64_t now = SDL_GetTicks();
    if (!since) since = now;
    frames++;
    if (now - since < 5000) return;
    uint32_t snap[256], total = 0;
    for (int i = 0; i < 256; i++) {
        snap[i] = samples[i];
        samples[i] = 0;
        total += snap[i];
    }
    const double ms_per_frame = static_cast<double>(now - since) / frames;
    rt::log("prof: %u frames in %llu ms (%.1f ms/frame), %u samples\n", frames,
            static_cast<unsigned long long>(now - since), ms_per_frame, total);
    for (int k = 0; k < 256 && total; k++) {   // largest first, down to 0.5%
        int best = -1;
        for (int i = 0; i < 256; i++)
            if (snap[i] && (best < 0 || snap[i] > snap[best])) best = i;
        if (best < 0 || snap[best] * 200u < total) break;
        const double share = static_cast<double>(snap[best]) / total;
        rt::log("prof:   %5.1f%%  %5.2f ms  %s\n", 100 * share, share * ms_per_frame, names[best] ? names[best] : "?");
        snap[best] = 0;
    }
#ifdef RECOMP_PROF_FUNCS
    {   // the hottest translated functions, share of all samples (top 24, down to 0.3%)
        static FnCount snap_fn[FN_SLOTS];
        memcpy(snap_fn, fn_counts, sizeof fn_counts);
        memset(fn_counts, 0, sizeof fn_counts);
        std::sort(snap_fn, snap_fn + FN_SLOTS, [](const FnCount &a, const FnCount &b) { return a.n > b.n; });
        char line[512];
        int len = snprintf(line, sizeof line, "prof: hot guest functions (%%):");
        for (uint32_t i = 0; i < 24 && snap_fn[i].n * 1000u >= total * 3u && total; i++) {
            len += snprintf(line + len, sizeof line - len, " %06x %.1f", snap_fn[i].va & 0xffffff, 100.0 * snap_fn[i].n / total);
            if (len > static_cast<int>(sizeof line) - 24) break;
        }
        rt::log("%s\n", line);
    }
#endif
    if (audio_blocks) {
        rt::log("prof: audio: %u blocks, %u late (channel ran dry), slowest %.1f ms to produce (block %.1f ms)\n",
                audio_blocks, audio_late, audio_worst_us / 1000.0, audio_budget_us / 1000.0);
        audio_blocks = audio_late = audio_worst_us = 0;
    }
    frames = 0;
    since = now;
}

}  // namespace prof
