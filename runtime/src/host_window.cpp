// The single host window (SDL3) that stands in for the game's top-level Win32 window, plus
// keyboard translation into WM_KEY* messages and GetKeyState.
#include <SDL3/SDL.h>

#include <cctype>
#include <string>

#include "runtime.h"
#include "settings.h"
#include "user32.h"

using namespace rt;

namespace host {
SDL_Window *sdl_window = nullptr;

// The one host window. Size and fullscreen state come from motoracer.ini [Recomp].
SDL_Window *ensure_window() {
    if (sdl_window) return sdl_window;
    const auto &cfg = settings::get();
    sdl_window = SDL_CreateWindow("Moto Racer", cfg.window_w, cfg.window_h,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!sdl_window) fatal("SDL_CreateWindow failed: %s", SDL_GetError());
    if (cfg.fullscreen) SDL_SetWindowFullscreen(sdl_window, true);
    return sdl_window;
}

void toggle_fullscreen() {
    if (!sdl_window) return;
    bool fs = SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_FULLSCREEN;
    SDL_SetWindowFullscreen(sdl_window, !fs);
}
}  // namespace host

namespace w32 {

extern uint64_t last_poll_ms;   // winmm_joy.cpp
void inject_key(uint32_t vk, uint32_t scancode, bool down);
void controller_event(const SDL_Event &e);

namespace {

uint8_t vk_down[256];
uint8_t vk_toggle[256];

// SDL scancode -> Windows virtual-key code (US layout).
uint32_t to_vk(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 0x70 + (sc - SDL_SCANCODE_F1);
    switch (sc) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_KP_1: case SDL_SCANCODE_KP_2: case SDL_SCANCODE_KP_3: case SDL_SCANCODE_KP_4:
    case SDL_SCANCODE_KP_5: case SDL_SCANCODE_KP_6: case SDL_SCANCODE_KP_7: case SDL_SCANCODE_KP_8:
    case SDL_SCANCODE_KP_9: return 0x61 + (sc - SDL_SCANCODE_KP_1);
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    default: return 0;
    }
}

}  // namespace

// PC/AT set-1 scan code for an SDL scancode, as Windows puts in lParam bits 16-23.
uint32_t pc_scancode(SDL_Scancode sc);

void host_window_shown(Window *w) {
    if (host::sdl_window) return;
    host::ensure_window();
    log("host window created for hwnd %08x\n", w->hwnd);
}

void host_window_resized(Window *w) { (void)w; }

void host_window_destroyed() {
    if (host::sdl_window) {
        SDL_DestroyWindow(host::sdl_window);
        host::sdl_window = nullptr;
    }
}

void host_event_hook(const SDL_Event &e) {
    if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
        // Host hotkeys: Cmd+F / Alt+Enter fullscreen, Cmd+Q quit. Cmd combos never reach the game.
        bool down_evt = e.type == SDL_EVENT_KEY_DOWN;
        if (e.key.mod & SDL_KMOD_GUI) {
            if (down_evt && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F) host::toggle_fullscreen();
            if (down_evt && e.key.scancode == SDL_SCANCODE_Q) {
                SDL_Event q{};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
            return;
        }
        if (down_evt && !e.key.repeat && e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
            host::toggle_fullscreen();
            return;
        }
        uint32_t vk = to_vk(e.key.scancode);
        if (!vk) return;
        inject_key(vk, pc_scancode(e.key.scancode), e.type == SDL_EVENT_KEY_DOWN);
        return;
    }
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) w32::gamepads_refresh();   // also the pads present at start
    controller_event(e);
}

void inject_key(uint32_t vk, uint32_t scancode, bool down) {
    bool was = vk_down[vk];
    vk_down[vk] = down;
    if (down && !was) vk_toggle[vk] ^= 1;
    uint32_t lp = 1 | (scancode << 16) | (was ? 1u << 30 : 0) | (down ? 0 : 0xC0000000u);
    bool alt = vk_down[0x12];
    uint32_t msg = down ? (alt ? WM_SYSKEYDOWN : WM_KEYDOWN) : (alt ? WM_SYSKEYUP : WM_KEYUP);
    if (main_window) post_message(main_window, msg, vk, lp);
}

// ---------- controller -> keyboard for the menus ----------
// The menus only read the keyboard/mouse. While the game is not polling the joystick itself
// (it does in races), the D-pad / left stick act as arrow keys, A as Enter and B as Escape.
// Start is Escape (pause) everywhere.
namespace {
constexpr uint32_t VK_RETURN_ = 0x0D, VK_ESCAPE_ = 0x1B, VK_LEFT_ = 0x25, VK_UP_ = 0x26, VK_RIGHT_ = 0x27,
                   VK_DOWN_ = 0x28;
uint32_t nav_scancode(uint32_t vk) {
    switch (vk) {
    case VK_RETURN_: return 0x1C;
    case VK_ESCAPE_: return 0x01;
    case VK_LEFT_: return 0x4B;
    case VK_UP_: return 0x48;
    case VK_RIGHT_: return 0x4D;
    case VK_DOWN_: return 0x50;
    default: return 0;
    }
}
bool nav_down[256];
uint32_t held_vk = 0;
uint64_t held_since = 0, last_repeat = 0;
bool stick_dir[4];   // left, up, right, down

bool game_reads_pad() { return SDL_GetTicks() - last_poll_ms < 300; }

void nav(uint32_t vk, bool down, bool always = false) {
    if (down && !always && game_reads_pad()) return;
    if (down == nav_down[vk]) return;
    nav_down[vk] = down;
    inject_key(vk, nav_scancode(vk), down);
    bool dir = vk >= VK_LEFT_ && vk <= VK_DOWN_;
    if (dir && down) { held_vk = vk; held_since = last_repeat = SDL_GetTicks(); }
    if (dir && !down && held_vk == vk) held_vk = 0;
}
}  // namespace

void controller_event(const SDL_Event &e) {
    if (!settings::get().controller_menus) return;
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        switch (e.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: nav(VK_LEFT_, down); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: nav(VK_RIGHT_, down); break;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: nav(VK_UP_, down); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: nav(VK_DOWN_, down); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: nav(VK_RETURN_, down); break;
        case SDL_GAMEPAD_BUTTON_EAST: nav(VK_ESCAPE_, down); break;
        case SDL_GAMEPAD_BUTTON_START: nav(VK_ESCAPE_, down, true); break;
        default: break;
        }
    } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
               (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
        int v = e.gaxis.value;
        int neg = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? 0 : 1, pos = neg + 2;
        uint32_t vneg = neg == 0 ? VK_LEFT_ : VK_UP_, vpos = neg == 0 ? VK_RIGHT_ : VK_DOWN_;
        // hysteresis: press beyond 60%, release below 30%
        bool want_neg = stick_dir[neg] ? v < -10000 : v < -20000;
        bool want_pos = stick_dir[pos] ? v > 10000 : v > 20000;
        if (want_neg != stick_dir[neg]) { stick_dir[neg] = want_neg; nav(vneg, want_neg); }
        if (want_pos != stick_dir[pos]) { stick_dir[pos] = want_pos; nav(vpos, want_pos); }
    }
}

// Developer aid for unattended testing: MOTORACER_AUTOKEYS="3000 ENTER 500 DOWN +UP 2000 -UP"
// waits (numbers, ms) and taps / presses (+) / releases (-) keys: ENTER ESC UP DOWN LEFT RIGHT
// SPACE or a single letter/digit. Taps are held for 80 ms.
namespace {
void autokeys_tick() {
    static const char *spec = getenv("MOTORACER_AUTOKEYS");
    static size_t pos = 0;
    static uint64_t next = 0;
    static uint32_t tapped = 0;
    if (!spec || !main_window) return;
    uint64_t now = SDL_GetTicks();
    if (!next) next = now;
    if (now < next) return;
    if (tapped) { inject_key(tapped, nav_scancode(tapped), false); tapped = 0; }
    while (spec[pos] == ' ') pos++;
    if (!spec[pos]) return;
    size_t end = pos;
    while (spec[end] && spec[end] != ' ') end++;
    std::string tok(spec + pos, end - pos);
    pos = end;
    if (isdigit(static_cast<unsigned char>(tok[0]))) { next = now + strtoull(tok.c_str(), nullptr, 10); return; }
    char mode = 0;
    if (tok[0] == '+' || tok[0] == '-') { mode = tok[0]; tok.erase(0, 1); }
    static const std::pair<const char *, uint32_t> names[] = {
        {"ENTER", VK_RETURN_}, {"ESC", VK_ESCAPE_}, {"UP", VK_UP_}, {"DOWN", VK_DOWN_},
        {"LEFT", VK_LEFT_},    {"RIGHT", VK_RIGHT_}, {"SPACE", 0x20}};
    uint32_t vk = tok.size() == 1 ? static_cast<uint32_t>(toupper(tok[0])) : 0;
    for (auto &[n, v] : names) if (tok == n) vk = v;
    if (!vk) { log("autokeys: unknown key '%s'\n", tok.c_str()); return; }
    log("autokeys: %c%s\n", mode ? mode : ' ', tok.c_str());
    inject_key(vk, nav_scancode(vk), mode != '-');
    if (!mode) tapped = vk;
    next = now + 80;
}
}  // namespace

// Auto-repeat for a held direction (like keyboard typematic): 400 ms delay, then every 120 ms.
void controller_tick() {
    autokeys_tick();
    if (!held_vk || !nav_down[held_vk]) return;
    uint64_t now = SDL_GetTicks();
    if (now - held_since > 400 && now - last_repeat > 120) {
        last_repeat = now;
        if (!game_reads_pad()) inject_key(held_vk, nav_scancode(held_vk), true);
    }
}

uint32_t key_state(uint32_t vk) {
    if (vk >= 256) return 0;
    return (vk_down[vk] ? 0xFF80u : 0) | vk_toggle[vk];
}

uint32_t pc_scancode(SDL_Scancode sc) {
    static const uint8_t letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return letters[sc - SDL_SCANCODE_A];
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) return 0x02 + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) return 0x3B + (sc - SDL_SCANCODE_F1);
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_MINUS: return 0x0C;
    case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E;
    case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x1C;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x1D;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A;
    case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x38;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_HOME: return 0x47;
    case SDL_SCANCODE_UP: return 0x48;
    case SDL_SCANCODE_PAGEUP: return 0x49;
    case SDL_SCANCODE_LEFT: return 0x4B;
    case SDL_SCANCODE_RIGHT: return 0x4D;
    case SDL_SCANCODE_END: return 0x4F;
    case SDL_SCANCODE_DOWN: return 0x50;
    case SDL_SCANCODE_PAGEDOWN: return 0x51;
    case SDL_SCANCODE_INSERT: return 0x52;
    case SDL_SCANCODE_DELETE: return 0x53;
    default: return 0;
    }
}

}  // namespace w32
