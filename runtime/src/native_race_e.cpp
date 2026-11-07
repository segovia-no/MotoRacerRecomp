// Native versions of hot race functions, group E (docs/25-psp-port.md): small functions the race calls
// hundreds of times per frame (instruction counts of the translated code, tools/icprof.py). Hooked
// through NATIVE_FUNCS (translate.py): f_<va> calls native_hot_<va>, which returns 0 to run the
// translated body f_<va>_t. MOTORACER_NATIVE=check runs both on the same state and compares
// (native_check.h). Floating point in fpu_t, in the x87 code's operation order (-ffp-contract=off).
#include <cstring>
#include <limits>

#include "runtime.h"
#include "recomp_mem_local.h"
#include "native_check.h"

extern "C" void f_00473a90_t(void);
extern "C" void f_00495bd0_t(void);
extern "C" void f_004b32d0_t(void);
extern "C" void f_004039c0_t(void);
extern "C" void f_00403a20_t(void);
extern "C" void f_00403740_t(void);
extern "C" void f_00458b80_t(void);
extern "C" void f_0041e1b0_t(void);
extern "C" void f_00490b90_t(void);
extern "C" void f_0040d3b0_t(void);
extern "C" void f_0049a160_t(void);
extern "C" void f_0046e8b0_t(void);
extern "C" void f_00422540_t(void);
extern "C" void f_00476700(void);
extern "C" void f_004767b0(void);
extern "C" void f_004b2bb0(void);
extern "C" void f_00472800(void);
extern "C" void f_00473540(void);
extern "C" void f_004b2ea0(void);
extern "C" void f_004099a0(void);
extern "C" void f_0046e8b0(void);
extern "C" void f_00462110(void);
extern "C" void f_0045f290(void);
extern "C" void f_00461f70(void);
extern "C" void game_hook_bikes_begin(void);
extern "C" void game_hook_cockpit_begin(void);
extern "C" void game_hook_cockpit_end(void);

namespace {
using namespace native;

void counted(const char *name, uint32_t &n) {
    if (++n == 1 || n % 20000 == 0)
        rt::log("native: %s checked %u times (mismatches so far: %llu)\n", name, n, static_cast<unsigned long long>(mismatches));
}

// ---- 0x473a90: (r, g, b) bytes -> a pixel of the display format (stdcall, ret 0xc) ----
// Each component: (c & 0xff) >> [0x4ee9b8 / bc / c0], then shifted left by [0x4ee9c4 / c8 / cc] as a
// 16-bit value, or-ed. eax = the pixel (upper half 0); cl = the last shift; edx = the blue part.
constexpr uint32_t FMT = 0x4ee9b8;

inline uint16_t part(uint32_t c, uint8_t down, uint8_t up) {
    const uint32_t v = (c & 0xff) >> (down & 31);
    return static_cast<uint16_t>(v << (up & 31));   // a 16-bit shl: 16 or more leaves 0
}

uint32_t pack_rgb(uint32_t r, uint32_t g, uint32_t b, uint32_t &ecx, uint32_t &edx) {
    MEM_LOCAL();
    const uint8_t rd = RD8(FMT), gd = RD8(FMT + 4), bd = RD8(FMT + 8), ru = RD8(FMT + 0xc), gu = RD8(FMT + 0x10),
                  bu = RD8(FMT + 0x14);
    const uint16_t pr = part(r, rd, ru), pg = part(g, gd, gu), pb = part(b, bd, bu);
    ecx = (ecx & 0xffffff00u) | bu;
    edx = pb;
    return static_cast<uint16_t>(pr | pg | pb);
}

void pack_rgb_fn() {
    MEM_LOCAL();
    const uint32_t sp = R.esp;
    uint32_t ecx = R.ecx, edx = R.edx;
    R.eax = pack_rgb(RD32A(sp + 4), RD32A(sp + 8), RD32A(sp + 12), ecx, edx);
    R.ecx = ecx;
    R.edx = edx;
    R.esp = sp + 16;
}

// ---- 0x495bd0: a palette range to display pixels (thiscall: ecx = object; args first, last; ret 8) ----
// With a palette ([obj + 0x549a], [obj + 0x549e] entries of 3 bytes) and last >= first: for i in
// first..last, entry i % count through 0x473a90 into the 16-bit table at obj + 0x418 + 2 i.
void palette_range() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, obj = R.ecx, first = RD32A(sp + 4), last = RD32A(sp + 8);
    R.esp = sp + 12;
    const uint32_t pal = RD32(obj + 0x549a);
    R.eax = pal;
    if (!pal || last < first) return;
    const uint32_t n = RD32(obj + 0x549e);
    uint32_t ecx = R.ecx, edx = R.edx, eax = 0;
    for (uint32_t i = first;; ) {
        const uint32_t p = pal + (i % n) * 3;
        ecx = p;
        eax = pack_rgb(RD8(p), RD8(p + 1), RD8(p + 2), ecx, edx);
        i++;
        WR16(obj + 0x416 + 2 * i, static_cast<uint16_t>(eax));
        if (i > last) break;
    }
    R.eax = eax;
    R.ecx = ecx;
    R.edx = edx;
}

// ---- 0x4b32d0: swap n bytes of a and b (cdecl a, b, n) — the sort's swap ----
// eax = 0 after a swap (unchanged when a == b), ecx / edx = the ends.
void swap_bytes() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, a = RD32A(sp + 4), b = RD32A(sp + 8), n = RD32A(sp + 12);
    R.esp = sp + 4;
    R.ecx = a;
    R.edx = b;
    if (a == b) return;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t x = RD8(a + i);
        WR8(a + i, RD8(b + i));
        WR8(b + i, x);
    }
    R.eax = 0;
    R.ecx = a + n;
    R.edx = b + n;
}

// ---- 0x4039c0 / 0x403a20: copy a 4x3 matrix (thiscall: ecx = dst; arg src; ret 4) ----
// eax = dst, edx = the last dword (the x86's copy registers ebx / ebp are restored).
void mat_copy() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, dst = R.ecx, src = RD32A(sp + 4);
    uint32_t last = 0;
    for (uint32_t k = 0; k < 12; k++) {
        last = RD32(src + 4 * k);
        WR32(dst + 4 * k, last);
    }
    R.eax = dst;
    R.edx = last;
    R.esp = sp + 8;
}

// ---- 0x403740: rotation from three angles and a translation (thiscall: ecx = 4x3 out; args angles, t) ----
// Per angle (radians) * [0x4d00d0] (_ftol) indexes the sine table 0x5fcb80: a = T[i & 0x3fff],
// b = T[(i - 0x1000) & 0x3fff]. The rows as the x87 code computes them, with the float temporaries it
// stores and reloads (t10 = b0 a2, t0c = a0 b2, t08 = b0 b2, t14 = a0 a2); row 3 = t.
// eax = out, ecx / edx = the bits of t.z / t.x.
constexpr uint32_t SIN_TABLE = 0x5fcb80;

inline float rnd(fpu_t v) { return static_cast<float>(v); }
inline fpu_t F_(float v) { return static_cast<fpu_t>(v); }

void rot_translate() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, out = R.ecx, ang = RD32A(sp + 4), t = RD32A(sp + 8);
    const fpu_t k = static_cast<fpu_t>(RDF64(0x4d00d0));
    auto ab = [&](uint32_t a, fpu_t &s, fpu_t &c) {
        const uint32_t i = static_cast<uint32_t>(FTOL(F_(RDF32(a)) * k));
        s = F_(RDF32A(SIN_TABLE + (i & 0x3fff) * 4));
        c = F_(RDF32A(SIN_TABLE + ((i - 0x1000) & 0x3fff) * 4));
    };
    fpu_t a0, b0, a1, b1, a2, b2;
    ab(ang, a0, b0);
    ab(ang + 4, a1, b1);
    ab(ang + 8, a2, b2);
    WRF32(out, rnd(a1 * a2));
    const float t10 = rnd(b0 * a2), t0c = rnd(a0 * b2);
    WRF32(out + 4, rnd(F_(t10) * b1 + F_(t0c)));
    const float t08 = rnd(b0 * b2);
    const fpu_t x = a0 * a2;
    const float t14 = rnd(x);
    WRF32(out + 8, rnd(F_(t08) - x * b1));
    WRF32(out + 0xc, rnd(-(b2 * a1)));
    WRF32(out + 0x10, rnd(F_(t14) - F_(t08) * b1));
    WRF32(out + 0x18, rnd(b1));
    WRF32(out + 0x14, rnd(F_(t0c) * b1 + F_(t10)));
    WRF32(out + 0x1c, rnd(-(a1 * b0)));
    WRF32(out + 0x20, rnd(a1 * a0));
    const uint32_t tx = RD32(t), ty = RD32(t + 4), tz = RD32(t + 8);
    WR32(out + 0x24, tx);
    WR32(out + 0x28, ty);
    WR32(out + 0x2c, tz);
    R.eax = out;
    R.ecx = tz;
    R.edx = tx;
    R.esp = sp + 12;
}

// A guest function called as the x86 does: ecx, the arguments pushed below `esp` (the caller's esp
// before the pushes), the return address under them; the callee pops its arguments. Returns eax.
uint32_t gcall(void (*fn)(void), uint32_t esp, uint32_t ret, uint32_t ecx, std::initializer_list<uint32_t> args) {
    MEM_LOCAL();
    const uint32_t at = esp - 4 * static_cast<uint32_t>(args.size()) - 4;
    WR32A(at, ret);
    uint32_t a = at + 4;
    for (uint32_t v : args) { WR32A(a, v); a += 4; }
    R.esp = at;
    R.ecx = ecx;
    fn();
    R.esp = esp;
    return R.eax;
}

// ---- 0x458b80: draw the other bikes (stdcall: skip list, its length; ret 8) ----
// For bikes 1 .. [0x509bac] - 1 (0x3209 bytes each from 0x584421; [bike] = drawn this frame): an
// active bike ([+0x68]) inside the camera's view (0x46e8b0, camera [0x4dc0f8] of the table 0x5161c0,
// radius 512) is drawn by 0x462110 in the replay view ([0x4dc008] and [0x508834] == 1), else — unless
// the skip list is given and holds none of the bike's sector [+0x31c1] — by 0x45f290 (counter
// [0x4f0dc0]) and 0x461f70. The list scan is per bike (the translated cost: bikes x list length).
// The callers read no register afterwards (eax / ecx / edx are not compared in check mode). The
// translated body's GAME_HOOKS (cockpit render nodes, native_render.cpp) are called at the same points.
constexpr uint32_t BIKES = 0x584421, BIKE_SIZE = 0x3209, CAMS = 0x5161c0;

void draw_bikes() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, list = RD32A(sp + 4), n = RD32A(sp + 8);
    const uint32_t B = sp - 0x14;   // esp after the prologue
    const uint32_t ebx0 = R.ebx, esi0 = R.esi, edi0 = R.edi, ebp0 = R.ebp;
    WR32A(sp - 4 - 4, ebx0);   // the saved registers, as the x86 pushes them
    WR32A(sp - 0xc, esi0);
    WR32A(sp - 0x10, edi0);
    WR32A(sp - 0x14, ebp0);
    uint32_t ebp = ebp0;
    game_hook_bikes_begin();
    for (uint32_t i = 1; static_cast<int32_t>(RD32A(0x509bac)) > static_cast<int32_t>(i); i++) {
        const uint32_t s = BIKES + BIKE_SIZE * (i - 1);
        WR32(s, 0);
        if (!RD32(s + 0x68)) continue;
        R.ebx = i;
        R.esi = s;
        R.edi = list;
        R.ebp = ebp;
        auto cam = [&] { return CAMS + RD32A(0x4dc0f8) * 0x238u; };
        if (!gcall(f_0046e8b0, B, 0x458bd7, cam(), {s + 0x30, 0x44000000})) continue;
        if (RD32A(0x4dc008) && RD32A(0x508834) == 1) {
            game_hook_cockpit_begin();
            gcall(f_00462110, B, 0x458c1e, s, {RD32A(0x4dc0c0), cam(), RD32(s + 0x307d)});
            game_hook_cockpit_end();
            continue;
        }
        bool found = false;
        if (static_cast<int32_t>(n) > 0) {
            const uint32_t sector = RD32(s + 0x31c1);
            for (uint32_t k = 0; k < n; k++) found |= RD32(list + 4 * k) == sector;
            WR32A(B + 0x10, i);
            ebp = list + 4 * n;
            R.ebp = ebp;
        }
        if (!found && list) continue;
        gcall(f_0045f290, B, 0x458c80, s, {RD32A(0x4dc0c0), cam(), 0xffffffffu});
        WR32A(0x4f0dc0, RD32A(0x4f0dc0) + 1);
        gcall(f_00461f70, B, 0x458cac, s, {RD32A(0x4dc0c0), cam()});
        WR32(s, 1);
    }
    R.ebx = ebx0;
    R.esi = esi0;
    R.edi = edi0;
    R.ebp = ebp0;
    R.esp = sp + 12;
}

// ---- 0x41e1b0: Direct3D render states into an execute buffer (thiscall: ecx = writer, arg on; ret 4) ----
// [ecx + 8] = the write pointer: a D3DOP_STATERENDER header (8, 8, count 3), then three (state, value)
// pairs: on: (0x1b, [0x4e4f10]), (0x13, 5), (0x14, 6); off: (0xf, 0), (0x1b, 0), (0x15, [0x53bc10]).
// [0x4e4f2c] = on, [0x4e4f3c] = -100. eax = the new pointer; edx = [0x53bc10] when off.
void blend_ops() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, w = R.ecx, on = RD32A(sp + 4);
    uint32_t p = RD32(w + 8);
    WR8(p, 8);
    WR8(p + 1, 8);
    WR16(p + 2, 3);
    p += 4;
    const uint32_t ops[2][6] = {{0xf, 0, 0x1b, 0, 0x15, RD32A(0x53bc10)}, {0x1b, RD32A(0x4e4f10), 0x13, 5, 0x14, 6}};
    const uint32_t *o = ops[on ? 1 : 0];
    for (int k = 0; k < 3; k++, p += 8) {
        WR32(p, o[2 * k]);
        WR32(p + 4, o[2 * k + 1]);
    }
    WR32(w + 8, p);
    WR32A(0x4e4f2c, on ? 1 : 0);
    WR32A(0x4e4f3c, 0xffffff9cu);
    R.eax = p;
    if (!on) R.edx = o[5];
    R.esp = sp + 8;
}

// ---- 0x490b90: a 32-step colour gradient into a palette range (thiscall: ecx = object; first, last; ret 8) ----
// From the bytes (r, g, b) at +0x5497 to those at +0x5494 in 16.16 steps of ((end - start) << 16) >> 5
// (unsigned, as the x86 shifts), 32 display pixels (0x473a90) into the table 0x60d1a0; then entries
// first + 1 .. last + 1 of the object's 16-bit table at +0x416 take table[i & 31].
void gradient() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, obj = R.ecx, first = RD32A(sp + 4), last = RD32A(sp + 8);
    uint32_t r = static_cast<uint32_t>(RD8(obj + 0x5497)) << 16, g = static_cast<uint32_t>(RD8(obj + 0x5498)) << 16,
             b = static_cast<uint32_t>(RD8(obj + 0x5499)) << 16;
    const uint32_t dr = ((static_cast<uint32_t>(RD8(obj + 0x5494)) - (r >> 16)) << 16) >> 5;
    const uint32_t dg = ((static_cast<uint32_t>(RD8(obj + 0x5495)) - (g >> 16)) << 16) >> 5;
    const uint32_t db = ((static_cast<uint32_t>(RD8(obj + 0x5496)) - (b >> 16)) << 16) >> 5;
    uint32_t ecx = R.ecx, edx = R.edx, eax = 0;
    for (uint32_t i = 0; i < 32; i++) {
        eax = pack_rgb(r >> 16, g >> 16, b >> 16, ecx, edx);
        WR16A(0x60d1a0 + 2 * i, static_cast<uint16_t>(eax));
        r += dr;
        g += dg;
        b += db;
    }
    eax = dg;
    edx = first;
    if (last >= first) {
        do {
            const uint32_t k = edx & 0x1f;
            edx++;
            eax = RD16A(0x60d1a0 + 2 * k);
            WR16(obj + 0x416 + 2 * edx, static_cast<uint16_t>(eax));
        } while (edx <= last);
    }
    R.eax = eax;
    R.ecx = last;
    R.edx = edx;
    R.esp = sp + 12;
}

// ---- 0x40d3b0: per surface type, the nearest sector around the current one (thiscall: ecx = track; out, n; ret 8) ----
// Track: sectors [ecx] (252 bytes: centre x y z at +0, type mask +0xf8), count [ecx + 4], current [ecx + 0x20].
// Over the 2n sectors from current - n (wrapped once), for each of the 7 mask bits a sector has, the
// sector with the smallest squared distance (floats as the x86 rounds them; ties keep the first) goes to
// out[bit]. eax = 2n, ecx = 2n (0 without sectors to scan), edx = the first sector.
void nearest_types() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, t = R.ecx, out = RD32A(sp + 4), n = RD32A(sp + 8);
    const int32_t count = static_cast<int32_t>(RD32(t + 4)), cur = static_cast<int32_t>(RD32(t + 0x20));
    const int32_t start = static_cast<int32_t>((static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(count) - n + static_cast<uint32_t>(cur)))) % count);
    const uint32_t base = RD32(t);
    const uint32_t c = base + static_cast<uint32_t>(cur) * 252u;
    const float px = RDF32(c), py = RDF32(c + 4), pz = RDF32(c + 8);
    float best[7];
    for (float &x : best) x = std::numeric_limits<float>::max();
    uint32_t fsw = F.sw;
    const int32_t total = static_cast<int32_t>(n * 2);
    int32_t i = 0;
    for (; i < total; i++) {
        int32_t sct = i + start;
        if (!(count > sct)) sct -= count;
        const uint32_t S = base + static_cast<uint32_t>(sct) * 252u;
        const float dx = static_cast<float>(static_cast<fpu_t>(RDF32(S)) - px);
        const float dy = static_cast<float>(static_cast<fpu_t>(RDF32(S + 4)) - py);
        const float dz = static_cast<float>(static_cast<fpu_t>(RDF32(S + 8)) - pz);
        const float d = static_cast<float>((static_cast<fpu_t>(dy) * dy + static_cast<fpu_t>(dz) * dz) + static_cast<fpu_t>(dx) * dx);
        const uint32_t mask = RD32(S + 0xf8);
        for (int k = 0; k < 7; k++) {
            if (!(mask & (1u << k))) continue;
            FCOM_L(fsw, static_cast<fpu_t>(best[k]), static_cast<fpu_t>(d));
            if (fsw & 0x4100u) continue;
            best[k] = d;
            WR32(out + 4 * k, static_cast<uint32_t>(sct));
        }
    }
    F.sw = static_cast<uint16_t>(fsw);
    R.eax = static_cast<uint32_t>(total);
    R.ecx = total > 0 ? static_cast<uint32_t>(total) : 0;
    R.edx = static_cast<uint32_t>(start);
    R.esp = sp + 12;
}

// ---- 0x49a160: begin a frame's render lists (stdcall: keep, no_lock; ret 8) ----
// Unless no_lock: lock the back buffer (0x476700 on [0x624afc]; failing: the x86's error message and
// exit), [0x624f5c] = its pixels at the viewport origin, unlock again when [0x4dc170] (0x4767b0);
// [0x4f1a20] = locked. Then, when locked or no_lock and not keep: the pool / record / node counters are
// reset ([0x53469c] += the pool count) and the span table rebuilt: [0x68b008] entries of 8 bytes at
// 0x60d3d0 (0, [0x62e448] + 1, 0), pointers at 0x623bd8 ([0x65e1d4] counts them). Last 0x4099a0 on
// [0x4dc0c4] when it holds something. Callers read no register afterwards.
void render_begin() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, keep = RD32A(sp + 4), no_lock = RD32A(sp + 8);
    const uint32_t B = sp - 0xc;   // esp after the prologue
    const uint32_t ebx0 = R.ebx, esi0 = R.esi, ebp0 = R.ebp;
    R.esi = no_lock;
    WR32A(0x4f1a20, 0);
    if (!no_lock) {
        WR32A(0x624f5c, gcall(f_00476700, B, 0x49a17f, RD32A(0x624afc), {0x62ebec}));
        if (!RD32A(0x624f5c)) {   // the x86's "cannot lock" message, then exit(1)
            gcall(f_004b2bb0, B, 0x49a1a6, R.ecx, {0x5d2e00, 0x4db258, 0x4f2554, 0x4f2568, 0x516});
            gcall(f_00472800, B, 0x49a1b3, R.ecx, {0x5d2e00});
            WR32A(0x4ea818, 1);
            gcall(f_00472800, B, 0x49a1c7, R.ecx, {0x4db248});
            gcall(f_00473540, B, 0x49a1cc, R.ecx, {});
            gcall(f_004b2ea0, B, 0x49a1d3, R.ecx, {1});
        }
        if (RD32A(0x624f5c)) {
            const uint32_t off = RD32A(0x62ebec) * RD32A(0x6d1040) + RD32A(0x624374);
            const uint32_t unlock = RD32A(0x4dc170);
            WR32A(0x624f5c, RD32A(0x624f5c) + off * 2);
            if (unlock) gcall(f_004767b0, B, 0x49a217, RD32A(0x624afc), {});
            WR32A(0x4f1a20, 1);
        }
    }
    if ((RD32A(0x4f1a20) || no_lock) && !keep) {
        WR32A(0x53469c, RD32A(0x53469c) + RD32A(0x68b014));
        for (uint32_t a : {0x655d14u, 0x65e1c8u, 0x68b014u, 0x624f60u, 0x65e1d4u}) WR32A(a, 0);
        WR32A(0x6d20c0, 1);
        if (static_cast<int32_t>(RD32A(0x68b008)) > 0) {
            uint32_t i;
            do {
                i = RD32A(0x65e1d4);
                const uint32_t e = 0x60d3d0 + 8 * i;
                WR32A(0x623bd8 + 4 * i, e);
                WR16A(e, 0);
                WR16A(e + 2, static_cast<uint16_t>(RD32A(0x62e448) + 1));
                WR32A(e + 4, 0);
                WR32A(0x65e1d4, i + 1);
            } while (static_cast<int32_t>(RD32A(0x68b008)) > static_cast<int32_t>(i + 1));
        }
        WR32A(0x6d1048, 0);
        WR32A(0x6d1f78, 0);
    }
    const uint32_t g = RD32A(0x4dc0c4);
    if (g && RD32(g)) gcall(f_004099a0, B, 0x49a2ed, g, {});
    R.ebx = ebx0;
    R.esi = esi0;
    R.ebp = ebp0;
    R.esp = sp + 12;
}

// ---- 0x46e8b0: is a sphere visible from a camera (thiscall: ecx = camera; centre, radius; ret 8) ----
// Not beyond the draw distance ([0x51d1bc + 36 * [0x4dc098]] * [0x4d13b0], against the unrounded
// distance to the camera position +0x98), then the four frustum planes (0x4b10c0: dot + d, counted at
// [0x709378]) at +0xd4 (> r), +0xe4 (< -r), +0x104 and +0xf4 (both > r), r = radius * [0x4d13b8]
// (stored over the radius argument, as the x86 does). eax = 1 when visible; ecx = the last plane.
void sphere_visible() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, cam = R.ecx, p = RD32A(sp + 4);
    uint32_t fsw = F.sw;
    auto ld = [&](uint32_t a) { return static_cast<fpu_t>(RDF32(a)); };
    const float dx = static_cast<float>(ld(p) - ld(cam + 0x98)), dy = static_cast<float>(ld(p + 4) - ld(cam + 0x9c)),
                dz = static_cast<float>(ld(p + 8) - ld(cam + 0xa0));
    const fpu_t dist = FM(sqrt)((static_cast<fpu_t>(dy) * dy + static_cast<fpu_t>(dz) * dz) + static_cast<fpu_t>(dx) * dx);
    const fpu_t lim = ld(0x51d1bc + 36 * RD32A(0x4dc098)) * ld(0x4d13b0);
    FCOM_L(fsw, lim, dist);
    R.esp = sp + 12;
    R.eax = 0;
    if (fsw & 0x100u) { F.sw = static_cast<uint16_t>(fsw); return; }
    const float r = static_cast<float>(ld(sp + 8) * static_cast<fpu_t>(RDF64(0x4d13b8)));
    WRF32A(sp + 8, r);
    auto plane = [&](uint32_t pl) {
        WR32A(0x709378, RD32A(0x709378) + 1);
        R.ecx = pl;
        return ((ld(pl + 4) * ld(p + 4) + ld(pl + 8) * ld(p + 8)) + ld(pl) * ld(p)) + ld(pl + 0xc);
    };
    FCOM_L(fsw, plane(cam + 0xd4), static_cast<fpu_t>(r));
    if (fsw & 0x4100u) { F.sw = static_cast<uint16_t>(fsw); return; }
    const fpu_t p2 = plane(cam + 0xe4);
    FCOM_L(fsw, -static_cast<fpu_t>(r), p2);
    if (fsw & 0x4100u) { F.sw = static_cast<uint16_t>(fsw); return; }
    FCOM_L(fsw, plane(cam + 0x104), static_cast<fpu_t>(r));
    const bool a = !(fsw & 0x4100u);
    FCOM_L(fsw, plane(cam + 0xf4), static_cast<fpu_t>(r));
    const bool b = !(fsw & 0x4100u);
    F.sw = static_cast<uint16_t>(fsw);
    R.eax = (a && b) ? 1 : 0;
}

// ---- 0x422540: per-vertex light of a model (thiscall: ecx = model; ? , light (3), matrix (12), out; ret 0x50) ----
// The light vector goes through the matrix's inverse (rotation transposed, translation -R^T t, applied
// with the translation as the x86 does) and is normalized (unless its length is [0x4d0664]); for each of
// the model's [+0x30] normals ([+0x10], 12 bytes): t = _ftol(K - (n . -l) * K) (K = [0x4d069c]),
// _ftol((t + [0x4d06a0]) * [0x4d0680]) + 8, at most 31 (signed), << 13 into the light field (+0xc) of
// the output vertices (28 bytes). Nothing without normals ([+0x10] == 0). Callers read no register.
void vertex_light() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, model = R.ecx;
    R.esp = sp + 0x54;
    if (!RD32(model + 0x10)) return;
    auto ld = [&](uint32_t a) { return static_cast<fpu_t>(RDF32(a)); };
    auto rnd = [](fpu_t v) { return static_cast<float>(v); };
    auto F_ = [](float v) { return static_cast<fpu_t>(v); };
    const uint32_t m = sp + 0x14;   // matrix by value: 9 rotation, 3 translation
    float mi[12];
    const int tr[9] = {0, 3, 6, 1, 4, 7, 2, 5, 8};
    for (int k = 0; k < 9; k++) mi[k] = RDF32(m + 4 * tr[k]);
    auto M = [&](int k) { return ld(m + 4 * k); };
    mi[9] = rnd(-((M(1) * M(10) + M(2) * M(11)) + M(9) * M(0)));
    mi[10] = rnd(-((M(5) * M(11) + M(9) * M(3)) + M(10) * M(4)));
    mi[11] = rnd(-((M(9) * M(6) + M(11) * M(8)) + M(10) * M(7)));
    const fpu_t L0 = ld(sp + 8), L1 = ld(sp + 0xc), L2 = ld(sp + 0x10);
    auto I = [&](int k) { return F_(mi[k]); };
    const float X = rnd(((I(6) * L2 + I(3) * L1) + I(0) * L0) + I(9));
    const float Y = rnd(((I(7) * L2 + I(4) * L1) + I(1) * L0) + I(10));
    const float Z = rnd(((I(5) * L1 + I(8) * L2) + I(2) * L0) + I(11));
    uint32_t fsw = F.sw;
    float lx = X, ly = Y, lz = Z;
    const fpu_t len = FM(sqrt)((F_(Y) * F_(Y) + F_(Z) * F_(Z)) + F_(X) * F_(X));
    FCOM_L(fsw, len, ld(0x4d0664));
    if (!(fsw & 0x4000u)) {
        const fpu_t r = static_cast<fpu_t>(1) / F_(rnd(len));
        lx = rnd(F_(X) * r);
        ly = rnd(F_(Y) * r);
        lz = rnd(r * F_(Z));
    }
    F.sw = static_cast<uint16_t>(fsw);
    const int32_t n = static_cast<int32_t>(RD32(model + 0x30));
    if (n <= 0) return;
    const fpu_t nx = -F_(lx), ny = -F_(ly), nz = -F_(lz);
    const fpu_t nxf = F_(rnd(nx)), nyf = F_(rnd(ny)), nzf = F_(rnd(nz));
    const fpu_t K = ld(0x4d069c), add = ld(0x4d06a0), mul = ld(0x4d0680);
    const uint32_t normals = RD32(model + 0x10), out = RD32A(sp + 0x44);
    for (int32_t i = 0; i < n; i++) {
        const uint32_t N = normals + 12u * static_cast<uint32_t>(i);
        const fpu_t d = (ld(N + 8) * nzf + ld(N + 4) * nyf) + ld(N) * nxf;
        const int32_t t = static_cast<int32_t>(FTOL(K - d * K));
        int32_t v = static_cast<int32_t>(FTOL((static_cast<fpu_t>(t) + add) * mul)) + 8;
        if (v >= 0x20) v = 0x1f;
        WR32(out + 0xc + 0x1c * static_cast<uint32_t>(i), static_cast<uint32_t>(v) << 13);
    }
}

uint32_t n_473a90, n_495bd0, n_4b32d0, n_4039c0, n_403a20, n_403740, n_458b80, n_41e1b0, n_490b90, n_40d3b0, n_49a160, n_46e8b0, n_422540;
}  // namespace

extern "C" int native_hot_473a90(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { pack_rgb_fn(); return 1; }
    check("pack rgb 0x473a90", pack_rgb_fn, f_00473a90_t, {});
    counted("pack rgb 0x473a90", n_473a90);
    return 1;
}

extern "C" int native_hot_495bd0(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { palette_range(); return 1; }
    const uint32_t obj = R.ecx, first = RD32A(R.esp + 4), last = RD32A(R.esp + 8);
    std::vector<Region> regions;
    if (last >= first && last - first < 0x10000) regions.push_back({obj + 0x418 + 2 * first, 2 * (last - first + 1) + 2});   // compared in dwords
    check("palette range 0x495bd0", palette_range, f_00495bd0_t, regions);
    counted("palette range 0x495bd0", n_495bd0);
    return 1;
}

extern "C" int native_hot_4b32d0(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { swap_bytes(); return 1; }
    const uint32_t a = RD32A(R.esp + 4), b = RD32A(R.esp + 8), n = RD32A(R.esp + 12);
    std::vector<Region> regions;
    if (n < 0x10000) regions = {{a, n}, {b, n}};
    check("swap bytes 0x4b32d0", swap_bytes, f_004b32d0_t, regions);
    counted("swap bytes 0x4b32d0", n_4b32d0);
    return 1;
}

extern "C" int native_hot_4039c0(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { mat_copy(); return 1; }
    check("matrix copy 0x4039c0", mat_copy, f_004039c0_t, {{R.ecx, 48}});
    counted("matrix copy 0x4039c0", n_4039c0);
    return 1;
}

extern "C" int native_hot_403a20(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { mat_copy(); return 1; }
    check("matrix copy 0x403a20", mat_copy, f_00403a20_t, {{R.ecx, 48}});
    counted("matrix copy 0x403a20", n_403a20);
    return 1;
}

extern "C" int native_hot_403740(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { rot_translate(); return 1; }
    check("rotation + translation 0x403740", rot_translate, f_00403740_t, {{R.ecx, 48}});
    counted("rotation + translation 0x403740", n_403740);
    return 1;
}

extern "C" int native_hot_458b80(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { draw_bikes(); return 1; }
    // the callees write the bikes, render lists and vertex pools: the whole writable image
    // (.data / .bss) and the stack below the caller are restored between the two runs
    const uint32_t lo = 0x4db000, hi = 0x70c568;
    check_abi("draw bikes 0x458b80", draw_bikes, f_00458b80_t, {{lo, hi - lo}, {R.esp - 0x2000, 0x2000}});
    counted("draw bikes 0x458b80", n_458b80);
    return 1;
}

extern "C" int native_hot_41e1b0(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { blend_ops(); return 1; }
    check("blend ops 0x41e1b0", blend_ops, f_0041e1b0_t, {{R.ecx + 8, 4}, {RD32(R.ecx + 8), 28}, {0x4e4f2c, 4}, {0x4e4f3c, 4}});
    counted("blend ops 0x41e1b0", n_41e1b0);
    return 1;
}

extern "C" int native_hot_490b90(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { gradient(); return 1; }
    const uint32_t obj = R.ecx, first = RD32A(R.esp + 4), last = RD32A(R.esp + 8);
    std::vector<Region> regions{{0x60d1a0, 64}};
    if (last >= first && last - first < 0x10000) regions.push_back({obj + 0x418 + 2 * first, 2 * (last - first + 1) + 2});
    check("gradient 0x490b90", gradient, f_00490b90_t, regions);
    counted("gradient 0x490b90", n_490b90);
    return 1;
}

extern "C" int native_hot_40d3b0(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { nearest_types(); return 1; }
    check("nearest types 0x40d3b0", nearest_types, f_0040d3b0_t, {{RD32A(R.esp + 4), 28}});
    counted("nearest types 0x40d3b0", n_40d3b0);
    return 1;
}

extern "C" int native_hot_49a160(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    // check mode: the back-buffer lock (host state) cannot run twice, so only calls without it compare
    if (md == ON || !RD32A(R.esp + 8)) { render_begin(); return 1; }
    const uint32_t lo = 0x4db000, hi = 0x70c568;
    check_abi("render begin 0x49a160", render_begin, f_0049a160_t, {{lo, hi - lo}});
    counted("render begin 0x49a160", n_49a160);
    return 1;
}

extern "C" int native_hot_46e8b0(void) {
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { sphere_visible(); return 1; }
    check("sphere visible 0x46e8b0", sphere_visible, f_0046e8b0_t, {{0x709378, 4}, {R.esp + 8, 4}});
    counted("sphere visible 0x46e8b0", n_46e8b0);
    return 1;
}

extern "C" int native_hot_422540(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { vertex_light(); return 1; }
    const uint32_t model = R.ecx, n = RD32(model + 0x30), out = RD32A(R.esp + 0x44);
    std::vector<Region> regions;
    if (n < 0x10000 && RD32(model + 0x10)) regions.push_back({out, 0x1c * n});
    check_abi("vertex light 0x422540", vertex_light, f_00422540_t, regions);
    counted("vertex light 0x422540", n_422540);
    return 1;
}
