// Game-specific behaviour layered on the recompiled code: callbacks the translator inserts at
// fixed instructions (GAME_HOOKS in recompiler/recomp/translate.py) and a per-frame tick.
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
#include <string>
#include <vector>

#include "game/game_hooks.h"
#include "platform/platform.h"
#include "core/textio.h"
#include "runtime.h"
#include "game/settings.h"
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
// MOTORACER_PSP_PROFILE=1 (dev, docs/notes/21-psp-feasibility.md): a 480x360 entry whose 16:9 band is
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
bool pad_seen = false;    // a controller answered at an earlier start (recomp_state.txt: Gamepad=1)
bool options_loaded = false;     // the game has read its options (CONFIG.BPT)   // first-run defaults done (recomp_state.txt: Defaults=1)
uint32_t last_area = 0;

bool active() {
    static const bool env_modes = getenv("MOTORACER_MODES") != nullptr;   // dev bisecting: hands off
    return settings::get().d3d && settings::get().custom_modes && !env_modes;
}

void load_state() {
    if (state_loaded) return;
    state_loaded = true;
    std::vector<std::string> lines;
    rt::read_lines(kStateFile, lines);
    for (std::string &line : lines) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Mode m{};
        if (sscanf(line.c_str(), "ScreenMode43=%ux%u", &m.w, &m.h) == 2) remembered[0] = m;
        if (sscanf(line.c_str(), "ScreenMode169=%ux%u", &m.w, &m.h) == 2) remembered[1] = m;
        if (line == "Defaults=1") defaults_applied = true;
        if (line == "Gamepad=1") pad_seen = true;
    }
}

void save_state() {
    if (psp_profile()) return;
    std::string out;
    if (remembered[0].w) out += "ScreenMode43=" + std::to_string(remembered[0].w) + "x" + std::to_string(remembered[0].h) + "\n";
    if (remembered[1].w) out += "ScreenMode169=" + std::to_string(remembered[1].w) + "x" + std::to_string(remembered[1].h) + "\n";
    if (defaults_applied) out += "Defaults=1\n";
    if (pad_seen) out += "Gamepad=1\n";
    rt::write_file(kStateFile, out);
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
    auto bound = [](const int32_t *layout) {
        for (uint32_t i = 0; i < 7; i++)
            if (static_cast<int32_t>(RD32(BINDINGS + 8 * i + 4)) != layout[i]) return false;
        return true;
    };
#ifdef __PSP__
    // also over the desktop's Xbox preset (a config copied from a PC: throttle on triggers the PSP lacks)
    if (!bound(kFactoryJoy) && !bound(kXboxJoy)) return;   // the player's own bindings
#else
    if (!bound(kFactoryJoy)) return;   // the player's own bindings
#endif
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
// (0x47310b -> 0x4311c0, which falls back to the keyboard if no joystick answers, and the game saves
// that fallback in CONFIG.BPT at exit, so one slow start used to stick):
// - wait for the controller: SDL finds some only while events are pumped (Steam Input's virtual pad
//   on the Steam Deck; PlayStation / Switch pads through SDL's HIDAPI drivers on Linux, issue #5:
//   the pad worked only on the first start, after the installer screen had pumped events for
//   minutes). Up to 3 s when a pad is expected (CONTROLLER = JOYSTICK / JOYPAD, or one answered at
//   an earlier start), 5 s on a Steam Deck, else a 300 ms look;
// - a controller answered (ControllerPreset=1) and CONTROLLER is KEYBOARD (never chosen, or the game's
//   fallback): CONTROLLER = JOYSTICK, the only analog mode; a player's JOYPAD choice stays. The
//   binding preset follows in frame_tick. The keyboard keeps working in JOYSTICK mode (the game reads
//   both), so keyboard and controller can be used together.
extern "C" void game_hook_input_loaded(void) {
    load_state();
    const bool deck = plat::host().info.is_steam_deck();
    const uint32_t device = RD32(INPUT_DEVICE);
    const bool expected = device == 2 || device == 4 || pad_seen;
    const uint64_t t0 = SDL_GetTicks();
    const bool found = w32::wait_for_gamepad(deck ? 5000 : expected ? 3000 : 300);
    log("gamepad: %s after %llu ms (CONTROLLER %u)\n", found ? "ready" : "none",
        static_cast<unsigned long long>(SDL_GetTicks() - t0), device);
    if (found && !pad_seen) {
        pad_seen = true;
        save_state();
    }
    if (settings::get().controller_preset && (found || deck) && device != 2 && device != 4) {
        WR32(INPUT_DEVICE, 2);
        log("%s: CONTROLLER set to JOYSTICK (analog)\n", deck ? "Steam Deck" : "controller found");
    }
    // Pad-only hosts (PSP) at every start: with CONTROLLER = KEYBOARD (a config copied from a PC, or
    // changed in Options) the game ignores the pad and the menu mapping turns Circle into Escape,
    // which pauses races.
    if (plat::host().info.pad_only() && RD32(INPUT_DEVICE) != 2) {
        WR32(INPUT_DEVICE, 2);
        log("pad-only host: CONTROLLER set to JOYSTICK\n");
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
// so their engine sound (driven by rpm, docs/notes/19-engine-sounds.md) only hums. The step is recomputed
// with the fraction carried per bike, which makes it frame-rate independent. The carry restarts when
// a bike was not updated for half a second (a new race, a replay, a pause), so a replay starts from
// the same state. (Restarting it whenever the speed was at its start value 2 kept the opponents
// stuck for seconds at 60 fps: one frame's step is at most 0.64, so it never added up there.)
constexpr uint32_t BIKES = 0x584421, BIKE_STRIDE = 0x3209, BIKE_DT = 0x581014;
// Race setup, after the opponent count (0x542874) was chosen: MOTORACER_OPPONENTS=<0..23> (dev,
// PSP profiling) overrides it. -NbComp<n> only applies to another start path (0x40b924).
void ai_lod_race_setup();

extern "C" void game_hook_opponents(void) {
    ai_lod_race_setup();   // a new race: the AI level of detail starts over
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

// Record page: page 9 (TOP 5, per frame 0x44d540) takes up to 3 initials from WM_CHAR in solo races
// (0x4dbffc) for the rows marked in 0x5401c4; count at 0x540070 (3 = nothing left to enter). The page
// manager [0x4dc0c4] keeps the current page at +0x3c. Only while initials are wanted: with no new record,
// or once they are entered, TOP 5 needs the normal menu keys for Continue (owner, PSP: stuck there).
bool initials_entry_active() {
    uint32_t pages = RD32(0x4dc0c4);
    return pages && RD32(pages + 0x3c) == 9 && RD32(0x4dbffc) && RD32(0x5401c4) && RD32(0x540070) < 3;
}

int initials_count() { return static_cast<int>(RD32(0x540070)); }

// The live flag: 0 while the 640x480 menus run (0x409f97). The Widescreen crop looks at it so
// that black-barred 4:3 pictures (the DSI intro logo, GitHub issue #3) are not cropped.
bool mode_169_live() { return RD32(0x4dc0d4) != 0; }

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

// The title screen's line above the logo, drawn by the game's font from this .data string (0x446f85):
// "RECOMPILED" instead of "FULL D3D VERSION" (17 bytes there; only when the original is in place).
void patch_image() {
    constexpr uint32_t TITLE_LINE = 0x4e94e0;
    static const char from[] = "FULL D3D VERSION", to[] = "RECOMPILED";
    static_assert(sizeof to <= sizeof from, "fits in the original string");
    if (memcmp(M + TITLE_LINE, from, sizeof from) != 0) return;
    memset(M + TITLE_LINE, 0, sizeof from);
    memcpy(M + TITLE_LINE, to, sizeof to);
}

#ifdef RECOMP_PGO_GEN
extern "C" void __gcov_dump(void);
#endif

void frame_tick() {
#ifdef RECOMP_PGO_GEN
    // profile-guided build, training run (CMake MOTORACER_PGO=gen): write the execution counts once
    // MOTORACER_GCOV_DUMP_MS have passed (GCOV_PREFIX / GCOV_PREFIX_STRIP place the .gcda files)
    {
        static const uint64_t at = getenv("MOTORACER_GCOV_DUMP_MS") ? strtoull(getenv("MOTORACER_GCOV_DUMP_MS"), nullptr, 10) : 0;
        static bool dumped = false;
        if (at && !dumped && SDL_GetTicks() >= at) {
            dumped = true;
            log("pgo: writing the execution counts\n");
            __gcov_dump();
            log("pgo: done\n");
        }
    }
#endif
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
    // cumulative per-function counts ("frames <n>" then "<entry va> <count>"); scripts/dev/icprof.py ranks
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
    // MOTORACER_BIKE_LOD_SCALE=<s> (dev experiment): every threshold x s (0.5: sprites from 1500)
    static const float lod_scale = getenv("MOTORACER_BIKE_LOD_SCALE") ? static_cast<float>(atof(getenv("MOTORACER_BIKE_LOD_SCALE"))) : 1.f;
    const double k = (d > 1.f ? 3.0 * d : 1.0) * (lod_scale > 0.f ? lod_scale : 1.f);
    if (k != 1.0) ST(0) /= k;
}

// ---- AI level of detail (setting AiLod; PSP default on) ----
// The per-frame loop 0x47df40 updates every racing opponent with 0x45d540(..., dt), dt = the frame
// time in hundredths of a second ([0x4e9808], at least 1). On a PSP that is ~22 ms of a race frame,
// bound by data memory (24 records of 0x3209 bytes). Opponents far from the player are updated less
// often: the skipped frames' time accumulates and the next update gets all of it (the frame-time
// global holds the sum for that one call, restored by game_hook_ai_lod_after), so they cover the same
// ground in fewer, larger steps. Near the player (where you see and touch them) every frame, exact.
//   near:  distance < R1 (40000: opponents are drawn up to 2 x LenBPlane = 36000 at detail 5): every
//          frame — every bike you can see or touch is simulated as before
//   mid:   R1..R2: an update per P_MID hundredths of a second (10: ~10 Hz)
//   far:   beyond R2: an update per P_FAR (10)
// The player's bike (bike +8 nonzero, any slot: the first version assumed slot 0 and skipped the
// player's own updates on a real PSP) is never skipped and is the reference for the distances.
// MOTORACER_AI_LOD="R1,R2,P_MID,P_FAR" (dev) changes them; "0" turns it off.
namespace {
constexpr uint32_t FRAME_DT = 0x4e9808, BIKE_POS = 0x3c;
constexpr uint32_t WARM_UP = 60;   // the grid / first seconds: skipping an opponent's first updates broke the race
struct AiLod {
    float r1 = 40000.f, r2 = 80000.f, p_mid = 10.f, p_far = 20.f;   // beyond twice the drawing range: 5 updates a second
    uint32_t player = 0x584421;   // the player's bike, as seen in the loop (previous frame until then)
    float acc[32] = {};
    uint32_t warm[32] = {};   // normal updates since the race was set up: none skipped before WARM_UP
    uint32_t saved_dt = 0;
    bool restore = false;
    uint64_t updates = 0, skips = 0;
};
AiLod &ai_lod() {
    static AiLod a = [] {
        AiLod l;
        if (const char *e = getenv("MOTORACER_AI_LOD"); e && strchr(e, ','))
            sscanf(e, "%f,%f,%f,%f", &l.r1, &l.r2, &l.p_mid, &l.p_far);
        log("ai lod: %s (near < %g every frame, to %g one update per %g, beyond one per %g hundredths)\n",
            settings::get().ai_lod ? "on" : "off", l.r1, l.r2, l.p_mid, l.p_far);
        return l;
    }();
    return a;
}
}  // namespace

void ai_lod_race_setup() {
    AiLod &l = ai_lod();
    for (uint32_t i = 0; i < 32; i++) l.acc[i] = 0.f, l.warm[i] = 0;
}

extern "C" int game_hook_ai_lod(void) {
    if (!settings::get().ai_lod) return 0;
    AiLod &l = ai_lod();
    const uint32_t slot = R.ebx, bike = R.edi - 0x68;   // loop registers at 0x47e189
    if (slot >= 32 || !RD32(R.edi)) return 0;           // not active: the game skips it anyway
    if (RD32(bike + 8)) {                               // the player: always, and the distance reference
        l.player = bike;
        l.acc[slot] = 0.f;
        return 0;
    }
    const float dt = RDF32(FRAME_DT);
    float &acc = l.acc[slot];
    if (acc > 4.f * l.p_far) acc = 0.f;                 // stale (previous race, menus)
    const uint32_t me = l.player;
    const float dx = RDF32(bike + BIKE_POS) - RDF32(me + BIKE_POS);
    const float dy = RDF32(bike + BIKE_POS + 4) - RDF32(me + BIKE_POS + 4);
    const float dz = RDF32(bike + BIKE_POS + 8) - RDF32(me + BIKE_POS + 8);
    const float d2 = dx * dx + dy * dy + dz * dz;
    const float period = (d2 < l.r1 * l.r1 || l.warm[slot] < WARM_UP) ? 0.f : d2 < l.r2 * l.r2 ? l.p_mid : l.p_far;
    acc += dt;
    if (acc < period) {   // not yet: skip this frame's update
        l.skips++;
        return 1;
    }
    if (acc != dt) {      // this update covers the skipped frames too
        l.saved_dt = RD32(FRAME_DT);
        WRF32(FRAME_DT, acc);
        l.restore = true;
    }
    acc = 0.f;
    if (l.warm[slot] < WARM_UP) l.warm[slot]++;
    l.updates++;
    return 0;
}

extern "C" void game_hook_ai_lod_after(void) {
    AiLod &l = ai_lod();
    if (l.restore) {
        WR32(FRAME_DT, l.saved_dt);
        l.restore = false;
    }
    static uint64_t last = 0;
    static const bool report = getenv("MOTORACER_PROF") != nullptr;
    if (report && SDL_GetTicks() - last >= 5000) {   // with the profiler: how much is skipped
        last = SDL_GetTicks();
        if (l.updates + l.skips)
            log("ai lod: %llu updates, %llu skipped (%.0f%%) in 5 s\n", static_cast<unsigned long long>(l.updates),
                static_cast<unsigned long long>(l.skips), 100.0 * l.skips / static_cast<double>(l.updates + l.skips));
        l.updates = l.skips = 0;
    }
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
    // per instruction address: <name>.va ("<va> <count>"), scripts/dev/icprof.py --va <function>
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
