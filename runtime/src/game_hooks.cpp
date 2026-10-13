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
#include <SDL3/SDL.h>

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
constexpr uint32_t D3D_OFF = 0x4dc070;        // Options > D3D: 1 = OFF (software renderer)
constexpr uint32_t OPTIONS_CLASS = 0x504bc4;  // menu copy of the mode class 0x4dc0c8
constexpr uint32_t MODE_LABEL = 0x53f3c0;     // "%d*%d" text of the Options page
constexpr uint32_t MODE_MIN = 0x4ee97c, MODE_MAX = 0x4ee980, MODE_COUNT = 0x4ee984;
constexpr uint32_t MODE_TABLE = 0x5d8954, MODE_STRIDE = 36;   // r,g,b masks, w, h, bpp, area, class
constexpr uint32_t AREA_640 = 640 * 480;
const char *kStateFile = "recomp_state.txt";
constexpr uint32_t TRACK_SECTIONS = 0x52c108 + 0x24;   // track sections drawn ahead (see below)
constexpr uint32_t ORIGINAL_SECTIONS = 10;
constexpr uint32_t DETAIL_TABLE = 0x51d1b8;

struct Mode { uint32_t w, h; };
const std::vector<Mode> kModes43 = {{320, 240},  {512, 384},   {640, 480},   {800, 600},
                                    {1024, 768}, {1280, 960},  {1600, 1200}, {1920, 1440}};
const std::vector<Mode> kModes169 = {{320, 240},  {512, 384},   {640, 480},  {1280, 960},
                                     {1920, 1440}, {2560, 1920}, {3840, 2880}};
const std::vector<Mode> kModesOriginal = {{320, 240}, {512, 384}, {640, 480}};
const Mode kDefault169 = {1920, 1440};   // 1920x1080 band
enum List { LIST_43 = 0, LIST_169 = 1, LIST_ORIGINAL = 2 };

Mode remembered[2];       // [0] 4:3, [1] 16:9 (internal display mode)
bool state_loaded = false;
int table_list = -1;      // which List is in the game's table (-1: not built yet)
bool reselect = false;    // pick the remembered mode on the next tick (after an options load)
bool defaults_applied = false;
bool options_loaded = false;     // the game has read its options (CONFIG.BPT)   // first-run defaults done (recomp_state.txt: Defaults=1)
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
        if (line == "Defaults=1") defaults_applied = true;
    }
}

void save_state() {
    std::ofstream out(kStateFile);
    if (remembered[0].w) out << "ScreenMode43=" << remembered[0].w << 'x' << remembered[0].h << '\n';
    if (remembered[1].w) out << "ScreenMode169=" << remembered[1].w << 'x' << remembered[1].h << '\n';
    if (defaults_applied) out << "Defaults=1\n";
}

// First run (no Defaults=1 in recomp_state.txt): a fresh GOG install starts at 640x480 4:3. Turn
// on the game's 16/9 MODE and pick 2560x1440 if the display is at least that tall, else 1920x1080. The game saves its
// options when it exits, so this happens once; later choices are the player's.
// Controller bindings: {keyboard VK, joystick input} per action at 0x53eae0, in the order of the
// game's control settings screen (Accelerate, Brake, Turn Left, Turn Right, Higher Gear, Lower Gear,
// Turbo/Stunts). Joystick inputs: -2/-3 X left/right, -4/-5 Y up/down, 0.. buttons (the runtime's
// order: A, B, X, Y, LB, RB, Back, Start, then RT/LT as 8/9 in joypad mode). The game's own default
// joystick layout is buttons 0/1 for throttle/brake and no stunt button; the preset is an Xbox 360
// layout: analog RT/LT on the Y axis, left stick steers, RB/LB shift up/down, A for stunts (Start
// already pauses, as Escape).
constexpr uint32_t BINDINGS = 0x53eae0;
constexpr uint32_t INPUT_DEVICE = 0x4dc11c;   // "InputDevice" (CONTROLLER): 2 = JOYSTICK
constexpr int32_t kFactoryJoy[7] = {0, 1, -2, -3, 2, 3, -1};
constexpr int32_t kXboxJoy[7] = {-4, -5, -2, -3, 5, 4, 0};

void apply_controller_preset() {
    if (!settings::get().controller_preset || RD32(INPUT_DEVICE) != 2) return;
    for (uint32_t i = 0; i < 7; i++)
        if (static_cast<int32_t>(RD32(BINDINGS + 8 * i + 4)) != kFactoryJoy[i]) return;   // player's own
    for (uint32_t i = 0; i < 7; i++) WR32(BINDINGS + 8 * i + 4, static_cast<uint32_t>(kXboxJoy[i]));
    log("controller: Xbox layout preset applied (RT/LT throttle/brake, LB/RB gears, A stunts)\n");
}

void apply_first_run_defaults() {
    int display_h = 1080;
    if (SDL_DisplayID d = SDL_GetPrimaryDisplay())
        if (const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(d))
            display_h = static_cast<int>(m->h * (m->pixel_density > 0 ? m->pixel_density : 1.f));
    Mode pick = kDefault169;
    if (2560 * 9 / 16 <= display_h) pick = {2560, 1920};   // 4K stays a manual choice (slower Macs)
    WR32(MODE_169, 1);
    if (!remembered[1].w) remembered[1] = pick;
    defaults_applied = true;
    save_state();
    log("first run: 16/9 MODE on, screen mode %ux%u (display %d px tall)\n", remembered[1].w, remembered[1].w * 9 / 16,
        display_h);
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

// Options > D3D: OFF (0x4dc070 = 1) is the game's software renderer. The original game offers
// modes above 640x480 only with Direct3D, and the software HUD cannot be scaled, so OFF shows the
// game's own modes.
List current_list() {
    if (RD32(D3D_OFF)) return LIST_ORIGINAL;
    return RD32(MODE_169) ? LIST_169 : LIST_43;
}

void write_table(List which) {
    const auto &list = which == LIST_169 ? kModes169 : which == LIST_43 ? kModes43 : kModesOriginal;
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
    table_list = which;
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

// Mode to use for a list: the remembered one, else the current one if the list has it, else
// 1920x1080 for 16:9 or 640x480.
void select_for(List which) {
    bool wide = which == LIST_169;
    if (which == LIST_ORIGINAL) {
        int cur = find_mode(RD32(MODE_AREA));
        return select(cur >= 0 ? mode_at(cur) : Mode{640, 480}, false);
    }
    const Mode &r = remembered[wide ? 1 : 0];
    if (r.w && find_mode(r.w * r.h) >= 0) return select(r, wide);
    int cur = find_mode(RD32(MODE_AREA));
    if (cur >= 0 && !(wide && RD32(MODE_AREA) == AREA_640 && !r.w)) return select(mode_at(cur), wide);
    select(wide ? kDefault169 : Mode{640, 480}, wide);
}

}  // namespace

// First run with a gamepad connected: CONTROLLER = JOYSTICK (analog), set right after the game read
// "InputDevice" and before it initialises the input device at startup (0x47310b -> 0x4311c0, which
// falls back to the keyboard if no joystick answers). The binding preset follows in frame_tick.
extern "C" void game_hook_input_loaded(void) {
    load_state();
    if (defaults_applied || !settings::get().controller_preset) return;
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (SDL_HasGamepad() && RD32(INPUT_DEVICE) != 2) {
        WR32(INPUT_DEVICE, 2);
        log("first run: gamepad found, CONTROLLER set to JOYSTICK\n");
    }
}

extern "C" void game_hook_screen_mode_loaded(void) {
    // The 16/9 option is read after this point, so the mode is (re)selected on the next frame.
    options_loaded = true;
    if (active()) reselect = true;
}

// Before sprintf(label, "%d*%d", w, h) on the Options page: in 16/9 mode show the band.
extern "C" void game_hook_mode_label(void) {
    if (!active() || !RD32(MODE_169) || RD32(D3D_OFF)) return;
    WR32(R.esp + 12, RD32(R.esp + 8) * 9 / 16);
}

namespace game {

void frame_tick() {
    if (settings::get().draw_distance > 1.f && RD32(TRACK_SECTIONS) == ORIGINAL_SECTIONS)
        WR32(TRACK_SECTIONS, static_cast<uint32_t>(ORIGINAL_SECTIONS * settings::get().draw_distance + 0.5f));
    if (!active() || !RD32(MODE_COUNT)) return;   // before the game enumerated display modes
    load_state();
    if (!defaults_applied && options_loaded) apply_first_run_defaults();
    if (options_loaded) apply_controller_preset();
    List which = current_list();
    if (table_list != which) {
        write_table(which);
        select_for(which);
        static const char *names[3] = {"4:3", "16:9", "original (D3D off)"};
        log("screen mode: %s list, %ux%u\n", names[which], mode_at(static_cast<int>(RD32(MODE_INDEX))).w,
            mode_at(static_cast<int>(RD32(MODE_INDEX))).h);
        reselect = false;
        return;
    }
    if (reselect) {
        reselect = false;
        select_for(which);
        return;
    }
    if (which == LIST_ORIGINAL) return;   // the game saves its own modes; keep the D3D choices
    bool wide = which == LIST_169;
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

// Draw distance (DrawDistance setting, 1 = original game).
// - Track: the track object (0x52c108) draws a fixed number of sections ahead of the camera,
//   [track + 0x24] = 10 set by its constructor (0x40c5b4) for every detail level; the visible list is
//   built by 0x40ce60.
// - Per-detail-level table (Options > DETAIL LEVELS 1..5 = index 0..4) at 0x51d1b8, 36 bytes per
//   level, read from the track configuration (loader 0x428580): LenRLPlane (scales the in-front
//   sign models' width, not a distance), LenBPlane (opponents are culled beyond 2 x LenBPlane,
//   0x458b80 -> 0x46e8b0), FlatZ, HorizFlatZ (0), MediumZone / HightZone (ints, copied into the
//   camera object; camera logic, not bike LOD), gouraudTexturedZ, texturedZ, gouraudZ (shading
//   falls back beyond these). Detail 5 originally: 3500, 18000, 8000, 0, 5500, 3500, 32000, 48000,
//   52000.
// - Opponent bike models: 0x45f160 picks the model from d = distance / FOV with hardcoded
//   thresholds: < 1000 full model, < 2500 / < 3000 reduced models, beyond 3000 a sprite.
extern "C" void game_hook_detail_loaded(void) {
    float d = settings::get().draw_distance;
    if (d <= 1.f) return;
    for (uint32_t lvl = 0; lvl < 5; lvl++) {
        uint32_t e = DETAIL_TABLE + 36 * lvl;
        WRF32(e + 4, RDF32(e + 4) * d);                                           // LenBPlane
        WRF32(e + 8, RDF32(e + 8) * d);                                           // FlatZ
        for (uint32_t f : {24u, 28u, 32u}) WRF32(e + f, RDF32(e + f) * d);        // shading distances
    }
    log("draw distance x%g: detail 5 LenBPlane %g, FlatZ %g, bike models x%g\n", d, RDF32(DETAIL_TABLE + 144 + 4),
        RDF32(DETAIL_TABLE + 144 + 8), 3.f * d);
}

// Before the bike LOD comparisons: shrink the distance so every threshold moves 3 x DrawDistance
// further out (at 2: full model to 6000, sprites from 18000). The value is also stored at
// [bike + 0x634] and read by a 900..1000 blend next to the full-model threshold, which stays matched.
extern "C" void game_hook_bike_lod(void) {
    float d = settings::get().draw_distance;
    if (d > 1.f) ST(0) /= 3.0 * d;
}
