// In-game Recomp settings page, in the game's own Options look (docs/17-recomp-menu.md).
//
// Options' Info button (item 21, which opens the DIAGNOSTIC page) opens it instead. It is the
// Options page (11) in another mode: the game's per-frame function 0x443200 still draws the
// background, header and buttons and runs the menu; jump hooks replace its rows with ours (drawn
// with the game's own font and item functions) and handle our items. Changes apply at once where
// possible and are saved to motoracer.ini [Recomp].
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
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

bool page_open = false;
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
    page_open = false;
    WR32(RD32(MENU) + MENU_CURSOR, INFO_ITEM);
    log("recomp menu: closed\n");
}

}  // namespace

extern "C" void game_hook_options_init(void) {
    page_open = false;
}

// Info (item 21): open the Recomp page instead of the DIAGNOSTIC page.
extern "C" int game_hook_options_info(void) {
    if (rows.empty()) rows = make_rows();
    if (!text_mem) text_mem = heap_alloc(SLOT * 64, true);
    page_open = true;
    WR32(RD32(MENU) + MENU_CURSOR, 0);
    log("recomp menu: open\n");
    return 1;
}

// Options frame, first row about to be drawn (ebx = cursor item): draw our rows and Back, then
// continue after the game's buttons.
extern "C" int game_hook_options_rows(void) {
    if (!page_open) return 0;
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
    if (!page_open) return 0;
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

// ---------- the "Recomp" button image ----------
// The Info button's label is part of its sprite (LEZ1-packed TGA, unpacked by 0x43eb00 into RGB565
// pixels, black = transparent). The runtime repaints the unpacked pixels: the old label's strokes
// are found by local contrast and filled from the oval on either side, then "Recomp" is drawn with
// a small stroked, sheared vector font sized like the game's button lettering. Only the game's own
// pixels on this machine are used; nothing of the game's art is shipped.
namespace {

constexpr uint32_t INFO_SPRITE = 0x504b28;

struct Rgb { float r, g, b; };
Rgb from565(uint16_t v) { return {float((v >> 11) << 3), float(((v >> 5) & 63) << 2), float((v & 31) << 3)}; }
uint16_t to565(Rgb c) {
    auto q = [](float v, int bits) { return std::min((1 << bits) - 1, std::max(0, int(v) >> (8 - bits))); };
    uint16_t v = static_cast<uint16_t>(q(c.r, 5) << 11 | q(c.g, 6) << 5 | q(c.b, 5));
    return v ? v : 1;   // 0 is the transparent key
}
int lum(uint16_t v) { Rgb c = from565(v); return int(c.r * 3 + c.g * 6 + c.b) / 10; }

struct Seg { float x0, y0, x1, y1; };

// Stroked glyphs in pixels, y up from the baseline (before the italic shear).
constexpr float CAP = 17.f, XH = 12.f, DESC = 5.f, STROKE = 2.5f, SHEAR = 0.45f, BASELINE = 22.5f;

void arc(std::vector<std::pair<float, float>> &p, float cx, float cy, float rx, float ry, float a0, float a1) {
    const int n = 24;
    for (int i = 0; i <= n; i++) {
        float a = (a0 + (a1 - a0) * i / n) * 3.14159265f / 180.f;
        p.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
    }
}

std::vector<Seg> layout_recomp(float centre_x) {
    using Poly = std::vector<std::pair<float, float>>;
    struct Glyph { float advance; std::vector<Poly> polys; };
    const float r = XH / 2, bw = 7.5f, a = 3.3f;
    std::vector<Glyph> text;
    {   // R
        Poly stem{{0, 0}, {0, CAP}}, bowl{{0, CAP}, {bw - 3.6f, CAP}};
        arc(bowl, bw - 3.6f, CAP - 3.6f, 3.6f, 3.6f, 90, -90);
        bowl.push_back({0, CAP - 7.2f});
        text.push_back({bw + 3.5f, {stem, bowl, {{bw - 4.2f, CAP - 7.2f}, {bw + 1.2f, 0}}}});
    }
    {   // e
        Poly e{{0.2f, r}, {2 * r, r}};
        arc(e, r, r, r, r, 0, 315);
        text.push_back({2 * r + 1.5f, {e}});
    }
    {   // c
        Poly c;
        arc(c, r, r, r, r, 45, 315);
        text.push_back({2 * r + 1.f, {c}});
    }
    {   // o
        Poly o;
        arc(o, r, r, r, r, 0, 360);
        text.push_back({2 * r + 1.5f, {o}});
    }
    {   // m
        Poly a1{{0, XH - a}}, a2{{2 * a, XH - a}};
        arc(a1, a, XH - a, a, a, 180, 0);
        a1.push_back({2 * a, 0});
        arc(a2, 3 * a, XH - a, a, a, 180, 0);
        a2.push_back({4 * a, 0});
        text.push_back({4 * a + 2.f, {{{0, 0}, {0, XH}}, a1, a2}});
    }
    {   // p
        Poly bowl;
        arc(bowl, r, r, r, r, 0, 360);
        text.push_back({2 * r + 2.f, {{{0, -DESC}, {0, XH}}, bowl}});
    }
    float total = -1.5f;
    for (auto &g : text) total += g.advance;
    std::vector<Seg> segs;
    float pen = 0;
    for (auto &g : text) {
        for (auto &poly : g.polys)
            for (size_t i = 1; i < poly.size(); i++) {
                auto map = [&](std::pair<float, float> q) {
                    return std::pair<float, float>{centre_x - total / 2 + pen + q.first + (q.second - CAP / 2) * SHEAR,
                                                   BASELINE - q.second};
                };
                auto [x0, y0] = map(poly[i - 1]);
                auto [x1, y1] = map(poly[i]);
                segs.push_back({x0, y0, x1, y1});
            }
        pen += g.advance;
    }
    return segs;
}

float coverage(const std::vector<Seg> &segs, float x, float y) {
    float best = 1e9f;
    for (const Seg &s : segs) {
        float dx = s.x1 - s.x0, dy = s.y1 - s.y0, l = dx * dx + dy * dy;
        float t = l == 0 ? 0 : std::clamp(((x - s.x0) * dx + (y - s.y0) * dy) / l, 0.f, 1.f);
        best = std::min(best, std::hypot(x - s.x0 - t * dx, y - s.y0 - t * dy));
    }
    return std::clamp(STROKE / 2 - best + 0.5f, 0.f, 1.f);
}

void repaint_info_button(uint32_t pixels, int w, int h) {
    if (w < 80 || h < 30) return;
    std::vector<uint16_t> px(static_cast<size_t>(w * h));
    for (int i = 0; i < w * h; i++) px[i] = RD16(pixels + 2 * static_cast<uint32_t>(i));
    auto at = [&](int x, int y) { return px[static_cast<size_t>(y * w + x)]; };
    // 1. the old label: dark against its row neighbourhood, in the text band
    const int y0 = 4, y1 = std::min(h, 24);
    std::vector<uint8_t> mask(px.size(), 0);
    Rgb ink{16, 16, 16};
    int ink_n = 0, bx0 = w, bx1 = 0;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < w; x++) {
            uint16_t v = at(x, y);
            if (!v) continue;
            int l = lum(v), nb = -1;
            for (int xx : {x - 4, x - 3, x + 3, x + 4})
                if (xx >= 0 && xx < w && at(xx, y)) nb = std::max(nb, lum(at(xx, y)));
            if (l < 60 || (nb >= 0 && l < nb - 35)) {
                mask[static_cast<size_t>(y * w + x)] = 1;
                if (x > w / 5 && x < w * 5 / 6) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); }
                if (l < 50) { Rgb c = from565(v); ink = {(ink.r * ink_n + c.r) / (ink_n + 1), (ink.g * ink_n + c.g) / (ink_n + 1), (ink.b * ink_n + c.b) / (ink_n + 1)}; ink_n++; }
            }
        }
    if (bx0 >= bx1) return;
    bx0 = std::max(1, bx0 - 2);
    bx1 = std::min(w - 2, bx1 + 2);
    // 2. fill each run of label pixels (grown by one) from the oval on both sides
    for (int y = y0; y < y1; y++) {
        auto m = [&](int x) { return mask[static_cast<size_t>(y * w + x)] || mask[static_cast<size_t>(y * w + x - 1)] || mask[static_cast<size_t>(y * w + x + 1)]; };
        for (int x = bx0; x <= bx1;) {
            if (!m(x) || !at(x, y)) { x++; continue; }
            int a = x;
            while (x <= bx1 && m(x)) x++;
            Rgb l = from565(at(a - 1, y)), r = from565(at(std::min(x, w - 1), y));
            for (int i = a; i < x; i++) {
                float t = float(i - a + 1) / float(x - a + 1);
                px[static_cast<size_t>(y * w + i)] = to565({l.r + (r.r - l.r) * t, l.g + (r.g - l.g) * t, l.b + (r.b - l.b) * t});
            }
        }
    }
    // 3. "Recomp", 3x3 supersampled
    std::vector<Seg> segs = layout_recomp((bx0 + bx1) / 2.f);
    for (int y = 2; y < std::min(h, 32); y++)
        for (int x = 0; x < w; x++) {
            uint16_t v = at(x, y);
            if (!v) continue;
            float c = 0;
            for (int i = 0; i < 9; i++) c += coverage(segs, x + (i % 3 + 0.5f) / 3, y + (i / 3 + 0.5f) / 3);
            c /= 9;
            if (c <= 0) continue;
            Rgb b = from565(v);
            px[static_cast<size_t>(y * w + x)] = to565({b.r + (ink.r - b.r) * c, b.g + (ink.g - b.g) * c, b.b + (ink.b - b.b) * c});
        }
    for (int i = 0; i < w * h; i++) WR16(pixels + 2 * static_cast<uint32_t>(i), px[static_cast<size_t>(i)]);
}

}  // namespace

// A menu button sprite was just unpacked (eax = its 16-bit pixels, ebx = the sprite object).
extern "C" void game_hook_button_unpacked(void) {
    uint32_t sprite = R.ebx;
    if (sprite != RD32(INFO_SPRITE) || !R.eax) return;
    repaint_info_button(R.eax, static_cast<int>(RD32(sprite + 8)), static_cast<int>(RD32(sprite + 0xc)));
    log("recomp menu: Info button relabelled\n");
}

// ---------- Options bottom row navigation ----------
// The buttons sit left to right as Info (Recomp, item 21), Data disk (22), Accept (20). The game's
// nav masks (up | down | right | left) skip buttons: Accept 0x12ffff15 (left = Info, no right), Info
// 0x12ff1416 (right = Accept, left = Data disk), Data disk 0x12ff14ff (no left). Right now moves
// along the row and left back, both wrapping; without Data disk ([0x4dc04c] <= 1, only Info and
// Accept) the two point at each other. Called just before each button's registration call.
extern "C" void game_hook_options_button_nav(void) {
    constexpr uint32_t ACCEPT = 20, INFO = 21, DATA_DISK = 22;
    uint32_t sprite = RD32(R.esp), nav = RD32(R.esp + 8);
    bool disk = static_cast<int32_t>(RD32(0x4dc04c)) > 1;
    uint32_t right, left;
    if (sprite == RD32(ACCEPT_SPRITE)) { right = INFO; left = disk ? DATA_DISK : INFO; }
    else if (sprite == RD32(INFO_SPRITE)) { right = disk ? DATA_DISK : ACCEPT; left = ACCEPT; }
    else if (sprite == RD32(0x504b5c)) { right = ACCEPT; left = INFO; }
    else return;
    WR32(R.esp + 8, (nav & 0xffff0000u) | right << 8 | left);
}
