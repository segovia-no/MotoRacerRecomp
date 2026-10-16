// In-game Recomp settings page, in the game's own Options look (docs/17-recomp-menu.md).
//
// Options' Info button (item 21, which opens the DIAGNOSTIC page) opens it instead. It is the
// Options page (11) in another mode: the game's per-frame function 0x443200 still draws the
// background, header and buttons and runs the menu; jump hooks replace its rows with ours (drawn
// with the game's own font and item functions) and handle our items. Changes apply at once where
// possible and are saved to motoracer.ini [Recomp].
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "host_gpu.h"
#include "runtime.h"
#include "settings.h"
#include "user32.h"

using namespace rt;

namespace {

// Game functions and data (Options page 11, see docs/17-recomp-menu.md).
constexpr uint32_t GET_OBJECT = 0x4903d0;   // stdcall (id, 0) -> font / page object
constexpr uint32_t DRAW_TEXT = 0x493ee0;    // thiscall font (text, x, y, 0, 0, 0, 0, 0)
constexpr uint32_t ADD_ITEM = 0x40bd60;     // thiscall menu (&point, text, font, nav, 0, -1, 0)
constexpr uint32_t ADD_BUTTON = 0x40bef0;   // thiscall menu (sprite, 0, nav, -1, -1, 0, 0, 0, x, y, 0)
constexpr uint32_t MENU = 0x4dc0c4;         // the page's menu object
constexpr uint32_t MENU_CURSOR = 0x10;      // keyboard cursor item (mouse mode hit-tests instead)
constexpr uint32_t INFO_ITEM = 21;
constexpr uint32_t ACCEPT_SPRITE = 0x504998;
constexpr uint32_t FONT_ID = 0xc;
constexpr uint32_t COLOR_NORMAL = 0x4f0dd4, COLOR_SELECTED = 0x4f0de8;
constexpr uint32_t LABEL_X = 50, VALUE_X = 300, ROW_Y = 100, ROW_STEP = 20;

// nav: item numbers reached with up / down / right / left (0xff = none)
uint32_t nav(uint32_t up, uint32_t down) { return up << 24 | down << 16 | 0xffffu; }

struct Row {
    const char *label;
    std::function<std::string()> value;
    std::function<void()> next;   // Enter / click: next value (saved)
};

const char *on_off(bool v) { return v ? "ON" : "OFF"; }

template <typename T>
T next_of(const std::vector<T> &list, T cur) {
    for (size_t i = 0; i < list.size(); i++)
        if (list[i] == cur) return list[(i + 1) % list.size()];
    return list[0];
}

std::string num(float v) {
    char b[16];
    snprintf(b, sizeof b, "%g", v);
    return b;
}

std::vector<Row> make_rows() {
    using settings::edit;
    using settings::save;
    return {
        {"FRAME RATE",
         [] {
             int r = settings::get().frame_rate;
             if (r == 0) return std::to_string(settings::get().game_frame_cap) + "   (ORIGINAL)";
             if (r == -2) return std::string("DISPLAY");
             if (r == -1) return std::string("UNLIMITED");
             return std::to_string(r);
         },
         [] {
             int r = next_of<int>({0, -2, 30, 60, 90, 120, 144, 165, 240}, settings::get().frame_rate);
             edit().frame_rate = r;
             save("FrameRate", r == 0 ? "game" : r == -2 ? "display" : std::to_string(r));
         }},
        {"VSYNC", [] { return std::string(on_off(settings::get().vsync)); },
         [] {
             edit().vsync = !settings::get().vsync;
             host::gpu::apply_vsync();
             save("VSync", settings::get().vsync ? "1" : "0");
         }},
        {"SCALING FILTER", [] { return std::string(settings::get().nearest ? "SHARP" : "SMOOTH"); },
         [] {
             edit().nearest = !settings::get().nearest;
             save("Filter", settings::get().nearest ? "nearest" : "linear");
         }},
        {"HUD SCALE",
         [] {
             float k = settings::get().hud_scale;
             return k <= 0 ? std::string("AUTO") : k == 1 ? std::string("ORIGINAL") : num(k) + " x";
         },
         [] {
             float k = next_of<float>({0, 1, 1.5f, 2, 2.5f, 3, 4}, settings::get().hud_scale);
             edit().hud_scale = k;
             save("HudScale", k <= 0 ? "auto" : num(k));
         }},
        {"DRAW DISTANCE",
         [] {
             float d = settings::get().draw_distance;
             return d <= 1 ? std::string("ORIGINAL") : num(d) + " x   (NEXT RACE)";
         },
         [] {
             float d = next_of<float>({1, 1.5f, 2, 3}, settings::get().draw_distance);
             edit().draw_distance = d;
             save("DrawDistance", num(d));
         }},
        {"WIDESCREEN CROP", [] { return std::string(settings::get().widescreen ? "AUTO" : "OFF"); },
         [] {
             edit().widescreen = !settings::get().widescreen;
             save("Widescreen", settings::get().widescreen ? "auto" : "off");
         }},
        {"GAMEPAD IN MENUS", [] { return std::string(on_off(settings::get().controller_menus)); },
         [] {
             edit().controller_menus = !settings::get().controller_menus;
             save("ControllerMenus", settings::get().controller_menus ? "1" : "0");
         }},
    };
}

bool open = false;
std::vector<Row> rows;
uint32_t text_mem = 0;   // guest strings and points, stable while the page is shown
constexpr uint32_t SLOT = 64;

uint32_t put_string(uint32_t slot, const std::string &s) {
    uint32_t va = text_mem + slot * SLOT;
    size_t n = std::min<size_t>(s.size(), SLOT - 1);
    for (size_t i = 0; i < n; i++) WR8(va + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
    WR8(va + static_cast<uint32_t>(n), 0);
    return va;
}

uint32_t font() { return guest_call_stdcall(GET_OBJECT, {FONT_ID, 0}); }

void set_color(uint32_t color) {
    uint32_t f = font();
    guest_call_thiscall(RD32(RD32(f)), f, {color, color});
}

// Back to the Options rows, the cursor on Info.
void close_page() {
    open = false;
    WR32(RD32(MENU) + MENU_CURSOR, INFO_ITEM);
    log("recomp menu: closed\n");
}

}  // namespace

extern "C" void game_hook_options_init(void) { open = false; }

// Info (item 21): open the Recomp page instead of the DIAGNOSTIC page.
extern "C" int game_hook_options_info(void) {
    if (rows.empty()) rows = make_rows();
    if (!text_mem) text_mem = heap_alloc(SLOT * 64, true);
    open = true;
    WR32(RD32(MENU) + MENU_CURSOR, 0);
    log("recomp menu: open\n");
    return 1;
}

// Options frame, first row about to be drawn (ebx = cursor item): draw our rows and Back, then
// continue after the game's buttons.
extern "C" int game_hook_options_rows(void) {
    if (!open) return 0;
    // Escape (gamepad B) = Back
    static bool esc_was = false;
    bool esc = w32::key_state(0x1B) & 0x8000;
    if (esc && !esc_was) {
        esc_was = true;
        close_page();
        return 0;   // draw the game's own rows this frame
    }
    esc_was = esc;
    uint32_t menu = RD32(MENU), cursor = R.ebx, n = static_cast<uint32_t>(rows.size());
    uint32_t saved[8] = {R.eax, R.ecx, R.edx, R.ebx, R.esi, R.edi, R.ebp, 0};
    for (uint32_t i = 0; i < n; i++) {
        uint32_t y = ROW_Y + ROW_STEP * i;
        set_color(i == cursor ? COLOR_SELECTED : COLOR_NORMAL);
        uint32_t label = put_string(3 * i, std::string(rows[i].label) + " :");
        guest_call_thiscall(DRAW_TEXT, font(), {label, LABEL_X, y, 0, 0, 0, 0, 0});
        uint32_t value = put_string(3 * i + 1, rows[i].value());
        uint32_t pt = text_mem + (3 * i + 2) * SLOT;
        WR32(pt, VALUE_X);
        WR32(pt + 4, y);
        guest_call_thiscall(ADD_ITEM, menu, {pt, value, font(), nav(i ? i - 1 : 0xff, i + 1), 0, 0xffffffffu, 0});
    }
    set_color(COLOR_NORMAL);
    // Back: the Accept button, item n
    guest_call_thiscall(ADD_BUTTON, menu,
                        {RD32(ACCEPT_SPRITE), 0, nav(n - 1, 0xff), 0xffffffffu, 0xffffffffu, 0, 0, 0,
                         0xffffffffu, 0xffffffffu, 0});
    R.eax = saved[0]; R.ecx = saved[1]; R.edx = saved[2]; R.ebx = saved[3];
    R.esi = saved[4]; R.edi = saved[5]; R.ebp = saved[6];
    return 1;
}

// Options frame, item dispatch (esi = activated item, -1 = none).
extern "C" int game_hook_options_action(void) {
    if (!open) return 0;
    int32_t item = static_cast<int32_t>(R.esi);
    if (item < 0) return 1;
    if (static_cast<size_t>(item) < rows.size()) {
        rows[static_cast<size_t>(item)].next();
        log("recomp menu: %s = %s\n", rows[static_cast<size_t>(item)].label, rows[static_cast<size_t>(item)].value().c_str());
    } else {
        close_page();   // Back
    }
    return 1;
}
