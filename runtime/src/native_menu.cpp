// Native versions of the game's 2D menu drawing (the PSP hardware profile: 37% of a menu frame in
// two translated functions, docs/25-psp-port.md). Hooked through NATIVE_FUNCS (translate.py); a
// native function returns 0 to run the translated body instead. MOTORACER_NATIVE=check runs both on
// the same state and compares (native_check.h).
#include <cstring>

#include "runtime.h"
#include "recomp_mem_local.h"
#include "native_check.h"

extern "C" void f_0043ede0_t(void);
extern "C" void f_0043f0f0_t(void);
extern "C" void f_0049d0f0(void);

namespace {

using namespace native;

constexpr uint32_t DRAW_ON = 0x4f1824;      // nonzero while 2D drawing is enabled
constexpr uint32_t SCREEN_W = 0x655d10, SCREEN_H = 0x624b18;
constexpr uint32_t BACK_PIXELS = 0x624f5c;  // back buffer pixels (16 bit)
constexpr uint32_t BACK_PITCH = 0x62ebec;   // in pixels

// n 16-bit pixels from src to dst (guest addresses, both even). newlib's memmove copies byte by byte
// when the two are not equally word-aligned (sprite and tile rows often are 2 bytes apart): ~2x
// slower than the translated code on a real PSP. Here: words when the phases match, else a funnel
// of aligned word loads (one word read past the source span, still guest memory).
void copy_px(uint8_t *mb, uint32_t dst, uint32_t src, uint32_t n) {
    if ((dst | src) & 1) { memmove(mb + dst, mb + src, static_cast<size_t>(n) * 2); return; }
    if (n && (dst & 2)) {   // a halfword first: dst word-aligned
        memcpy(mb + dst, mb + src, 2);
        dst += 2, src += 2, n--;
    }
    uint32_t *d = reinterpret_cast<uint32_t *>(mb + dst);
    const uint32_t words = n / 2;
    if (!(src & 2)) {
        const uint32_t *s = reinterpret_cast<const uint32_t *>(mb + src);
        for (uint32_t i = 0; i < words; i++) d[i] = s[i];
    } else {
        const uint32_t *s = reinterpret_cast<const uint32_t *>(mb + src - 2);
        uint32_t prev = s[0];
        for (uint32_t i = 0; i < words; i++) {
            const uint32_t next = s[i + 1];
            d[i] = (prev >> 16) | (next << 16);   // little-endian: pixels src+0, src+2
            prev = next;
        }
    }
    if (n & 1) memcpy(mb + dst + 4 * words, mb + src + 4 * words, 2);
}

// ---- 0x43ede0: sprite blit (thiscall, 4 stack args, ret 0x10) ----
// ecx = sprite {x, y, w, h at +0..+0xc, mode +0x14, pixels +0x2c}; arg1 = mode (5: the sprite's),
// arg2 / arg3 = percentages 0..100 that crop the sprite from the top / right. Clipped to the screen,
// copied into the back buffer row by row in one of four ways (jump table 0x43f0d4):
//   0 plain copy (rep movsd), 1 colour key 0 (pixels of value 0 skipped),
//   2 / 3 every other pixel (the checker "stipple"), the start shifting by one pixel per row: plain /
//   colour key. The sprite's pixels are created on first use (0x43eb00) and mode 4 is a tiled
//   background (0x43f0f0): both stay translated (the native returns 0 before changing anything).
// Callers read no register the function leaves (eax / ecx / edx are reloaded).
struct Blit {
    uint32_t dst, src, w, rows, src_pitch, dst_pitch;
    uint32_t mode;
};

// The clipping of 0x43ee32..0x43ef91 in the x86's integer arithmetic. False: nothing is drawn.
bool setup(uint32_t obj, int32_t mode_arg, int32_t top, int32_t right, Blit &b) {
    MEM_LOCAL();
    int32_t y = static_cast<int32_t>(RD32(obj + 4)), x = static_cast<int32_t>(RD32(obj));
    uint32_t src = RD32(obj + 0x2c);
    const int32_t w = static_cast<int32_t>(RD32(obj + 8)), h = static_cast<int32_t>(RD32(obj + 0xc));
    int32_t cols = w, rows = h;
    top = top < 0 ? 0 : top > 100 ? 100 : top;
    right = right < 0 ? 0 : right > 100 ? 100 : right;
    const int32_t skip = (100 - top) * h / 100;   // imul / idiv: truncation toward zero
    y += skip;
    src += static_cast<uint32_t>(skip * w) * 2u;
    rows -= skip;
    cols += right * w / -100;
    const int32_t sw = static_cast<int32_t>(RD32A(SCREEN_W)), sh = static_cast<int32_t>(RD32A(SCREEN_H));
    if (sw <= x) return false;
    if (x + w < 0) return false;
    if (y >= sh) return false;
    int32_t bottom = rows + y;
    if (bottom < 0) return false;
    if (x < 0) {
        cols += x;
        src -= static_cast<uint32_t>(x) * 2u;
        x = 0;
    }
    if (y < 0) {
        rows = bottom;
        src -= static_cast<uint32_t>(y * w) * 2u;
        y = 0;
    }
    if (w + x > sw) cols += sw - w - x;
    if (rows + y > sh) rows = sh - y;
    const uint32_t pitch = RD32A(BACK_PITCH);
    b.dst = RD32A(BACK_PIXELS) + (static_cast<uint32_t>(y) * pitch + static_cast<uint32_t>(x)) * 2u;
    if (!rows || !cols) return false;
    uint32_t m = static_cast<uint32_t>(mode_arg);
    if (m == 5) m = RD32(obj + 0x14);
    if (m > 3) return false;
    b.src = src;
    b.w = static_cast<uint32_t>(cols);
    b.rows = static_cast<uint32_t>(rows);
    b.src_pitch = static_cast<uint32_t>(w);
    b.dst_pitch = pitch;
    b.mode = m;
    return true;
}

void run(const Blit &b) {
    MEM_LOCAL();
    uint32_t dst = b.dst, src = b.src;
    const uint32_t n = b.w;
    // 16-bit pixels at even addresses (always, in practice): plain pointer loads / stores
    const bool aligned = !((dst | src) & 1);
    switch (b.mode) {
    case 0:   // plain rows
        for (uint32_t r = b.rows; r; r--) {
            copy_px(mb, dst, src, n);
            dst += b.dst_pitch * 2;
            src += b.src_pitch * 2;
        }
        break;
    case 1:   // colour key 0
        for (uint32_t r = b.rows; r; r--) {
            if (aligned) {
                const uint16_t *s = reinterpret_cast<const uint16_t *>(mb + src);
                uint16_t *d = reinterpret_cast<uint16_t *>(mb + dst);
                for (uint32_t i = 0; i < n; i++)
                    if (const uint16_t p = s[i]) d[i] = p;
            } else {
                for (uint32_t i = 0; i < n; i++) {
                    const uint16_t p = RD16(src + 2 * i);
                    if (p) WR16(dst + 2 * i, p);
                }
            }
            dst += b.dst_pitch * 2;
            src += b.src_pitch * 2;
        }
        break;
    default: {   // 2 / 3: every other pixel, the phase alternating per row
        const bool keyed = b.mode == 3;
        const uint32_t pairs = static_cast<uint32_t>(static_cast<int32_t>(n) >> 1);
        int32_t shift = 1;
        for (uint32_t r = b.rows; r; r--) {
            if (aligned) {
                const uint16_t *s = reinterpret_cast<const uint16_t *>(mb + src);
                uint16_t *d = reinterpret_cast<uint16_t *>(mb + dst);
                for (uint32_t i = 0; static_cast<int32_t>(i) < static_cast<int32_t>(pairs); i++) {
                    const uint16_t p = s[2 * i];
                    if (!keyed || p) d[2 * i] = p;
                }
            } else {
                for (uint32_t i = 0; static_cast<int32_t>(i) < static_cast<int32_t>(pairs); i++) {
                    const uint16_t p = RD16(src + 4 * i);
                    if (!keyed || p) WR16(dst + 4 * i, p);
                }
            }
            src += (b.src_pitch + static_cast<uint32_t>(shift)) * 2;
            dst += (b.dst_pitch + static_cast<uint32_t>(shift)) * 2;
            shift = -shift;
        }
        break;
    }
    }
}

bool sprite_blit() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, obj = R.ecx;
    if (!obj || !RD32A(DRAW_ON)) { R.esp = sp + 0x14; return true; }
    if (!RD32(obj + 0x2c) || RD32(obj + 0x14) == 4) return false;   // translated: first use / tiles
    Blit b;
    if (setup(obj, static_cast<int32_t>(RD32A(sp + 8)), static_cast<int32_t>(RD32A(sp + 12)),
              static_cast<int32_t>(RD32A(sp + 16)), b))
        run(b);
    R.esp = sp + 0x14;
    return true;
}

// ---- 0x43f0f0: tiled background (thiscall, no stack args, plain ret) ----
// The menu's scrolling background: [0x49d0f0] (ecx = [0x4dc0c0], two zero args) returns the back
// buffer's pixels; all 480 rows get the tile row ((y + r) & 127) of the 256-pixel-wide tile at
// [ecx+0x2c], from x = [ecx+0x18] & 127, 128 pixels repeated 5 times (640). The x86 has a plain
// path and an MMX one (flag [0x4dbcf4]) that write the same pixels. ~600 KB per menu frame.
constexpr uint32_t BACK_SURFACE = 0x4dc0c0, MMX_ON = 0x4dbcf4;
constexpr uint32_t LOCK_RETURN = 0x43f126;   // after the call to 0x49d0f0

void tile_fill() {
    MEM_LOCAL();
    const uint32_t sp = R.esp, obj = R.ecx;
    if (!RD32A(DRAW_ON)) { R.esp = sp + 4; return; }
    const uint32_t frame = sp - 0x38;   // esp after the prologue (sub 0x28, four pushes)
    const uint32_t xoff = RD32(obj + 0x18), yoff = RD32(obj + 0x1c);
    WR32A(frame - 4, 0);
    WR32A(frame - 8, 0);
    WR32A(frame - 12, LOCK_RETURN);
    R.esp = frame - 12;
    R.ecx = RD32A(BACK_SURFACE);
    f_0049d0f0();   // stdcall, pops its two arguments
    const uint32_t dst = R.eax, pitch = RD32A(BACK_PITCH);
    const uint32_t tile = RD32(obj + 0x2c), x = (xoff & 0x7f) * 2;
    for (uint32_t r = 0; r < 480; r++) {
        const uint32_t row = tile + (((yoff & 0x7f) + r) & 0x7f) * 512 + x;
        const uint32_t d = dst + r * pitch * 2;
        for (uint32_t k = 0; k < 5; k++) copy_px(mb, d + k * 256, row, 128);
    }
    R.esp = sp + 4;
}

}  // namespace

extern "C" int native_tile_fill(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    if (md == ON) { tile_fill(); return 1; }
    std::vector<Region> regions;
    if (RD32A(DRAW_ON) && RD32A(BACK_PIXELS)) regions.push_back({RD32A(BACK_PIXELS), 480 * RD32A(BACK_PITCH) * 2});
    check_abi("tile fill 0x43f0f0", tile_fill, f_0043f0f0_t, regions);
    static uint32_t n = 0;
    if (++n == 1 || n % 500 == 0) rt::log("native: tile fill checked %u times (mismatches so far: %llu)\n", n,
                                          static_cast<unsigned long long>(mismatches));
    return 1;
}

extern "C" int native_sprite_blit(void) {
    MEM_LOCAL();
    const Mode md = mode();
    if (md == OFF) return 0;
    const uint32_t obj = R.ecx;
    if (obj && RD32A(DRAW_ON) && (!RD32(obj + 0x2c) || RD32(obj + 0x14) == 4)) return 0;   // translated (0x43f0f0 is native itself)
    if (md == ON) return sprite_blit() ? 1 : 0;
    // check: the back buffer region the blit can touch, compared after both versions; eax / ecx / edx
    // are not part of the function's contract (see above)
    Blit b;
    std::vector<Region> regions;
    if (obj && RD32A(DRAW_ON) &&
        setup(obj, static_cast<int32_t>(RD32A(R.esp + 8)), static_cast<int32_t>(RD32A(R.esp + 12)),
              static_cast<int32_t>(RD32A(R.esp + 16)), b))
        regions.push_back({b.dst, (b.rows * b.dst_pitch + 2) * 2});
    check_abi("sprite blit 0x43ede0", [] { sprite_blit(); }, f_0043ede0_t, regions);
    static uint32_t n = 0;
    if (++n == 1 || n % 5000 == 0) rt::log("native: sprite blit checked %u times (mismatches so far: %llu)\n", n,
                                           static_cast<unsigned long long>(mismatches));
    return 1;
}
