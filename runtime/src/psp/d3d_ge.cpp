// dx::gpu on the PSP (docs/25-psp-port.md): the game's Direct3D triangles drawn by the GE.
//
// The render target is the GE draw buffer itself (480x360, 1:1 with the game's PSP-profile mode);
// TLVERTEX screen positions are used directly in the GE's 2D ("through") mode: colour ARGB -> ABGR,
// depth sz -> 65535 * (1 - sz) (GE depth range 65535..0, test GEQUAL), uv in texels. Textures are
// converted once per change to GE formats (R in the low bits). The guest back buffer and the GE
// buffer are synchronised when the CPU touches it (Lock / Blt), as on the desktop.
// Not yet: fog and specular, perspective-correct texturing (through mode is affine).
#include <malloc.h>
#include <SDL3/SDL.h>
#include <pspgu.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "../dx/d3d_gpu.h"
#include "../dx/ddraw_internal.h"
#include "game_hooks.h"
#include "runtime.h"
#include "ge.h"

namespace dx::gpu {

using namespace rt;

namespace {

enum {
    RS_TEXTUREADDRESS = 3, RS_ZENABLE = 7, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15,
    RS_TEXTUREMAG = 17, RS_TEXTUREMIN = 18, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_TEXTUREMAPBLEND = 21,
    RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAREF = 24, RS_ALPHABLENDENABLE = 27, RS_COLORKEYENABLE = 41,
};

struct GeVertex {
    float u, v;
    uint32_t color;
    float x, y, z;
};

// A frame vertex: the GE vertex plus rhw (1/w), for perspective-correct subdivision.
struct FrameVertex {
    GeVertex g;
    float rhw;
    float sz;
};

struct Texture {
    uint32_t version = ~0u, pal_version = ~0u;
    void *data = nullptr;
    int psm = GU_PSM_5650, tw = 0, th = 0, tbw = 0;
    bool alpha = false;
    bool shared = false;        // data points into guest memory (not owned)
    bool in_place = false;      // 16-bit: guest pixels converted to the GE's order in place
    bool swizzled = false;      // stored in the GE's swizzled layout (16-byte x 8-row blocks)
    uint32_t *clut = nullptr;   // T8: 256 palette entries (GE 8888), 16-byte aligned
};

// The game sends 3-6 render states with every polygon, mostly unchanged: GE commands are sent only
// for values that differ from the last ones sent (the GE keeps its state across display lists).
// psp_ge::state_epoch() changes when other code (the CPU-frame presenter) changes GE state.
struct GeState {
    uint32_t epoch = ~0u;
    int depth_test = -1, depth_func = -1, depth_mask = -1, blend = -1, bsrc = -1, bdst = -1;
    unsigned bsfix = 0, bdfix = 0;
    int alpha_test = -1, alpha_ref = -1, shade = -1, texturing = -1, psm = -1, tfx = -1, tcc = -1, swz = -1;
    const void *clut = nullptr;
    int fmin = -1, fmag = -1, wrap = -1;
    const void *tdata = nullptr;
    float ts_u = -1.f, ts_v = -1.f, to_u = -1.f, to_v = -1.f;   // 3D mode texture scale / offset
    int cull = -1;                                               // hardware culling: 0 off, D3D mode 2 / 3
} gs;

Surface *target = nullptr;
uint32_t g_w = 0, g_h = 0;
int off_x = 0, off_y = 0;           // the target is centred in the 480x360 buffer
float scale = 1.f;                  // and scaled down to fit the visible 480x272 window
uint32_t s_w = 0, s_h = 0;          // its size in the buffer
bool frame_used = false;            // GE drawing since the last present / download
// A Clear with nothing drawn yet is kept here instead of being sent to the GE: menu frames are
// "Clear, then CPU drawing" — the colour clear is then applied to the guest buffer by the CPU and
// the frame presented as a CPU frame, without reading the GE buffer back (docs/25).
uint32_t pending_flags = 0, pending_argb = 0;
float pending_depth = 1.f;
bool cpu_dirty = false;             // the guest back buffer is newer than the GE buffer
std::vector<FrameVertex> frame_vertices;
uint32_t st_frames = 0;   // presented GPU frames (MOTORACER_GE_DUMP)

std::map<Surface *, Texture> textures;
Surface *last_tex_surface = nullptr;   // texture_for's last lookup (std::map nodes do not move)
Texture *last_tex = nullptr;
// Guest pixel memory converted in place to the GE's order (keyed by address: DuplicateSurface makes
// several surfaces share one memory block). sync_to_cpu converts it back.
std::set<uint32_t> in_place_mem;

// ---------- swizzled textures ----------
// The GE reads textures in RAM much faster in its swizzled layout (blocks of 16 bytes x 8 rows, stored
// one after the other): on a real PSP the race's texture reads kept the GE ~8 ms per frame behind the
// CPU once the CPU-side drawing got faster. Done in place for textures that live in guest memory (and
// undone with the in-place conversion, revert_in_place); for the backend's own copies once.
// Needs a row of 16n bytes and 8n rows. MOTORACER_GE_SWIZZLE=0: off.
bool swizzle_on() {
    static const bool on = !getenv("MOTORACER_GE_SWIZZLE") || strcmp(getenv("MOTORACER_GE_SWIZZLE"), "0");
    return on;
}
alignas(16) uint8_t swizzle_band[1024 * 8];   // one band of 8 rows (rows up to 1,024 bytes)
bool can_swizzle(uint32_t row_bytes, uint32_t rows) {
    return swizzle_on() && !(row_bytes & 15) && !(rows & 7) && row_bytes * 8 <= sizeof swizzle_band;
}
void swizzle(uint8_t *p, uint32_t row_bytes, uint32_t rows) {
    const uint32_t blocks = row_bytes / 16;
    for (uint32_t by = 0; by < rows / 8; by++) {
        uint8_t *band = p + by * row_bytes * 8;
        memcpy(swizzle_band, band, row_bytes * 8);
        for (uint32_t bx = 0; bx < blocks; bx++)
            for (uint32_t y = 0; y < 8; y++) memcpy(band + (bx * 8 + y) * 16, swizzle_band + y * row_bytes + bx * 16, 16);
    }
}
void unswizzle(uint8_t *p, uint32_t row_bytes, uint32_t rows) {
    const uint32_t blocks = row_bytes / 16;
    for (uint32_t by = 0; by < rows / 8; by++) {
        uint8_t *band = p + by * row_bytes * 8;
        memcpy(swizzle_band, band, row_bytes * 8);
        for (uint32_t bx = 0; bx < blocks; bx++)
            for (uint32_t y = 0; y < 8; y++) memcpy(band + y * row_bytes + bx * 16, swizzle_band + (bx * 8 + y) * 16, 16);
    }
}
std::set<uint32_t> swizzled_mem;   // guest memory (in in_place_mem too) stored swizzled

// Debug counters, logged every 5 s with MOTORACER_GE_STATS=1.
struct Stats {
    uint32_t draws, tris, clears, flips_gpu, flips_cpu, blts_gpu, blts_cpu, downloads, uploads, tex_converts, subdivided, indexed;
    uint64_t ns_draw, ns_tex;
} st{};
bool stats_on() {
    static const bool on = getenv("MOTORACER_GE_STATS") != nullptr;
    return on;
}
void stats_tick() {
    static uint64_t last = 0;
    if (!stats_on()) return;
    uint64_t now = SDL_GetTicks();
    if (now - last < 5000) return;
    last = now;
    uint64_t ns_ge = psp_ge::take_wait_ns();
    uint64_t tex_bytes = 0, tex8 = 0;
    for (auto &[surf, t] : textures)
        if (t.data && !t.shared) {
            const uint64_t b = static_cast<uint64_t>(t.tbw) * surf->h * (t.psm == GU_PSM_8888 ? 4 : 2);
            tex_bytes += b;
            if (t.psm == GU_PSM_8888) tex8 += b;
        }
    uint32_t list_peak = 0, early = psp_ge::take_list_stats(&list_peak);
    log("ge: %zu textures %.2f MB (from 8-bit %.2f MB), list peak %u KB, early flushes %u | ", textures.size(),
        tex_bytes / 1048576.0, tex8 / 1048576.0, list_peak >> 10, early);
    log("ge: draws %u (indexed %u) tris %u (+%u split) clears %u flip gpu/cpu %u/%u blt gpu/cpu %u/%u downloads %u uploads %u texconv %u | "
        "ms in 5 s: execute %llu, draw %llu (textures %llu), GE wait %llu\n",
        st.draws, st.indexed, st.tris, st.subdivided, st.clears, st.flips_gpu, st.flips_cpu, st.blts_gpu, st.blts_cpu, st.downloads, st.uploads,
        st.tex_converts, (unsigned long long)(execute_ns / 1000000), (unsigned long long)(st.ns_draw / 1000000), (unsigned long long)(st.ns_tex / 1000000),
        (unsigned long long)(ns_ge / 1000000));
    st = {};
    execute_ns = 0;
}

uint32_t argb_to_abgr(uint32_t c) { return (c & 0xFF00FF00u) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16); }

bool usable() { return target != nullptr; }

// Guest back buffer (RGB565) <-> GE draw buffer (scaled when the target is larger), through a RAM
// staging buffer and GE block transfers (psp_ge::read_rect / write_rect).
uint16_t *staging() { return psp_ge::staging(); }

// profiler regions inside the backend (MOTORACER_PROF): where its time goes
uint8_t prof_id(const char *name) { return prof_register(name); }

void upload_target() {
    static const uint8_t region = prof_id("ge: CPU frame upload (downscale)");
    prof::Scope prof_scope(region);
    st.uploads++;
    static const bool ge_halve = !getenv("MOTORACER_GE_HALVE") || strcmp(getenv("MOTORACER_GE_HALVE"), "0");
    if (scale < 1.f && ge_halve &&
        psp_ge::draw_halved_565(M + target->mem, target->pitch, g_w, g_h, off_x, off_y, static_cast<int>(s_w), static_cast<int>(s_h))) {
        cpu_dirty = false;
        return;
    }
    psp_ge::finish();
    uint16_t *buf = staging();
    if (scale >= 1.f) {
        for (uint32_t y = 0; y < s_h; y++) {
            uint16_t *dst = buf + y * psp_ge::FB_STRIDE;
            uint32_t src = target->mem + y * target->pitch;
            for (uint32_t x = 0; x < s_w; x++) dst[x] = psp_ge::swap_rb565(RD16(src + 2 * x));
        }
    } else {
        // Downscaled (640x480 -> 363x272): average 2x2 source pixels. The game draws some menu
        // elements with a 1-pixel checker ("stippled" transparency); picking single pixels turned it
        // into coarse yellow / black blocks, averaging gives the intended blend.
        psp_ge::downscale_565(M + target->mem, target->pitch, g_w, g_h, buf, psp_ge::FB_STRIDE, s_w, s_h);
    }
    psp_ge::write_rect(buf, psp_ge::FB_STRIDE, off_x, off_y, static_cast<int>(s_w), static_cast<int>(s_h));
    cpu_dirty = false;
}

void download_target() {
    static const uint8_t region = prof_id("ge: CPU frame download (upscale)");
    prof::Scope prof_scope(region);
    st.downloads++;
    uint16_t *buf = staging();
    psp_ge::read_rect(buf, psp_ge::FB_STRIDE, off_x, off_y, static_cast<int>(s_w), static_cast<int>(s_h));
    // nearest source column per target column, computed once per size; a target row that maps to the
    // same source row as the one above is a copy of it (scale 0.57: ~3 of 4 rows)
    static std::vector<uint16_t> cols;
    static uint32_t cols_w = 0;
    static float cols_scale = 0.f;
    if (cols_w != g_w || cols_scale != scale) {
        cols.resize(g_w);
        for (uint32_t x = 0; x < g_w; x++) cols[x] = static_cast<uint16_t>(static_cast<uint32_t>(x * scale));
        cols_w = g_w;
        cols_scale = scale;
    }
    const bool aligned = !((target->mem | target->pitch) & 1);
    uint32_t prev_sy = ~0u;
    for (uint32_t y = 0; y < g_h; y++) {
        const uint32_t sy = static_cast<uint32_t>(y * scale);
        const uint32_t dst = target->mem + y * target->pitch;
        if (sy == prev_sy && y) {
            memcpy(M + dst, M + dst - target->pitch, g_w * 2u);
            continue;
        }
        prev_sy = sy;
        const uint16_t *src = buf + sy * psp_ge::FB_STRIDE;
        if (aligned) {
            uint16_t *d = reinterpret_cast<uint16_t *>(M + dst);
            for (uint32_t x = 0; x < g_w; x++) d[x] = psp_ge::swap_rb565(src[cols[x]]);
        } else {
            for (uint32_t x = 0; x < g_w; x++) WR16(dst + 2 * x, psp_ge::swap_rb565(src[cols[x]]));
        }
    }
}

// Applies a pending colour clear to the guest back buffer (CPU fill).
void clear_guest() {
    if (!(pending_flags & 1)) return;
    uint32_t a = pending_argb;
    uint16_t c = static_cast<uint16_t>(((a >> 8) & 0xF800) | ((a >> 5) & 0x07E0) | ((a >> 3) & 0x001F));
    for (uint32_t y = 0; y < g_h; y++) {
        uint32_t row = target->mem + y * target->pitch;
        if (!(row & 3) && !(g_w & 1)) {   // two pixels per store
            uint32_t *d = reinterpret_cast<uint32_t *>(M + row);
            const uint32_t cc = c | static_cast<uint32_t>(c) << 16;
            for (uint32_t x = 0; x < g_w / 2; x++) d[x] = cc;
        } else {
            for (uint32_t x = 0; x < g_w; x++) WR16(row + 2 * x, c);
        }
    }
    pending_flags &= ~1u;
    cpu_dirty = true;   // the GE buffer no longer matches
}

void issue_pending_clear() {
    if (!pending_flags) return;
    int bits = 0;
    if (pending_flags & 1) { sceGuClearColor(argb_to_abgr(pending_argb)); bits |= GU_COLOR_BUFFER_BIT; }
    if (pending_flags & 2) {
        float z = 65535.f * (1.f - pending_depth);
        sceGuClearDepth(static_cast<unsigned>(z < 0 ? 0 : z > 65535 ? 65535 : z));
        bits |= GU_DEPTH_BUFFER_BIT;
    }
    pending_flags = 0;
    if (bits) sceGuClear(bits);
}

void prepare() {
    if (cpu_dirty) upload_target();
    psp_ge::begin();
    issue_pending_clear();
    frame_used = true;
}

// ---------- textures ----------
Texture *texture_for(Surface *s) {
    if (!s) return nullptr;
    if (s->w > 512 || s->h > 512) {
        static int warned = 0;
        if (warned++ < 5) log("d3d ge: texture %ux%u is larger than the GE's 512x512, drawn untextured\n", s->w, s->h);
        return nullptr;
    }
    uint32_t pal_version = s->palette ? s->palette->version : 0;
    // most draws use the same texture as the one before: skip the map lookup
    if (last_tex_surface == s && last_tex->data && last_tex->version == s->version && last_tex->pal_version == pal_version)
        return last_tex;
    Texture &t = textures[s];
    last_tex_surface = s;
    last_tex = &t;
    if (t.data && t.version == s->version && t.pal_version == pal_version) return &t;
    st.tex_converts++;
    struct TexTimer { uint64_t t0 = SDL_GetTicksNS(); ~TexTimer() { st.ns_tex += SDL_GetTicksNS() - t0; } } timer;
    t.version = s->version;
    t.pal_version = pal_version;
    gs.tdata = nullptr;   // new contents (memalign may return the same address): reload the GE texture
    t.tw = 1;
    while (static_cast<uint32_t>(t.tw) < s->w) t.tw <<= 1;
    t.th = 1;
    while (static_cast<uint32_t>(t.th) < s->h) t.th <<= 1;
    t.tbw = static_cast<int>((s->w + 7) & ~7u);
    const bool keyed = s->ckey_src;
    gs.clut = nullptr;
    if (!t.shared) free(t.data);
    t.data = nullptr;
    t.shared = false;
    t.swizzled = t.in_place = false;
    if (s->bpp == 8) {
        t.alpha = keyed;
        uint32_t pal[256];
        for (int i = 0; i < 256; i++) {
            uint32_t e = s->palette ? s->palette->entries[i] : static_cast<uint32_t>(i * 0x010101);
            pal[i] = (e & 0xFFFFFF) | 0xFF000000u;   // PALETTEENTRY bytes R, G, B = the GE's 8888 order
        }
        if (keyed)
            for (uint32_t i = s->ckey_lo; i <= s->ckey_hi && i < 256; i++) pal[i] &= 0x00FFFFFF;
        // PSP-1000 memory: the GE reads 8-bit textures as T8 + CLUT straight from the guest surface
        // (no 32-bit copy, 4x the size); needs a 16-byte aligned start and a pitch of 16n texels.
        uint8_t *src = reinterpret_cast<uint8_t *>(rt::host_ptr(s->mem));
        if ((reinterpret_cast<uintptr_t>(src) & 15) == 0 && (s->pitch & 15) == 0) {
            if (!t.clut) t.clut = static_cast<uint32_t *>(memalign(16, 256 * 4));
            std::memcpy(t.clut, pal, sizeof pal);
            t.psm = GU_PSM_T8;
            t.tbw = static_cast<int>(s->pitch);
            t.data = src;
            t.shared = true;
            if (swizzled_mem.count(s->mem)) {   // already (another surface on this memory, or a palette change)
                t.swizzled = t.in_place = true;
            } else if (can_swizzle(s->pitch, s->h)) {
                swizzle(src, s->pitch, s->h);
                swizzled_mem.insert(s->mem);
                in_place_mem.insert(s->mem);
                t.swizzled = t.in_place = true;
            }
            psp_ge::writeback(t.clut, 256 * 4);
            psp_ge::writeback(src, static_cast<size_t>(s->pitch) * s->h);
            return &t;
        }
        t.psm = GU_PSM_8888;
        t.data = memalign(16, static_cast<size_t>(t.tbw) * s->h * 4);
        auto *out = static_cast<uint32_t *>(t.data);
        for (uint32_t y = 0; y < s->h; y++)
            for (uint32_t x = 0; x < s->w; x++) out[y * t.tbw + x] = pal[RD8(s->mem + y * s->pitch + x)];
        if (can_swizzle(t.tbw * 4u, s->h)) {
            swizzle(static_cast<uint8_t *>(t.data), t.tbw * 4u, s->h);
            t.swizzled = true;
        }
        psp_ge::writeback(t.data, static_cast<size_t>(t.tbw) * s->h * 4);
        return &t;
    }
    const bool is1555 = s->gmask == 0x03E0, is4444 = s->gmask == 0x00F0;
    t.psm = is4444 ? GU_PSM_4444 : (is1555 || keyed) ? GU_PSM_5551 : GU_PSM_5650;
    t.alpha = is4444 || keyed || (is1555 && s->amask);
    // PSP-1000 memory: convert in place (exactly reversible; sync_to_cpu converts back before the game
    // touches the pixels) instead of keeping a second copy. A colour-keyed 565 texture becomes 5551
    // (green loses a bit): that one keeps a copy.
    uint16_t *guest = reinterpret_cast<uint16_t *>(rt::host_ptr(s->mem));
    if ((reinterpret_cast<uintptr_t>(guest) & 15) == 0 && (s->pitch & 15) == 0 && (is1555 || is4444 || !keyed)) {
        if (!in_place_mem.count(s->mem)) {
            for (uint32_t y = 0; y < s->h; y++) {
                uint16_t *row = guest + y * (s->pitch / 2);
                for (uint32_t x = 0; x < s->w; x++) {
                    const uint16_t p = row[x];
                    if (is4444) {
                        row[x] = static_cast<uint16_t>(((p >> 8) & 15) | ((p >> 4) & 15) << 4 | (p & 15) << 8 | ((p >> 12) & 15) << 12);
                    } else if (is1555) {
                        uint16_t a = s->amask ? (p & 0x8000) : 0x8000;
                        if (keyed && p >= s->ckey_lo && p <= s->ckey_hi) a = 0;
                        row[x] = static_cast<uint16_t>(((p >> 10) & 31) | ((p >> 5) & 31) << 5 | (p & 31) << 10 | a);
                    } else {
                        row[x] = psp_ge::swap_rb565(p);
                    }
                }
            }
            in_place_mem.insert(s->mem);
            if (can_swizzle(s->pitch, s->h)) {
                swizzle(reinterpret_cast<uint8_t *>(guest), s->pitch, s->h);
                swizzled_mem.insert(s->mem);
            }
        }
        t.tbw = static_cast<int>(s->pitch / 2);
        t.data = guest;
        t.shared = true;
        t.in_place = true;
        t.swizzled = swizzled_mem.count(s->mem) != 0;
        psp_ge::writeback(guest, static_cast<size_t>(s->pitch) * s->h);
        return &t;
    }
    t.data = memalign(16, static_cast<size_t>(t.tbw) * s->h * 2);
    auto *out = static_cast<uint16_t *>(t.data);
    for (uint32_t y = 0; y < s->h; y++)
        for (uint32_t x = 0; x < s->w; x++) {
            uint16_t p = RD16(s->mem + y * s->pitch + 2 * x), o;
            bool key = keyed && p >= s->ckey_lo && p <= s->ckey_hi;
            if (is4444) {
                o = static_cast<uint16_t>(((p >> 8) & 15) | ((p >> 4) & 15) << 4 | (p & 15) << 8 | ((p >> 12) & 15) << 12);
            } else if (is1555) {
                uint16_t a = s->amask ? (p & 0x8000) : 0x8000;
                if (key) a = 0;
                o = static_cast<uint16_t>(((p >> 10) & 31) | ((p >> 5) & 31) << 5 | (p & 31) << 10 | a);
            } else if (keyed) {   // 565 with a colour key -> 5551
                o = static_cast<uint16_t>(((p >> 11) & 31) | ((p >> 6) & 31) << 5 | (p & 31) << 10 | (key ? 0 : 0x8000));
            } else {
                o = psp_ge::swap_rb565(p);
            }
            out[y * t.tbw + x] = o;
        }
    if (can_swizzle(t.tbw * 2u, s->h)) {
        swizzle(static_cast<uint8_t *>(t.data), t.tbw * 2u, s->h);
        t.swizzled = true;
    }
    psp_ge::writeback(t.data, static_cast<size_t>(t.tbw) * s->h * 2);
    return &t;
}

// ---------- render states ----------
int blend_factor(uint32_t d3d, bool src, unsigned *fix) {
    switch (d3d) {
    case 1: *fix = 0; return GU_FIX;                                // ZERO
    case 2: *fix = 0xFFFFFFFFu; return GU_FIX;                      // ONE
    case 3: return src ? GU_SRC_COLOR : GU_SRC_COLOR;               // SRCCOLOR (as dst factor)
    case 4: return GU_ONE_MINUS_SRC_COLOR;                          // INVSRCCOLOR
    case 5: return GU_SRC_ALPHA;
    case 6: return GU_ONE_MINUS_SRC_ALPHA;
    case 7: return GU_DST_ALPHA;
    case 8: return GU_ONE_MINUS_DST_ALPHA;
    case 9: return GU_SRC_COLOR;                                    // DESTCOLOR (as src factor)
    case 10: return GU_ONE_MINUS_SRC_COLOR;                         // INVDESTCOLOR
    default: *fix = 0xFFFFFFFFu; return GU_FIX;
    }
}

int depth_func(uint32_t d3d) {   // depth is inverted (near = 65535)
    switch (d3d) {
    case 1: return GU_NEVER;
    case 2: return GU_GREATER;   // LESS
    case 3: return GU_EQUAL;
    case 4: return GU_GEQUAL;    // LESSEQUAL
    case 5: return GU_LESS;      // GREATER
    case 6: return GU_NOTEQUAL;
    case 7: return GU_LEQUAL;    // GREATEREQUAL
    default: return GU_ALWAYS;
    }
}


void apply_states(const uint32_t *rs, Texture *tex) {
    if (gs.epoch != psp_ge::state_epoch()) gs = GeState{}, gs.epoch = psp_ge::state_epoch();
    int dt = rs[RS_ZENABLE] ? 1 : 0;
    if (dt != gs.depth_test) { dt ? sceGuEnable(GU_DEPTH_TEST) : sceGuDisable(GU_DEPTH_TEST); gs.depth_test = dt; }
    if (dt) {
        int f = depth_func(rs[23]);
        if (f != gs.depth_func) { sceGuDepthFunc(f); gs.depth_func = f; }
    }
    int dm = rs[RS_ZWRITEENABLE] ? GU_FALSE : GU_TRUE;
    if (dm != gs.depth_mask) { sceGuDepthMask(dm); gs.depth_mask = dm; }
    int bl = rs[RS_ALPHABLENDENABLE] ? 1 : 0;
    if (bl != gs.blend) { bl ? sceGuEnable(GU_BLEND) : sceGuDisable(GU_BLEND); gs.blend = bl; }
    if (bl) {
        unsigned sfix = 0, dfix = 0;
        int sf = blend_factor(rs[RS_SRCBLEND], true, &sfix), df = blend_factor(rs[RS_DESTBLEND], false, &dfix);
        if (sf != gs.bsrc || df != gs.bdst || sfix != gs.bsfix || dfix != gs.bdfix) {
            sceGuBlendFunc(GU_ADD, sf, df, sfix, dfix);
            gs.bsrc = sf; gs.bdst = df; gs.bsfix = sfix; gs.bdfix = dfix;
        }
    }
    bool keyed = rs[RS_COLORKEYENABLE] && tex && tex->alpha;
    int at = (rs[RS_ALPHATESTENABLE] || keyed) ? 1 : 0;
    if (at != gs.alpha_test) { at ? sceGuEnable(GU_ALPHA_TEST) : sceGuDisable(GU_ALPHA_TEST); gs.alpha_test = at; }
    if (at) {
        int ref = rs[RS_ALPHATESTENABLE] ? static_cast<int>(rs[RS_ALPHAREF] & 255) : 0;
        if (ref != gs.alpha_ref) { sceGuAlphaFunc(GU_GREATER, ref, 0xFF); gs.alpha_ref = ref; }
    }
    int sh = rs[RS_SHADEMODE] == 1 ? GU_FLAT : GU_SMOOTH;
    if (sh != gs.shade) { sceGuShadeModel(sh); gs.shade = sh; }
    int tx = tex ? 1 : 0;
    if (tx != gs.texturing) { tx ? sceGuEnable(GU_TEXTURE_2D) : sceGuDisable(GU_TEXTURE_2D); gs.texturing = tx; }
    if (!tex) return;
    if (tex->psm == GU_PSM_T8 && tex->clut != gs.clut) {
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        sceGuClutLoad(256 / 8, tex->clut);
        gs.clut = tex->clut;
    }
    if (tex->data != gs.tdata || tex->psm != gs.psm || static_cast<int>(tex->swizzled) != gs.swz) {
        sceGuTexMode(tex->psm, 0, 0, tex->swizzled ? 1 : 0);
        gs.swz = tex->swizzled;
        sceGuTexImage(0, tex->tw, tex->th, tex->tbw, tex->data);
        sceGuTexFlush();
        gs.tdata = tex->data;
        gs.psm = tex->psm;
    }
    int tfx, tcc;
    switch (rs[RS_TEXTUREMAPBLEND]) {
    case 1: case 7: tfx = GU_TFX_REPLACE; tcc = GU_TCC_RGBA; break;   // DECAL / COPY
    case 3: tfx = GU_TFX_DECAL; tcc = GU_TCC_RGBA; break;             // DECALALPHA
    case 4: tfx = GU_TFX_MODULATE; tcc = GU_TCC_RGBA; break;          // MODULATEALPHA
    case 8: tfx = GU_TFX_ADD; tcc = GU_TCC_RGBA; break;               // ADD
    default: tfx = GU_TFX_MODULATE; tcc = tex->alpha ? GU_TCC_RGBA : GU_TCC_RGB; break;   // MODULATE
    }
    if (tfx != gs.tfx || tcc != gs.tcc) { sceGuTexFunc(tfx, tcc); gs.tfx = tfx; gs.tcc = tcc; }
    int fmag = rs[RS_TEXTUREMAG] >= 2 ? GU_LINEAR : GU_NEAREST, fmin = rs[RS_TEXTUREMIN] >= 2 ? GU_LINEAR : GU_NEAREST;
    if (fmag != gs.fmag || fmin != gs.fmin) { sceGuTexFilter(fmin, fmag); gs.fmin = fmin; gs.fmag = fmag; }
    int wrap = rs[RS_TEXTUREADDRESS] == 3 ? GU_CLAMP : GU_REPEAT;
    if (wrap != gs.wrap) { sceGuTexWrap(wrap, wrap); gs.wrap = wrap; }
}

// 3D-mode texture coordinates: u' = u * scale + offset (through-mode draws ignore these)
void set_tex_transform(float su, float sv, float ou, float ov) {
    if (su != gs.ts_u || sv != gs.ts_v) { sceGuTexScale(su, sv); gs.ts_u = su; gs.ts_v = sv; }
    if (ou != gs.to_u || ov != gs.to_v) { sceGuTexOffset(ou, ov); gs.to_u = ou; gs.to_v = ov; }
}

// Direct3D culling (D3DCULL_CW 2 culls triangles clockwise on screen, D3DCULL_CCW 3 counter-
// clockwise) by the GE in 3D mode; other paths cull in software and pass 0.
// MOTORACER_GE_CULL_FLIP=1 swaps the GE's front face (dev).
void set_cull(uint32_t mode) {
    const int c = (mode == 2 || mode == 3) ? static_cast<int>(mode) : 0;
    if (c == gs.cull) return;
    static const bool flip = getenv("MOTORACER_GE_CULL_FLIP") && strcmp(getenv("MOTORACER_GE_CULL_FLIP"), "0");
    if (!c) {
        sceGuDisable(GU_CULL_FACE);
    } else {
        const bool front_ccw = (c == 2) != flip;   // the kept faces
        sceGuFrontFace(front_ccw ? GU_CCW : GU_CW);
        if (gs.cull <= 0) sceGuEnable(GU_CULL_FACE);
    }
    gs.cull = c;
}

// ---------- indexed 3D draws ----------
// The textured 3D-mode draws of one PROCESSVERTICES batch share its vertices: they are converted once
// (w = 1 / rhw, position x*w, y*w, w) into display-list memory, and each draw adds only 16-bit
// indices; texture size and the 1/16-texel nudge go in the GE's texture scale / offset, culling to the
// GE. Before, every triangle wrote its three vertices (24 bytes each, a division per vertex use).
// MOTORACER_GE_INDEXED=0: the per-triangle path.
bool indexed_on() {
    static const bool on = !getenv("MOTORACER_GE_INDEXED") || strcmp(getenv("MOTORACER_GE_INDEXED"), "0");
    return on;
}
uint32_t batch_id = 0;                // add_vertices calls
GeVertex *batch_ge = nullptr;         // the batch's 3D vertices in list memory
uint32_t batch_ge_id = ~0u, batch_ge_gen = ~0u;

bool batch_ge_valid() {
    return batch_ge && batch_ge_id == batch_id && batch_ge_gen == psp_ge::list_generation();
}

// The batch's 3D vertex array (list memory for all of its vertices, after psp_ge::reserve covering
// it when not valid); each vertex is converted when a draw first uses it (ensure_3d): the 3D-mode
// draws use only the textured ones, and the conversion (a division, 24 bytes to uncached list memory)
// was done for the whole batch (~1,400 vertices) before.
std::vector<uint32_t> v3d_stamp;   // per frame vertex: the batch_stamp it was converted for
uint32_t batch_stamp = 0;

GeVertex *batch_vertices_3d() {
    if (batch_ge_valid()) return batch_ge;
    const size_t n = frame_vertices.size();
    batch_ge = static_cast<GeVertex *>(psp_ge::list_memory(n * sizeof(GeVertex)));
    batch_ge_id = batch_id;
    batch_ge_gen = psp_ge::list_generation();
    if (v3d_stamp.size() < n) v3d_stamp.resize(std::max<size_t>(n, 1536));
    batch_stamp++;
    return batch_ge;
}

inline void ensure_3d(uint32_t i) {
    if (v3d_stamp[i] == batch_stamp) return;
    v3d_stamp[i] = batch_stamp;
    const FrameVertex &v = frame_vertices[i];
    const float w = 1.f / v.rhw;
    batch_ge[i] = GeVertex{v.g.u, v.g.v, v.g.color, v.g.x * w, v.g.y * w, w};
}

// ---------- perspective-correct texturing by subdivision ----------
// The GE's through mode interpolates texture coordinates linearly on screen (affine); Direct3D
// interpolates them perspective-correctly. Textured triangles that are large on screen and span a
// wide depth range are split (up to 3 levels, 64 pieces) at edge midpoints whose uv come from
// perspective-correct interpolation (uv/w averaged and divided by the averaged 1/w). Position,
// depth and colour stay linear on screen, which is how Direct3D rasterises them too, so the depth
// buffer is unchanged. MOTORACER_GE_PERSPECTIVE=0 turns it off.
std::vector<FrameVertex> tri_out;

// MOTORACER_GE_3D (default on): textured triangles with varying depth are drawn in the GE's 3D mode,
// which interpolates texture coordinates perspective-correctly in hardware (no subdivision). The
// vertex goes in as (x*w, y*w, w) with w = 1/rhw; the projection set in psp_ge::init maps it back to
// pixel (x, y) and depth 65535 * W0 / w. Every draw then uses that depth (through mode included, as
// 65535 * min(1, W0 * rhw)), W0 = the game's near plane [0x625774], so 2D and 3D draws compare
// consistently. Depth is no longer linear in distance: coarser far away, where the game's own
// back-to-front bucket order decides anyway (depth test <=).
bool ge3d() {
    static const bool on = !getenv("MOTORACER_GE_3D") || strcmp(getenv("MOTORACER_GE_3D"), "0");
    return on;
}
// Depth for this frame, from sz (the game writes rhw only for textured vertices, sz for all):
// depth = 65535 * min(1, S0 / sz), S0 = half the previous frame's smallest sz (0 < sz < 1: not the 2D
// pieces), so the depth range is spent where the geometry is. The game's sz is k * w (camera distance
// times a scale), k = sz * rhw measured on the previous frame's textured vertices; 3D-mode draws use
// W0 = S0 / k, which gives them the same depth 65535 * W0 / w.
float frame_s0 = 1.f / 1024, frame_min_sz = 1e30f;
float frame_k = 1.f / 65536, frame_k_sample = 0.f;
float near_w() { return frame_s0 / frame_k; }
void frame_depth_tick() {
    if (frame_min_sz < 1e29f) frame_s0 = std::max(1e-7f, 0.5f * frame_min_sz);
    if (frame_k_sample > 0.f) frame_k = frame_k_sample;
    frame_min_sz = 1e30f;
    frame_k_sample = 0.f;
}

bool perspective() {
    static const bool on = !getenv("MOTORACER_GE_PERSPECTIVE") || strcmp(getenv("MOTORACER_GE_PERSPECTIVE"), "0");
    return on;
}

uint32_t mix_color(uint32_t a, uint32_t b) {
    return ((a >> 1) & 0x7F7F7F7Fu) + ((b >> 1) & 0x7F7F7F7Fu) + (a & b & 0x01010101u);
}

FrameVertex midpoint(const FrameVertex &a, const FrameVertex &b) {
    FrameVertex m;
    m.g.x = (a.g.x + b.g.x) * 0.5f;
    m.g.y = (a.g.y + b.g.y) * 0.5f;
    m.g.z = (a.g.z + b.g.z) * 0.5f;
    m.g.color = mix_color(a.g.color, b.g.color);
    m.rhw = (a.rhw + b.rhw) * 0.5f;
    m.g.u = (a.g.u * a.rhw + b.g.u * b.rhw) * 0.5f / m.rhw;
    m.g.v = (a.g.v * a.rhw + b.g.v * b.rhw) * 0.5f / m.rhw;
    return m;
}

void subdivide(const FrameVertex &a, const FrameVertex &b, const FrameVertex &c, int depth) {
    if (depth < 3) {
        float rmin = std::min(a.rhw, std::min(b.rhw, c.rhw)), rmax = std::max(a.rhw, std::max(b.rhw, c.rhw));
        auto len2 = [](const FrameVertex &p, const FrameVertex &q) {
            float dx = p.g.x - q.g.x, dy = p.g.y - q.g.y;
            return dx * dx + dy * dy;
        };
        float longest = std::max(len2(a, b), std::max(len2(b, c), len2(c, a)));
        // split when the depth ratio is > 1.15 and an edge is longer than 24 pixels
        if (rmax > rmin * 1.15f && longest > 24.f * 24.f) {
            FrameVertex ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            subdivide(a, ab, ca, depth + 1);
            subdivide(ab, b, bc, depth + 1);
            subdivide(ca, bc, c, depth + 1);
            subdivide(ab, bc, ca, depth + 1);
            return;
        }
    }
    tri_out.push_back(a);
    tri_out.push_back(b);
    tri_out.push_back(c);
}

void end_frame() {
    frame_depth_tick();
    frame_used = false;
    frame_vertices.clear();
}

}  // namespace

bool init() {
    psp_ge::init();
    return true;
}

void set_target(Surface *s) {
    init();
    if (target == s && g_w == s->w && g_h == s->h) return;
    if (target && target != s && frame_used) psp_ge::finish();
    target = s;
    g_w = s->w;
    g_h = s->h;
    // fit the visible 480x272 window (4:3 targets: 640x480 -> 363x272; the 16:9 band 480x270 is 1:1)
    scale = std::min(1.f, std::min(static_cast<float>(psp_ge::SCREEN_W) / g_w,
                                   static_cast<float>(psp_ge::SCREEN_H) / g_h));
    s_w = static_cast<uint32_t>(g_w * scale);
    s_h = static_cast<uint32_t>(g_h * scale);
    off_x = (psp_ge::FB_W - static_cast<int>(s_w)) / 2;
    off_y = (psp_ge::FB_H - static_cast<int>(s_h)) / 2;
    log("d3d ge: render target %ux%u (scale %.2f)\n", g_w, g_h, scale);
}

namespace {
// One TLVERTEX in the GE backend's form (screen position scaled into the buffer, depth for this frame).
inline FrameVertex frame_vertex(float sx, float sy, float sz, float rhw, uint32_t argb, float tu, float tv) {
    rhw = rhw > 1e-12f ? rhw : 1e-12f;
    float z;
    if (ge3d()) {
        if (sz > 1e-7f && sz < 1.f) frame_min_sz = std::min(frame_min_sz, sz);
        z = sz > 1e-12f ? 65535.f * frame_s0 / sz : 65535.f;
    } else {
        z = 65535.f * (1.f - sz);
    }
    z = z < 0.f ? 0.f : z > 65535.f ? 65535.f : z;
    // Direct3D 5 has pixel centres at integer coordinates, the GE at half-integers: shift by half
    // a game pixel (as the desktop backend does) so coverage and texel sampling match — otherwise
    // point-sampled HUD pieces pick the neighbouring texel (black seams in the red bars).
    return {{tu, tv, argb_to_abgr(argb), (sx + 0.5f) * scale + off_x, (sy + 0.5f) * scale + off_y, z}, rhw, sz};
}

// ---------- direct vertices ----------
// The native render loops (native_render.cpp) hand their vertices over here instead of writing
// TLVERTEX records into the locked execute buffer: each goes straight into frame_vertices at its slot
// (offset in the buffer / 32), PROCESSVERTICES then uses them as they are (no copy out of guest
// memory, no conversion); slots written by other code are converted from guest memory as before.
// The flush runs between Executes, so frame_vertices (the previous Execute's) is free by then.
// MOTORACER_GE_DIRECT=0: off.
constexpr uint32_t DIRECT_SLOTS = 1536;   // 48 KB execute buffers: at most 1,472 vertices
uint32_t direct_mem = 0, direct_size = 0, direct_count = 0;
uint8_t direct_has[DIRECT_SLOTS];
uint32_t direct_argb[DIRECT_SLOTS];   // each slot's last colour (the textured loop leaves most unwritten)

bool direct_on() {
    static const bool on = !getenv("MOTORACER_GE_DIRECT") || strcmp(getenv("MOTORACER_GE_DIRECT"), "0");
    return on;
}
}  // namespace

void execute_buffer_locked(uint32_t mem, uint32_t size) {
    if (direct_count) memset(direct_has, 0, sizeof direct_has);
    if (mem != direct_mem)   // the colours the records hold now
        for (uint32_t k = 0; k < DIRECT_SLOTS && 32 * k + 32 <= size; k++) direct_argb[k] = RD32(mem + 32 * k + 0x10);
    direct_mem = mem;
    direct_size = std::min(size, DIRECT_SLOTS * 32u);
    direct_count = 0;
}

bool direct_span(uint32_t va, uint32_t n, DirectSpan *out) {
    static_assert(sizeof(DirectVertex) == sizeof(FrameVertex) && offsetof(FrameVertex, rhw) == offsetof(DirectVertex, rhw) &&
                      offsetof(FrameVertex, sz) == offsetof(DirectVertex, sz),
                  "direct_put writes FrameVertex records");
    const uint32_t off = va - direct_mem;
    if (!direct_on() || off >= direct_size || off + 32 * n > direct_size || (off & 31)) return false;
    const uint32_t slot = off >> 5;
    if (frame_vertices.size() < slot + n) {
        if (frame_vertices.capacity() < DIRECT_SLOTS) frame_vertices.reserve(DIRECT_SLOTS);
        frame_vertices.resize(slot + n);
    }
    memset(direct_has + slot, 1, n);
    direct_count += n;
    out->v = reinterpret_cast<DirectVertex *>(&frame_vertices[slot]);
    out->argb = direct_argb + slot;
    out->scale = scale;
    out->off_x = static_cast<float>(off_x);
    out->off_y = static_cast<float>(off_y);
    out->s0 = frame_s0;
    out->min_sz = &frame_min_sz;
    out->ge3d = ge3d();
    return true;
}

// PROCESSVERTICES (COPY) of n vertices from guest `src` (in execute buffer `mem`) to frame slot
// `dest`, when the native loops filled this buffer: false otherwise (d3d.cpp copies and converts).
bool direct_process(uint32_t mem, uint32_t src, uint32_t dest, uint32_t n, uint32_t *nverts) {
    if (!direct_count || mem != direct_mem) return false;
    static const uint8_t region = prof_id("ge: vertex conversion");
    prof::Scope prof_scope(region);
    if (ge3d()) psp_ge::set_near_w(near_w());
    batch_id++;
    const uint32_t first = (src - mem) >> 5;
    if (frame_vertices.size() < dest + n) frame_vertices.resize(dest + n);
    if (first != dest) {   // not seen from the game; kept correct
        std::vector<FrameVertex> tmp(n);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t k = first + i;
            if (k < DIRECT_SLOTS && direct_has[k] && k < frame_vertices.size()) tmp[i] = frame_vertices[k];
            else {
                TLVertex v;
                memcpy(&v, M + src + 32 * i, 32);
                tmp[i] = frame_vertex(v.sx, v.sy, v.sz, v.rhw, v.color, v.tu, v.tv);
            }
        }
        std::copy(tmp.begin(), tmp.end(), frame_vertices.begin() + dest);
    } else {
        static const bool check = getenv("MOTORACER_GE_DIRECT_CHECK") != nullptr;
        static uint64_t checked = 0, bad = 0;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t k = first + i;
            if (k < DIRECT_SLOTS && direct_has[k]) {
                if (check) {   // the records were written too: same vertex both ways?
                    TLVertex v;
                    memcpy(&v, M + src + 32 * i, 32);
                    const FrameVertex a = frame_vertices[k], b = frame_vertex(v.sx, v.sy, v.sz, v.rhw, v.color, v.tu, v.tv);
                    checked++;
                    const bool flat = a.rhw == 1.f && a.g.u == 0.f && a.g.v == 0.f;   // untextured: uv, rhw unused
                    const bool tex_ok = flat || (a.g.u == b.g.u && a.g.v == b.g.v && a.rhw == b.rhw);
                    if (memcmp(&a.g.x, &b.g.x, 12) || a.g.color != b.g.color || a.sz != b.sz || !tex_ok) {
                        if (++bad <= 20)
                            log("ge direct check: slot %u: xyz %g %g %g / %g %g %g colour %08x / %08x uv %g %g / %g %g rhw %g / %g\n", k,
                                a.g.x, a.g.y, a.g.z, b.g.x, b.g.y, b.g.z, a.g.color, b.g.color, a.g.u, a.g.v, b.g.u, b.g.v, a.rhw, b.rhw);
                    }
                    if (checked % 200000 == 0) log("ge direct check: %llu vertices, %llu differ\n", (unsigned long long)checked, (unsigned long long)bad);
                }
                continue;
            }
            TLVertex v;
            memcpy(&v, M + src + 32 * i, 32);
            if (k < DIRECT_SLOTS) direct_argb[k] = v.color;
            frame_vertices[k] = frame_vertex(v.sx, v.sy, v.sz, v.rhw, v.color, v.tu, v.tv);
        }
    }
    *nverts = static_cast<uint32_t>(frame_vertices.size());
    return true;
}

void execute_done(uint32_t mem) {
    if (mem != direct_mem || !direct_count) return;
    memset(direct_has, 0, sizeof direct_has);
    direct_count = 0;
}

uint32_t add_vertices(const TLVertex *verts, uint32_t n) {
    static const uint8_t region = prof_id("ge: vertex conversion");
    prof::Scope prof_scope(region);
    // Draws only use the vertices of the latest PROCESSVERTICES (d3d.cpp passes this call's base), so
    // the list restarts here instead of growing over the whole frame: PSP-1000 memory (a race frame
    // reached ~10,000 vertices, and the vector's doubling briefly needed twice that).
    if (direct_count) {   // another buffer executed while the native loops were filling one
        static int warned = 0;
        if (warned++ < 3) log("d3d ge: direct vertices of the locked execute buffer dropped by another Execute\n");
    }
    frame_vertices.clear();
    if (frame_vertices.capacity() < DIRECT_SLOTS) frame_vertices.reserve(DIRECT_SLOTS);   // once, early (as d3d.cpp's copy)
    batch_id++;
    if (ge3d()) psp_ge::set_near_w(near_w());
    for (uint32_t i = 0; i < n; i++) {
        const TLVertex &v = verts[i];
        frame_vertices.push_back(frame_vertex(v.sx, v.sy, v.sz, v.rhw, v.color, v.tu, v.tv));
    }
    return 0;
}

void draw(const uint32_t *rs, Surface *texture, uint32_t base, uint32_t nverts, const uint16_t *indices,
          uint32_t ntri) {
    if (!usable() || !ntri) return;
    st.draws++;
    st.tris += ntri;
    struct DrawTimer {   // two clock reads per draw: only with the stats on
        uint64_t t0 = stats_on() ? SDL_GetTicksNS() : 0;
        ~DrawTimer() { if (t0) st.ns_draw += SDL_GetTicksNS() - t0; }
    } timer;
    static const uint8_t region_draw = prof_id("ge: draw (vertices out)"), region_state = prof_id("ge: texture + state");
    prof::Scope prof_scope(region_state);
    prepare();
    Texture *tex = texture_for(texture);
    apply_states(rs, tex);
    prof_region = region_draw;
    const float su = tex ? static_cast<float>(texture->w) : 1.f, sv = tex ? static_cast<float>(texture->h) : 1.f;
    // Direct3D MODULATE takes alpha from the texture alone when it has an alpha channel; the GE's
    // modulate multiplies it with the vertex alpha, and the game's HUD vertices carry alpha 0.
    const uint32_t blend = rs[RS_TEXTUREMAPBLEND];
    const uint32_t force_alpha = (tex && tex->alpha && blend != 3 && blend != 4) ? 0xFF000000u : 0;
    const uint32_t cull = rs[RS_CULLMODE];
    static const int dump_frame = getenv("MOTORACER_GE_DUMP") ? atoi(getenv("MOTORACER_GE_DUMP")) : -1;
    if (tex && ge3d() && dump_frame < 0) {
        // perspective needed? (rhw varies within the draw: not for HUD / 2D pieces, whose w = 1 the
        // 3D mode would clip at the near plane)
        float rmin = 1e30f, rmax = 0.f, szmin = 1e30f;
        for (uint32_t t = 0; t < 3 * ntri; t++) {
            const FrameVertex &fv = frame_vertices[base + (indices[t] < nverts ? indices[t] : 0)];
            rmin = std::min(rmin, fv.rhw);
            rmax = std::max(rmax, fv.rhw);
            szmin = std::min(szmin, fv.sz);
        }
        if (rmax > rmin * 1.0001f && frame_k_sample == 0.f) {
            const FrameVertex &fv = frame_vertices[base + (indices[0] < nverts ? indices[0] : 0)];
            if (fv.sz > 0.f && fv.sz < 1.f) frame_k_sample = fv.sz * fv.rhw;
        }
        if (rmax > rmin * 1.0001f && szmin >= frame_s0 && szmin < 1.f && !force_alpha && indexed_on()) {
            const float iu = 1.f / static_cast<float>(tex->tw), iv = 1.f / static_cast<float>(tex->th);
            set_tex_transform(su * iu, sv * iv, 0.0625f * iu, 0.0625f * iv);
            set_cull(cull);
            const size_t ibytes = (6u * ntri + 15) & ~size_t{15};
            psp_ge::reserve(ibytes + (batch_ge_valid() ? 0 : frame_vertices.size() * sizeof(GeVertex)));
            auto *idx = static_cast<uint16_t *>(psp_ge::list_memory(ibytes));
            GeVertex *verts = batch_vertices_3d();
            for (uint32_t t = 0; t < 3 * ntri; t++) {
                const uint32_t i = base + (indices[t] < nverts ? indices[t] : 0);
                ensure_3d(i);
                idx[t] = static_cast<uint16_t>(i);
            }
            psp_ge::draw_array(GU_TRIANGLES, GU_INDEX_16BIT | GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                           static_cast<int>(3 * ntri), idx, verts);
            st.indexed++;
            return;
        }
        if (rmax > rmin * 1.0001f && szmin >= frame_s0 && szmin < 1.f) {
            const float iu = 1.f / static_cast<float>(tex->tw), iv = 1.f / static_cast<float>(tex->th);
            set_tex_transform(1.f, 1.f, 0.f, 0.f);
            set_cull(0);
            auto *out = static_cast<GeVertex *>(psp_ge::list_memory(3u * ntri * sizeof(GeVertex)));
            uint32_t k = 0;
            for (uint32_t t = 0; t < ntri; t++) {
                const FrameVertex *v[3];
                for (int j = 0; j < 3; j++) {
                    uint32_t idx = indices[3 * t + j];
                    v[j] = &frame_vertices[base + (idx < nverts ? idx : 0)];
                }
                if (cull == 2 || cull == 3) {
                    float c = (v[1]->g.x - v[0]->g.x) * (v[2]->g.y - v[0]->g.y) - (v[1]->g.y - v[0]->g.y) * (v[2]->g.x - v[0]->g.x);
                    if ((cull == 3 && c < 0.f) || (cull == 2 && c > 0.f)) continue;
                }
                for (int j = 0; j < 3; j++) {
                    const float w = 1.f / v[j]->rhw;
                    GeVertex g;
                    g.u = (v[j]->g.u * su + 0.0625f) * iu;
                    g.v = (v[j]->g.v * sv + 0.0625f) * iv;
                    g.color = v[j]->g.color | force_alpha;
                    g.x = v[j]->g.x * w;
                    g.y = v[j]->g.y * w;
                    g.z = w;
                    out[k++] = g;
                }
            }
            if (k)
                psp_ge::draw_array(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                               static_cast<int>(k), nullptr, out);
            return;
        }
    }
    set_cull(0);
    if (!(tex && perspective()) && dump_frame < 0) {
        // no subdivision: straight into the display list (at most 3 vertices per triangle)
        auto *out = static_cast<GeVertex *>(psp_ge::list_memory(3u * ntri * sizeof(GeVertex)));
        uint32_t k = 0;
        for (uint32_t t = 0; t < ntri; t++) {
            const FrameVertex *v[3];
            for (int j = 0; j < 3; j++) {
                uint32_t idx = indices[3 * t + j];
                v[j] = &frame_vertices[base + (idx < nverts ? idx : 0)];
            }
            if (cull == 2 || cull == 3) {
                float c = (v[1]->g.x - v[0]->g.x) * (v[2]->g.y - v[0]->g.y) - (v[1]->g.y - v[0]->g.y) * (v[2]->g.x - v[0]->g.x);
                if ((cull == 3 && c < 0.f) || (cull == 2 && c > 0.f)) continue;
            }
            for (int j = 0; j < 3; j++) {
                GeVertex g = v[j]->g;
                g.u = g.u * su + (tex ? 0.0625f : 0.f);
                g.v = g.v * sv + (tex ? 0.0625f : 0.f);
                g.color |= force_alpha;
                out[k++] = g;
            }
        }
        if (k)
            psp_ge::draw_array(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                           static_cast<int>(k), nullptr, out);
        return;
    }
    tri_out.clear();
    // Direct3D culling (D3DCULL_CW 2 / CCW 3), done here as in the desktop backend: the game relies
    // on it, e.g. the gauge cluster is drawn twice, once mirrored (back-facing).
    for (uint32_t t = 0; t < ntri; t++) {
        const FrameVertex *v[3];
        for (int k = 0; k < 3; k++) {
            uint32_t idx = indices[3 * t + k];
            v[k] = &frame_vertices[base + (idx < nverts ? idx : 0)];
        }
        if (cull == 2 || cull == 3) {
            float c = (v[1]->g.x - v[0]->g.x) * (v[2]->g.y - v[0]->g.y) - (v[1]->g.y - v[0]->g.y) * (v[2]->g.x - v[0]->g.x);
            if ((cull == 3 && c < 0.f) || (cull == 2 && c > 0.f)) continue;
        }
        if (tex && perspective()) {
            subdivide(*v[0], *v[1], *v[2], 0);
        } else {
            for (int k = 0; k < 3; k++) tri_out.push_back(*v[k]);
        }
    }
    // Debug: MOTORACER_GE_DUMP=<frame> logs every draw of that presented frame (HUD investigations).
    if (dump_frame >= 0 && static_cast<int>(st_frames) == dump_frame) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
        float r0 = 1e9f, r1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
        for (uint32_t t = 0; t < ntri * 3; t++) {
            const FrameVertex &fv = frame_vertices[base + (indices[t] < nverts ? indices[t] : 0)];
            x0 = std::min(x0, fv.g.x); x1 = std::max(x1, fv.g.x); y0 = std::min(y0, fv.g.y); y1 = std::max(y1, fv.g.y);
            u0 = std::min(u0, fv.g.u); u1 = std::max(u1, fv.g.u); v0 = std::min(v0, fv.g.v); v1 = std::max(v1, fv.g.v);
            r0 = std::min(r0, fv.rhw); r1 = std::max(r1, fv.rhw); z0 = std::min(z0, fv.g.z); z1 = std::max(z1, fv.g.z);
        }
        // save the guest surface and the converted GE copy of each texture once (snapshot dir)
        static std::map<Surface *, bool> saved;
        const char *dir = getenv("MOTORACER_SNAPSHOT_DIR");
        if (texture && tex && dir && !saved[texture] && texture->bpp == 16) {
            saved[texture] = true;
            char path[512];
            snprintf(path, sizeof path, "%s/tex_%p_guest.bmp", dir, (void *)texture);
            if (SDL_Surface *g = SDL_CreateSurfaceFrom(static_cast<int>(texture->w), static_cast<int>(texture->h),
                                                       texture->gmask == 0x00F0 ? SDL_PIXELFORMAT_ARGB4444
                                                       : texture->gmask == 0x03E0 ? SDL_PIXELFORMAT_ARGB1555
                                                                                  : SDL_PIXELFORMAT_RGB565,
                                                       M + texture->mem, static_cast<int>(texture->pitch))) {
                SDL_SaveBMP(g, path);
                SDL_DestroySurface(g);
            }
            snprintf(path, sizeof path, "%s/tex_%p_ge.bmp", dir, (void *)texture);
            SDL_PixelFormat f = tex->psm == GU_PSM_4444 ? SDL_PIXELFORMAT_ABGR4444
                                : tex->psm == GU_PSM_5551 ? SDL_PIXELFORMAT_ABGR1555 : SDL_PIXELFORMAT_BGR565;
            if (tex->psm != GU_PSM_8888 && tex->psm != GU_PSM_T8)
                if (SDL_Surface *g = SDL_CreateSurfaceFrom(static_cast<int>(texture->w), static_cast<int>(texture->h), f,
                                                           tex->data, tex->tbw * 2)) {
                    SDL_SaveBMP(g, path);
                    SDL_DestroySurface(g);
                }
            log("ge dump: saved texture %p (version %u, converted version %u, ckey %d %04x-%04x, masks %04x/%04x/%04x/%04x)\n",
                (void *)texture, texture->version, tex->version, texture->ckey_src, texture->ckey_lo, texture->ckey_hi,
                texture->rmask, texture->gmask, texture->bmask, texture->amask);
        }
        log("ge dump: tex %p %ux%u psm %d a%d | tris %u -> %zu | xy %.1f,%.1f..%.1f,%.1f uv %.3f,%.3f..%.3f,%.3f "
            "rhw %.4g..%.4g z %.0f..%.0f | zen %u zw %u zf %u blend %u %u/%u at %u ck %u tmb %u\n",
            (void *)texture, texture ? texture->w : 0, texture ? texture->h : 0, tex ? tex->psm : -1, tex ? tex->alpha : 0,
            ntri, tri_out.size() / 3, x0, y0, x1, y1, u0, v0, u1, v1, r0, r1, z0, z1, rs[RS_ZENABLE],
            rs[RS_ZWRITEENABLE], rs[RS_ZFUNC], rs[RS_ALPHABLENDENABLE], rs[RS_SRCBLEND], rs[RS_DESTBLEND],
            rs[RS_ALPHATESTENABLE], rs[RS_COLORKEYENABLE], rs[RS_TEXTUREMAPBLEND]);
    }
    if (tri_out.empty()) return;
    auto *out = static_cast<GeVertex *>(psp_ge::list_memory(tri_out.size() * sizeof(GeVertex)));
    for (size_t i = 0; i < tri_out.size(); i++) {
        GeVertex g = tri_out[i].g;
        // + 1/16 texel: with the half-pixel shift (add_vertices) 1:1 sprites sample exactly on texel
        // boundaries, where Direct3D takes the texel to the right / below; nudge them there.
        g.u = g.u * su + (tex ? 0.0625f : 0.f);
        g.v = g.v * sv + (tex ? 0.0625f : 0.f);
        g.color |= force_alpha;
        out[i] = g;
    }
    st.subdivided += static_cast<uint32_t>(tri_out.size() / 3 - ntri);
    psp_ge::draw_array(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   static_cast<int>(tri_out.size()), nullptr, out);
}

void clear(uint32_t flags, uint32_t argb, float depth) {
    if (!usable()) return;
    st.clears++;
    // a later clear replaces an earlier pending one; after drawing it goes to the GE in order
    if (flags & 1) pending_argb = argb;
    if (flags & 2) pending_depth = depth;
    pending_flags |= flags & 3;
    if (frame_used) {
        psp_ge::begin();
        issue_pending_clear();
    }
}

// Undo an in-place texture conversion (see the texture cache) before the game reads or writes pixels.
void revert_in_place(Surface *s) {
    in_place_mem.erase(s->mem);
    uint16_t *guest = reinterpret_cast<uint16_t *>(rt::host_ptr(s->mem));
    if (swizzled_mem.erase(s->mem)) unswizzle(reinterpret_cast<uint8_t *>(guest), s->pitch, s->h);
    const bool is1555 = s->gmask == 0x03E0, is4444 = s->gmask == 0x00F0;
    for (uint32_t y = 0; s->bpp == 16 && y < s->h; y++) {
        uint16_t *row = guest + y * (s->pitch / 2);
        for (uint32_t x = 0; x < s->w; x++) {
            const uint16_t o = row[x];
            if (is4444)
                row[x] = static_cast<uint16_t>((o & 15) << 8 | ((o >> 4) & 15) << 4 | ((o >> 8) & 15) | ((o >> 12) & 15) << 12);
            else if (is1555)
                row[x] = static_cast<uint16_t>((o & 31) << 10 | ((o >> 5) & 31) << 5 | ((o >> 10) & 31) | (s->amask ? (o & 0x8000) : 0));
            else
                row[x] = psp_ge::swap_rb565(o);
        }
    }
    for (auto &[surf, t] : textures)   // every surface sharing this memory converts again at its next use
        if (t.in_place && t.data == guest) {
            if (gs.tdata == t.data) gs.tdata = nullptr;
            t.data = nullptr;
            t.shared = t.in_place = t.swizzled = false;
            t.version = ~0u;
        }
}

void sync_to_cpu(Surface *s) {
    if (s && !in_place_mem.empty() && in_place_mem.count(s->mem)) revert_in_place(s);
    if (!s || s != target || !usable()) return;
    if (frame_used) {
        download_target();
        frame_used = false;   // the guest buffer now holds the frame; further drawing re-uploads it
        cpu_dirty = true;
    } else {
        clear_guest();
    }
}

void cpu_wrote(Surface *s) {
    if (s && s == target) cpu_dirty = true;
}

void surface_destroyed(Surface *s) {
    auto it = textures.find(s);
    if (in_place_mem.count(s->mem)) {   // a later surface may get this memory: forget the mark (unless shared)
        bool shared_mem = false;
        for (auto &[surf, t] : textures) shared_mem |= surf != s && surf->mem == s->mem;
        if (!shared_mem) in_place_mem.erase(s->mem), swizzled_mem.erase(s->mem);
    }
    if (it != textures.end()) {
        if (!it->second.shared) free(it->second.data);
        free(it->second.clut);
        if (gs.clut == it->second.clut) gs.clut = nullptr;
        textures.erase(it);
    }
    if (last_tex_surface == s) last_tex_surface = nullptr, last_tex = nullptr;
    if (s == target) target = nullptr;
}

bool present_flip(Surface *back) {
    stats_tick();
    if (back != target || !frame_used || !usable()) {
        if (back == target && usable()) clear_guest();   // CPU frame: presented from guest memory
        st.flips_cpu++;
        end_frame();
        return false;
    }
    st.flips_gpu++;
    if (cpu_dirty) upload_target();
    psp_ge::show();
    end_frame();
    st_frames++;
    game::frame_tick();   // per-frame hooks (memory stats, state saving) also during races
    return true;
}

bool present_blt(Surface *src, Surface *, int32_t, int32_t, uint32_t, uint32_t) {
    bool r = present_flip(src);
    if (r) { st.flips_gpu--; st.blts_gpu++; } else { st.flips_cpu--; st.blts_cpu++; }
    // the race presents by Blt: log when that starts (loading-time measurements, with the stats)
    static uint32_t blts = 0;
    if (r && stats_on() && ++blts == 1) log("ge: first Blt present (race) at %llu ms\n", (unsigned long long)SDL_GetTicks());
    return r;
}

}  // namespace dx::gpu
