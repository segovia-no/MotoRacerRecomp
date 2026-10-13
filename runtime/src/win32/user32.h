// Window manager internals shared between USER32 and the host (SDL) layer.
#pragma once
#include <SDL3/SDL_events.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace w32 {

enum : uint32_t {
    WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_MOVE = 0x0003, WM_SIZE = 0x0005, WM_ACTIVATE = 0x0006,
    WM_SETFOCUS = 0x0007, WM_KILLFOCUS = 0x0008, WM_ENABLE = 0x000A, WM_SETTEXT = 0x000C,
    WM_GETTEXT = 0x000D, WM_GETTEXTLENGTH = 0x000E, WM_PAINT = 0x000F, WM_CLOSE = 0x0010,
    WM_QUERYENDSESSION = 0x0011, WM_QUIT = 0x0012, WM_ERASEBKGND = 0x0014, WM_SHOWWINDOW = 0x0018,
    WM_ACTIVATEAPP = 0x001C, WM_SETCURSOR = 0x0020, WM_MOUSEACTIVATE = 0x0021, WM_GETMINMAXINFO = 0x0024,
    WM_WINDOWPOSCHANGING = 0x0046, WM_WINDOWPOSCHANGED = 0x0047, WM_NCCREATE = 0x0081,
    WM_NCDESTROY = 0x0082, WM_NCCALCSIZE = 0x0083, WM_NCHITTEST = 0x0084, WM_NCACTIVATE = 0x0086,
    WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105,
    WM_SYSCOMMAND = 0x0112, WM_TIMER = 0x0113, WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201,
    WM_LBUTTONUP = 0x0202, WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205,
};

enum : uint32_t {
    WS_POPUP = 0x80000000, WS_CHILD = 0x40000000, WS_VISIBLE = 0x10000000, WS_DISABLED = 0x08000000,
    WS_CAPTION = 0x00C00000, WS_BORDER = 0x00800000, WS_THICKFRAME = 0x00040000,
};

enum : int32_t {
    GWL_WNDPROC = -4, GWL_HINSTANCE = -6, GWL_HWNDPARENT = -8, GWL_STYLE = -16, GWL_EXSTYLE = -20,
    GWL_USERDATA = -21, GWL_ID = -12,
};

constexpr int WH_CBT = 5;
constexpr uint32_t HCBT_CREATEWND = 3;
constexpr uint32_t WA_ACTIVE = 1;

struct Rect { int32_t left, top, right, bottom; };

struct Window {
    uint32_t hwnd = 0, wndproc = 0, style = 0, exstyle = 0, parent = 0, id = 0, menu = 0;
    uint32_t hinstance = 0, userdata = 0;
    std::string class_name, text;
    std::vector<uint8_t> extra;
    std::map<std::string, uint32_t> props;
    Rect rect{};           // window rect, relative to parent client area (screen for top-level)
    Rect client_inset{};   // non-client border widths
    bool destroying = false;
    uint32_t dialog_result = 0;
    int32_t client_w() const { return rect.right - rect.left - client_inset.left - client_inset.right; }
    int32_t client_h() const { return rect.bottom - rect.top - client_inset.top - client_inset.bottom; }
};

extern std::map<uint32_t, std::unique_ptr<Window>> windows;
extern uint32_t main_window;

Window *window(uint32_t hwnd);
uint32_t send_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t create_window(uint32_t exstyle, uint32_t cls, uint32_t name, uint32_t style, int32_t x, int32_t y,
                       int32_t cx, int32_t cy, uint32_t parent, uint32_t menu, uint32_t hinst, uint32_t param);
void destroy_window(uint32_t hwnd);
uint32_t show_window(uint32_t hwnd, uint32_t cmd);
void pump_host_events();
uint32_t default_window_proc_va();   // guest-callable DefWindowProcA
uint32_t dialog_class_proc();        // guest-callable DefDlgProcA
uint32_t create_dialog_indirect(uint32_t hinst, uint32_t tmpl, uint32_t parent, uint32_t dlgproc, uint32_t param);

// Implemented by the host layer (host_window.cpp).
void host_window_shown(Window *w);
void host_window_resized(Window *w);
void host_window_destroyed();
void host_event_hook(const SDL_Event &e);
uint32_t key_state(uint32_t vk);
void cd_audio_tick();   // winmm_cd.cpp

}  // namespace w32
