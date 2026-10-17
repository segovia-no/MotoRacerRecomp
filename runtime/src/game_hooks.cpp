// Game-specific behaviour layered on the recompiled code: callbacks the translator inserts at
// fixed instructions (GAME_HOOKS in tools/recomp/recomp/translate.py) and a per-frame tick.
//
// SCREEN MODE list (Direct3D renderer, CustomModes=1). The game cycles through a table of display
// modes it built from EnumDisplayModes (0x5d8954, 36-byte entries, count at 0x4ee984), ordered by
// area. The runtime owns that table and shows one of two curated lists:
//  - 16/9 MODE off: 4:3 modes 320x240 .. 1920x1440.
//  - 16/9 MODE on: modes whose wide band is a common resolution. The game's 16/9 mode keeps a 4:3
//    display mode and renders a w x w*9/16 band into an offscreen target, so 1920x1440 renders a
//    native 1920x1080 picture. The label hook shows every entry as its band (1920*1080, and the
//    original 640x480 as 640*360).
//  - 16:10 entries in the same list (1280*800 for the Steam Deck, 1920*1200, 2560*1600): the band
//    height is computed once in the viewport setup (0x499b80); game_hook_band makes it w*10/16.
//    The game keys modes by area, so a 16:10 entry uses its 4:3 mode made 2 rows taller (1280x962):
//    distinct from the 16:9 entry, 0.2% off square pixels (focal lengths scale with H/480).
// The original 320x240 / 512x384 / 640x480 stay in both lists: the menus and the game's own
// fallbacks set those modes and require them in the table (0x474ca0).
//
// Screen mode memory: the game saves only the size class (0 = 320x240, 1 = 512x384,
// 2 = 640x480), so a larger mode came back as 640x480 after a restart. The runtime remembers the
// chosen mode per orientation in <game dir>/recomp_state.txt and restores it after the game loads
// its options.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "game_hooks.h"
#include "platform/platform.h"
#include "runtime.h"
#include "settings.h"
#include "user32.h"

using namespace rt;

#ifdef RECOMP_COUNT_INSNS
extern "C" uint64_t recomp_icount;
uint64_t recomp_icount = 0;
#ifdef RECOMP_COUNT_INSNS
extern "C" uint64_t recomp_icf[0x100000];
uint64_t recomp_icf[0x100000];   // per guest function (entry - 0x400000), see recomp.h
extern "C" uint32_t recomp_icv[0x100000];
uint32_t recomp_icv[0x100000];   // per instruction address
#endif
#endif

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
// 16:10 marker: 2 extra rows on the 4:3 internal mode (see the top of this file).
constexpr uint32_t ROWS_1610 = 2;
constexpr Mode wide_mode(uint32_t w, bool w1610) { return {w, w * 3 / 4 + (w1610 ? ROWS_1610 : 0)}; }
bool is_1610(Mode m) { return m.h == m.w * 3 / 4 + ROWS_1610; }
uint32_t band_height(Mode m) { return is_1610(m) ? m.w * 10 / 16 : m.w * 9 / 16; }
const std::vector<Mode> kModes169Base = {{320, 240},  {512, 384},   {640, 480},
                                     wide_mode(1280, false), wide_mode(1280, true),
                                     wide_mode(1920, false), wide_mode(1920, true),
                                     wide_mode(2560, false), wide_mode(2560, true),
                                     wide_mode(3840, false)};
// MOTORACER_PSP_PROFILE=1 (dev, docs/21-psp-feasibility.md): a 480x360 entry whose 16:9 band is
// 480x270, the PSP screen; selected with 16/9 MODE on and never saved to recomp_state.txt.
bool psp_profile() {
#ifdef __PSP__
    return true;   // the PSP build always renders in this mode (GE target 480x360, docs/25)
#else
    static const bool on = getenv("MOTORACER_PSP_PROFILE") != nullptr;
    return on;
#endif
}
const std::vector<Mode> &modes169() {
    static const std::vector<Mode> list = [] {
        std::vector<Mode> l = kModes169Base;
        if (psp_profile()) l.insert(l.begin() + 1, Mode{480, 360});   // ordered by area
        return l;
    }();
    return list;
}
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
    if (psp_profile()) return;
    std::ofstream out(kStateFile);
    if (remembered[0].w) out << "ScreenMode43=" << remembered[0].w << 'x' << remembered[0].h << '\n';
    if (remembered[1].w) out << "ScreenMode169=" << remembered[1].w << 'x' << remembered[1].h << '\n';
    if (defaults_applied) out << "Defaults=1\n";
}

// First run (no Defaults=1 in recomp_state.txt): a fresh GOG install starts at 640x480 4:3. Turn
// on the game's 16/9 MODE and pick the largest band that fits the display, in the display's
// shape (16:10 if it is nearer 1.6 than 1.78): Steam Deck 1280x800, 1080p 1920x1080, 1440p
// 2560x1440; at least 1280 wide, 4K stays a manual choice (slower machines). The game saves its
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
// PSP (docs/25): no analog triggers — Cross accelerates, Square brakes (the usual PSP racing layout),
// the nub / D-pad steer, R / L shift up / down, Circle for stunts / turbo; Start pauses, Select
// toggles the speedometer (Back, see speedometer_button).
constexpr int32_t kPspJoy[7] = {0, 2, -2, -3, 5, 4, 1};

void apply_controller_preset() {
    if (!settings::get().controller_preset || RD32(INPUT_DEVICE) != 2) return;
    for (uint32_t i = 0; i < 7; i++)
        if (static_cast<int32_t>(RD32(BINDINGS + 8 * i + 4)) != kFactoryJoy[i]) return;   // player's own
#ifdef __PSP__
    for (uint32_t i = 0; i < 7; i++) WR32(BINDINGS + 8 * i + 4, static_cast<uint32_t>(kPspJoy[i]));
    log("controller: PSP layout applied (Cross/Square throttle/brake, nub/D-pad steer, R/L gears, Circle stunts)\n");
#else
    for (uint32_t i = 0; i < 7; i++) WR32(BINDINGS + 8 * i + 4, static_cast<uint32_t>(kXboxJoy[i]));
    log("controller: Xbox layout preset applied (RT/LT throttle/brake, LB/RB gears, A stunts)\n");
#endif
}

void apply_first_run_defaults() {
    uint32_t dw = 1920, dh = 1080;
    if (SDL_DisplayID d = SDL_GetPrimaryDisplay())
        if (const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(d)) {
            float density = m->pixel_density > 0 ? m->pixel_density : 1.f;
            dw = static_cast<uint32_t>(m->w * density);
            dh = static_cast<uint32_t>(m->h * density);
        }
    bool w1610 = dw * 100 < dh * 169;
    Mode pick = wide_mode(1280, w1610);
    for (uint32_t w : {1920u, 2560u})
        if (w <= dw && band_height(wide_mode(w, w1610)) <= dh) pick = wide_mode(w, w1610);
    WR32(MODE_169, 1);
    if (!remembered[1].w) remembered[1] = pick;
    defaults_applied = true;
    save_state();
    log("first run: 16/9 MODE on, screen mode %ux%u (display %ux%u px)\n", remembered[1].w,
        band_height(remembered[1]), dw, dh);
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
    snprintf(text, sizeof text, "%u*%u", m.w, wide ? band_height(m) : m.h);
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
    const auto &list = which == LIST_169 ? modes169() : which == LIST_43 ? kModes43 : kModesOriginal;
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

// Right after the game read "InputDevice" (0x4dc11c) and before it initialises the input device
// (0x47310b -> 0x4311c0, which falls back to the keyboard if no joystick answers):
// - wait for the gamepad when one is expected: Steam Input creates its virtual pad just after the
//   game started (Steam Deck: seen in the first on-device test, the game fell back to KEYBOARD);
// - first run with a gamepad, or on a Steam Deck: CONTROLLER = JOYSTICK (analog). The binding
//   preset follows in frame_tick.
extern "C" void game_hook_input_loaded(void) {
    load_state();
    bool deck = plat::host().info.is_steam_deck();
    bool first = !defaults_applied && settings::get().controller_preset;
    if (deck || RD32(INPUT_DEVICE) == 2) {
        uint64_t t0 = SDL_GetTicks();
        bool found = w32::wait_for_gamepad(deck ? 5000 : 1000);
        log("gamepad: %s after %llu ms\n", found ? "ready" : "none", static_cast<unsigned long long>(SDL_GetTicks() - t0));
    }
    if (first && (SDL_HasGamepad() || deck) && RD32(INPUT_DEVICE) != 2) {
        WR32(INPUT_DEVICE, 2);
        log("first run: %s, CONTROLLER set to JOYSTICK\n", deck ? "Steam Deck" : "gamepad found");
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
    WR32(R.esp + 12, band_height({RD32(R.esp + 8), RD32(R.esp + 12)}));
}

// Viewport setup in 16/9 mode, eax = band height just rounded from (W/16*9) * 4/3 * H/W; ebp = W,
// edi = H of the display mode. A 16:10 entry gets a W*10/16 band; everything after (viewport,
// centre, bar offset 0x6d1040) derives from it.
extern "C" void game_hook_band(void) {
    if (!active()) return;
    Mode m{R.ebp, R.edi};
    if (is_1610(m)) R.eax = band_height(m);
}

// Speedometer: F7 in a race (key handler 0x431250, case at 0x432107) cycles "CounterType" 1 -> 2 ->
// hidden ("ShowCounter" = 0) -> 1. Both are options, saved to CONFIG.BPT on exit, so a hidden
// speedometer stays hidden in every later race. A gamepad has no F7: Back (View on Xbox / Steam Deck)
// sends it during races unless the player bound Back to a game action (joystick input 6).
constexpr uint32_t SHOW_COUNTER = 0x4f0dc8, COUNTER_TYPE = 0x4f0dcc;
constexpr int32_t kJoyBack = 6;
uint64_t race_hud_ms = 0;

extern "C" void game_hook_counter(void) {
    race_hud_ms = SDL_GetTicks();
    static uint32_t logged = ~0u;
    uint32_t state = RD32(SHOW_COUNTER) ? RD32(COUNTER_TYPE) : 0;
    if (state == logged) return;
    logged = state;
    if (state) log("speedometer: style %u (F7 / gamepad Back cycles it)\n", state);
    else log("speedometer: hidden (F7 / gamepad Back shows it)\n");
}

// Player bike lean (0x469c60, path at 0x46ac1b; replays take another path). The lean is
// x / divisor, x = a per-frame change (it scales with the frame time: measured 0.75 at 30 fps, 0.40
// at 60 fps in the same turn) and divisor = max(smoothed frame time 0x4dc150, 20 ms), or the
// smoothed time alone when +0x2e29 is set. The 20 ms floor makes the lean shrink with the frame time
// above 50 fps (measured: ~0.36 at ~85 fps vs 0.68 at 60 fps; 144 fps would give 40% of the 60 fps
// lean). The floor is scaled as it acts at 60 fps, GOG's default: max(t, min(20, 1.2 t)), t = the
// smoothed time. 60 fps and below look as before; faster rates lean as at 60 fps.
// (Dividing by this frame's own time instead jittered: x does not belong to the same frame's time.)
constexpr uint32_t SMOOTHED_FRAME_MS = 0x4dc150;
extern "C" void game_hook_lean(void) {
    if (RD32(R.esi + 0x2e29)) return;   // no floor on this path
    uint32_t bits = RD32(SMOOTHED_FRAME_MS);
    float t;
    memcpy(&t, &bits, 4);
    if (!(t > 0)) return;
    float div = std::max(t, std::min(20.f, 1.2f * t));
    memcpy(&bits, &div, 4);
    WR32(R.esp + 0x10, bits);
}

// AI bike speed (0x467930, bikes with +8 = 0, i.e. not driven by the player's physics): the speed
// +0x118 follows the bike's measured motion through
//     accel = clamp(((measured - speed) / dt * 100 + accel) * 0.5, -50, 40)     (+0x120)
//     speed += trunc(accel * dt * 0.01)                                         (_ftol truncates)
// with dt = the frame time in hundredths of a second (0x581014). At 30 fps (dt 3.3, the game's
// default cap) a step is 1; above 40 fps (GOG passes -FrameRateMax60, dt 1.6) every step truncates
// to 0: the opponents' speed stays at its start value, their gearbox in 1st and their rpm constant,
// so their engine sound (driven by rpm, docs/19-engine-sounds.md) only hums. The step is recomputed
// with the fraction carried per bike, which makes it frame-rate independent. The carry restarts when
// a bike was not updated for half a second (a new race, a replay, a pause), so a replay starts from
// the same state. (Restarting it whenever the speed was at its start value 2 kept the opponents
// stuck for seconds at 60 fps: one frame's step is at most 0.64, so it never added up there.)
constexpr uint32_t BIKES = 0x584421, BIKE_STRIDE = 0x3209, BIKE_DT = 0x581014;
// Race setup, after the opponent count (0x542874) was chosen: MOTORACER_OPPONENTS=<0..23> (dev,
// PSP profiling) overrides it. -NbComp<n> only applies to another start path (0x40b924).
extern "C" void game_hook_opponents(void) {
    static const char *env = getenv("MOTORACER_OPPONENTS");
    if (env) WR32(0x542874, static_cast<uint32_t>(std::clamp(atoi(env), 0, 23)));
}

extern "C" void game_hook_ai_speed(void) {
    static float carry[32];
    static uint64_t seen[32];
    uint32_t bike = R.esi;
    if (bike < BIKES || (bike - BIKES) % BIKE_STRIDE) return;
    uint32_t i = (bike - BIKES) / BIKE_STRIDE;
    if (i >= 32) return;
    int32_t old = static_cast<int32_t>(R.edi);
    uint64_t now = SDL_GetTicks();
    if (now - seen[i] > 500) carry[i] = 0;
    seen[i] = now;
    uint32_t a = RD32(bike + 0x120), d = RD32(BIKE_DT);
    float accel, dt;
    memcpy(&accel, &a, 4);
    memcpy(&dt, &d, 4);
    float step = accel * dt * 0.01f + carry[i];
    int32_t whole = static_cast<int32_t>(step);   // truncates, like the game
    carry[i] = step - static_cast<float>(whole);
    R.eax = static_cast<uint32_t>(old + whole);
}

namespace game {

bool first_run() {
    load_state();
    return !defaults_applied;
}

// Record page: page 9 (per frame 0x44d540) takes up to 3 initials from WM_CHAR in solo races
// (0x4dbffc), count at 0x540070. The page manager [0x4dc0c4] keeps the current page at +0x3c.
bool initials_entry_active() {
    uint32_t pages = RD32(0x4dc0c4);
    return pages && RD32(pages + 0x3c) == 9 && RD32(0x4dbffc);
}

int initials_count() { return static_cast<int>(RD32(0x540070)); }

int preferred_frame_width() {
    load_state();
    return static_cast<int>(std::max(remembered[0].w, remembered[1].w));
}

bool speedometer_button() {
    if (SDL_GetTicks() - race_hud_ms > 300) return false;
    for (uint32_t i = 0; i < 7; i++)
        if (static_cast<int32_t>(RD32(BINDINGS + 8 * i + 4)) == kJoyBack) return false;
    return true;
}

void frame_tick() {
    static const bool mem_stats = getenv("MOTORACER_MEM_STATS") != nullptr;
    if (mem_stats) {
        static uint64_t last = 0;
        if (SDL_GetTicks() - last >= 5000) { last = SDL_GetTicks(); memory_stats_log(); }
    }
#ifdef RECOMP_COUNT_INSNS
    // MOTORACER_ICOUNT=1: executed x86 instructions per frame, averaged over 2 s
    static const bool icount_log = getenv("MOTORACER_ICOUNT") != nullptr;
    if (icount_log) {
        static uint64_t last_count = 0, last_ms = 0, frames = 0;
        uint64_t now = SDL_GetTicks();
        if (!last_ms) last_ms = now;
        if (++frames, now - last_ms >= 2000) {
            log("icount: %.2f M x86 instructions/frame (%llu frames, %u opponents)\n",
                (recomp_icount - last_count) / 1e6 / frames, (unsigned long long)frames, RD32(0x542874));
            last_count = recomp_icount;
            last_ms = now;
            frames = 0;
        }
    }
    // MOTORACER_ICOUNT_DUMP=<dir>: every 5 s (MOTORACER_ICOUNT_DUMP_MS), <dir>/ic_<ms>.txt with the
    // cumulative per-function counts ("frames <n>" then "<entry va> <count>"); tools/icprof.py ranks
    // the difference of two. (Also at every file open with MOTORACER_IO_STATS: icount_dump.)
    if (getenv("MOTORACER_ICOUNT_DUMP")) {
        static uint64_t last_dump = 0;
        static const uint64_t every = getenv("MOTORACER_ICOUNT_DUMP_MS") ? strtoull(getenv("MOTORACER_ICOUNT_DUMP_MS"), nullptr, 10) : 5000;
        icount_frames++;
        uint64_t now = SDL_GetTicks();
        if (now - last_dump >= every) {
            last_dump = now;
            rt::icount_dump("");
        }
    }
#endif
    if (settings::get().draw_distance > 1.f && RD32(TRACK_SECTIONS) == ORIGINAL_SECTIONS)
        WR32(TRACK_SECTIONS, static_cast<uint32_t>(ORIGINAL_SECTIONS * settings::get().draw_distance + 0.5f));
    if (!active() || !RD32(MODE_COUNT)) return;   // before the game enumerated display modes
    load_state();
    if (!defaults_applied && options_loaded) apply_first_run_defaults();
    if (options_loaded) apply_controller_preset();
    if (options_loaded && psp_profile() && remembered[1].w != 480) {
        WR32(MODE_169, 1);
        remembered[1] = {480, 360};
        reselect = true;
        log("PSP profile: 16/9 MODE, 480x360 (480x270 band)\n");
    }
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


#ifdef RECOMP_COUNT_INSNS
#endif
void rt::icount_dump(const char *tag) {
#ifdef RECOMP_COUNT_INSNS
    static const char *dir = getenv("MOTORACER_ICOUNT_DUMP");
    if (!dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/ic_%07llu%s%s.txt", dir, (unsigned long long)SDL_GetTicks(), *tag ? "_" : "", tag);
    if (FILE *f = fopen(path, "w")) {
        fprintf(f, "frames %llu\n", (unsigned long long)icount_frames);
        for (uint32_t i = 0; i < 0x100000; i++)
            if (recomp_icf[i]) fprintf(f, "%x %llu\n", 0x400000 + i, (unsigned long long)recomp_icf[i]);
        fclose(f);
    }
    // per instruction address: <name>.va ("<va> <count>"), tools/icprof.py --va <function>
    snprintf(path, sizeof path, "%s/ic_%07llu%s%s.va", dir, (unsigned long long)SDL_GetTicks(), *tag ? "_" : "", tag);
    if (FILE *f = fopen(path, "w")) {
        for (uint32_t i = 0; i < 0x100000; i++)
            if (recomp_icv[i]) fprintf(f, "%x %u\n", 0x400000 + i, recomp_icv[i]);
        fclose(f);
    }
#else
    (void)tag;
#endif
}
