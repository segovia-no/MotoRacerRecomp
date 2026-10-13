#include "d3d_gpu.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "ddraw_internal.h"
#include "host_gpu.h"
#include "host_video.h"
#include "settings.h"

using namespace rt;

namespace dx::gpu {

namespace {

// ---------- render-state numbers (d3dtypes.h, DX5) ----------
enum : uint32_t {
    RS_TEXTUREHANDLE = 1, RS_TEXTUREADDRESS = 3, RS_WRAPU = 5, RS_WRAPV = 6, RS_ZENABLE = 7, RS_SHADEMODE = 9,
    RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15, RS_TEXTUREMAG = 17, RS_TEXTUREMIN = 18, RS_SRCBLEND = 19,
    RS_DESTBLEND = 20, RS_TEXTUREMAPBLEND = 21, RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAREF = 24,
    RS_ALPHAFUNC = 25, RS_ALPHABLENDENABLE = 27, RS_FOGENABLE = 28, RS_SPECULARENABLE = 29, RS_FOGCOLOR = 34,
    RS_COLORKEYENABLE = 41,
};

const char *kShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VIn {
    float4 pos   [[attribute(0)]];
    float4 color [[attribute(1)]];   // D3DCOLOR bytes B,G,R,A
    float4 spec  [[attribute(2)]];
    float2 uv    [[attribute(3)]];
};
struct VOut {
    float4 pos [[position]];
    float4 color;
    float4 spec;
    float2 uv;
};

struct VertUniforms { float4 offset; };   // xy: clip-space shift per unit w

vertex VOut vs_main(VIn in [[stage_in]], constant VertUniforms &vu [[buffer(0)]]) {
    VOut o;
    o.pos = in.pos;
    o.pos.xy += vu.offset.xy * in.pos.w;
    o.color = in.color.zyxw;
    o.spec = in.spec.zyxw;
    o.uv = in.uv;
    return o;
}

struct FragUniforms {
    float4 fog_color;
    float4 mode;     // x: textured, y: TEXTUREMAPBLEND, z: fog, w: specular
    float4 alpha;    // x: alpha test on, y: ref (0..1), z: colour key on, w: texture format has alpha
    float4 snap;     // x: sample at the original 640x480 pixel centre, y: render scale
};

fragment float4 fs_main(VOut in [[stage_in]], constant FragUniforms &u [[buffer(0)]],
                        texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    float4 c = in.color;
    if (u.mode.x > 0.5) {
        float2 uv = in.uv;
        if (u.snap.x > 0.5) {
            // Flat 2D element (HUD/text): use the texture coordinate the original 640x480 frame
            // sampled for this pixel, so glyph atlases don't bleed at high resolution.
            // Unshifted geometry: D3D's sample point for original pixel i is edge coordinate i.
            float2 gpos = in.pos.xy / u.snap.y;
            float2 d = (floor(gpos) - gpos) * u.snap.y;
            uv += dfdx(in.uv) * d.x + dfdy(in.uv) * d.y;
        }
        float4 t = tex.sample(smp, uv);
        if (u.alpha.z > 0.5 && t.a < 0.5) discard_fragment();
        int blend = int(u.mode.y + 0.5);
        if (blend == 1 || blend == 7)       c = t;                                  // DECAL / COPY
        else if (blend == 3)                c = float4(mix(c.rgb, t.rgb, t.a), c.a); // DECALALPHA
        else if (blend == 4)                c = c * t;                              // MODULATEALPHA
        else if (blend == 8)                c = float4(c.rgb + t.rgb, c.a);         // ADD
        else                                c = float4(c.rgb * t.rgb, u.alpha.w > 0.5 ? t.a : c.a); // MODULATE
    }
    if (u.mode.w > 0.5) c.rgb = saturate(c.rgb + in.spec.rgb);
    if (u.mode.z > 0.5) c.rgb = mix(u.fog_color.rgb, c.rgb, in.spec.a);
    if (u.alpha.x > 0.5 && c.a <= u.alpha.y) discard_fragment();
    return c;
}

// Full-screen overlay: pixels the CPU changed in the 640x480 back buffer, drawn over the
// high-resolution render target (alpha = changed mask).
struct OOut { float4 pos [[position]]; float2 uv; };
vertex OOut ov_vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    OOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}
fragment float4 ov_fs(OOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    float4 t = tex.sample(smp, in.uv);
    if (t.a < 0.5) discard_fragment();
    return float4(t.rgb, 1.0);
}
)MSL";

struct GpuVertex {
    float x, y, z, w;
    uint32_t color, specular;
    float u, v;
};

struct FragUniforms {
    float fog_color[4];
    float mode[4];
    float alpha[4];
    float snap[4];
};

struct PipelineKey {
    bool ztest, zwrite, blend;
    uint32_t zfunc, src, dst, cull;
    bool operator<(const PipelineKey &o) const {
        return std::tie(ztest, zwrite, blend, zfunc, src, dst, cull) <
               std::tie(o.ztest, o.zwrite, o.blend, o.zfunc, o.src, o.dst, o.cull);
    }
};

struct Cmd {
    enum Kind { Draw, Clear } kind;
    // draw
    PipelineKey pipe{};
    Surface *texture = nullptr;
    uint32_t sampler_key = 0;
    FragUniforms uniforms{};
    float voffset[4] = {};
    uint32_t first_index = 0, index_count = 0;
    float vp[4] = {};
    // clear
    uint32_t clear_flags = 0, clear_argb = 0;
    float clear_depth = 1.f;
};

struct CachedTexture {
    SDL_GPUTexture *tex = nullptr;
    uint32_t w = 0, h = 0, version = ~0u, pal_version = ~0u;
};

SDL_GPUDevice *dev = nullptr;
SDL_GPUShader *vs = nullptr, *fs = nullptr, *ovs = nullptr, *ofs = nullptr;
SDL_GPUGraphicsPipeline *overlay_pipe = nullptr;
SDL_GPUTexture *rt = nullptr, *depth = nullptr, *white = nullptr;
SDL_GPUTexture *lowres = nullptr;    // guest-size staging: full uploads and downscaled readback
SDL_GPUTexture *overlay = nullptr;   // guest-size changed-pixel layer
uint32_t rt_w = 0, rt_h = 0;         // render target = guest size * scale
uint32_t g_w = 0, g_h = 0;           // guest back-buffer size
Surface *target = nullptr;
bool depth_initialised = false;
// Synchronisation state between the guest back buffer and the high-resolution target:
bool rt_valid = false;               // RT holds this back buffer's picture (plus queued work)
bool cpu_dirty = false;              // guest pixels changed since `baseline` was taken
bool gpu_frame_used = false;         // 3D was drawn since the last Flip
bool guest_stale = false;            // RT was presented without reading it back into guest memory
// MOTORACER_D3D_DEBUG=1: classification statistics for flat (screen-aligned) triangles.
bool dbg = getenv("MOTORACER_D3D_DEBUG") != nullptr;
std::map<std::string, uint64_t> dbg_tri;
uint64_t dbg_overlay_px = 0, dbg_overlay_flushes = 0, dbg_full_uploads = 0, dbg_last = 0;
// Vertical extent of non-flat (3D) geometry in the current frame, in guest pixels.
float frame_min_y = 1e9f, frame_max_y = -1e9f;
std::map<std::pair<int, int>, uint64_t> dbg_extents;   // (min/10, max/10) -> frames
std::vector<uint16_t> baseline;      // guest pixels as of the last upload/readback
std::map<PipelineKey, SDL_GPUGraphicsPipeline *> pipelines;
std::map<uint32_t, SDL_GPUSampler *> samplers;
std::map<Surface *, CachedTexture> textures;
std::vector<GpuVertex> frame_vertices;
std::vector<uint32_t> frame_indices;
std::vector<Cmd> frame_cmds;
bool failed = false;

SDL_GPUCompareOp compare_op(uint32_t f) {
    switch (f) {
    case 1: return SDL_GPU_COMPAREOP_NEVER;
    case 2: return SDL_GPU_COMPAREOP_LESS;
    case 3: return SDL_GPU_COMPAREOP_EQUAL;
    case 5: return SDL_GPU_COMPAREOP_GREATER;
    case 6: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case 7: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    case 8: return SDL_GPU_COMPAREOP_ALWAYS;
    default: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    }
}

SDL_GPUBlendFactor blend_factor(uint32_t b) {
    switch (b) {
    case 1: return SDL_GPU_BLENDFACTOR_ZERO;
    case 2: return SDL_GPU_BLENDFACTOR_ONE;
    case 3: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case 4: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 5: case 12: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 6: case 13: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case 8: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case 10: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case 11: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    default: return SDL_GPU_BLENDFACTOR_ONE;
    }
}

SDL_GPUGraphicsPipeline *pipeline(const PipelineKey &k) {
    auto it = pipelines.find(k);
    if (it != pipelines.end()) return it->second;

    SDL_GPUVertexBufferDescription vb{};
    vb.slot = 0;
    vb.pitch = sizeof(GpuVertex);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[4] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 20},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24},
    };
    SDL_GPUColorTargetDescription color{};
    color.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    if (k.blend) {
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = blend_factor(k.src);
        color.blend_state.dst_color_blendfactor = blend_factor(k.dst);
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = blend_factor(k.src);
        color.blend_state.dst_alpha_blendfactor = blend_factor(k.dst);
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }

    SDL_GPUGraphicsPipelineCreateInfo ci{};
    ci.vertex_shader = vs;
    ci.fragment_shader = fs;
    ci.vertex_input_state.vertex_buffer_descriptions = &vb;
    ci.vertex_input_state.num_vertex_buffers = 1;
    ci.vertex_input_state.vertex_attributes = attrs;
    ci.vertex_input_state.num_vertex_attributes = 4;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    // D3D culls in screen space (y down); our NDC is y up, which mirrors the winding.
    ci.rasterizer_state.cull_mode = k.cull == 2 ? SDL_GPU_CULLMODE_FRONT : k.cull == 3 ? SDL_GPU_CULLMODE_BACK
                                                                                      : SDL_GPU_CULLMODE_NONE;
    ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    ci.rasterizer_state.enable_depth_clip = false;
    ci.depth_stencil_state.enable_depth_test = k.ztest;
    ci.depth_stencil_state.enable_depth_write = k.ztest && k.zwrite;
    ci.depth_stencil_state.compare_op = compare_op(k.zfunc);
    ci.target_info.color_target_descriptions = &color;
    ci.target_info.num_color_targets = 1;
    ci.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    ci.target_info.has_depth_stencil_target = true;
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &ci);
    if (!p) fatal("SDL_CreateGPUGraphicsPipeline: %s", SDL_GetError());
    pipelines[k] = p;
    return p;
}

SDL_GPUSampler *sampler(uint32_t key) {
    auto it = samplers.find(key);
    if (it != samplers.end()) return it->second;
    bool mag_lin = key & 1, min_lin = key & 2, clamp = key & 4;
    SDL_GPUSamplerCreateInfo ci{};
    ci.mag_filter = mag_lin ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    ci.min_filter = min_lin ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    ci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    SDL_GPUSamplerAddressMode am = clamp ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    ci.address_mode_u = ci.address_mode_v = ci.address_mode_w = am;
    SDL_GPUSampler *s = SDL_CreateGPUSampler(dev, &ci);
    samplers[key] = s;
    return s;
}

SDL_GPUTexture *make_texture(uint32_t w, uint32_t h, SDL_GPUTextureFormat fmt, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo ci{};
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = fmt;
    ci.usage = usage;
    ci.width = w;
    ci.height = h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    SDL_GPUTexture *t = SDL_CreateGPUTexture(dev, &ci);
    if (!t) fatal("SDL_CreateGPUTexture(%ux%u): %s", w, h, SDL_GetError());
    return t;
}

// Upload RGBA8 pixels into a texture through a temporary transfer buffer.
void upload_rgba(SDL_GPUCopyPass *copy, SDL_GPUTexture *t, const uint32_t *px, uint32_t w, uint32_t h,
                 std::vector<SDL_GPUTransferBuffer *> &garbage) {
    SDL_GPUTransferBufferCreateInfo tci{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, w * h * 4, 0};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tci);
    memcpy(SDL_MapGPUTransferBuffer(dev, tb, false), px, static_cast<size_t>(w) * h * 4);
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_GPUTextureTransferInfo src{tb, 0, w, h};
    SDL_GPUTextureRegion dst{};
    dst.texture = t;
    dst.w = w;
    dst.h = h;
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    garbage.push_back(tb);
}

// ---------- guest surface -> RGBA8 ----------
uint32_t expand16(uint16_t p, const Surface &s, bool keyed, bool *transparent) {
    uint32_t r, g, b, a = 255;
    if (s.gmask == 0x03E0) {          // 1555
        r = (p >> 10) & 31; g = (p >> 5) & 31; b = p & 31;
        r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2;
        if (s.amask) a = (p & 0x8000) ? 255 : 0;
    } else if (s.gmask == 0x00F0) {   // 4444
        r = ((p >> 8) & 15) * 17; g = ((p >> 4) & 15) * 17; b = (p & 15) * 17; a = ((p >> 12) & 15) * 17;
    } else {                          // 565
        r = (p >> 11) & 31; g = (p >> 5) & 63; b = p & 31;
        r = r << 3 | r >> 2; g = g << 2 | g >> 4; b = b << 3 | b >> 2;
    }
    if (keyed && p >= s.ckey_lo && p <= s.ckey_hi) { a = 0; *transparent = true; }
    return r | g << 8 | b << 16 | a << 24;
}

void surface_to_rgba(const Surface &s, std::vector<uint32_t> &out) {
    out.resize(static_cast<size_t>(s.w) * s.h);
    bool t = false;
    if (s.bpp == 8) {
        uint32_t pal[256];
        for (int i = 0; i < 256; i++) {
            uint32_t e = s.palette ? s.palette->entries[i] : static_cast<uint32_t>(i * 0x010101);
            pal[i] = (e & 0xFFFFFF) | 0xFF000000u;   // PALETTEENTRY R,G,B,flags -> RGBA bytes
        }
        if (s.ckey_src)
            for (uint32_t i = s.ckey_lo; i <= s.ckey_hi && i < 256; i++) pal[i] &= 0x00FFFFFF;
        for (uint32_t y = 0; y < s.h; y++)
            for (uint32_t x = 0; x < s.w; x++) out[y * s.w + x] = pal[RD8(s.mem + y * s.pitch + x)];
    } else {
        for (uint32_t y = 0; y < s.h; y++)
            for (uint32_t x = 0; x < s.w; x++)
                out[y * s.w + x] = expand16(RD16(s.mem + y * s.pitch + 2 * x), s, s.ckey_src, &t);
    }
}

CachedTexture *texture_for(Surface *s, SDL_GPUCopyPass *copy, std::vector<SDL_GPUTransferBuffer *> &garbage,
                           std::vector<uint32_t> &scratch) {
    if (!s->w || !s->h || s->w > 8192 || s->h > 8192 || !s->mem || (s->bpp != 8 && s->bpp != 16)) {
        static int warned = 0;
        if (warned++ < 8)
            log("d3d gpu: texture surface %ux%u %ubpp caps %08x not usable (drawn untextured)\n", s->w, s->h, s->bpp,
                s->caps);
        return nullptr;
    }
    CachedTexture &c = textures[s];
    uint32_t pal_version = s->palette ? s->palette->version : 0;
    if (c.tex && c.w == s->w && c.h == s->h && c.version == s->version && c.pal_version == pal_version) return &c;
    if (c.tex && (c.w != s->w || c.h != s->h)) { SDL_ReleaseGPUTexture(dev, c.tex); c.tex = nullptr; }
    if (!c.tex) {
        c.tex = make_texture(s->w, s->h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        c.w = s->w;
        c.h = s->h;
    }
    surface_to_rgba(*s, scratch);
    upload_rgba(copy, c.tex, scratch.data(), s->w, s->h, garbage);
    c.version = s->version;
    c.pal_version = pal_version;
    return &c;
}

uint32_t rgb565_to_rgba(uint16_t p) {
    uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    return (r << 3 | r >> 2) | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2) << 16 | 0xFF000000u;
}

std::vector<uint16_t> read_guest() {
    std::vector<uint16_t> g(static_cast<size_t>(g_w) * g_h);
    for (uint32_t y = 0; y < g_h; y++) memcpy(&g[y * g_w], M + target->mem + y * target->pitch, g_w * 2);
    return g;
}

void blit(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, uint32_t sw, uint32_t sh, SDL_GPUTexture *dst, uint32_t dw,
          uint32_t dh, SDL_GPUFilter filter) {
    SDL_GPUBlitInfo bi{};
    bi.source.texture = src;
    bi.source.w = sw;
    bi.source.h = sh;
    bi.destination.texture = dst;
    bi.destination.w = dw;
    bi.destination.h = dh;
    bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
    bi.filter = filter;
    SDL_BlitGPUTexture(cmd, &bi);
}

enum class After { Nothing, Download, Present };
host::gpu::Crop crop_now;   // what the current GPU frame presents (widescreen detection)

// Every 10th GPU frame: read the top/bottom 60-row bands (downscaled, nearest) and check
// whether they are pure black — the game's 16/9 letterbox.
void check_widescreen() {
    static uint32_t n = 0;
    if (n++ % 10) return;
    uint32_t band = (g_h - g_w * 9 / 16) / 2;
    if (!band || !settings::get().widescreen) { crop_now = host::gpu::widescreen_vote(false, g_w, g_h); return; }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    blit(cmd, rt, rt_w, rt_h, lowres, g_w, g_h, SDL_GPU_FILTER_NEAREST);
    uint32_t bytes = g_w * band * 4;
    SDL_GPUTransferBufferCreateInfo tci{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes * 3, 0};
    SDL_GPUTransferBuffer *down = SDL_CreateGPUTransferBuffer(dev, &tci);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    uint32_t ys[3] = {0, g_h - band, g_h / 2};
    for (int k = 0; k < 3; k++) {
        SDL_GPUTextureRegion src{};
        src.texture = lowres;
        src.y = ys[k];
        src.w = g_w;
        src.h = k == 2 ? 1 : band;
        src.d = 1;
        SDL_GPUTextureTransferInfo dst{down, bytes * static_cast<uint32_t>(k), g_w, src.h};
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
    }
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(dev, true, &fence, 1);
    SDL_ReleaseGPUFence(dev, fence);
    auto *px = static_cast<const uint32_t *>(SDL_MapGPUTransferBuffer(dev, down, false));
    bool black = true;
    for (uint32_t i = 0; i < 2 * g_w * band && black; i++) black = !(px[i] & 0xFFFFFF);
    bool picture = false;
    for (uint32_t x = 0; x < g_w && !picture; x++) picture = px[2 * g_w * band + x] & 0xFFFFFF;
    SDL_UnmapGPUTransferBuffer(dev, down);
    SDL_ReleaseGPUTransferBuffer(dev, down);
    crop_now = host::gpu::widescreen_vote(black && picture, g_w, g_h);
}

// Debug: MOTORACER_SNAPSHOT_DIR also captures GPU frames (full render-target resolution).
void snapshot_rt() {
    static const char *dir = getenv("MOTORACER_SNAPSHOT_DIR");
    static uint64_t n = 0;
    if (!dir || (n++ % 120)) return;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUTransferBufferCreateInfo tci{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, rt_w * rt_h * 4, 0};
    SDL_GPUTransferBuffer *down = SDL_CreateGPUTransferBuffer(dev, &tci);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = rt;
    src.w = rt_w;
    src.h = rt_h;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst{down, 0, rt_w, rt_h};
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(dev, true, &fence, 1);
    SDL_ReleaseGPUFence(dev, fence);
    void *px = SDL_MapGPUTransferBuffer(dev, down, false);
    if (SDL_Surface *surf = SDL_CreateSurfaceFrom(rt_w, rt_h, SDL_PIXELFORMAT_ABGR8888, px, rt_w * 4)) {
        char path[1024];
        snprintf(path, sizeof path, "%s/gpu_%06llu.bmp", dir, static_cast<unsigned long long>(n));
        SDL_SaveBMP(surf, path);
        SDL_DestroySurface(surf);
    }
    SDL_UnmapGPUTransferBuffer(dev, down);
    SDL_ReleaseGPUTransferBuffer(dev, down);
}

// Render all queued work. Before it: bring CPU changes of the back buffer into the target
// (full upload if the target is stale, otherwise only the changed pixels as an overlay).
// After it: optionally read the picture back (downscaled) into guest memory, or present it.
void flush(After after) {
    if (failed || !target || !rt) return;
    bool full_clear_first = !frame_cmds.empty() && frame_cmds.front().kind == Cmd::Clear &&
                            (frame_cmds.front().clear_flags & 1);
    bool need_full = !rt_valid && !full_clear_first;
    bool need_overlay = rt_valid && cpu_dirty && !full_clear_first;
    if (frame_cmds.empty() && !need_full && !need_overlay && after == After::Nothing) return;

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    std::vector<SDL_GPUTransferBuffer *> garbage;
    std::vector<uint32_t> scratch;
    std::vector<uint16_t> guest;
    if (need_full || need_overlay) guest = read_guest();

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    bool have_overlay = false;
    if (need_full) {
        scratch.resize(guest.size());
        for (size_t i = 0; i < guest.size(); i++) scratch[i] = rgb565_to_rgba(guest[i]);
        upload_rgba(copy, lowres, scratch.data(), g_w, g_h, garbage);
    } else if (need_overlay) {
        scratch.assign(guest.size(), 0);
        for (size_t i = 0; i < guest.size(); i++)
            if (guest[i] != baseline[i]) { scratch[i] = rgb565_to_rgba(guest[i]); have_overlay = true; }
        if (have_overlay) upload_rgba(copy, overlay, scratch.data(), g_w, g_h, garbage);
        if (dbg && have_overlay) {
            dbg_overlay_flushes++;
            for (size_t i = 0; i < guest.size(); i++) dbg_overlay_px += guest[i] != baseline[i];
        }
    }
    std::map<Surface *, SDL_GPUTexture *> bound;
    for (auto &c : frame_cmds)
        if (c.kind == Cmd::Draw && c.texture && !bound.count(c.texture)) {
            CachedTexture *t = texture_for(c.texture, copy, garbage, scratch);
            bound[c.texture] = t ? t->tex : white;
        }
    SDL_GPUBuffer *vbuf = nullptr, *ibuf = nullptr;
    if (!frame_vertices.empty()) {
        uint32_t vbytes = static_cast<uint32_t>(frame_vertices.size() * sizeof(GpuVertex));
        uint32_t ibytes = static_cast<uint32_t>(frame_indices.size() * 4);
        SDL_GPUBufferCreateInfo vci{SDL_GPU_BUFFERUSAGE_VERTEX, vbytes, 0};
        SDL_GPUBufferCreateInfo ici{SDL_GPU_BUFFERUSAGE_INDEX, ibytes ? ibytes : 4, 0};
        vbuf = SDL_CreateGPUBuffer(dev, &vci);
        ibuf = SDL_CreateGPUBuffer(dev, &ici);
        SDL_GPUTransferBufferCreateInfo tci{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, vbytes + ibytes, 0};
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tci);
        auto *m = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(dev, tb, false));
        memcpy(m, frame_vertices.data(), vbytes);
        if (ibytes) memcpy(m + vbytes, frame_indices.data(), ibytes);
        SDL_UnmapGPUTransferBuffer(dev, tb);
        SDL_GPUTransferBufferLocation vsrc{tb, 0}, isrc{tb, vbytes};
        SDL_GPUBufferRegion vdst{vbuf, 0, vbytes}, idst{ibuf, 0, ibytes};
        SDL_UploadToGPUBuffer(copy, &vsrc, &vdst, false);
        if (ibytes) SDL_UploadToGPUBuffer(copy, &isrc, &idst, false);
        garbage.push_back(tb);
    }
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFilter up = settings::get().nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    if (need_full) { blit(cmd, lowres, g_w, g_h, rt, rt_w, rt_h, up); dbg_full_uploads++; }
    if (have_overlay) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = rt;
        ct.load_op = SDL_GPU_LOADOP_LOAD;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, overlay_pipe);
        SDL_GPUTextureSamplerBinding tsb{overlay, sampler(4)};   // nearest, clamp
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    if (need_full || need_overlay) {
        baseline = std::move(guest);
        cpu_dirty = false;
    }
    if (need_full || full_clear_first) rt_valid = true;

    // 3D passes: a Clear command starts a new pass with clear load ops.
    size_t i = 0;
    while (i < frame_cmds.size()) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = rt;
        ct.load_op = SDL_GPU_LOADOP_LOAD;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo dt{};
        dt.texture = depth;
        dt.load_op = depth_initialised ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
        dt.clear_depth = 1.f;
        dt.store_op = SDL_GPU_STOREOP_STORE;
        dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        while (i < frame_cmds.size() && frame_cmds[i].kind == Cmd::Clear) {
            const Cmd &c = frame_cmds[i++];
            if (c.clear_flags & 1) {
                ct.load_op = SDL_GPU_LOADOP_CLEAR;
                ct.clear_color = {((c.clear_argb >> 16) & 255) / 255.f, ((c.clear_argb >> 8) & 255) / 255.f,
                                  (c.clear_argb & 255) / 255.f, 1.f};
            }
            if (c.clear_flags & 2) { dt.load_op = SDL_GPU_LOADOP_CLEAR; dt.clear_depth = c.clear_depth; }
        }
        depth_initialised = true;
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
        if (vbuf) {
            SDL_GPUBufferBinding vb{vbuf, 0}, ib{ibuf, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
            SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        }
        SDL_GPUViewport vp{0, 0, static_cast<float>(rt_w), static_cast<float>(rt_h), 0.f, 1.f};
        SDL_SetGPUViewport(pass, &vp);
        for (; i < frame_cmds.size() && frame_cmds[i].kind == Cmd::Draw; i++) {
            const Cmd &c = frame_cmds[i];
            SDL_BindGPUGraphicsPipeline(pass, pipeline(c.pipe));
            SDL_GPUTextureSamplerBinding tsb{c.texture ? bound[c.texture] : white, sampler(c.sampler_key)};
            SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &c.uniforms, sizeof c.uniforms);
            SDL_PushGPUVertexUniformData(cmd, 0, c.voffset, sizeof c.voffset);
            SDL_DrawGPUIndexedPrimitives(pass, c.index_count, 1, c.first_index, 0, 0);
        }
        SDL_EndGPURenderPass(pass);
    }

    if (after == After::Present) {
        host::gpu::set_game_size(g_w, g_h);
        host::gpu::present(cmd, rt, rt_w, rt_h, crop_now);   // submits
        host::count_frame();
        snapshot_rt();
    } else {
        SDL_GPUTransferBuffer *down = nullptr;
        if (after == After::Download) {
            blit(cmd, rt, rt_w, rt_h, lowres, g_w, g_h, SDL_GPU_FILTER_LINEAR);
            SDL_GPUTransferBufferCreateInfo tci{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, g_w * g_h * 4, 0};
            down = SDL_CreateGPUTransferBuffer(dev, &tci);
            SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureRegion src{};
            src.texture = lowres;
            src.w = g_w;
            src.h = g_h;
            src.d = 1;
            SDL_GPUTextureTransferInfo dst{down, 0, g_w, g_h};
            SDL_DownloadFromGPUTexture(cp, &src, &dst);
            SDL_EndGPUCopyPass(cp);
        }
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
        if (down) {
            auto *px = static_cast<const uint32_t *>(SDL_MapGPUTransferBuffer(dev, down, false));
            baseline.resize(static_cast<size_t>(g_w) * g_h);
            for (uint32_t y = 0; y < g_h; y++)
                for (uint32_t x = 0; x < g_w; x++) {
                    uint32_t p = px[y * g_w + x];
                    uint16_t r = (p & 255) >> 3, g = ((p >> 8) & 255) >> 2, b = ((p >> 16) & 255) >> 3;
                    uint16_t v = static_cast<uint16_t>(r << 11 | g << 5 | b);
                    WR16(target->mem + y * target->pitch + 2 * x, v);
                    baseline[y * g_w + x] = v;
                }
            SDL_UnmapGPUTransferBuffer(dev, down);
            SDL_ReleaseGPUTransferBuffer(dev, down);
            cpu_dirty = false;
        }
    }
    for (auto *tb : garbage) SDL_ReleaseGPUTransferBuffer(dev, tb);
    if (vbuf) SDL_ReleaseGPUBuffer(dev, vbuf);
    if (ibuf) SDL_ReleaseGPUBuffer(dev, ibuf);
    frame_vertices.clear();
    frame_indices.clear();
    frame_cmds.clear();
}

}  // namespace

bool init() {
    if (dev) return true;
    if (failed) return false;
    dev = host::gpu::device();
    SDL_GPUShaderCreateInfo sci{};
    sci.code = reinterpret_cast<const Uint8 *>(kShaders);
    sci.code_size = strlen(kShaders);
    sci.format = SDL_GPU_SHADERFORMAT_MSL;
    sci.entrypoint = "vs_main";
    sci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    sci.num_uniform_buffers = 1;
    vs = SDL_CreateGPUShader(dev, &sci);
    sci.entrypoint = "fs_main";
    sci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    sci.num_samplers = 1;
    sci.num_uniform_buffers = 1;
    fs = SDL_CreateGPUShader(dev, &sci);
    sci.entrypoint = "ov_fs";
    ofs = SDL_CreateGPUShader(dev, &sci);
    sci.entrypoint = "ov_vs";
    sci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    sci.num_samplers = 0;
    sci.num_uniform_buffers = 0;
    ovs = SDL_CreateGPUShader(dev, &sci);
    if (!vs || !fs || !ovs || !ofs) fatal("d3d gpu: shader compile failed: %s", SDL_GetError());
    {
        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo ci{};
        ci.vertex_shader = ovs;
        ci.fragment_shader = ofs;
        ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        ci.target_info.color_target_descriptions = &color;
        ci.target_info.num_color_targets = 1;
        overlay_pipe = SDL_CreateGPUGraphicsPipeline(dev, &ci);
        if (!overlay_pipe) fatal("d3d gpu: overlay pipeline: %s", SDL_GetError());
    }
    white = make_texture(1, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    std::vector<SDL_GPUTransferBuffer *> garbage;
    uint32_t px = 0xFFFFFFFF;
    upload_rgba(copy, white, &px, 1, 1, garbage);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(dev);
    for (auto *tb : garbage) SDL_ReleaseGPUTransferBuffer(dev, tb);
    log("d3d gpu: %s device ready\n", SDL_GetGPUDeviceDriver(dev));
    return true;
}

void set_target(Surface *s) {
    if (!init()) return;
    if (target == s && rt && g_w == s->w && g_h == s->h) return;
    if (target && target != s && !frame_cmds.empty()) flush(After::Download);
    target = s;
    uint32_t scale = host::gpu::render_scale();
    if (rt && (g_w != s->w || g_h != s->h || rt_w != s->w * scale || rt_h != s->h * scale)) {
        SDL_ReleaseGPUTexture(dev, rt);
        SDL_ReleaseGPUTexture(dev, depth);
        SDL_ReleaseGPUTexture(dev, lowres);
        SDL_ReleaseGPUTexture(dev, overlay);
        rt = depth = lowres = overlay = nullptr;
    }
    if (!rt) {
        g_w = s->w;
        g_h = s->h;
        rt_w = g_w * scale;
        rt_h = g_h * scale;
        rt = make_texture(rt_w, rt_h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                          SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        depth = make_texture(rt_w, rt_h, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
        lowres = make_texture(g_w, g_h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                              SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        overlay = make_texture(g_w, g_h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        depth_initialised = false;
        log("d3d gpu: render target %ux%u (scale %u)\n", rt_w, rt_h, scale);
    }
    rt_valid = false;
    cpu_dirty = false;
}

uint32_t add_vertices(const TLVertex *verts, uint32_t n) {
    uint32_t base = static_cast<uint32_t>(frame_vertices.size());
    if (!dev || !rt) return base;
    // Vertex x/y are in guest pixels (640x480); NDC is resolution independent, so the same
    // positions land correctly on the larger render target.
    float sx = 2.f / static_cast<float>(g_w), sy = 2.f / static_cast<float>(g_h);
    for (uint32_t i = 0; i < n; i++) {
        const TLVertex &v = verts[i];
        // Clip-space position with w = 1/rhw so the GPU interpolates perspective-correctly.
        float w = v.rhw > 1e-8f ? 1.f / v.rhw : 1.f;
        float nx = v.sx * sx - 1.f, ny = 1.f - v.sy * sy;
        frame_vertices.push_back({nx * w, ny * w, v.sz * w, w, v.color, v.specular, v.tu, v.tv});
    }
    return base;
}

void draw(const uint32_t *rs, Surface *texture, uint32_t base, uint32_t nverts, const uint16_t *indices,
          uint32_t ntri) {
    if (!dev || !rt || !ntri) return;
    gpu_frame_used = true;
    Cmd c{};
    c.kind = Cmd::Draw;
    c.pipe.ztest = rs[RS_ZENABLE] != 0;
    c.pipe.zwrite = rs[RS_ZWRITEENABLE] != 0;
    c.pipe.zfunc = rs[RS_ZFUNC];
    c.pipe.blend = rs[RS_ALPHABLENDENABLE] != 0;
    c.pipe.src = rs[RS_SRCBLEND];
    c.pipe.dst = rs[RS_DESTBLEND];
    c.pipe.cull = rs[RS_CULLMODE];
    c.texture = texture;
    c.sampler_key = (rs[RS_TEXTUREMAG] >= 2 ? 1u : 0u) | (rs[RS_TEXTUREMIN] >= 2 ? 2u : 0u) |
                    (rs[RS_TEXTUREADDRESS] == 3 ? 4u : 0u);
    uint32_t fog = rs[RS_FOGCOLOR];
    c.uniforms.fog_color[0] = ((fog >> 16) & 255) / 255.f;
    c.uniforms.fog_color[1] = ((fog >> 8) & 255) / 255.f;
    c.uniforms.fog_color[2] = (fog & 255) / 255.f;
    c.uniforms.mode[0] = texture ? 1.f : 0.f;
    c.uniforms.mode[1] = static_cast<float>(rs[RS_TEXTUREMAPBLEND]);
    c.uniforms.mode[2] = rs[RS_FOGENABLE] ? 1.f : 0.f;
    c.uniforms.mode[3] = rs[RS_SPECULARENABLE] ? 1.f : 0.f;
    c.uniforms.alpha[0] = rs[RS_ALPHATESTENABLE] ? 1.f : 0.f;
    c.uniforms.alpha[1] = (rs[RS_ALPHAREF] & 255) / 255.f;
    c.uniforms.alpha[2] = (rs[RS_COLORKEYENABLE] && texture && texture->ckey_src) ? 1.f : 0.f;
    // D3D MODULATE: alpha comes from the texture when its format has an alpha channel (even if
    // opaque), otherwise from the vertex. The game's vertices often carry alpha 0.
    c.uniforms.alpha[3] = (texture && (texture->amask || texture->gmask == 0x00F0 || texture->ckey_src)) ? 1.f : 0.f;
    c.first_index = static_cast<uint32_t>(frame_indices.size());
    bool flat = rt_w != g_w;   // only matters when rendering above the original resolution
    float w0 = 0, z0 = 0;
    for (uint32_t t = 0; t < ntri * 3; t++) {
        uint32_t idx = indices[t];
        uint32_t vi = base + (idx < nverts ? idx : 0);
        frame_indices.push_back(vi);
        if (flat) {
            const GpuVertex &v = frame_vertices[vi];
            float z = v.z / v.w;
            if (t == 0) { w0 = v.w; z0 = z; }
            else if (std::fabs(v.w - w0) > 1e-4f * std::fabs(w0) || std::fabs(z - z0) > 1e-5f) flat = false;
        }
    }
    c.uniforms.snap[0] = flat && texture ? 1.f : 0.f;
    if (!flat) {
        for (uint32_t t = 0; t < ntri * 3; t++) {
            const GpuVertex &v = frame_vertices[base + (indices[t] < nverts ? indices[t] : 0)];
            float y = (1.f - v.y / v.w) * 0.5f * static_cast<float>(g_h);
            frame_min_y = std::min(frame_min_y, y);
            frame_max_y = std::max(frame_max_y, y);
        }
    }
    if (dbg && texture) {
        for (uint32_t t = 0; t < ntri; t++) {
            const GpuVertex *v[3];
            for (int k = 0; k < 3; k++) v[k] = &frame_vertices[base + (indices[3 * t + k] < nverts ? indices[3 * t + k] : 0)];
            bool tf = std::fabs(v[0]->w - v[1]->w) < 1e-4f * v[0]->w && std::fabs(v[0]->w - v[2]->w) < 1e-4f * v[0]->w;
            float rhw = 1.f / v[0]->w, z = v[0]->z / v[0]->w;
            float x = (v[0]->x / v[0]->w + 1) * 0.5f * g_w, y = (1 - v[0]->y / v[0]->w) * 0.5f * g_h;
            char key[160];
            snprintf(key, sizeof key, "%s run_flat=%d rhw=%.3g z=%.3g y_band=%d blend=%d", tf ? "FLAT " : "slant",
                     flat ? 1 : 0, tf ? rhw : -1.f, tf ? z : -1.f, static_cast<int>(y) / 60, c.pipe.blend ? 1 : 0);
            dbg_tri[key]++;
            (void)x;
        }
        uint64_t now = SDL_GetTicks();
        if (now - dbg_last > 15000) {
            dbg_last = now;
            log("D3D debug: overlay flushes %llu, changed px %llu, full uploads %llu\n",
                (unsigned long long)dbg_overlay_flushes, (unsigned long long)dbg_overlay_px,
                (unsigned long long)dbg_full_uploads);
            std::vector<std::pair<uint64_t, std::string>> top;
            for (auto &[k, n] : dbg_tri) top.push_back({n, k});
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 40; i++) log("  %8llu %s\n", (unsigned long long)top[i].first, top[i].second.c_str());
            dbg_tri.clear();
        }
    }
    c.uniforms.snap[1] = static_cast<float>(rt_w) / static_cast<float>(g_w ? g_w : 1);
    // D3D samples at integer pixel coordinates, Metal at half-integers: shift by half an
    // original pixel. Flat 2D overlays stay on the original pixel grid instead, so they cover
    // exactly the pixels they covered at 640x480 (their sampling is snapped in the shader).
    if (!(flat && texture)) {
        c.voffset[0] = 1.f / static_cast<float>(g_w);
        c.voffset[1] = -1.f / static_cast<float>(g_h);
    }
    c.index_count = ntri * 3;
    if (!frame_cmds.empty()) {   // merge with the previous draw if nothing changed
        Cmd &p = frame_cmds.back();
        if (p.kind == Cmd::Draw && !(p.pipe < c.pipe) && !(c.pipe < p.pipe) && p.texture == c.texture &&
            p.sampler_key == c.sampler_key && !memcmp(&p.uniforms, &c.uniforms, sizeof c.uniforms) &&
            !memcmp(p.voffset, c.voffset, sizeof c.voffset) &&
            p.first_index + p.index_count == c.first_index) {
            p.index_count += c.index_count;
            return;
        }
    }
    frame_cmds.push_back(c);
}

void clear(uint32_t flags, uint32_t argb, float depth_value) {
    if (!dev || !rt) return;
    gpu_frame_used = true;
    Cmd c{};
    c.kind = Cmd::Clear;
    c.clear_flags = flags;
    c.clear_argb = argb;
    c.clear_depth = depth_value;
    frame_cmds.push_back(c);
}

void sync_to_cpu(Surface *s) {
    if (s && s == target && (!frame_cmds.empty() || guest_stale)) {
        flush(After::Download);
        guest_stale = false;
    }
}

void cpu_wrote(Surface *s) {
    if (s && s == target) {
        if (!frame_cmds.empty()) flush(After::Download);
        cpu_dirty = true;
    }
}

bool present_blt(Surface *src, Surface *dst, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (!dev || !rt || src != target || !gpu_frame_used || w != g_w || h != g_h) return false;
    if (x < 0 || y < 0 || x + w > dst->w || y + h > dst->h) return false;
    host::gpu::Crop placed{static_cast<uint32_t>(x), static_cast<uint32_t>(y), w, h};
    host::gpu::Crop frame{0, 0, dst->w, dst->h};
    // Widescreen: the game's 16/9 mode blits a 16:9 band; show just that band.
    host::gpu::Crop crop = (settings::get().widescreen && w * 9 == h * 16) ? placed : frame;
    host::gpu::set_game_size(dst->w, dst->h);
    // Render queued work and present the target where the game put it.
    SDL_GPUCommandBuffer *cmd = nullptr;
    flush(After::Nothing);
    cmd = SDL_AcquireGPUCommandBuffer(dev);
    host::gpu::present_placed(cmd, rt, rt_w, rt_h, placed, crop);
    host::count_frame();
    snapshot_rt();
    gpu_frame_used = false;
    guest_stale = true;
    return true;
}

bool present_flip(Surface *back) {
    if (!dev || !rt || back != target || !gpu_frame_used) return false;
    if (crop_now.w != g_w) crop_now = host::gpu::Crop{0, 0, g_w, g_h};
    if (dbg && frame_max_y > frame_min_y) {
        dbg_extents[{static_cast<int>(frame_min_y) / 10, static_cast<int>(frame_max_y) / 10}]++;
        static uint64_t last = 0;
        if (SDL_GetTicks() - last > 15000) {
            last = SDL_GetTicks();
            log("D3D extents (min_y/10, max_y/10) -> frames:\n");
            for (auto &[k, n] : dbg_extents) if (n > 5) log("  %4d..%4d  %llu\n", k.first * 10, k.second * 10, (unsigned long long)n);
            dbg_extents.clear();
        }
    }
    frame_min_y = 1e9f;
    frame_max_y = -1e9f;
    flush(After::Present);
    check_widescreen();
    gpu_frame_used = false;
    rt_valid = false;   // after the flip the back buffer is a different picture
    cpu_dirty = false;
    return true;
}

void surface_destroyed(Surface *s) {
    if (s == target) target = nullptr;
    auto it = textures.find(s);
    if (it != textures.end()) {
        if (it->second.tex) SDL_ReleaseGPUTexture(dev, it->second.tex);
        textures.erase(it);
    }
}

}  // namespace dx::gpu
