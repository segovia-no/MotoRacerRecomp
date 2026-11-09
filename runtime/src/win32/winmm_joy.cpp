// WINMM joystick API backed by SDL gamepads.
//
// The game polls joyGetPosEx with JOY_RETURNX|Y|BUTTONS|CENTERED (0x483) and decides at startup
// whether a stick is digital: resting axes of exactly 0, 0x8000 or 0xFFFF mean digital. We centre
// at 0x7FFF so pads are treated as analog.
//   X = left stick X (steering), D-pad left/right = full deflection
//   Y = left stick Y + left trigger - right trigger (TriggerAxis=1, default): in the game's
//       analog JOYSTICK mode the Y directions are bindable actions with analog strength, so
//       right/left trigger give analog accelerate/brake (bind "up"/"down" in the game's config).
//       D-pad up/down = full deflection.
//   buttons 1-8 = A, B, X, Y, LB, RB, Back, Start; 9 = right trigger, 10 = left trigger (past
//       halfway) — for the digital JOYPAD mode, where actions are bound to buttons.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "game/settings.h"
#include "win32.h"

using namespace rt;

namespace {

constexpr uint32_t JOYERR_NOERROR = 0, JOYERR_PARMS = 165, JOYERR_UNPLUGGED = 167;
constexpr int MAX_JOYSTICKS = 16;
constexpr int DEADZONE = 4000;

constexpr int NUM_BUTTONS = 10;
constexpr int TRIGGER_PRESSED = 16384;   // half of the 0..32767 trigger range

// A connected controller: an SDL gamepad (a device SDL has a mapping for: Xbox, PlayStation, Switch,
// most USB pads), or else a plain SDL joystick read by its raw axes, buttons and hat (adapters for old
// analog sticks and pads without a mapping, which SDL does not list as gamepads).
struct Device {
    SDL_Gamepad *g = nullptr;
    SDL_Joystick *j = nullptr;
    SDL_JoystickID id = 0;
};
std::vector<Device> pads;

void refresh_pads() {
    static bool init = false;
    if (!init) {
        init = true;
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) log("gamepad: init failed: %s\n", SDL_GetError());
#ifndef __PSP__
        // MOTORACER_VIRTUAL_PAD=1 (dev, E2E tests of the controller handling): an idle virtual gamepad;
        // =<ms>: unplugged again that many ms after it was attached
        if (const char *e = getenv("MOTORACER_VIRTUAL_PAD"); e && *e && *e != '0') {
            SDL_VirtualJoystickDesc desc;
            SDL_INIT_INTERFACE(&desc);
            desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
            desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
            desc.name = "Virtual test pad";
            static SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
            static uint64_t unplug_at = atoi(e) > 1 ? SDL_GetTicks() + static_cast<uint64_t>(atoi(e)) : 0;
            log("gamepad: virtual test pad attached (id %u)%s\n", vid, unplug_at ? ", unplugged later" : "");
            if (unplug_at)
                SDL_AddTimer(static_cast<uint32_t>(unplug_at - SDL_GetTicks()), [](void *, SDL_TimerID, uint32_t) -> uint32_t {
                    SDL_DetachVirtualJoystick(vid);
                    return 0;
                }, nullptr);
        }
#endif
    }
    std::erase_if(pads, [](Device &d) {
#ifndef __PSP__
        if (d.j) {
            if (SDL_JoystickConnected(d.j)) return false;
            SDL_CloseJoystick(d.j);
            return true;
        }
#endif
        if (SDL_GamepadConnected(d.g)) return false;
        SDL_CloseGamepad(d.g);
        return true;
    });
    auto have = [](SDL_JoystickID id) {
        return std::any_of(pads.begin(), pads.end(), [&](const Device &d) { return d.id == id; });
    };
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        if (have(ids[i])) continue;
        if (SDL_Gamepad *p = SDL_OpenGamepad(ids[i])) {
            pads.push_back({p, nullptr, ids[i]});
            log("gamepad %zu: %s\n", pads.size() - 1, SDL_GetGamepadName(p));
        }
    }
    SDL_free(ids);
#ifndef __PSP__   // the PSP's SDL subset has gamepads only (its own buttons)
    ids = SDL_GetJoysticks(&n);
    for (int i = 0; ids && i < n; i++) {
        if (have(ids[i]) || SDL_IsGamepad(ids[i])) continue;
        if (SDL_Joystick *j = SDL_OpenJoystick(ids[i])) {
            pads.push_back({nullptr, j, ids[i]});
            log("joystick %zu (no gamepad mapping, raw axes / buttons): %s, %d axes, %d buttons, %d hats\n",
                pads.size() - 1, SDL_GetJoystickName(j), SDL_GetNumJoystickAxes(j), SDL_GetNumJoystickButtons(j),
                SDL_GetNumJoystickHats(j));
        }
    }
    SDL_free(ids);
#endif
}

Device *pad(uint32_t id) {
    refresh_pads();
    return id < pads.size() ? &pads[id] : nullptr;
}

// The game's controller mode global (race code 0x411e06: value 2 keeps the axes analog, any other
// value centres them).
constexpr uint32_t GAME_JOY_MODE = 0x4dc11c;

bool triggers_as_axis() {
    switch (settings::get().trigger_mode) {
    case settings::TriggerMode::Axis: return true;
    case settings::TriggerMode::Buttons: return false;
    default: break;
    }
    static uint32_t last = 0xFFFFFFFF;
    uint32_t mode = RD32(GAME_JOY_MODE);
    if (mode != last) {
        last = mode;
        log("joystick: game controller mode %u -> triggers as %s\n", mode, mode == 2 ? "Y axis (analog)" : "buttons 9/10");
    }
    return mode == 2;
}

int dz(int v) { return std::abs(v) < DEADZONE ? 0 : v; }

uint32_t to_axis(int v) {   // SDL -32768..32767 -> WINMM 0..65535, centre 0x7FFF
    return static_cast<uint32_t>(std::clamp(v + 32767, 0, 65535));
}

}  // namespace

namespace w32 {
uint64_t last_poll_ms = 0;   // last joyGetPosEx: the game is reading the pad itself

void gamepads_refresh() { refresh_pads(); }

// Steam Input (Steam Deck, Big Picture) creates its virtual gamepad only after the game started,
// so it can be missing when the game first looks for a joystick.
bool wait_for_gamepad(uint32_t ms) {
    uint64_t until = SDL_GetTicks() + ms;
    for (;;) {
        refresh_pads();
        if (!pads.empty()) return true;
        if (SDL_GetTicks() >= until) return false;
        SDL_Delay(50);
        SDL_PumpEvents();   // runs device detection; the queued events are handled later as usual
    }
}

// The name of a joystick input (the game's encoding: 0.. buttons, -2/-3 X left/right, -4/-5 Y up /
// down) on the first controller, in its family's own terms (Xbox, PlayStation, Nintendo; face buttons
// by SDL's label for the pad), for the game's controller settings screen; nullptr: keep the game's
// own text / arrow (steering, a raw joystick without a gamepad mapping, no controller, the PSP).
const char *pad_input_label(int32_t input) {
#ifdef __PSP__
    (void)input;
    return nullptr;
#else
    refresh_pads();
    if (pads.empty() || !pads[0].g) return nullptr;
    SDL_Gamepad *g = pads[0].g;
    enum Family { XBOX, PLAYSTATION, NINTENDO } family = XBOX;
    switch (SDL_GetGamepadType(g)) {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: family = PLAYSTATION; break;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        family = NINTENDO; break;
    default: break;
    }
    static const char *const shoulders[3][2] = {{"LB", "RB"}, {"L1", "R1"}, {"L", "R"}};
    static const char *const triggers[3][2] = {{"LT", "RT"}, {"L2", "R2"}, {"ZL", "ZR"}};
    static const char *const menu[3][2] = {{"Back", "Start"}, {"Share", "Options"}, {"Minus", "Plus"}};
    const bool trig_axis = triggers_as_axis();
    switch (input) {
    case -4: return trig_axis ? triggers[family][1] : nullptr;   // Y up: the right trigger (or the stick)
    case -5: return trig_axis ? triggers[family][0] : nullptr;
    case 0: case 1: case 2: case 3: {   // south, east, west, north (joyGetPosEx order)
        static const SDL_GamepadButton face[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                  SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};
        switch (SDL_GetGamepadButtonLabel(g, face[input])) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default: return nullptr;
        }
    }
    case 4: return shoulders[family][0];
    case 5: return shoulders[family][1];
    case 6: return menu[family][0];
    case 7: return menu[family][1];
    case 8: return trig_axis ? nullptr : triggers[family][1];   // buttons 9/10 in the digital mode
    case 9: return trig_axis ? nullptr : triggers[family][0];
    default: return nullptr;
    }
#endif
}
}
using w32::last_poll_ms;

STDCALL(WINMM, joyGetNumDevs, 0) { return MAX_JOYSTICKS; }

STDCALL(WINMM, joyGetDevCapsA, 3) {
    uint32_t id = arg(0), p = arg(1), size = arg(2);
    if (!p || size < 404) return JOYERR_PARMS;
    Device *d = pad(id);
    if (!d) return JOYERR_UNPLUGGED;
    memset(M + p, 0, 404);
    WR16(p + 0, 0x045E);
    WR16(p + 2, 0x0001);
#ifndef __PSP__
    const char *name = d->g ? SDL_GetGamepadName(d->g) : SDL_GetJoystickName(d->j);
#else
    const char *name = SDL_GetGamepadName(d->g);
#endif
    write_cstr(p + 4, name ? name : "Gamepad", 32);
    WR32(p + 40, 65535);   // X max
    WR32(p + 48, 65535);   // Y max
    WR32(p + 56, 65535);   // Z max
    WR32(p + 60, NUM_BUTTONS);
    WR32(p + 64, 10);      // period min
    WR32(p + 68, 1000);    // period max
    WR32(p + 100, 2);      // max axes
    WR32(p + 104, 2);      // axes
    WR32(p + 108, 32);     // max buttons
    return JOYERR_NOERROR;
}

STDCALL(WINMM, joyGetPosEx, 2) {
    uint32_t id = arg(0), p = arg(1);
    Device *d = pad(id);
    if (!d) return JOYERR_UNPLUGGED;
    last_poll_ms = SDL_GetTicks();
    int x = 0, y = 0;
    uint32_t buttons = 0, count = 0;
    bool up = false, down = false, left = false, right = false;
    if (SDL_Gamepad *g = d->g) {
        x = dz(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX));
        y = dz(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
        // Triggers go either to the Y axis (analog mode) or to buttons 9/10 (digital mode), never
        // both: the game's control setup takes whichever input it notices first.
        bool trig_axis = triggers_as_axis();
        if (trig_axis)
            y += SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) - SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        left = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        right = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        up = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP);
        down = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        static const SDL_GamepadButton order[8] = {
            SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_BACK,
            SDL_GAMEPAD_BUTTON_START};
        for (int i = 0; i < 8; i++)
            if (SDL_GetGamepadButton(g, order[i])) { buttons |= 1u << i; count++; }
        if (!trig_axis && SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_PRESSED) { buttons |= 1u << 8; count++; }
        if (!trig_axis && SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_PRESSED) { buttons |= 1u << 9; count++; }
    }
#ifndef __PSP__
    else if (SDL_Joystick *j = d->j) {   // raw joystick: axes 0 / 1, the first buttons, hat 0
        if (SDL_GetNumJoystickAxes(j) > 0) x = dz(SDL_GetJoystickAxis(j, 0));
        if (SDL_GetNumJoystickAxes(j) > 1) y = dz(SDL_GetJoystickAxis(j, 1));
        const int nb = std::min(SDL_GetNumJoystickButtons(j), NUM_BUTTONS);
        for (int i = 0; i < nb; i++)
            if (SDL_GetJoystickButton(j, i)) { buttons |= 1u << i; count++; }
        if (SDL_GetNumJoystickHats(j) > 0) {
            const uint8_t h = SDL_GetJoystickHat(j, 0);
            up = h & SDL_HAT_UP; down = h & SDL_HAT_DOWN; left = h & SDL_HAT_LEFT; right = h & SDL_HAT_RIGHT;
        }
    }
#endif
    if (left) x = -32768;
    if (right) x = 32767;
    if (up) y = -32768;
    if (down) y = 32767;
    uint32_t pov = 0xFFFF;   // JOY_POVCENTERED
    if (up) pov = 0;
    else if (right) pov = 9000;
    else if (down) pov = 18000;
    else if (left) pov = 27000;
    WR32(p + 8, to_axis(x));
    WR32(p + 12, to_axis(y));
    WR32(p + 16, 0x7FFF);
    WR32(p + 20, 0x7FFF);
    WR32(p + 24, 0x7FFF);
    WR32(p + 28, 0x7FFF);
    WR32(p + 32, buttons);
    WR32(p + 36, count);
    WR32(p + 40, pov);
    return JOYERR_NOERROR;
}
