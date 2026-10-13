// Dialog boxes: DLGTEMPLATE parsing, the #32770 class procedure (DefDlgProc) and
// CreateDialogIndirectParamA. Controls are plain windows; nothing is drawn yet.
#include "user32.h"
#include "win32.h"

using namespace rt;
using namespace w32;

namespace {

constexpr uint32_t WM_INITDIALOG = 0x0110;
constexpr int32_t DWL_MSGRESULT = 0, DWL_DLGPROC = 4;
constexpr uint32_t DS_SETFONT = 0x40;

uint32_t dlgproc_of(Window *w) {
    uint32_t v = 0;
    if (w && w->extra.size() >= 12) memcpy(&v, w->extra.data() + DWL_DLGPROC, 4);
    return v;
}

void set_extra(Window *w, int32_t off, uint32_t v) {
    if (w && w->extra.size() >= static_cast<size_t>(off) + 4) memcpy(w->extra.data() + off, &v, 4);
}

uint32_t def_dlg_proc(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Window *w = window(hwnd);
    uint32_t proc = dlgproc_of(w);
    if (proc && guest_call_stdcall(proc, {hwnd, msg, wp, lp})) {
        w = window(hwnd);
        uint32_t r = 0;
        if (w && w->extra.size() >= 4) memcpy(&r, w->extra.data() + DWL_MSGRESULT, 4);
        return msg == WM_INITDIALOG ? 1 : r;
    }
    if (msg == WM_CLOSE) return 0;   // dialogs close via IDCANCEL/EndDialog, not DestroyWindow
    return guest_call_stdcall(default_window_proc_va(), {hwnd, msg, wp, lp});
}

void def_dlg_proc_thunk() {
    uint32_t r = def_dlg_proc(arg(0), arg(1), arg(2), arg(3));
    R.eax = r;
    R.esp += 4 + 16;
}

// Reads a sz_Or_Ord field; returns the guest VA of a string or an atom (< 0x10000).
uint32_t sz_or_ord(uint32_t &p, std::string *text) {
    uint16_t first = RD16(p);
    if (first == 0xFFFF) { uint16_t ord = RD16(p + 2); p += 4; return ord; }
    uint32_t start = p;
    std::string s;
    while (uint16_t c = RD16(p)) { s.push_back(static_cast<char>(c & 0xFF)); p += 2; }
    p += 2;
    if (text) *text = s;
    return start;
}

uint32_t guest_ansi(const std::string &s) { return alloc_cstr(s); }

}  // namespace

namespace w32 {

uint32_t dialog_class_proc() {
    static uint32_t thunk = register_thunk(def_dlg_proc_thunk, "DefDlgProc(builtin)");
    return thunk;
}

uint32_t create_dialog_indirect(uint32_t hinst, uint32_t tmpl, uint32_t parent, uint32_t dlgproc, uint32_t param) {
    uint32_t p = tmpl;
    if (RD32(p) == 0xFFFF0001) fatal("DLGTEMPLATEEX not supported (template 0x%08x)", tmpl);
    uint32_t style = RD32(p), exstyle = RD32(p + 4);
    uint16_t count = RD16(p + 8);
    int32_t x = static_cast<int16_t>(RD16(p + 10)), y = static_cast<int16_t>(RD16(p + 12));
    int32_t cx = static_cast<int16_t>(RD16(p + 14)), cy = static_cast<int16_t>(RD16(p + 16));
    p += 18;
    sz_or_ord(p, nullptr);                 // menu
    std::string cls_text, title;
    uint32_t cls = sz_or_ord(p, &cls_text);
    sz_or_ord(p, &title);
    if (style & DS_SETFONT) { p += 2; sz_or_ord(p, nullptr); }

    // Dialog units -> pixels for MS Sans Serif 8 (base units 6x13).
    auto px = [](int32_t v) { return v * 6 / 4; };
    auto py = [](int32_t v) { return v * 13 / 8; };

    uint32_t cls_name = (cls >= 0x10000 && !cls_text.empty()) ? guest_ansi(cls_text) : guest_ansi("#32770");
    uint32_t title_va = guest_ansi(title);
    uint32_t create_style = style & ~0x10000000u;   // show after WM_INITDIALOG
    uint32_t hwnd = create_window(exstyle, cls_name, title_va, create_style, px(x), py(y), px(cx), py(cy) + 25,
                                  parent, 0, hinst, param);
    if (!hwnd) return 0;
    Window *dw = window(hwnd);
    if (dw->extra.size() < 30) dw->extra.resize(30, 0);
    set_extra(dw, DWL_DLGPROC, dlgproc);
    log("CreateDialogIndirectParamA: \"%s\" hwnd %08x, %u controls\n", title.c_str(), hwnd, count);

    for (uint16_t i = 0; i < count; i++) {
        p = (p + 3) & ~3u;
        uint32_t cstyle = RD32(p), cex = RD32(p + 4);
        int32_t ix = static_cast<int16_t>(RD16(p + 8)), iy = static_cast<int16_t>(RD16(p + 10));
        int32_t icx = static_cast<int16_t>(RD16(p + 12)), icy = static_cast<int16_t>(RD16(p + 14));
        uint16_t id = RD16(p + 16);
        p += 18;
        std::string ctext, ttext;
        uint32_t ccls = sz_or_ord(p, &ctext);
        uint32_t ctitle = sz_or_ord(p, &ttext);
        uint16_t extra = RD16(p);
        p += 2 + extra;
        static const char *atoms[] = {"BUTTON", "EDIT", "STATIC", "LISTBOX", "SCROLLBAR", "COMBOBOX"};
        std::string cname = ccls < 0x10000 ? (ccls >= 0x80 && ccls <= 0x85 ? atoms[ccls - 0x80] : "STATIC") : ctext;
        uint32_t text_va = ctitle < 0x10000 ? 0 : guest_ansi(ttext);
        create_window(cex, guest_ansi(cname), text_va, cstyle | 0x40000000u, px(ix), py(iy), px(icx), py(icy),
                      hwnd, id, hinst, 0);
    }
    uint32_t first_child = 0;
    for (auto &[h, c] : windows) if (c->parent == hwnd) { first_child = h; break; }
    send_message(hwnd, WM_INITDIALOG, first_child, param);
    if ((style & 0x10000000u) && window(hwnd)) show_window(hwnd, 1);
    return hwnd;
}

}  // namespace w32

STDCALL(USER32, CreateDialogIndirectParamA, 5) {
    return create_dialog_indirect(arg(0), arg(1), arg(2), arg(3), arg(4));
}

STDCALL(USER32, EndDialog, 2) {
    if (Window *w = window(arg(0))) {
        w->dialog_result = arg(1);
        show_window(arg(0), 0);
    }
    return 1;
}
