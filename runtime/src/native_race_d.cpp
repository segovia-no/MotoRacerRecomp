// Native versions of hot race functions, group D (docs/25-psp-port.md, the PSP hardware profile).
// Hooked through NATIVE_FUNCS (translate.py): f_<va> calls native_hot_<va>, which returns 0 to run the
// translated body f_<va>_t. MOTORACER_NATIVE=check runs both on the same state and compares
// (native_check.h). Floating point in fpu_t, in the x87 code's operation order (-ffp-contract=off).
//
// Group D is integer code around 2D text and sprites:
//   0x4b3400  CRT strncmp
//   0x492b10  font: text size (width of the widest line, height)
//   0x485e50  quad (sprite / glyph) into the render list: polygon record, 4 vertices, poly setup
//   0x493490  font: draw a string (one virtual glyph call per character)
// 0x493820 (print a string on a surface: SEH frame, CString, heap buffer, surface locks) stays
// translated: mostly orchestration around calls, little work of its own per call.
#include <algorithm>
#include <cstring>

#include "runtime.h"
#include "recomp_mem_local.h"
#include "native_check.h"

extern "C" void f_00493820_t(void);
extern "C" void f_00493490_t(void);
extern "C" void f_00492b10_t(void);
extern "C" void f_00485e50_t(void);
extern "C" void f_004b3400_t(void);
extern "C" void f_00492b10(void);
extern "C" void f_0045a2e0(void);
extern "C" void f_0049c590(void);

namespace {
using namespace native;

// per-function check counters ("native: <name> checked N times")
void checked(const char *name, uint32_t &n, uint32_t every) {
    if (++n == 1 || n % every == 0)
        rt::log("native: %s checked %u times (mismatches so far: %llu)\n", name, n,
                static_cast<unsigned long long>(mismatches));
}

// ---------------------------------------------------------------------------------------------
// 0x4b3400: strncmp(s1, s2, n), cdecl (MSVC CRT: repne scasb for the length within n, repe cmpsb).
// eax = ecx = -1 / 0 / 1 (unsigned bytes), edx untouched.
void strncmp_native() {
    MEM_LOCAL();
    const uint32_t sp = R.esp;
    const uint32_t s1 = RD32A(sp + 4), s2 = RD32A(sp + 8), n = RD32A(sp + 12);
    uint32_t r = 0;
    if (n) {
        uint32_t c = 0;   // bytes the scan covers: up to and including the NUL, at most n
        while (c < n) {
            if (!RD8(s1 + c++)) break;
        }
        uint32_t k = 0;   // the last byte repe cmpsb compares
        while (k + 1 < c && RD8(s2 + k) == RD8(s1 + k)) k++;
        const uint8_t a = RD8(s2 + k), b = RD8(s1 + k);
        r = a > b ? 0xffffffffu : a == b ? 0u : 1u;
    }
    R.eax = r;
    R.ecx = r;
    R.esp = sp + 4;
}

// ---------------------------------------------------------------------------------------------
// 0x492b10: font text size (thiscall: ecx = font; text, int *w, int *h, single_line; ret 0x10).
// Character widths [font + 0x618 + 4c] (> 0: plus the spacing [font + 0xa20]); '\n' starts a line
// (height += [0xa1c] + [0xa18], rounded up to even when [0x624b14]) unless single_line, which stops
// there. *w = the widest line, *h = the total height (starting at [0xa18]).
void text_size() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, font = R.ecx;
    const uint32_t text = RD32A(sp + 4), pw = RD32A(sp + 8), ph = RD32A(sp + 12), single = RD32A(sp + 16);
    R.esp = sp + 20;
    if (!pw) return;               // eax / ecx / edx as on entry
    R.edx = ph;
    if (!ph) return;
    R.eax = text;
    if (!text) return;
    int32_t maxw = 0;
    WR32(pw, 0);
    WR32(ph, RD32(font + 0xa18));
    int32_t len = 0;
    while (RD8(text + static_cast<uint32_t>(len))) len++;
    const uint32_t even = RD32A(0x624b14);
    for (int32_t i = 0; i < len; i++) {
        const uint32_t ch = RD8(text + static_cast<uint32_t>(i));
        if (ch == 10) {
            if (single) break;
            const int32_t w = static_cast<int32_t>(RD32(pw));
            if (maxw < w) maxw = w;
            WR32(pw, 0);
            const uint32_t nh = RD32(font + 0xa1c) + RD32(ph) + RD32(font + 0xa18);
            WR32(ph, nh);
            if (even && (nh & 1)) WR32(ph, nh + 1);
        } else {
            const int32_t cw = static_cast<int32_t>(RD32(font + ch * 4 + 0x618));
            if (cw > 0) WR32(pw, RD32(pw) + RD32(font + 0xa20) + static_cast<uint32_t>(cw));
        }
    }
    const int32_t w = static_cast<int32_t>(RD32(pw));
    R.ecx = static_cast<uint32_t>(maxw);
    if (maxw < w) maxw = w;
    R.eax = static_cast<uint32_t>(maxw);
    WR32(pw, static_cast<uint32_t>(maxw));
}

// ---------------------------------------------------------------------------------------------
// 0x485e50: a textured quad (or triangle) into the render list (stdcall, 10 args, ret 0x28):
//   texture word, 4 vertices (else 3), texture-map word, uv[4][2] (or NULL), xy[4][2], 565 colour,
//   depth mode, existing polygon record (or NULL: the next of 40 records at 0x5dbc90, 64 bytes),
//   poly-setup argument, alpha (6 bits).
// Fills the record (texture, mode words, uvs, colour via 0x423110 inline), the vertices in the ring
// 0x5dc698 (28 bytes each, 0xaa0 of them, index [0x4f0e4c] stepping by 4): z / 1/z constants, light
// word, screen x / y; then 0x49c590 (polygon setup, native) puts it into a depth bucket.
constexpr uint32_t INIT_GUARD = 0x5ef050, RECORDS = 0x5dbc90, RECORD_NEXT = 0x4f0e48, RECORDS_USED = 0x5dbc88;
constexpr uint32_t VERTS = 0x5dc698, VERT_NEXT = 0x4f0e4c, QUADS = 0x5ef058;
constexpr uint32_t Z_ON = 0x53bce4, Z_RAW = 0x4dc1cc, FULL_LIGHT = 0x5f1310, RENDER_CTX = 0x4dc0c0;

void quad() {
    MEM_LOCAL();
    const uint32_t sp = R.esp;
    auto arg = [&](int i) { return RD32A(sp + 4 + 4 * static_cast<uint32_t>(i)); };
    uint32_t za = 0x45000000, zb = 0x3a000000;   // float bits stored into each vertex
    if (RD32A(Z_ON)) {
        if (arg(6) == 1) za = 0, zb = 0x3f800000;
        else if (RD32A(Z_RAW)) za = 0x497fff00, zb = 0x35800080;
        else za = 0x477f7800, zb = 0x37804424;
    }
    const uint32_t nverts = arg(1) == 0 ? 3u : 4u;
    const uint32_t existing = arg(7), colour = arg(5);
    uint32_t rec = 0;
    if (!existing) {
        rec = RECORDS + (RD32A(RECORD_NEXT) << 6);
        WR16(rec + 0, static_cast<uint16_t>(arg(0)));
        WR16(rec + 0x38, 4);
        WR16(rec + 0x36, static_cast<uint16_t>(arg(2)));
        WR16(rec + 0x2c, 0);
        WR16(rec + 0x2e, 1);
        WR16(rec + 0x30, 2);
        WR16(rec + 0x32, 3);
        WR16(rec + 0x34, 0);
        if (const uint32_t uv = arg(3)) {
            for (uint32_t k = 0; k < 8; k++) WR32(rec + 4 + 4 * k, RD32(uv + 4 * k));
            WR32(rec + 0x24, RD32(uv));
            WR32(rec + 0x28, RD32(uv + 4));
        }
        // 0x423110 (thiscall, ret 4): the 565 colour word and its 888 expansion
        const uint32_t c = colour & 0xffff;
        WR16(rec + 2, static_cast<uint16_t>(c));
        WR32(rec + 0x3c, ((((c & 0x7c0) >> 6) << 8) | ((c >> 11) << 16) | (c & 0x1f)) << 3);
    } else {
        WR16(existing + 0x36, static_cast<uint16_t>(arg(2)));
        WR16(existing + 0, static_cast<uint16_t>(arg(0)));
    }
    const uint32_t vi = RD32A(VERT_NEXT), vbase = VERTS + vi * 28u;
    const uint32_t alpha = arg(9);
    const bool full = RD32A(FULL_LIGHT) != 0;
    uint32_t light;
    if (existing) {
        uint32_t a = (alpha & 0x3f) | 0xffffff80u;
        a = ((a << 16) | (colour & 0xf800)) << 3;
        a = (a | (colour & 0x7c0)) << 2;
        light = (a | (colour & 0x1f)) << 3;
    } else {
        light = ((alpha & 0x3f) << 24) | 0x3e000;
    }
    for (uint32_t k = 0; k < nverts; k++) {
        const uint32_t v = vbase + k * 28u;
        WR32(v + 8, za);
        WR32(v + 0x18, zb);
        WR32(v + 0xc, full ? 0x40000000u : light);
    }
    const uint32_t xy = arg(4);
    for (uint32_t k = 0; k < 4; k++) {
        WR32(vbase + 0x10 + k * 28u, RD32(xy + 8 * k));
        WR32(vbase + 0x14 + k * 28u, RD32(xy + 8 * k + 4));
    }
    // 0x49c590(record, 0, vertices, arg 8, depth mode), thiscall on the render context
    const uint32_t call_sp = sp - 24 - 24;
    WR32A(call_sp, existing ? 0x4861ceu : 0x486192u);
    WR32A(call_sp + 4, existing ? existing : rec);
    WR32A(call_sp + 8, 0);
    WR32A(call_sp + 12, vbase);
    WR32A(call_sp + 16, arg(8));
    WR32A(call_sp + 20, arg(6));
    R.esp = call_sp;
    R.ecx = RD32A(RENDER_CTX);
    R.eax = existing ? vi * 7u : rec;   // as the x86 leaves them at the call
    R.edx = existing ? vi : vbase + 0x64;
    f_0049c590();
    if (!existing) {
        WR32A(RECORDS_USED, RD32A(RECORDS_USED) + 1);
        WR32A(RECORD_NEXT, RD32A(RECORD_NEXT) + 1);
    }
    WR32A(QUADS, RD32A(QUADS) + 1);
    const uint32_t nv = RD32A(VERT_NEXT) + 4;
    WR32A(VERT_NEXT, nv);
    const uint32_t nr = RD32A(RECORD_NEXT);
    if (static_cast<int32_t>(nr) >= 0x28) WR32A(RECORD_NEXT, 0);
    if (static_cast<int32_t>(nv) >= 0xaa0) WR32A(VERT_NEXT, 0);
    R.eax = nv;
    R.ecx = nv;
    R.edx = nr;
    R.esp = sp + 4 + 0x28;
}

// render-list state 0x49c590 can change (check mode restores it before the translated run)
constexpr uint32_t BUCKET_MAX = 0x65e1d0, BUCKET_MIN = 0x68b030, NODES = 0x65f0e0, NODE_COUNT = 0x655d14,
                   HEADS = 0x655d38;
void render_list_regions(std::vector<Region> &r) {
    MEM_LOCAL();
    r.push_back({BUCKET_MIN, 4});
    r.push_back({BUCKET_MAX, 4});
    r.push_back({NODE_COUNT, 4});
    r.push_back({HEADS, 0x1f40 * 4});   // buckets 0..0x1f3f (depth mode 2: 0x1f3f)
    const uint32_t n = RD32A(NODE_COUNT);
    if (static_cast<int32_t>(n) >= 0 && n + 16 <= 0x1d4b) r.push_back({NODES + n * 24u, 24 * 16});
    // the clipping path of 0x49c590 (translated): transformed-vertex pool and clip records
    r.push_back({0x68b014, 4});
    r.push_back({0x65e1c8, 4});
    const uint32_t used = RD32A(0x68b014), recs = RD32A(0x65e1c8);
    if (used < 0x10000) r.push_back({0x6d20d8 + used * 28u, 28 * 64});
    if (recs < 0x10000) r.push_back({0x62ebf0 + recs * 64u, 64 * 16});
    r.push_back({RECORD_NEXT, 4});
    r.push_back({VERT_NEXT, 4});
    r.push_back({RECORDS_USED, 4});
    r.push_back({QUADS, 4});
    r.push_back({RECORDS, 40 * 64});
    r.push_back({VERTS, 0xaa0 * 28});
}

// ---------------------------------------------------------------------------------------------
// 0x493490: font draw string (thiscall: ecx = font; ret 0x38, 14 args):
//   0 uint16 **dst (line start in the surface, advanced), 1 pitch (pixels), 2..4 passed to the glyph
//   call, 5 text, 6 flags (& 0x1f: 4 centre, 0x10 right-align within 7 = width), 8 x, 9 y,
//   10 passed to the glyph call, 11 rect out (or NULL), 12 / 13 rgb byte triples (or NULL).
// The glyph method [[font] + 0x18] gets pointers to the x / y argument slots and moves them.
// Returns (eax) the last glyph call's result (1 before the first, 0 for an empty text).
void draw_text() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, font = R.ecx;
    const uint32_t fr = sp - 0x2c;   // esp after the prologue (sub 0x1c, four pushes)
    auto argp = [&](int i) { return sp + 4 + 4 * static_cast<uint32_t>(i); };
    auto arg = [&](int i) { return RD32A(argp(i)); };
    const uint32_t text = arg(5);
    if (!text) {
        R.eax = 0, R.ecx = 0, R.edx = 0, R.esp = sp + 0x3c;
        return;
    }
    if (!RD8(text)) {
        R.eax = 0, R.ecx = text & 0xffffff00u, R.edx = 0, R.esp = sp + 0x3c;
        return;
    }
    uint32_t result = 1;
    const uint32_t x0 = arg(8);   // the x the text starts with (lines restart there)
    int32_t len = 0;
    while (RD8(text + static_cast<uint32_t>(len))) len++;
    int32_t maxw = 0;
    if (const uint32_t c = arg(12)) {
        WR8(font + 0x5494, RD8(c));
        WR8(font + 0x5495, RD8(c + 1));
        WR8(font + 0x5496, RD8(c + 2));
    }
    if (const uint32_t c = arg(13)) {
        WR8(font + 0x5497, RD8(c));
        WR8(font + 0x5498, RD8(c + 1));
        WR8(font + 0x5499, RD8(c + 2));
    }
    bool lookup = true;   // the colour's palette index (0x45a2e0) or 0xffff
    if (RD32(font + 0x547c) && !RD32(font + 0x5480)) lookup = false;
    if (lookup) {
        const uint32_t idx = RD32A(0x53be30);
        if (!RD32(idx * 404u + 0x53c774) || !RD32A(0x4f1814)) lookup = false;
    }
    if (lookup) {
        WR32A(fr - 4, RD8(font + 0x5496) >> 3);
        WR32A(fr - 8, RD8(font + 0x5495) >> 3);
        WR32A(fr - 12, RD8(font + 0x5494) >> 3);
        WR32A(fr - 16, 0x493589);
        R.esp = fr - 16;
        R.ecx = RD8(font + 0x5494) >> 3;
        f_0045a2e0();   // stdcall, pops its three arguments
        WR32A(0x4f1804, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(R.eax))));
    } else {
        WR32A(0x4f1804, 0xffff);
    }
    const uint32_t pitch = arg(1), pdst = arg(0);
    if (RD32A(0x624b14) && (arg(9) & 1)) {   // odd y with even-line rendering: one line up or down
        const uint32_t y = arg(9);
        if (static_cast<int32_t>(RD32(font + 0xa18) + y) > static_cast<int32_t>(RD32A(0x5d8948))) {
            WR32A(argp(9), y - 1);
            WR32(pdst, RD32(pdst) - pitch * 2);
        } else {
            WR32A(argp(9), y + 1);
            WR32(pdst, RD32(pdst) + pitch * 2);
        }
    }
    const uint32_t align = arg(6) & 0x1f;
    uint32_t line = RD32(pdst);   // ebx: start of the current line in the surface
    // 0x492b10(from, &w, &h, 1) into the locals at fr+0x10 (w) / fr+0x1c (h)
    auto measure = [&](uint32_t from, uint32_t ret) {
        WR32A(fr - 4, 1);
        WR32A(fr - 8, fr + 0x1c);
        WR32A(fr - 12, fr + 0x10);
        WR32A(fr - 16, from);
        WR32A(fr - 20, ret);
        R.esp = fr - 20;
        R.ecx = font;
        f_00492b10();
    };
    auto offset_for = [&](uint32_t from, uint32_t ret_centre, uint32_t ret_right) -> int32_t {
        if (align == 4) {
            measure(from, ret_centre);
            return (static_cast<int32_t>(arg(7)) - static_cast<int32_t>(RD32A(fr + 0x10))) / 2;
        }
        if (align == 0x10) {
            measure(from, ret_right);
            return static_cast<int32_t>(arg(7)) - static_cast<int32_t>(RD32A(fr + 0x10));
        }
        return 0;
    };
    int32_t off = offset_for(text, 0x493624, 0x493658);
    WR32A(argp(8), arg(8) + static_cast<uint32_t>(off));
    WR32(pdst, RD32(pdst) + static_cast<uint32_t>(off) * 2);
    if (const uint32_t rect = arg(11)) {
        WR32(rect + 4, arg(9));
        WR32(rect, arg(8));
    }
    uint32_t edx = 0;
    for (int32_t i = 0; i < len; i++) {
        const uint32_t ch = RD8(text + static_cast<uint32_t>(i));
        if (ch != 10) {
            // [[font] + 0x18](arg0, arg1, arg2, arg3, arg4, char, &x, &y, arg10)
            const uint32_t s = fr - 40;
            WR32A(s, 0x4936df);
            WR32A(s + 4, arg(0));
            WR32A(s + 8, arg(1));
            WR32A(s + 12, arg(2));
            WR32A(s + 16, arg(3));
            WR32A(s + 20, arg(4));
            WR32A(s + 24, (text & 0xffffff00u) | ch);
            WR32A(s + 28, argp(8));
            WR32A(s + 32, argp(9));
            WR32A(s + 36, arg(10));
            R.esp = s;
            R.ecx = font;
            R.eax = RD32(font);
            R.edx = arg(0);
            call_indirect(RD32(RD32(font) + 0x18));
            result = R.eax;
            edx = R.edx;
            continue;
        }
        off = offset_for(text + static_cast<uint32_t>(i) + 1, 0x49370d, 0x493745);
        const int32_t x = static_cast<int32_t>(arg(8));
        if (maxw < x) maxw = x;   // the widest line so far (signed)
        const uint32_t a18 = RD32(font + 0xa18), lh = RD32(font + 0xa1c) + a18;
        WR32A(argp(8), x0 + static_cast<uint32_t>(off));
        const uint32_t y = arg(9) + lh;
        WR32A(argp(9), y);
        if (RD32A(0x624b14) && (y & 1)) {
            if (static_cast<int32_t>(a18 + y) > static_cast<int32_t>(RD32A(0x5d8948))) {
                WR32A(argp(9), y - 1);
                line -= pitch * 2;
            } else {
                WR32A(argp(9), y + 1);
                line += pitch * 2;
            }
        }
        line += pitch * lh * 2;
        edx = line + static_cast<uint32_t>(off) * 2;
        WR32(pdst, edx);
    }
    const int32_t x = static_cast<int32_t>(arg(8));
    uint32_t ecx = static_cast<uint32_t>(x);
    if (maxw < x) maxw = x;
    if (const uint32_t rect = arg(11)) {
        WR32(rect + 8, static_cast<uint32_t>(maxw) - RD32(font + 0xa20));
        WR32(rect + 0xc, RD32(font + 0xa1c) + arg(9));
        ecx = RD32(font + 0xa20);
        edx = rect;
    }
    R.eax = result;
    R.ecx = ecx;
    R.edx = edx;
    R.esp = sp + 0x3c;
}

}  // namespace

extern "C" int native_hot_493820(void) { return 0; }   // translated (see the top of the file)

extern "C" int native_hot_493490(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { draw_text(); return 1; }
    // check: the glyph calls draw into the surface around the text and may add quads to the render
    // list (0x485e50): both are restored before the translated run and compared
    const uint32_t sp = R.esp, font = R.ecx;
    std::vector<Region> regions = {{sp + 4, 0x38}, {font + 0x5494, 6}, {0x4f1804, 4}};
    if (const uint32_t rect = RD32A(sp + 48)) regions.push_back({rect, 16});
    const uint32_t pdst = RD32A(sp + 4), pitch = RD32A(sp + 8);
    if (pdst) regions.push_back({pdst, 4});
    const uint32_t back = RD32A(0x624f5c), rows = RD32A(0x624b18), bpitch = RD32A(0x62ebec);
    const uint32_t dst = pdst ? RD32(pdst) : 0;
    if (back && dst >= back && dst < back + rows * bpitch * 2) regions.push_back({back, rows * bpitch * 2});
    else if (dst >= 0x400000 && pitch < 4096)   // a surface (glyphs drawn as quads get a dummy pointer)
        regions.push_back({dst - std::min(dst - 0x400000, pitch * 2 * 64), pitch * 2 * 256});
    render_list_regions(regions);
    check("text draw 0x493490", draw_text, f_00493490_t, regions);
    static uint32_t n = 0;
    checked("text draw 0x493490", n, 2000);
    return 1;
}

extern "C" int native_hot_492b10(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { text_size(); return 1; }
    const uint32_t sp = R.esp;
    std::vector<Region> regions;
    if (const uint32_t w = RD32A(sp + 8)) regions.push_back({w, 4});
    if (const uint32_t h = RD32A(sp + 12)) regions.push_back({h, 4});
    check("text size 0x492b10", text_size, f_00492b10_t, regions);
    static uint32_t n = 0;
    checked("text size 0x492b10", n, 5000);
    return 1;
}

extern "C" int native_hot_485e50(void) {
    MEM_LOCAL();
    if ((RD8(INIT_GUARD) & 3) != 3) return 0;   // first call: the record / vertex constructors
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { quad(); return 1; }
    std::vector<Region> regions;
    if (const uint32_t e = RD32A(R.esp + 32)) regions.push_back({e, 64});
    render_list_regions(regions);
    check("quad 0x485e50", quad, f_00485e50_t, regions);
    static uint32_t n = 0;
    checked("quad 0x485e50", n, 5000);
    return 1;
}

extern "C" int native_hot_4b3400(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { strncmp_native(); return 1; }
    check("strncmp 0x4b3400", strncmp_native, f_004b3400_t, {});
    static uint32_t n = 0;
    checked("strncmp 0x4b3400", n, 20000);
    return 1;
}
