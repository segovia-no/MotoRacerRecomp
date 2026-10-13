// Game-specific behaviour layered on the recompiled code: callbacks the translator inserts at
// fixed instructions (GAME_HOOKS in tools/recomp/recomp/translate.py) and a per-frame tick.
//
// Screen mode memory: the game saves only the SCREEN MODE *size class* (0 = 320x240,
// 1 = 512x384, 2 = 640x480) and maps it back to a mode area (w*h at 0x4dc0cc) on load, so a mode
// above 640x480 comes back as 640x480 after a restart. The runtime remembers the chosen mode in
// <game dir>/recomp_state.txt and restores it right after the game loads its options.
#include <cstdio>
#include <fstream>
#include <string>

#include "game_hooks.h"
#include "runtime.h"
#include "settings.h"

using namespace rt;

namespace {

constexpr uint32_t MODE_CLASS = 0x4dc0c8;   // 0..2, saved by the game as "ScreenMode"
constexpr uint32_t MODE_AREA = 0x4dc0cc;    // width*height of the selected mode
constexpr uint32_t MODE_COUNT = 0x4ee984;   // entries in the game's display-mode table
constexpr uint32_t MODE_W = 0x5d8960, MODE_H = 0x5d8964, MODE_STRIDE = 36;
constexpr uint32_t AREA_640 = 640 * 480;
const char *kStateFile = "recomp_state.txt";

uint32_t saved_w = 0, saved_h = 0;
uint32_t last_area = 0;
bool tracking = false;   // starts after the options were loaded

void load_state() {
    std::ifstream in(kStateFile);
    std::string line;
    while (std::getline(in, line)) sscanf(line.c_str(), "ScreenMode=%ux%u", &saved_w, &saved_h);
}

void save_state(uint32_t w, uint32_t h) {
    std::ofstream out(kStateFile);
    if (w && h) out << "ScreenMode=" << w << 'x' << h << '\n';
    saved_w = w;
    saved_h = h;
}

// Index of a mode in the game's table whose area is `area`, or -1.
int find_mode(uint32_t area) {
    uint32_t n = RD32(MODE_COUNT);
    for (uint32_t i = 0; i < n && i < 50; i++)
        if (RD32(MODE_W + MODE_STRIDE * i) * RD32(MODE_H + MODE_STRIDE * i) == area) return static_cast<int>(i);
    return -1;
}

}  // namespace

extern "C" void game_hook_screen_mode_loaded(void) {
    load_state();
    uint32_t area = saved_w * saved_h;
    if (area > AREA_640 && RD32(MODE_CLASS) == 2 && settings::get().d3d && settings::get().custom_modes &&
        find_mode(area) >= 0) {
        WR32(MODE_AREA, area);
        log("screen mode: restored %ux%u\n", saved_w, saved_h);
    }
    last_area = RD32(MODE_AREA);
    tracking = true;
}

namespace game {

void frame_tick() {
    if (!tracking) return;
    uint32_t area = RD32(MODE_AREA);
    if (area == last_area) return;
    last_area = area;
    int i = find_mode(area);
    if (area > AREA_640 && i >= 0) {
        uint32_t w = RD32(MODE_W + MODE_STRIDE * i), h = RD32(MODE_H + MODE_STRIDE * i);
        if (w != saved_w || h != saved_h) save_state(w, h);
    } else if (saved_w) {
        save_state(0, 0);   // back to one of the game's own modes, which it saves itself
    }
}

}  // namespace game
