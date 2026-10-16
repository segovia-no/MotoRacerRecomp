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
#include <cstring>
#include <map>
#include <vector>

#include "../dx/d3d_gpu.h"
#include "../dx/ddraw_internal.h"
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
};

struct Texture {
    uint32_t version = ~0u, pal_version = ~0u;
    void *data = nullptr;
    int psm = GU_PSM_5650, tw = 0, th = 0, tbw = 0;
    bool alpha = false;
};

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

// Debug counters, logged every 5 s with MOTORACER_GE_STATS=1.
struct Stats {
    uint32_t draws, tris, clears, flips_gpu, flips_cpu, blts_gpu, blts_cpu, downloads, uploads, tex_converts, subdivided;
    uint64_t ns_draw, ns_tex;
} st{};
void stats_tick() {
    static const bool on = getenv("MOTORACER_GE_STATS") != nullptr;
    static uint64_t last = 0;
    if (!on) return;
    uint64_t now = SDL_GetTicks();
    if (now - last < 5000) return;
    last = now;
    uint64_t ns_ge = psp_ge::take_wait_ns();
    log("ge: draws %u tris %u (+%u split) clears %u flip gpu/cpu %u/%u blt gpu/cpu %u/%u downloads %u uploads %u texconv %u | "
        "ms in 5 s: execute %llu, draw %llu (textures %llu), GE wait %llu\n",
        st.draws, st.tris, st.subdivided, st.clears, st.flips_gpu, st.flips_cpu, st.blts_gpu, st.blts_cpu, st.downloads, st.uploads,
        st.tex_converts, (unsigned long long)(execute_ns / 1000000), (unsigned long long)(st.ns_draw / 1000000), (unsigned long long)(st.ns_tex / 1000000),
        (unsigned long long)(ns_ge / 1000000));
    st = {};
    execute_ns = 0;
}
std::map<Surface *, Texture> textures;

uint32_t argb_to_abgr(uint32_t c) { return (c & 0xFF00FF00u) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16); }

bool usable() { return target != nullptr; }

// Guest back buffer (RGB565) <-> GE draw buffer (nearest-neighbour when scaled), through a RAM
// staging buffer and GE block transfers (psp_ge::read_rect / write_rect).
uint16_t *staging() {
    static uint16_t *buf = static_cast<uint16_t *>(memalign(64, psp_ge::FB_STRIDE * psp_ge::FB_H * 2));
    return buf;
}

void upload_target() {
    st.uploads++;
    psp_ge::finish();
    uint16_t *buf = staging();
    for (uint32_t y = 0; y < s_h; y++) {
        uint16_t *dst = buf + y * psp_ge::FB_STRIDE;
        uint32_t src = target->mem + static_cast<uint32_t>(y / scale) * target->pitch;
        for (uint32_t x = 0; x < s_w; x++) dst[x] = psp_ge::swap_rb565(RD16(src + 2 * static_cast<uint32_t>(x / scale)));
    }
    psp_ge::write_rect(buf, psp_ge::FB_STRIDE, off_x, off_y, static_cast<int>(s_w), static_cast<int>(s_h));
    cpu_dirty = false;
}

void download_target() {
    st.downloads++;
    uint16_t *buf = staging();
    psp_ge::read_rect(buf, psp_ge::FB_STRIDE, off_x, off_y, static_cast<int>(s_w), static_cast<int>(s_h));
    for (uint32_t y = 0; y < g_h; y++) {
        const uint16_t *src = buf + static_cast<uint32_t>(y * scale) * psp_ge::FB_STRIDE;
        uint32_t dst = target->mem + y * target->pitch;
        for (uint32_t x = 0; x < g_w; x++) WR16(dst + 2 * x, psp_ge::swap_rb565(src[static_cast<uint32_t>(x * scale)]));
    }
}

// Applies a pending colour clear to the guest back buffer (CPU fill).
void clear_guest() {
    if (!(pending_flags & 1)) return;
    uint32_t a = pending_argb;
    uint16_t c = static_cast<uint16_t>(((a >> 8) & 0xF800) | ((a >> 5) & 0x07E0) | ((a >> 3) & 0x001F));
    for (uint32_t y = 0; y < g_h; y++) {
        uint32_t row = target->mem + y * target->pitch;
        for (uint32_t x = 0; x < g_w; x++) WR16(row + 2 * x, c);
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
    Texture &t = textures[s];
    uint32_t pal_version = s->palette ? s->palette->version : 0;
    if (t.data && t.version == s->version && t.pal_version == pal_version) return &t;
    st.tex_converts++;
    struct TexTimer { uint64_t t0 = SDL_GetTicksNS(); ~TexTimer() { st.ns_tex += SDL_GetTicksNS() - t0; } } timer;
    t.version = s->version;
    t.pal_version = pal_version;
    t.tw = 1;
    while (static_cast<uint32_t>(t.tw) < s->w) t.tw <<= 1;
    t.th = 1;
    while (static_cast<uint32_t>(t.th) < s->h) t.th <<= 1;
    t.tbw = static_cast<int>((s->w + 7) & ~7u);
    const bool keyed = s->ckey_src;
    if (s->bpp == 8) {
        t.psm = GU_PSM_8888;
        t.alpha = keyed;
        free(t.data);
        t.data = memalign(16, static_cast<size_t>(t.tbw) * s->h * 4);
        uint32_t pal[256];
        for (int i = 0; i < 256; i++) {
            uint32_t e = s->palette ? s->palette->entries[i] : static_cast<uint32_t>(i * 0x010101);
            pal[i] = (e & 0xFFFFFF) | 0xFF000000u;   // PALETTEENTRY bytes R, G, B = the GE's 8888 order
        }
        if (keyed)
            for (uint32_t i = s->ckey_lo; i <= s->ckey_hi && i < 256; i++) pal[i] &= 0x00FFFFFF;
        auto *out = static_cast<uint32_t *>(t.data);
        for (uint32_t y = 0; y < s->h; y++)
            for (uint32_t x = 0; x < s->w; x++) out[y * t.tbw + x] = pal[RD8(s->mem + y * s->pitch + x)];
        return &t;
    }
    free(t.data);
    t.data = memalign(16, static_cast<size_t>(t.tbw) * s->h * 2);
    auto *out = static_cast<uint16_t *>(t.data);
    const bool is1555 = s->gmask == 0x03E0, is4444 = s->gmask == 0x00F0;
    t.psm = is4444 ? GU_PSM_4444 : (is1555 || keyed) ? GU_PSM_5551 : GU_PSM_5650;
    t.alpha = is4444 || keyed || (is1555 && s->amask);
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
    if (rs[RS_ZENABLE]) {
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthFunc(depth_func(rs[23]));
    } else {
        sceGuDisable(GU_DEPTH_TEST);
    }
    sceGuDepthMask(rs[RS_ZWRITEENABLE] ? GU_FALSE : GU_TRUE);
    if (rs[RS_ALPHABLENDENABLE]) {
        unsigned sfix = 0, dfix = 0;
        int s = blend_factor(rs[RS_SRCBLEND], true, &sfix), d = blend_factor(rs[RS_DESTBLEND], false, &dfix);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, s, d, sfix, dfix);
    } else {
        sceGuDisable(GU_BLEND);
    }
    bool keyed = rs[RS_COLORKEYENABLE] && tex && tex->alpha;
    if (rs[RS_ALPHATESTENABLE] || keyed) {
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, rs[RS_ALPHATESTENABLE] ? static_cast<int>(rs[RS_ALPHAREF] & 255) : 0, 0xFF);
    } else {
        sceGuDisable(GU_ALPHA_TEST);
    }
    sceGuShadeModel(rs[RS_SHADEMODE] == 1 ? GU_FLAT : GU_SMOOTH);
    if (!tex) {
        sceGuDisable(GU_TEXTURE_2D);
        return;
    }
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(tex->psm, 0, 0, 0);
    sceGuTexImage(0, tex->tw, tex->th, tex->tbw, tex->data);
    switch (rs[RS_TEXTUREMAPBLEND]) {
    case 1: case 7: sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA); break;   // DECAL / COPY
    case 3: sceGuTexFunc(GU_TFX_DECAL, GU_TCC_RGBA); break;             // DECALALPHA
    case 4: sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA); break;          // MODULATEALPHA
    case 8: sceGuTexFunc(GU_TFX_ADD, GU_TCC_RGBA); break;               // ADD
    default: sceGuTexFunc(GU_TFX_MODULATE, tex->alpha ? GU_TCC_RGBA : GU_TCC_RGB); break;   // MODULATE
    }
    int filt = rs[RS_TEXTUREMAG] >= 2 ? GU_LINEAR : GU_NEAREST;
    sceGuTexFilter(rs[RS_TEXTUREMIN] >= 2 ? GU_LINEAR : GU_NEAREST, filt);
    int wrap = rs[RS_TEXTUREADDRESS] == 3 ? GU_CLAMP : GU_REPEAT;
    sceGuTexWrap(wrap, wrap);
    sceGuTexFlush();
}

// ---------- perspective-correct texturing by subdivision ----------
// The GE's through mode interpolates texture coordinates linearly on screen (affine); Direct3D
// interpolates them perspective-correctly. Textured triangles that are large on screen and span a
// wide depth range are split (up to 3 levels, 64 pieces) at edge midpoints whose uv come from
// perspective-correct interpolation (uv/w averaged and divided by the averaged 1/w). Position,
// depth and colour stay linear on screen, which is how Direct3D rasterises them too, so the depth
// buffer is unchanged. MOTORACER_GE_PERSPECTIVE=0 turns it off.
std::vector<FrameVertex> tri_out;

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

uint32_t add_vertices(const TLVertex *verts, uint32_t n) {
    uint32_t base = static_cast<uint32_t>(frame_vertices.size());
    for (uint32_t i = 0; i < n; i++) {
        const TLVertex &v = verts[i];
        float z = 65535.f * (1.f - v.sz);
        z = z < 0.f ? 0.f : z > 65535.f ? 65535.f : z;
        float rhw = v.rhw > 1e-12f ? v.rhw : 1e-12f;
        frame_vertices.push_back({{v.tu, v.tv, argb_to_abgr(v.color), v.sx * scale + off_x, v.sy * scale + off_y, z}, rhw});
    }
    return base;
}

void draw(const uint32_t *rs, Surface *texture, uint32_t base, uint32_t nverts, const uint16_t *indices,
          uint32_t ntri) {
    if (!usable() || !ntri) return;
    st.draws++;
    st.tris += ntri;
    struct DrawTimer { uint64_t t0 = SDL_GetTicksNS(); ~DrawTimer() { st.ns_draw += SDL_GetTicksNS() - t0; } } timer;
    prepare();
    Texture *tex = texture_for(texture);
    apply_states(rs, tex);
    const float su = tex ? static_cast<float>(texture->w) : 1.f, sv = tex ? static_cast<float>(texture->h) : 1.f;
    tri_out.clear();
    // Direct3D culling (D3DCULL_CW 2 / CCW 3), done here as in the desktop backend: the game relies
    // on it, e.g. the gauge cluster is drawn twice, once mirrored (back-facing).
    const uint32_t cull = rs[RS_CULLMODE];
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
    static const int dump_frame = getenv("MOTORACER_GE_DUMP") ? atoi(getenv("MOTORACER_GE_DUMP")) : -1;
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
            if (tex->psm != GU_PSM_8888)
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
    // Direct3D MODULATE takes alpha from the texture alone when it has an alpha channel; the GE's
    // modulate multiplies it with the vertex alpha, and the game's HUD vertices carry alpha 0.
    const uint32_t blend = rs[RS_TEXTUREMAPBLEND];
    const uint32_t force_alpha = (tex && tex->alpha && blend != 3 && blend != 4) ? 0xFF000000u : 0;
    if (tri_out.empty()) return;
    auto *out = static_cast<GeVertex *>(psp_ge::list_memory(tri_out.size() * sizeof(GeVertex)));
    for (size_t i = 0; i < tri_out.size(); i++) {
        GeVertex g = tri_out[i].g;
        g.u *= su;
        g.v *= sv;
        g.color |= force_alpha;
        out[i] = g;
    }
    st.subdivided += static_cast<uint32_t>(tri_out.size() / 3 - ntri);
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
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

void sync_to_cpu(Surface *s) {
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
    if (it != textures.end()) {
        free(it->second.data);
        textures.erase(it);
    }
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
    return true;
}

bool present_blt(Surface *src, Surface *, int32_t, int32_t, uint32_t, uint32_t) {
    bool r = present_flip(src);
    if (r) { st.flips_gpu--; st.blts_gpu++; } else { st.flips_cpu--; st.blts_cpu++; }
    return r;
}

}  // namespace dx::gpu
