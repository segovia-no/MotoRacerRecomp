// Native fast path of the game's per-byte file reader (docs/25-psp-port.md, loading time).
//
// Track / object loading decodes files byte by byte through getc(handle) 0x42f650, which goes either
// to an archive layer 0x4309e0 (sub-files inside a pack: offset, size, seek of the real file when the
// position moved) or straight to 0x42f690 (a 16 KB buffer per open file, refilled with fread). On the
// PSP those three translated calls per byte were ~70% of a race load. The native version does the
// common case — the byte is in the buffer, no seek — with the same memory and registers as the
// translated chain; refills, seeks and errors run the translated code (decided before any change).
#include "runtime.h"
#include "recomp_mem_local.h"
#include "native_check.h"

extern "C" void f_0042f650_t(void);
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
