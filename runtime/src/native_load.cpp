// Native fast path of the game's per-byte file reader (docs/25-psp-port.md, loading time).
//
// Track / object loading decodes files byte by byte through getc(handle) 0x42f650, which goes either
// to an archive layer 0x4309e0 (sub-files inside a pack: offset, size, seek of the real file when the
// position moved) or straight to 0x42f690 (a 16 KB buffer per open file, refilled with fread). On the
// PSP those three translated calls per byte were ~70% of a race load. The native version does the
// common case — the byte is in the buffer, no seek — with the same memory and registers as the
// translated chain; refills, seeks and errors run the translated code (decided before any change).
#include <map>
#include <string>

#include "runtime.h"
#include "recomp_mem_local.h"
#include "native_check.h"

extern "C" void f_0042f650_t(void);
extern "C" void f_0042f650(void);
extern "C" void f_004c5e9d_t(void);

namespace {

using namespace native;

constexpr uint32_t ARCHIVE_ON = 0x4e8584;   // handles 1..2 are archive sub-files
constexpr uint32_t FILES = 0x53e608;        // 2 open-file entries, 0x1c bytes: handle, buffer, buffer start, length, position, -, reads
constexpr uint32_t CUR_FILE = 0x4e859c, FILE_READS = 0x4e85c4, ARCHIVE_READS = 0x4e85b0;
constexpr uint32_t SUBFILES = 0x53ea48;     // per handle (48 bytes): +0 index, +4 offset, +8 pack
constexpr uint32_t PACKS = 0x53ea50;        // per index (40 bytes): +0 file handle, +0x14 file position, +0x20 directory

struct Byte { uint32_t eax, ecx, edx; };

// 0x42f690 for file handle h: the entry when the next byte is in its buffer, else 0
uint32_t buffered(uint32_t h) {
    MEM_LOCAL();
    uint32_t e = FILES;
    if (RD32A(e) != h) {
        e += 0x1c;
        if (RD32A(e) != h) return 0;
    }
    const int32_t pos = static_cast<int32_t>(RD32A(e + 0x10));
    if (pos < 0) return 0;
    const uint32_t rel = static_cast<uint32_t>(pos) - RD32A(e + 8);
    if (RD32A(e + 0xc) - rel == 0) return 0;   // buffer used up: refill (fread)
    return e;
}

Byte read_buffered(uint32_t e) {
    MEM_LOCAL();
    const uint32_t pos = RD32A(e + 0x10), rel = pos - RD32A(e + 8);
    WR32A(CUR_FILE, e);
    WR32A(FILE_READS, RD32A(FILE_READS) + 1);
    const uint32_t b = RD8(RD32A(e + 4) + rel);
    WR32A(e + 0x10, pos + 1);
    WR32A(e + 0x18, RD32A(e + 0x18) + 1);
    return {b, (rel & ~0xffu) | b, e};
}

// returns false: run the translated code
bool getc_native() {
    MEM_LOCAL();
    const uint32_t h = RD32A(R.esp + 4);
    if (!h) return false;
    Byte r;
    if (RD32A(ARCHIVE_ON) && h <= 2) {
        const uint32_t sub = SUBFILES + h * 48;
        const int32_t idx = static_cast<int32_t>(RD32A(sub));
        if (idx < 0) return false;
        const uint32_t pack = RD32A(sub + 8), p = PACKS + static_cast<uint32_t>(idx) * 40;
        const uint32_t dir = RD32A(p + 0x20) + 44 * pack;
        const uint32_t off = RD32A(sub + 4);
        if (static_cast<int32_t>(RD32(dir + 0x28) - off) < 1) return false;   // end of the sub-file
        const uint32_t cur = RD32A(p + 0x14);
        if (cur != RD32(dir + 0x24) + off) return false;                       // seek needed
        const uint32_t e = buffered(RD32A(p));
        if (!e) return false;
        WR32A(ARCHIVE_READS, RD32A(ARCHIVE_READS) + 1);
        r = read_buffered(e);
        WR32A(p + 0x14, cur + 1);
        WR32A(sub + 4, off + 1);
        r.ecx = cur + 1;
        r.edx = off + 1;
    } else {
        const uint32_t e = buffered(h);
        if (!e) return false;
        r = read_buffered(e);
    }
    R.eax = r.eax;
    R.ecx = r.ecx;
    R.edx = r.edx;
    R.esp += 8;   // ret 4
    return true;
}

// getc for native loops: the native fast path, else a guest call of 0x42f650 (refill / seek; sets
// *slow). Registers other than eax / ecx / edx are kept.
uint32_t guest_getc(uint32_t h, bool *slow) {
    MEM_LOCAL();
    const uint32_t sp = R.esp;
    WR32A(sp - 4, h);
    R.esp = sp - 8;   // the argument and a return address, as the call leaves them
    if (getc_native()) return R.eax;
    *slow = true;
    WR32A(sp - 8, 0);
    f_0042f650();
    return R.eax;
}

}  // namespace

extern "C" int native_getc(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) return getc_native() ? 1 : 0;
    // check: memory the fast path may touch
    const uint32_t h = RD32A(R.esp + 4);
    std::vector<Region> regions = {{CUR_FILE, 4}, {FILE_READS, 4}, {ARCHIVE_READS, 4}, {FILES, 0x38}};
    if (h && h <= 2) {
        regions.push_back({SUBFILES + h * 48, 12});
        const int32_t idx = static_cast<int32_t>(RD32A(SUBFILES + h * 48));
        if (idx >= 0 && idx < 64) regions.push_back({PACKS + static_cast<uint32_t>(idx) * 40, 0x18});
    }
    Snapshot before = take(regions);
    if (!getc_native()) return 0;
    Snapshot nat = take(regions);
    restore(before);
    f_0042f650_t();
    compare("getc 0x42f650", nat, take(regions));
    return 1;
}

// ---- 0x4c5e9d: CFile::Read(buffer, count), thiscall (this+4 = Win32 handle), returns the count read ----
// The track loader reads its .trk field by field (2-10 bytes, ~190,000 calls): the translated
// version goes through the ReadFile import each time. Same result: eax = bytes read (0 for count
// 0), ecx = this, edx = the frame slot it passed to ReadFile. A failing read (the MFC version
// throws) runs the translated code.
extern "C" int native_cfile_read(void) {
    MEM_LOCAL();
    if (mode() == OFF) return 0;
    const uint32_t sp = R.esp, buf = RD32A(sp + 4), n = RD32A(sp + 8);
    uint32_t got = 0;
    if (n) {
        if (mode() == CHECK) return 0;   // file reads cannot be repeated: check mode runs the translated code
        if (!rt::read_file(RD32A(R.ecx + 4), buf, n, &got)) return 0;
        R.edx = sp - 8;   // lea edx, [ebp - 4] with ebp = esp after push ebp
    }
    R.eax = got;
    R.esp = sp + 12;      // ret 8
    return 1;
}

// ---- 0x41c050 (texture file loader): the 24-bit pixel loop of a row, 0x41cdb6..0x41d087 ----
// Per pixel three bytes c1, c2, c3 (getc), optional colour-key fix-ups ([ebp+0x1c]: pure green
// 0/255/0 -> black, near-black lifted so it is not keyed), an alpha value (format with alpha bits:
// 0 for near-black, else 1, or for the format with alpha mode 4: 15, or 8 inside a rectangle; inside
// another rectangle the translated code converts the colour with double-precision helpers), then
// each channel rounded to its bit count and shifted into a 16-bit pixel. Rows that touch that
// conversion rectangle, and 128-wide rows that skip every other source pixel, run the translated
// loop. Exit state as the translated loop leaves it (0x41d08d).
namespace {

struct RowOut { uint32_t eax, ecx, edx, ebx, edi; };
struct RowPending {
    bool active = false;
    Snapshot native;
    std::vector<Region> regions;
} row_pending;

inline uint32_t shl32(uint32_t v, uint32_t c) { return v << (c & 31); }

// false: the translated loop handles this row
bool rgb_row_supported(uint32_t ebp, uint32_t esi) {
    MEM_LOCAL();
    const uint32_t count = RD32A(ebp - 0x1e8);
    if (count == 0x80 && RD32A(ebp - 0xe8) != 0x80) return false;   // skips source pixels (seek per pixel)
    if (RD32A(esi + 0x70) && RD32A(esi + 0x80) == 4) {
        const int32_t x0 = static_cast<int32_t>(RD32A(ebp - 0x5ec)), x1 = static_cast<int32_t>(RD32A(ebp - 0x3c));
        const int32_t y0 = static_cast<int32_t>(RD32A(ebp - 0x38)), y1 = static_cast<int32_t>(RD32A(ebp - 0x34));
        const int32_t y = static_cast<int32_t>(RD32A(ebp - 0x18)), xs = static_cast<int32_t>(RD32A(ebp - 0x24));
        if (x0 != -1 && y0 <= y && y1 >= y && x1 >= xs && x0 < static_cast<int32_t>(count)) return false;
    }
    return true;
}

RowOut rgb_row(uint32_t ebp, uint32_t esi, bool *slow) {
    MEM_LOCAL();
    const uint32_t h = RD32A(ebp - 0x14), count = RD32A(ebp - 0x1e8), key = RD32A(ebp + 0x1c);
    const int32_t y = static_cast<int32_t>(RD32A(ebp - 0x18));
    const uint32_t alpha_bits = RD32A(esi + 0x70), mode = RD32A(esi + 0x80);
    const uint32_t b1 = RD8(esi + 0x74), b2 = RD8(esi + 0x7c), b3 = RD8(esi + 0x78);
    const uint32_t s1 = RD8(esi + 0x88), s2 = RD8(esi + 0x8c), s3 = RD8(esi + 0x90), sa = RD8(esi + 0x94);
    const uint32_t g1 = RD8(0x4ee9ac), g2 = RD8(0x4ee9b0), g3 = RD8(0x4ee9b4);
    const int32_t x0 = static_cast<int32_t>(RD32A(ebp - 0x5ec));
    const int32_t bx0 = static_cast<int32_t>(RD32A(ebp - 0x30)), bx1 = static_cast<int32_t>(RD32A(ebp - 0x2c));
    const int32_t by0 = static_cast<int32_t>(RD32A(ebp - 0x6ec)), by1 = static_cast<int32_t>(RD32A(ebp - 0x7f0));
    uint32_t dest = RD32A(ebp - 0x4ec);
    uint32_t x = RD32A(ebp - 0x24);
    RowOut o{};
    uint32_t c1o = 0;
    for (; static_cast<int32_t>(x) < static_cast<int32_t>(count); x++, dest += 2) {
        int32_t c1 = static_cast<int32_t>(guest_getc(h, slow));
        int32_t c2 = static_cast<int32_t>(guest_getc(h, slow));
        int32_t c3 = static_cast<int32_t>(guest_getc(h, slow));
        if (key) {
            if (c1 == 0 && c2 == 0xff && c3 == 0) {
                c2 = 0;
            } else {
                const uint32_t e = RD32A(0x4e4f48) + 1;
                if (static_cast<int32_t>(shl32(e, 8 - g1)) > c1 && static_cast<int32_t>(shl32(e, 8 - g2)) > c2 &&
                    static_cast<int32_t>(shl32(e, 8 - g3)) > c3)
                    c3 = static_cast<int32_t>(shl32(e, 8 - b2));
            }
        }
        uint32_t a;
        if (!alpha_bits) {
            a = 0;
        } else if (static_cast<int32_t>(shl32(1, 8 - g1)) > c1 && static_cast<int32_t>(shl32(1, 8 - g2)) > c2 &&
                   static_cast<int32_t>(shl32(1, 8 - g3)) > c3) {
            a = 0;
        } else if (mode != 4) {
            a = 1;
        } else if (x0 == -1) {
            a = 0xf;
        } else {
            const int32_t xi = static_cast<int32_t>(x);
            a = (xi >= bx0 && xi <= bx1 && by0 <= y && by1 >= y) ? 8 : 0xf;
        }
        uint32_t v1 = static_cast<uint32_t>(c1) + shl32(1, 7 - b1);
        if (v1 > 0xff) v1 = 0xff;
        v1 >>= (8 - b1) & 31;
        uint32_t v2 = static_cast<uint32_t>(c2) + shl32(1, 7 - b2);
        if (v2 > 0xff) v2 = 0xff;
        v2 >>= (8 - b2) & 31;
        uint32_t v3 = static_cast<uint32_t>(c3) + shl32(1, 7 - b3);
        if (v3 > 0xff) v3 = 0xff;
        v3 >>= (8 - b3) & 31;
        const uint32_t p2 = shl32(v2, s2) & 0xffff, p1 = shl32(v1, s1) & 0xffff, pa = shl32(a, sa) & 0xffff,
                       p3 = shl32(v3, s3) & 0xffff;
        const uint32_t px = p2 | p1 | pa | p3;
        WR16(dest, static_cast<uint16_t>(px));
        c1o = v1;
        o.ebx = px;
        o.edx = p1;
        o.edi = pa;
    }
    WR32A(ebp - 0x2e8, c1o);
    WR32A(ebp - 0x24, x);
    WR32A(ebp - 0x4ec, dest);
    o.eax = x;
    o.ecx = count;
    return o;
}

void apply(const RowOut &o) {
    R.eax = o.eax;
    R.ecx = o.ecx;
    R.edx = o.edx;
    R.ebx = o.ebx;
    R.edi = o.edi;
}

}  // namespace

// jump hook at 0x41cdb6 (row loop entry, count > 0): nonzero = done, continue at 0x41d08d
extern "C" int native_rgb_row(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    const uint32_t ebp = R.ebp, esi = R.esi, esp = R.esp;
    if (!rgb_row_supported(ebp, esi)) return 0;
    bool slow = false;
    if (md == ON) {
        apply(rgb_row(ebp, esi, &slow));
        R.esp = esp;
        return 1;
    }
    // check: native first; the translated loop then runs on the same input (unless a buffer refill
    // happened: file reads cannot be repeated) and the results are compared at 0x41d08d
    const uint32_t h = RD32A(ebp - 0x14), count = RD32A(ebp - 0x1e8);
    std::vector<Region> regions = {{RD32A(ebp - 0x4ec), count * 2}, {ebp - 0x4ec, 4}, {ebp - 0x2e8, 4}, {ebp - 0x24, 4},
                                   {CUR_FILE, 4}, {FILE_READS, 4}, {ARCHIVE_READS, 4}, {FILES, 0x38}};
    if (h && h <= 2) {
        regions.push_back({SUBFILES + h * 48, 12});
        const int32_t idx = static_cast<int32_t>(RD32A(SUBFILES + h * 48));
        if (idx >= 0 && idx < 64) regions.push_back({PACKS + static_cast<uint32_t>(idx) * 40, 0x18});
    }
    Snapshot before = take(regions);
    apply(rgb_row(ebp, esi, &slow));
    R.esp = esp;
    if (slow) return 1;
    row_pending.native = take(regions);
    row_pending.regions = regions;
    row_pending.active = true;
    restore(before);
    return 0;
}

// GAME_HOOK at 0x41d08d (after the row loop)
extern "C" void native_rgb_row_check(void) {
    if (!row_pending.active) return;
    row_pending.active = false;
    Snapshot translated = take(row_pending.regions);
    row_pending.native.f = translated.f;
    row_pending.native.r.esi = translated.r.esi;
    row_pending.native.r.ebp = translated.r.ebp;
    compare("rgb row 0x41cdb6", row_pending.native, translated);
    static uint64_t rows = 0;
    if (++rows % 1000 == 0) rt::log("native: %llu rgb rows compared\n", static_cast<unsigned long long>(rows));
}

// ---- CRT sprintf 0x4b2bb0 / vsprintf 0x4b6350 (the HUD formats strings every frame) ----
// MOTORACER_PRINTF_STATS=1: count the format strings (logged every 10 s).
namespace {
std::map<std::string, uint64_t> printf_formats;
void printf_stats(uint32_t fmt) {
    static const bool on = getenv("MOTORACER_PRINTF_STATS") != nullptr;
    if (!on) return;
    MEM_LOCAL();
    std::string f;
    for (uint32_t a = fmt; RD8(a) && f.size() < 60; a++) f += static_cast<char>(RD8(a));
    printf_formats[f]++;
    static uint64_t n = 0;
    if (++n % 2000 == 0) {
        for (auto &[k, c] : printf_formats) rt::log("printf format x%llu: \"%s\"\n", static_cast<unsigned long long>(c), k.c_str());
        printf_formats.clear();
    }
}
}  // namespace

extern "C" int native_sprintf(void) {    // sprintf(buffer, format, ...)
    MEM_LOCAL();
    printf_stats(RD32A(R.esp + 8));
    return 0;
}
extern "C" int native_vsprintf(void) {   // vsprintf(buffer, format, args)
    MEM_LOCAL();
    printf_stats(RD32A(R.esp + 8));
    return 0;
}
