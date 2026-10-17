// USER32: a small window manager faithful to what MFC 4.2 relies on.
//
// Windows are host objects; window procedures are guest code (or host thunks for built-in
// classes). Message order during CreateWindowEx follows Windows: CBT hook (MFC subclasses the
// window there), WM_GETMINMAXINFO, WM_NCCREATE, WM_NCCALCSIZE, WM_CREATE.
#include <SDL3/SDL.h>

#include <deque>
#include <map>
#include <vector>

#include "ddraw.h"
#include "host_video.h"
#include "user32.h"
#include "win32.h"

using namespace rt;
using namespace w32;

namespace w32 {

std::map<uint32_t, std::unique_ptr<Window>> windows;
uint32_t main_window = 0;

namespace {

struct WndClass {
    uint32_t style, wndproc, cls_extra, wnd_extra, hinstance, hicon, hcursor, hbrush;
    std::string menu_name, name;
    uint16_t atom;
};
std::vector<WndClass> classes;
uint16_t next_atom = 0xC000;
uint32_t next_hwnd = 0x00010010;

struct Msg { uint32_t hwnd, msg, wparam, lparam, time, x, y; };
std::deque<Msg> queue;
bool quit_posted = false;
uint32_t quit_code = 0;
Msg last_msg{};

struct Timer { uint32_t hwnd, id, interval, proc; uint64_t due; };
std::vector<Timer> timers;

struct Hook { int type; uint32_t proc; uint32_t handle; };
std::vector<Hook> hooks;
uint32_t next_hook = 0x7700;

uint32_t focus_hwnd, active_hwnd, capture_hwnd, cursor_handle;
uint32_t mouse_buttons;   // MK_LBUTTON | MK_RBUTTON
int32_t cursor_x, cursor_y;
std::map<std::string, uint32_t> registered_messages;

uint32_t default_proc_thunk;   // guest-callable DefWindowProcA for built-in classes

std::string upper(std::string s) { for (auto &c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c))); return s; }

uint32_t now_ms() { return static_cast<uint32_t>(SDL_GetTicks()); }

WndClass *find_class(uint32_t name_or_atom) {
    if (name_or_atom < 0x10000) {
        for (auto &c : classes) if (c.atom == name_or_atom) return &c;
        return nullptr;
    }
    std::string n = upper(read_cstr(name_or_atom));
    for (auto &c : classes) if (upper(c.name) == n) return &c;
    return nullptr;
}

void post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    queue.push_back({hwnd, msg, wp, lp, now_ms(), static_cast<uint32_t>(cursor_x), static_cast<uint32_t>(cursor_y)});
}

}  // namespace

Window *window(uint32_t hwnd) {
    auto it = windows.find(hwnd);
    return it == windows.end() ? nullptr : it->second.get();
}

uint32_t send_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Window *w = window(hwnd);
    if (!w) return 0;
    return guest_call_stdcall(w->wndproc, {hwnd, msg, wp, lp});
}

void post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) { post(hwnd, msg, wp, lp); }

// Pump SDL and translate host events into window messages.
void pump_host_events() {
    dx::ddraw_tick();
    cd_audio_tick();
    controller_tick();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            if (main_window) post(main_window, WM_CLOSE, 0, 0);
            break;
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            float wx = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x;
            float wy = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y;
            int gx, gy;
            if (!host::window_to_game(wx, wy, &gx, &gy)) break;
            cursor_x = gx;
            cursor_y = gy;
            if (e.type != SDL_EVENT_MOUSE_MOTION) {
                bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                if (e.button.button == SDL_BUTTON_LEFT) mouse_buttons = down ? mouse_buttons | 1 : mouse_buttons & ~1u;
                if (e.button.button == SDL_BUTTON_RIGHT) mouse_buttons = down ? mouse_buttons | 2 : mouse_buttons & ~2u;
            }
            uint32_t target = capture_hwnd ? capture_hwnd : main_window;
            if (!target) break;
            uint32_t mk = mouse_buttons | (key_state(0x10) & 0x8000 ? 4 : 0) | (key_state(0x11) & 0x8000 ? 8 : 0);
            uint32_t lp = (static_cast<uint32_t>(gy) << 16) | (static_cast<uint32_t>(gx) & 0xFFFF);
            uint32_t msg = WM_MOUSEMOVE;
            if (e.type != SDL_EVENT_MOUSE_MOTION) {
                bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                if (e.button.button == SDL_BUTTON_LEFT) msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                else if (e.button.button == SDL_BUTTON_RIGHT) msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
                else break;
            }
            post(target, msg, mk, lp);
            break;
        }
        default:
            host_event_hook(e);
            break;
        }
    }
}

namespace {

bool next_message(Msg &out, uint32_t hwnd_filter, uint32_t min, uint32_t max, bool remove) {
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if (hwnd_filter && it->hwnd != hwnd_filter) continue;
        if ((min || max) && (it->msg < min || it->msg > max)) continue;
        out = *it;
        if (remove) queue.erase(it);
        return true;
    }
    if (quit_posted && (!min && !max)) {
        out = {0, WM_QUIT, quit_code, 0, now_ms(), 0, 0};
        if (remove) quit_posted = false;
        return true;
    }
    uint64_t t = now_ms();
    for (auto &tm : timers) {
        if (t >= tm.due && (!hwnd_filter || tm.hwnd == hwnd_filter) &&
            ((!min && !max) || (WM_TIMER >= min && WM_TIMER <= max))) {
            out = {tm.hwnd, WM_TIMER, tm.id, tm.proc, static_cast<uint32_t>(t), 0, 0};
            if (remove) tm.due = t + tm.interval;
            return true;
        }
    }
    return false;
}

void write_msg(uint32_t p, const Msg &m) {
    WR32(p + 0, m.hwnd);
    WR32(p + 4, m.msg);
    WR32(p + 8, m.wparam);
    WR32(p + 12, m.lparam);
    WR32(p + 16, m.time);
    WR32(p + 20, m.x);
    WR32(p + 24, m.y);
    last_msg = m;
}

void activate(uint32_t hwnd) {
    if (active_hwnd == hwnd) return;
    uint32_t old = active_hwnd;
    active_hwnd = hwnd;
    if (!old) send_message(hwnd, WM_ACTIVATEAPP, 1, 1);
    send_message(hwnd, WM_NCACTIVATE, 1, 0);
    send_message(hwnd, WM_ACTIVATE, WA_ACTIVE, old);
    if (focus_hwnd != hwnd) {
        uint32_t prev = focus_hwnd;
        focus_hwnd = hwnd;
        send_message(hwnd, WM_SETFOCUS, prev, 0);
    }
}

void client_offset(Window *w, int32_t &dx, int32_t &dy) {
    dx = w->rect.left + w->client_inset.left;
    dy = w->rect.top + w->client_inset.top;
    for (Window *p = window(w->parent); p && (w->style & WS_CHILD); w = p, p = window(p->parent)) {
        dx += p->rect.left + p->client_inset.left;
        dy += p->rect.top + p->client_inset.top;
    }
}

Rect nonclient_inset(uint32_t style, uint32_t exstyle) {
    Rect r{0, 0, 0, 0};
    int32_t b = 0;
    if (style & WS_THICKFRAME) b = 4;
    else if ((style & WS_CAPTION) || (style & WS_BORDER) || (exstyle & 1)) b = (style & WS_CAPTION) == WS_CAPTION ? 3 : 1;
    r.left = r.top = r.right = r.bottom = b;
    if ((style & WS_CAPTION) == WS_CAPTION) r.top += 19;
    return r;
}

// Built-in window procedure (DefWindowProcA semantics), also used for system classes.
uint32_t def_window_proc(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Window *w = window(hwnd);
    switch (msg) {
    case WM_NCCREATE:
        if (w && lp) w->text = read_cstr(RD32(lp + 36));
        return 1;
    case WM_SETTEXT:
        if (w) w->text = read_cstr(lp);
        return 1;
    case WM_GETTEXT:
        return w ? write_cstr(lp, w->text, wp) : 0;
    case WM_GETTEXTLENGTH:
        return w ? static_cast<uint32_t>(w->text.size()) : 0;
    case WM_CLOSE:
        destroy_window(hwnd);
        return 0;
    case WM_WINDOWPOSCHANGED:
        if (w) {
            uint32_t flags = RD32(lp + 24);
            if (!(flags & 2)) send_message(hwnd, WM_MOVE, 0, ((w->rect.top & 0xFFFF) << 16) | (w->rect.left & 0xFFFF));
            if (!(flags & 1)) {
                uint32_t cw = static_cast<uint32_t>(w->client_w()), ch = static_cast<uint32_t>(w->client_h());
                send_message(hwnd, WM_SIZE, 0, (ch << 16) | (cw & 0xFFFF));
            }
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == 0xF060) send_message(hwnd, WM_CLOSE, 0, 0);   // SC_CLOSE
        return 0;
    case WM_NCHITTEST: return 1;           // HTCLIENT
    case WM_MOUSEACTIVATE: return 1;       // MA_ACTIVATE
    case WM_ERASEBKGND: return 1;
    case WM_QUERYENDSESSION: return 1;
    case WM_ACTIVATE:
        if ((wp & 0xFFFF) && w) focus_hwnd = hwnd;
        return 0;
    case WM_PAINT:
        return 0;
    default:
        return 0;
    }
}

void def_window_proc_thunk() {
    uint32_t r = def_window_proc(arg(0), arg(1), arg(2), arg(3));
    R.eax = r;
    R.esp += 4 + 16;
}

void ensure_builtin_classes() {
    if (default_proc_thunk) return;
    default_proc_thunk = register_thunk(def_window_proc_thunk, "DefWindowProc(builtin)");
    for (const char *n : {"BUTTON", "EDIT", "STATIC", "LISTBOX", "COMBOBOX", "SCROLLBAR", "#32770", "MDICLIENT"}) {
        WndClass c{};
        c.name = n;
        bool dlg = std::string(n) == "#32770";
        c.wndproc = dlg ? dialog_class_proc() : default_proc_thunk;
        c.wnd_extra = dlg ? 30 : 0;
        c.atom = next_atom++;
        classes.push_back(c);
    }
}

uint32_t call_hooks(int type, uint32_t code, uint32_t wp, uint32_t lp) {
    for (auto it = hooks.rbegin(); it != hooks.rend(); ++it)
        if (it->type == type) return guest_call_stdcall(it->proc, {code, wp, lp});
    return 0;
}

}  // namespace

uint32_t default_window_proc_va() {
    ensure_builtin_classes();
    return default_proc_thunk;
}

void destroy_window(uint32_t hwnd) {
    Window *w = window(hwnd);
    if (!w || w->destroying) return;
    w->destroying = true;
    std::vector<uint32_t> children;
    for (auto &[h, c] : windows) if (c->parent == hwnd) children.push_back(h);
    send_message(hwnd, WM_DESTROY, 0, 0);
    for (uint32_t c : children) destroy_window(c);
    send_message(hwnd, WM_NCDESTROY, 0, 0);
    if (focus_hwnd == hwnd) focus_hwnd = 0;
    if (active_hwnd == hwnd) active_hwnd = 0;
    if (capture_hwnd == hwnd) capture_hwnd = 0;
    if (main_window == hwnd) { main_window = 0; host_window_destroyed(); }
    std::erase_if(timers, [&](const Timer &t) { return t.hwnd == hwnd; });
    windows.erase(hwnd);
}

uint32_t create_window(uint32_t exstyle, uint32_t cls_arg, uint32_t name, uint32_t style, int32_t x, int32_t y,
                       int32_t cx, int32_t cy, uint32_t parent, uint32_t menu, uint32_t hinst, uint32_t param) {
    ensure_builtin_classes();
    WndClass *cls = find_class(cls_arg);
    if (!cls) {
        log("CreateWindowExA: unknown class %s\n", cls_arg < 0x10000 ? "(atom)" : read_cstr(cls_arg).c_str());
        return 0;
    }
    if (x == static_cast<int32_t>(0x80000000)) x = y = 0;
    if (cx == static_cast<int32_t>(0x80000000)) { cx = 640; cy = 480; }

    auto w = std::make_unique<Window>();
    uint32_t hwnd = next_hwnd;
    next_hwnd += 4;
    w->hwnd = hwnd;
    w->wndproc = cls->wndproc;
    w->style = style;
    w->exstyle = exstyle;
    w->parent = parent;
    w->id = (style & WS_CHILD) ? menu : 0;
    w->menu = (style & WS_CHILD) ? 0 : menu;
    w->hinstance = hinst;
    w->class_name = cls->name;
    w->extra.assign(cls->wnd_extra, 0);
    w->rect = {x, y, x + cx, y + cy};
    w->client_inset = nonclient_inset(style, exstyle);
    windows[hwnd] = std::move(w);
    bool top = !(style & WS_CHILD);
    if (top && !main_window) main_window = hwnd;
    log("CreateWindowExA(class \"%s\", \"%s\", style %08x ex %08x, %d,%d %dx%d) -> %08x\n", cls->name.c_str(),
        read_cstr(name).c_str(), style, exstyle, x, y, cx, cy, hwnd);

    // CREATESTRUCTA lives in guest memory for the duration of creation.
    uint32_t cs = heap_alloc(48 + 8, true);
    WR32(cs + 0, param);
    WR32(cs + 4, hinst);
    WR32(cs + 8, menu);
    WR32(cs + 12, parent);
    WR32(cs + 16, static_cast<uint32_t>(cy));
    WR32(cs + 20, static_cast<uint32_t>(cx));
    WR32(cs + 24, static_cast<uint32_t>(y));
    WR32(cs + 28, static_cast<uint32_t>(x));
    WR32(cs + 32, style);
    WR32(cs + 36, name);
    WR32(cs + 40, cls_arg);
    WR32(cs + 44, exstyle);
    uint32_t cbt = cs + 48;   // CBT_CREATEWNDA { lpcs, hwndInsertAfter }
    WR32(cbt, cs);
    WR32(cbt + 4, 0);

    call_hooks(WH_CBT, HCBT_CREATEWND, hwnd, cbt);
    uint32_t mmi = heap_alloc(40, true);
    send_message(hwnd, WM_GETMINMAXINFO, 0, mmi);
    heap_free(mmi);
    if (!send_message(hwnd, WM_NCCREATE, 0, cs)) {
        log("  WM_NCCREATE refused\n");
        destroy_window(hwnd);
        heap_free(cs);
        return 0;
    }
    if (static_cast<int32_t>(send_message(hwnd, WM_CREATE, 0, cs)) == -1) {
        log("  WM_CREATE failed\n");
        destroy_window(hwnd);
        heap_free(cs);
        return 0;
    }
    heap_free(cs);
    if (Window *nw = window(hwnd); nw && (style & WS_VISIBLE)) {
        nw->style &= ~WS_VISIBLE;
        show_window(hwnd, 1);
    }
    return hwnd;
}

uint32_t show_window(uint32_t hwnd, uint32_t cmd) {
    Window *w = window(hwnd);
    if (!w) return 0;
    bool was = w->style & WS_VISIBLE;
    bool show = cmd != 0;   // SW_HIDE = 0
    if (show != was) {
        send_message(hwnd, WM_SHOWWINDOW, show, 0);
        if ((w = window(hwnd)) == nullptr) return was;
        if (show) w->style |= WS_VISIBLE; else w->style &= ~WS_VISIBLE;
        if (show && !(w->style & WS_CHILD)) {
            if (hwnd == main_window) host_window_shown(w);
            activate(hwnd);
            uint32_t cw = static_cast<uint32_t>(w->client_w()), ch = static_cast<uint32_t>(w->client_h());
            send_message(hwnd, WM_SIZE, 0, (ch << 16) | (cw & 0xFFFF));
            send_message(hwnd, WM_MOVE, 0, ((w->rect.top & 0xFFFF) << 16) | (w->rect.left & 0xFFFF));
        }
    }
    return was;
}

}  // namespace w32

// =====================================================================================
// Classes
// =====================================================================================
STDCALL(USER32, RegisterClassA, 1) {
    ensure_builtin_classes();
    uint32_t p = arg(0);
    WndClass c{};
    c.style = RD32(p);
    c.wndproc = RD32(p + 4);
    c.cls_extra = RD32(p + 8);
    c.wnd_extra = RD32(p + 12);
    c.hinstance = RD32(p + 16);
    c.hicon = RD32(p + 20);
    c.hcursor = RD32(p + 24);
    c.hbrush = RD32(p + 28);
    c.menu_name = RD32(p + 32) >= 0x10000 ? read_cstr(RD32(p + 32)) : "";
    c.name = read_cstr(RD32(p + 36));
    if (find_class(RD32(p + 36))) return 0;   // ERROR_CLASS_ALREADY_EXISTS
    c.atom = next_atom++;
    log("RegisterClassA(\"%s\", wndproc 0x%08x)\n", c.name.c_str(), c.wndproc);
    classes.push_back(c);
    return c.atom;
}

STDCALL(USER32, GetClassInfoA, 3) {
    ensure_builtin_classes();
    WndClass *c = find_class(arg(1));
    if (!c) return 0;
    uint32_t p = arg(2);
    WR32(p, c->style);
    WR32(p + 4, c->wndproc);
    WR32(p + 8, c->cls_extra);
    WR32(p + 12, c->wnd_extra);
    WR32(p + 16, c->hinstance);
    WR32(p + 20, c->hicon);
    WR32(p + 24, c->hcursor);
    WR32(p + 28, c->hbrush);
    WR32(p + 32, 0);
    WR32(p + 36, arg(1));
    return c->atom;
}

STDCALL(USER32, UnregisterClassA, 2) {
    std::string n = upper(arg(0) < 0x10000 ? "" : read_cstr(arg(0)));
    std::erase_if(classes, [&](const WndClass &c) { return arg(0) < 0x10000 ? c.atom == arg(0) : upper(c.name) == n; });
    return 1;
}

STDCALL(USER32, GetClassNameA, 3) {
    Window *w = window(arg(0));
    return w ? write_cstr(arg(1), w->class_name, arg(2)) : 0;
}

// =====================================================================================
// Window lifetime and properties
// =====================================================================================
STDCALL(USER32, CreateWindowExA, 12) {
    return create_window(arg(0), arg(1), arg(2), arg(3), static_cast<int32_t>(arg(4)), static_cast<int32_t>(arg(5)),
                         static_cast<int32_t>(arg(6)), static_cast<int32_t>(arg(7)), arg(8), arg(9), arg(10), arg(11));
}

STDCALL(USER32, DestroyWindow, 1) {
    if (!window(arg(0))) return 0;
    destroy_window(arg(0));
    return 1;
}

STDCALL(USER32, IsWindow, 1) { return window(arg(0)) ? 1 : 0; }
STDCALL(USER32, IsWindowVisible, 1) { Window *w = window(arg(0)); return w && (w->style & WS_VISIBLE); }
STDCALL(USER32, IsWindowEnabled, 1) { Window *w = window(arg(0)); return w && !(w->style & WS_DISABLED); }
STDCALL(USER32, EnableWindow, 2) {
    Window *w = window(arg(0));
    if (!w) return 0;
    bool was_disabled = w->style & WS_DISABLED;
    if (arg(1)) w->style &= ~WS_DISABLED; else w->style |= WS_DISABLED;
    send_message(arg(0), WM_ENABLE, arg(1) ? 1 : 0, 0);
    return was_disabled;
}

STDCALL(USER32, ShowWindow, 2) { return show_window(arg(0), arg(1)); }
STDCALL(USER32, UpdateWindow, 1) { return window(arg(0)) ? 1 : 0; }
STDCALL(USER32, ValidateRect, 2) { return 1; }

STDCALL(USER32, GetWindowLongA, 2) {
    Window *w = window(arg(0));
    if (!w) return 0;
    int32_t idx = static_cast<int32_t>(arg(1));
    switch (idx) {
    case GWL_WNDPROC: return w->wndproc;
    case GWL_HINSTANCE: return w->hinstance;
    case GWL_HWNDPARENT: return w->parent;
    case GWL_STYLE: return w->style;
    case GWL_EXSTYLE: return w->exstyle;
    case GWL_USERDATA: return w->userdata;
    case GWL_ID: return w->id;
    default:
        if (idx >= 0 && static_cast<size_t>(idx) + 4 <= w->extra.size()) {
            uint32_t v;
            memcpy(&v, w->extra.data() + idx, 4);
            return v;
        }
        return 0;
    }
}

STDCALL(USER32, SetWindowLongA, 3) {
    Window *w = window(arg(0));
    if (!w) return 0;
    int32_t idx = static_cast<int32_t>(arg(1));
    uint32_t v = arg(2), old = 0;
    switch (idx) {
    case GWL_WNDPROC: old = w->wndproc; w->wndproc = v; break;
    case GWL_HINSTANCE: old = w->hinstance; w->hinstance = v; break;
    case GWL_HWNDPARENT: old = w->parent; w->parent = v; break;
    case GWL_STYLE: old = w->style; w->style = v; break;
    case GWL_EXSTYLE: old = w->exstyle; w->exstyle = v; break;
    case GWL_USERDATA: old = w->userdata; w->userdata = v; break;
    case GWL_ID: old = w->id; w->id = v; break;
    default:
        if (idx >= 0 && static_cast<size_t>(idx) + 4 <= w->extra.size()) {
            memcpy(&old, w->extra.data() + idx, 4);
            memcpy(w->extra.data() + idx, &v, 4);
        }
    }
    return old;
}

STDCALL(USER32, SetPropA, 3) {
    Window *w = window(arg(0));
    if (!w) return 0;
    w->props[arg(1) < 0x10000 ? "#" + std::to_string(arg(1)) : read_cstr(arg(1))] = arg(2);
    return 1;
}

STDCALL(USER32, GetPropA, 2) {
    Window *w = window(arg(0));
    if (!w) return 0;
    auto it = w->props.find(arg(1) < 0x10000 ? "#" + std::to_string(arg(1)) : read_cstr(arg(1)));
    return it == w->props.end() ? 0 : it->second;
}

STDCALL(USER32, RemovePropA, 2) {
    Window *w = window(arg(0));
    if (!w) return 0;
    std::string k = arg(1) < 0x10000 ? "#" + std::to_string(arg(1)) : read_cstr(arg(1));
    auto it = w->props.find(k);
    if (it == w->props.end()) return 0;
    uint32_t v = it->second;
    w->props.erase(it);
    return v;
}

STDCALL(USER32, SetWindowTextA, 2) { return send_message(arg(0), WM_SETTEXT, 0, arg(1)); }
STDCALL(USER32, GetWindowTextA, 3) { return send_message(arg(0), WM_GETTEXT, arg(2), arg(1)); }
STDCALL(USER32, GetWindowTextLengthA, 1) { return send_message(arg(0), WM_GETTEXTLENGTH, 0, 0); }

STDCALL(USER32, GetParent, 1) {
    Window *w = window(arg(0));
    return w && (w->style & WS_CHILD) ? w->parent : 0;
}

STDCALL(USER32, GetWindow, 2) {
    Window *w = window(arg(0));
    if (!w) return 0;
    switch (arg(1)) {
    case 4: return w->parent;                   // GW_OWNER
    case 5:                                     // GW_CHILD
        for (auto &[h, c] : windows) if (c->parent == arg(0) && (c->style & WS_CHILD)) return h;
        return 0;
    case 2: {                                   // GW_HWNDNEXT
        auto it = windows.find(arg(0));
        for (++it; it != windows.end(); ++it)
            if (it->second->parent == w->parent && ((it->second->style ^ w->style) & WS_CHILD) == 0) return it->first;
        return 0;
    }
    default: return 0;
    }
}

STDCALL(USER32, GetTopWindow, 1) {
    for (auto &[h, c] : windows)
        if (arg(0) ? c->parent == arg(0) && (c->style & WS_CHILD) : !(c->style & WS_CHILD)) return h;
    return 0;
}

STDCALL(USER32, GetLastActivePopup, 1) { return arg(0); }
STDCALL(USER32, GetDlgItem, 2) {
    for (auto &[h, c] : windows) if (c->parent == arg(0) && c->id == arg(1) && (c->style & WS_CHILD)) return h;
    return 0;
}
STDCALL(USER32, GetDlgCtrlID, 1) { Window *w = window(arg(0)); return w ? w->id : 0; }
STDCALL(USER32, GetNextDlgTabItem, 3) { return 0; }

STDCALL(USER32, WindowFromPoint, 2) { return main_window; }

// =====================================================================================
// Geometry
// =====================================================================================
static void write_rect(uint32_t p, const Rect &r) {
    WR32(p, static_cast<uint32_t>(r.left)); WR32(p + 4, static_cast<uint32_t>(r.top));
    WR32(p + 8, static_cast<uint32_t>(r.right)); WR32(p + 12, static_cast<uint32_t>(r.bottom));
}

STDCALL(USER32, GetWindowRect, 2) {
    Window *w = window(arg(0));
    if (!w) return 0;
    int32_t dx = 0, dy = 0;
    if (w->style & WS_CHILD) { client_offset(w, dx, dy); dx -= w->rect.left + w->client_inset.left; dy -= w->rect.top + w->client_inset.top; }
    write_rect(arg(1), {w->rect.left + dx, w->rect.top + dy, w->rect.right + dx, w->rect.bottom + dy});
    return 1;
}

STDCALL(USER32, GetClientRect, 2) {
    Window *w = window(arg(0));
    if (!w) return 0;
    write_rect(arg(1), {0, 0, w->client_w(), w->client_h()});
    return 1;
}

STDCALL(USER32, AdjustWindowRectEx, 4) {
    Rect in = nonclient_inset(arg(1), arg(3));
    uint32_t p = arg(0);
    WR32(p, RD32(p) - static_cast<uint32_t>(in.left));
    WR32(p + 4, RD32(p + 4) - static_cast<uint32_t>(in.top) - (arg(2) ? 19u : 0u));
    WR32(p + 8, RD32(p + 8) + static_cast<uint32_t>(in.right));
    WR32(p + 12, RD32(p + 12) + static_cast<uint32_t>(in.bottom));
    return 1;
}

STDCALL(USER32, SetWindowPos, 7) {
    uint32_t hwnd = arg(0);
    Window *w = window(hwnd);
    if (!w) return 0;
    int32_t x = static_cast<int32_t>(arg(2)), y = static_cast<int32_t>(arg(3));
    int32_t cx = static_cast<int32_t>(arg(4)), cy = static_cast<int32_t>(arg(5));
    uint32_t flags = arg(6);
    uint32_t wp = heap_alloc(28, true);
    WR32(wp + 0, hwnd);
    WR32(wp + 4, arg(1));
    WR32(wp + 8, static_cast<uint32_t>(x)); WR32(wp + 12, static_cast<uint32_t>(y));
    WR32(wp + 16, static_cast<uint32_t>(cx)); WR32(wp + 20, static_cast<uint32_t>(cy));
    WR32(wp + 24, flags);
    send_message(hwnd, WM_WINDOWPOSCHANGING, 0, wp);
    if ((w = window(hwnd)) != nullptr) {
        if (!(flags & 2)) { int32_t ww = w->rect.right - w->rect.left, hh = w->rect.bottom - w->rect.top; w->rect = {x, y, x + ww, y + hh}; }
        if (!(flags & 1)) { w->rect.right = w->rect.left + cx; w->rect.bottom = w->rect.top + cy; }
        if (hwnd == main_window && !(flags & 1)) host_window_resized(w);
        if (flags & 0x40) show_window(hwnd, 1);    // SWP_SHOWWINDOW
        if (flags & 0x80) show_window(hwnd, 0);    // SWP_HIDEWINDOW
        send_message(hwnd, WM_WINDOWPOSCHANGED, 0, wp);
    }
    heap_free(wp);
    return 1;
}

STDCALL(USER32, ClientToScreen, 2) {
    Window *w = window(arg(0));
    if (!w) return 0;
    int32_t dx, dy;
    client_offset(w, dx, dy);
    WR32(arg(1), RD32(arg(1)) + static_cast<uint32_t>(dx));
    WR32(arg(1) + 4, RD32(arg(1) + 4) + static_cast<uint32_t>(dy));
    return 1;
}

STDCALL(USER32, ScreenToClient, 2) {
    Window *w = window(arg(0));
    if (!w) return 0;
    int32_t dx, dy;
    client_offset(w, dx, dy);
    WR32(arg(1), RD32(arg(1)) - static_cast<uint32_t>(dx));
    WR32(arg(1) + 4, RD32(arg(1) + 4) - static_cast<uint32_t>(dy));
    return 1;
}

STDCALL(USER32, MapWindowPoints, 4) {
    int32_t fx = 0, fy = 0, tx = 0, ty = 0;
    if (Window *f = window(arg(0))) client_offset(f, fx, fy);
    if (Window *t = window(arg(1))) client_offset(t, tx, ty);
    for (uint32_t i = 0; i < arg(3); i++) {
        uint32_t p = arg(2) + 8 * i;
        WR32(p, RD32(p) + static_cast<uint32_t>(fx - tx));
        WR32(p + 4, RD32(p + 4) + static_cast<uint32_t>(fy - ty));
    }
    return static_cast<uint32_t>(((fy - ty) << 16) | ((fx - tx) & 0xFFFF));
}

STDCALL(USER32, SetRect, 5) {
    write_rect(arg(0), {static_cast<int32_t>(arg(1)), static_cast<int32_t>(arg(2)), static_cast<int32_t>(arg(3)), static_cast<int32_t>(arg(4))});
    return 1;
}

STDCALL(USER32, PtInRect, 3) {
    int32_t x = static_cast<int32_t>(arg(1)), y = static_cast<int32_t>(arg(2));
    uint32_t r = arg(0);
    return x >= static_cast<int32_t>(RD32(r)) && x < static_cast<int32_t>(RD32(r + 8)) &&
           y >= static_cast<int32_t>(RD32(r + 4)) && y < static_cast<int32_t>(RD32(r + 12));
}

// =====================================================================================
// Messages
// =====================================================================================
STDCALL(USER32, SendMessageA, 4) { return send_message(arg(0), arg(1), arg(2), arg(3)); }
STDCALL(USER32, SendDlgItemMessageA, 5) {
    for (auto &[h, c] : windows)
        if (c->parent == arg(0) && c->id == arg(1)) return send_message(h, arg(2), arg(3), arg(4));
    return 0;
}

STDCALL(USER32, PostMessageA, 4) {
    if (arg(0) && !window(arg(0))) return 0;
    post(arg(0), arg(1), arg(2), arg(3));
    return 1;
}

STDCALL(USER32, PostQuitMessage, 1) { quit_posted = true; quit_code = arg(0); return 0; }

STDCALL(USER32, DefWindowProcA, 4) { return def_window_proc(arg(0), arg(1), arg(2), arg(3)); }

STDCALL(USER32, CallWindowProcA, 5) { return guest_call_stdcall(arg(0), {arg(1), arg(2), arg(3), arg(4)}); }

STDCALL(USER32, GetMessageA, 4) {
    uint32_t p = arg(0), hwnd = arg(1), mn = arg(2), mx = arg(3);
    Msg m;
    for (;;) {
        pump_host_events();
        if (next_message(m, hwnd, mn, mx, true)) break;
        prof::Scope prof_wait(prof::WAIT);
        SDL_Delay(1);
    }
    write_msg(p, m);
    return m.msg != WM_QUIT;
}

STDCALL(USER32, PeekMessageA, 5) {
    uint32_t p = arg(0), hwnd = arg(1), mn = arg(2), mx = arg(3), flags = arg(4);
    pump_host_events();
    Msg m;
    if (!next_message(m, hwnd, mn, mx, flags & 1)) return 0;
    write_msg(p, m);
    return 1;
}

// WM_KEYDOWN -> WM_CHAR (WM_SYSKEYDOWN -> WM_SYSCHAR) for keys that type a character, US layout,
// as Windows does. The game's window procedure (0x432980) keeps the last WM_CHAR in 0x51be90; the
// end-of-race record pages (8: 0x44aee0, 9: 0x44d540) read the initials from it.
namespace {
uint32_t vk_to_char(uint32_t vk, bool shift, bool caps) {
    if (vk >= 'A' && vk <= 'Z') return (shift != caps) ? vk : vk + 32;
    if (vk >= '0' && vk <= '9') return shift ? static_cast<uint8_t>(")!@#$%^&*("[vk - '0']) : vk;
    if (vk >= 0x60 && vk <= 0x69) return '0' + (vk - 0x60);   // numpad digits
    switch (vk) {
    case 0x08: case 0x09: case 0x0D: case 0x1B: case 0x20: return vk;   // Backspace Tab Enter Esc Space
    case 0x6A: return '*';
    case 0x6B: return '+';
    case 0x6D: return '-';
    case 0x6E: return '.';
    case 0x6F: return '/';
    case 0xBA: return shift ? ':' : ';';
    case 0xBB: return shift ? '+' : '=';
    case 0xBC: return shift ? '<' : ',';
    case 0xBD: return shift ? '_' : '-';
    case 0xBE: return shift ? '>' : '.';
    case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`';
    case 0xDB: return shift ? '{' : '[';
    case 0xDC: return shift ? '|' : '\\';
    case 0xDD: return shift ? '}' : ']';
    case 0xDE: return shift ? '"' : '\'';
    default: return 0;
    }
}
}  // namespace

STDCALL(USER32, TranslateMessage, 1) {
    uint32_t p = arg(0);
    uint32_t hwnd = RD32(p), msg = RD32(p + 4), vk = RD32(p + 8), lp = RD32(p + 12);
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return 0;
    uint32_t ch = vk_to_char(vk, w32::key_state(0x10) & 0x8000, w32::key_state(0x14) & 1);
    if (!ch || (w32::key_state(0x11) & 0x8000)) return 0;   // Ctrl combinations type control codes
    post_message(hwnd, msg == WM_KEYDOWN ? WM_CHAR : 0x0106 /* WM_SYSCHAR */, ch, lp);
    return 1;
}

STDCALL(USER32, DispatchMessageA, 1) {
    uint32_t p = arg(0);
    uint32_t hwnd = RD32(p), msg = RD32(p + 4), wp = RD32(p + 8), lp = RD32(p + 12);
    if (msg == WM_TIMER && lp) return guest_call_stdcall(lp, {hwnd, msg, wp, now_ms()});
    return send_message(hwnd, msg, wp, lp);
}

STDCALL(USER32, IsDialogMessageA, 2) { return 0; }
STDCALL(USER32, GetMessageTime, 0) { return last_msg.time; }
STDCALL(USER32, GetMessagePos, 0) { return ((last_msg.y & 0xFFFF) << 16) | (last_msg.x & 0xFFFF); }

STDCALL(USER32, RegisterWindowMessageA, 1) {
    std::string n = read_cstr(arg(0));
    auto it = registered_messages.find(n);
    if (it != registered_messages.end()) return it->second;
    uint32_t id = 0xC100 + static_cast<uint32_t>(registered_messages.size());
    registered_messages[n] = id;
    return id;
}

STDCALL(USER32, SetTimer, 4) {
    uint32_t id = arg(0) ? arg(1) : 0x7F00 + static_cast<uint32_t>(timers.size());
    std::erase_if(timers, [&](const Timer &t) { return t.hwnd == arg(0) && t.id == id; });
    timers.push_back({arg(0), id, arg(2) ? arg(2) : 1, arg(3), now_ms() + arg(2)});
    return id;
}

STDCALL(USER32, KillTimer, 2) {
    size_t n = timers.size();
    std::erase_if(timers, [&](const Timer &t) { return t.hwnd == arg(0) && t.id == arg(1); });
    return timers.size() != n;
}

// =====================================================================================
// Hooks
// =====================================================================================
STDCALL(USER32, SetWindowsHookExA, 4) {
    uint32_t h = next_hook++;
    hooks.push_back({static_cast<int>(arg(0)), arg(1), h});
    return h;
}
STDCALL(USER32, UnhookWindowsHookEx, 1) {
    std::erase_if(hooks, [&](const Hook &k) { return k.handle == arg(0); });
    return 1;
}
STDCALL(USER32, CallNextHookEx, 4) { return 0; }

// =====================================================================================
// Focus, activation, capture, input state
// =====================================================================================
STDCALL(USER32, GetFocus, 0) { return focus_hwnd; }
STDCALL(USER32, SetFocus, 1) {
    uint32_t old = focus_hwnd;
    if (old == arg(0)) return old;
    if (old) send_message(old, WM_KILLFOCUS, arg(0), 0);
    focus_hwnd = arg(0);
    if (arg(0)) send_message(arg(0), WM_SETFOCUS, old, 0);
    return old;
}
STDCALL(USER32, GetActiveWindow, 0) { return active_hwnd; }
STDCALL(USER32, SetActiveWindow, 1) { uint32_t o = active_hwnd; activate(arg(0)); return o; }
STDCALL(USER32, GetForegroundWindow, 0) { return active_hwnd; }
STDCALL(USER32, SetForegroundWindow, 1) { activate(arg(0)); return 1; }
STDCALL(USER32, GetCapture, 0) { return capture_hwnd; }
STDCALL(USER32, SetCapture, 1) { uint32_t o = capture_hwnd; capture_hwnd = arg(0); return o; }
STDCALL(USER32, ReleaseCapture, 0) { capture_hwnd = 0; return 1; }

STDCALL(USER32, GetCursorPos, 1) {
    WR32(arg(0), static_cast<uint32_t>(cursor_x));
    WR32(arg(0) + 4, static_cast<uint32_t>(cursor_y));
    return 1;
}
STDCALL(USER32, GetCursor, 0) { return cursor_handle; }
STDCALL(USER32, SetCursor, 1) { uint32_t o = cursor_handle; cursor_handle = arg(0); return o; }
STDCALL(USER32, GetKeyState, 1) { return key_state(arg(0)); }

// =====================================================================================
// System information
// =====================================================================================
STDCALL(USER32, GetSystemMetrics, 1) {
    switch (arg(0)) {
    case 0: return 1024;          // SM_CXSCREEN
    case 1: return 768;           // SM_CYSCREEN
    case 2: case 3: return 16;    // scroll bar sizes
    case 4: return 19;            // SM_CYCAPTION
    case 5: case 6: return 1;     // SM_CXBORDER
    case 7: case 8: return 3;     // SM_CXDLGFRAME
    case 11: case 12: return 32;  // SM_CXICON
    case 13: case 14: return 32;  // SM_CXCURSOR
    case 15: return 19;           // SM_CYMENU
    case 16: return 1024;         // SM_CXFULLSCREEN
    case 17: return 749;
    case 19: return 1;            // SM_MOUSEPRESENT
    case 32: case 33: return 4;   // SM_CXFRAME
    case 43: return 2;            // SM_CMOUSEBUTTONS
    case 49: case 50: return 16;  // SM_CXSMICON
    default: return 0;
    }
}

STDCALL(USER32, SystemParametersInfoA, 4) {
    if (arg(0) == 0x30 && arg(2)) write_rect(arg(2), {0, 0, 1024, 740});   // SPI_GETWORKAREA
    else if (arg(2) && arg(0) < 0x1000 && (arg(0) & 1) == 0 && arg(0) != 0x30) WR32(arg(2), 0);
    return 1;
}

STDCALL(USER32, GetSysColor, 1) {
    static const uint32_t colors[] = {0xC0C0C0, 0x808000, 0x800000, 0x808080, 0xC0C0C0, 0xFFFFFF, 0x000000,
                                      0x000000, 0x000000, 0xFFFFFF, 0xC0C0C0, 0xC0C0C0, 0x808080, 0x800000,
                                      0xFFFFFF, 0xC0C0C0, 0x808080, 0x808080, 0x000000, 0xC0C0C0, 0xFFFFFF,
                                      0x000000, 0xDFDFDF, 0x000000, 0xE1FFFF};
    return arg(0) < sizeof colors / 4 ? colors[arg(0)] : 0;
}
STDCALL(USER32, GetSysColorBrush, 1) { return 0x0B000000u | arg(0); }

// =====================================================================================
// Resources: strings, icons, cursors, bitmaps
// =====================================================================================
STDCALL(USER32, LoadStringA, 4) {
    uint32_t id = arg(1), buf = arg(2), cap = arg(3);
    uint32_t e = find_resource(6, id / 16 + 1);
    if (!e) { if (cap) WR8(buf, 0); return 0; }
    uint32_t p = resource_data(e);
    for (uint32_t i = 0; i < id % 16; i++) p += 2 + 2 * RD16(p);
    uint32_t n = RD16(p);
    std::string s;
    for (uint32_t i = 0; i < n; i++) s.push_back(static_cast<char>(RD16(p + 2 + 2 * i) & 0xFF));
    return write_cstr(buf, s, cap);
}

STDCALL(USER32, LoadIconA, 2) { return 0x0C000000u | (arg(1) & 0xFFFF); }
STDCALL(USER32, LoadCursorA, 2) { return 0x0D000000u | (arg(1) & 0xFFFF); }
STDCALL(USER32, LoadBitmapA, 2) { return 0x0E000000u | (arg(1) & 0xFFFF); }

// =====================================================================================
// Menus (the game window has none; keep MFC's bookkeeping calls harmless)
// =====================================================================================
STDCALL(USER32, GetMenu, 1) { Window *w = window(arg(0)); return w ? w->menu : 0; }
STDCALL(USER32, GetSubMenu, 2) { return 0; }
STDCALL(USER32, GetMenuItemCount, 1) { return 0xFFFFFFFF; }
STDCALL(USER32, GetMenuItemID, 2) { return 0xFFFFFFFF; }
STDCALL(USER32, GetMenuState, 3) { return 0xFFFFFFFF; }
STDCALL(USER32, GetMenuStringA, 5) { if (arg(3)) WR8(arg(2), 0); return 0; }
STDCALL(USER32, InsertMenuA, 5) { return 1; }
STDCALL(USER32, ModifyMenuA, 5) { return 1; }
STDCALL(USER32, DeleteMenu, 3) { return 1; }
STDCALL(USER32, DestroyMenu, 1) { return 1; }
STDCALL(USER32, CheckMenuItem, 3) { return 0xFFFFFFFF; }
STDCALL(USER32, EnableMenuItem, 3) { return 0xFFFFFFFF; }
STDCALL(USER32, SetMenuItemBitmaps, 5) { return 1; }
STDCALL(USER32, GetMenuCheckMarkDimensions, 0) { return (13u << 16) | 13u; }

// =====================================================================================
// Misc
// =====================================================================================
STDCALL(USER32, MessageBoxA, 4) {
    std::string text = read_cstr(arg(1)), cap = read_cstr(arg(2));
    log("MessageBoxA(\"%s\", \"%s\", type %x)\n", cap.c_str(), text.c_str(), arg(3));
    uint32_t type = arg(3) & 0xF;
    return type == 4 || type == 3 ? 6u : 1u;   // MB_YESNO(CANCEL) -> IDYES, else IDOK
}

STDCALL(USER32, WinHelpA, 4) { return 1; }
STDCALL(USER32, CharUpperA, 1) {
    if (arg(0) < 0x10000) return static_cast<uint32_t>(toupper(static_cast<int>(arg(0))));
    for (uint32_t p = arg(0); RD8(p); p++) WR8(p, static_cast<uint8_t>(toupper(RD8(p))));
    return arg(0);
}


// GDI-ish entry points that live in USER32
STDCALL(USER32, GetDC, 1) { return 0x0F000000u | (arg(0) & 0xFFFF); }
STDCALL(USER32, ReleaseDC, 2) { return 1; }
STDCALL(USER32, DrawTextA, 5) { return 16; }
STDCALL(USER32, TabbedTextOutA, 8) { return 0; }
STDCALL(USER32, GrayStringA, 9) { return 1; }
