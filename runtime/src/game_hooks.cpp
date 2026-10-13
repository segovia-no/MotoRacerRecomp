// Game-specific behaviour layered on the recompiled code: callbacks the translator inserts at
// fixed instructions (GAME_HOOKS in tools/recomp/recomp/translate.py) and a per-frame tick.
//
// SCREEN MODE list (Direct3D renderer, CustomModes=1). The game cycles through a table of display
// modes it built from EnumDisplayModes (0x5d8954, 36-byte entries, count at 0x4ee984), ordered by
// area. The runtime owns that table and shows one of two curated lists:
//  - 16/9 MODE off: 4:3 modes 320x240 .. 1920x1440.
//  - 16/9 MODE on: modes whose 16:9 band is a common resolution. The game's 16/9 mode keeps a 4:3
//    display mode and renders a w x w*9/16 band into an offscreen target, so 1920x1440 renders a
//    native 1920x1080 picture. The label hook shows every entry as its band (1920*1080, and the
//    original 640x480 as 640*360).
// The original 320x240 / 512x384 / 640x480 stay in both lists: the menus and the game's own
// fallbacks set those modes and require them in the table (0x474ca0).
//
// Screen mode memory: the game saves only the size class (0 = 320x240, 1 = 512x384,
// 2 = 640x480), so a larger mode came back as 640x480 after a restart. The runtime remembers the
// chosen mode per orientation in <game dir>/recomp_state.txt and restores it after the game loads
// its options.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "game_hooks.h"
#include "runtime.h"
#include "settings.h"

using namespace rt;

namespace {

// The live mode class (0x4dc0c8, saved as "ScreenMode") and 16/9 flag (0x4dc0d4, "Mode_169")
// are forced to 2 / 0 while the 640x480 menus run (0x409f97); the menus and Options page work on
// copies that are put back when a race starts (0x40ba44). The runtime uses the copies.
constexpr uint32_t MODE_AREA = 0x4dc0cc;      // width*height of the selected mode
constexpr uint32_t MODE_INDEX = 0x4dc0d0;     // table index of the selected mode
constexpr uint32_t MODE_169 = 0x504e80;       // Options > 16/9 MODE (menu copy of 0x4dc0d4)
constexpr uint32_t OPTIONS_CLASS = 0x504bc4;  // menu copy of the mode class 0x4dc0c8
constexpr uint32_t MODE_LABEL = 0x53f3c0;     // "%d*%d" text of the Options page
constexpr uint32_t MODE_MIN = 0x4ee97c, MODE_MAX = 0x4ee980, MODE_COUNT = 0x4ee984;
constexpr uint32_t MODE_TABLE = 0x5d8954, MODE_STRIDE = 36;   // r,g,b masks, w, h, bpp, area, class
constexpr uint32_t AREA_640 = 640 * 480;
const char *kStateFile = "recomp_state.txt";

struct Mode { uint32_t w, h; };
const std::vector<Mode> kModes43 = {{320, 240},  {512, 384},   {640, 480},   {800, 600},
                                    {1024, 768}, {1280, 960},  {1600, 1200}, {1920, 1440}};
const std::vector<Mode> kModes169 = {{320, 240},  {512, 384},   {640, 480},  {1280, 960},
                                     {1920, 1440}, {2560, 1920}, {3840, 2880}};
const Mode kDefault169 = {1920, 1440};   // 1920x1080 band

Mode remembered[2];       // [0] 4:3, [1] 16:9 (internal display mode)
bool state_loaded = false;
int table_wide = -1;      // which list is in the game's table (-1: not built yet)
bool reselect = false;    // pick the remembered mode on the next tick (after an options load)
uint32_t last_area = 0;

bool active() {
    static const bool env_modes = getenv("MOTORACER_MODES") != nullptr;   // dev bisecting: hands off
    return settings::get().d3d && settings::get().custom_modes && !env_modes;
}

void load_state() {
    if (state_loaded) return;
    state_loaded = true;
    std::ifstream in(kStateFile);
    std::string line;
    while (std::getline(in, line)) {
        Mode m{};
        if (sscanf(line.c_str(), "ScreenMode43=%ux%u", &m.w, &m.h) == 2) remembered[0] = m;
        if (sscanf(line.c_str(), "ScreenMode169=%ux%u", &m.w, &m.h) == 2) remembered[1] = m;
    }
}

void save_state() {
    std::ofstream out(kStateFile);
    if (remembered[0].w) out << "ScreenMode43=" << remembered[0].w << 'x' << remembered[0].h << '\n';
    if (remembered[1].w) out << "ScreenMode169=" << remembered[1].w << 'x' << remembered[1].h << '\n';
}

uint32_t size_class(uint32_t area) { return area < 0x30000 ? 0 : area == 0x30000 ? 1 : 2; }

int find_mode(uint32_t area) {
    uint32_t n = RD32(MODE_COUNT);
    for (uint32_t i = 0; i < n && i < 50; i++)
        if (RD32(MODE_TABLE + MODE_STRIDE * i + 24) == area) return static_cast<int>(i);
    return -1;
}

Mode mode_at(int i) {
    uint32_t e = MODE_TABLE + MODE_STRIDE * static_cast<uint32_t>(i);
    return {RD32(e + 12), RD32(e + 16)};
}

void write_label(Mode m, bool wide) {
    char text[32];
    snprintf(text, sizeof text, "%u*%u", m.w, wide ? m.w * 9 / 16 : m.h);
    for (size_t i = 0; i <= strlen(text); i++) WR8(MODE_LABEL + static_cast<uint32_t>(i), static_cast<uint8_t>(text[i]));
}

void write_table(bool wide) {
    const auto &list = wide ? kModes169 : kModes43;
    for (uint32_t i = 0; i < list.size(); i++) {
        uint32_t e = MODE_TABLE + MODE_STRIDE * i, area = list[i].w * list[i].h;
        WR32(e + 0, 0xF800);
        WR32(e + 4, 0x07E0);
        WR32(e + 8, 0x001F);
        WR32(e + 12, list[i].w);
        WR32(e + 16, list[i].h);
        WR32(e + 20, 16);
        WR32(e + 24, area);
        WR32(e + 28, size_class(area));
    }
    WR32(MODE_COUNT, static_cast<uint32_t>(list.size()));
    WR32(MODE_MIN, 0);
    WR32(MODE_MAX, static_cast<uint32_t>(list.size() - 1));
    table_wide = wide ? 1 : 0;
}

void select(Mode m, bool wide) {
    uint32_t area = m.w * m.h;
    int i = find_mode(area);
    if (i < 0) return;
    WR32(MODE_AREA, area);
    WR32(MODE_INDEX, static_cast<uint32_t>(i));
    WR32(OPTIONS_CLASS, size_class(area));
    write_label(m, wide);
    last_area = area;
}

// Mode to use for an orientation: the remembered one, else the current one if the list has it,
// else 1920x1080 for 16:9 or 640x480 for 4:3.
void select_for(bool wide) {
    const Mode &r = remembered[wide ? 1 : 0];
    if (r.w && find_mode(r.w * r.h) >= 0) return select(r, wide);
    int cur = find_mode(RD32(MODE_AREA));
    if (cur >= 0 && !(wide && RD32(MODE_AREA) == AREA_640 && !r.w)) return select(mode_at(cur), wide);
    select(wide ? kDefault169 : Mode{640, 480}, wide);
}

}  // namespace

extern "C" void game_hook_screen_mode_loaded(void) {
    // The 16/9 option is read after this point, so the mode is (re)selected on the next frame.
    if (active()) reselect = true;
}

// Before sprintf(label, "%d*%d", w, h) on the Options page: in 16/9 mode show the band.
extern "C" void game_hook_mode_label(void) {
    if (!active() || !RD32(MODE_169)) return;
    WR32(R.esp + 12, RD32(R.esp + 8) * 9 / 16);
}

namespace game {

void frame_tick() {
    if (!active() || !RD32(MODE_COUNT)) return;   // before the game enumerated display modes
    load_state();
    bool wide = RD32(MODE_169) != 0;
    if (table_wide != (wide ? 1 : 0)) {
        write_table(wide);
        select_for(wide);
        log("screen mode: %s list, %ux%u\n", wide ? "16:9" : "4:3", mode_at(static_cast<int>(RD32(MODE_INDEX))).w,
            mode_at(static_cast<int>(RD32(MODE_INDEX))).h);
        reselect = false;
        return;
    }
    if (reselect) {
        reselect = false;
        select_for(wide);
        return;
    }
    uint32_t area = RD32(MODE_AREA);
    if (area == last_area) return;
    last_area = area;
    int i = find_mode(area);
    if (i < 0) return;
    Mode m = mode_at(i);
    Mode &r = remembered[wide ? 1 : 0];
    if (m.w != r.w || m.h != r.h) {
        r = m;
        save_state();
    }
}

}  // namespace game
