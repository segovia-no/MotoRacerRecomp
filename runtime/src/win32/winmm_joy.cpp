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

#include "settings.h"
#include "win32.h"

using namespace rt;

namespace {

constexpr uint32_t JOYERR_NOERROR = 0, JOYERR_PARMS = 165, JOYERR_UNPLUGGED = 167;
constexpr int MAX_JOYSTICKS = 16;
constexpr int DEADZONE = 4000;

std::vector<SDL_Gamepad *> pads;
constexpr int NUM_BUTTONS = 10;
constexpr int TRIGGER_PRESSED = 16384;   // half of the 0..32767 trigger range

void refresh_pads() {
    static bool init = false;
    if (!init) {
        init = true;
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) log("gamepad: init failed: %s\n", SDL_GetError());
    }
    std::erase_if(pads, [](SDL_Gamepad *p) {
        if (SDL_GamepadConnected(p)) return false;
        SDL_CloseGamepad(p);
        return true;
    });
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        bool have = std::any_of(pads.begin(), pads.end(), [&](SDL_Gamepad *p) { return SDL_GetGamepadID(p) == ids[i]; });
        if (!have) {
            if (SDL_Gamepad *p = SDL_OpenGamepad(ids[i])) {
                pads.push_back(p);
                log("gamepad %zu: %s\n", pads.size() - 1, SDL_GetGamepadName(p));
            }
        }
    }
    SDL_free(ids);
}

SDL_Gamepad *pad(uint32_t id) {
    refresh_pads();
    return id < pads.size() ? pads[id] : nullptr;
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
}
using w32::last_poll_ms;

STDCALL(WINMM, joyGetNumDevs, 0) { return MAX_JOYSTICKS; }

STDCALL(WINMM, joyGetDevCapsA, 3) {
    uint32_t id = arg(0), p = arg(1), size = arg(2);
    if (!p || size < 404) return JOYERR_PARMS;
    SDL_Gamepad *g = pad(id);
    if (!g) return JOYERR_UNPLUGGED;
    memset(M + p, 0, 404);
    WR16(p + 0, 0x045E);
    WR16(p + 2, 0x0001);
    write_cstr(p + 4, SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "Gamepad", 32);
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
    SDL_Gamepad *g = pad(id);
    if (!g) return JOYERR_UNPLUGGED;
    last_poll_ms = SDL_GetTicks();
    int x = dz(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX));
    int y = dz(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
    // Triggers go either to the Y axis (analog mode) or to buttons 9/10 (digital mode), never
    // both: the game's control setup takes whichever input it notices first.
    bool trig_axis = triggers_as_axis();
    if (trig_axis)
        y += SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) - SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) x = -32768;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) x = 32767;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP)) y = -32768;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) y = 32767;
    static const SDL_GamepadButton order[8] = {
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_START};
    uint32_t buttons = 0, count = 0;
    for (int i = 0; i < 8; i++)
        if (SDL_GetGamepadButton(g, order[i])) { buttons |= 1u << i; count++; }
    if (!trig_axis && SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_PRESSED) { buttons |= 1u << 8; count++; }
    if (!trig_axis && SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_PRESSED) { buttons |= 1u << 9; count++; }
    uint32_t pov = 0xFFFF;   // JOY_POVCENTERED
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP)) pov = 0;
    else if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) pov = 9000;
    else if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) pov = 18000;
    else if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) pov = 27000;
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
