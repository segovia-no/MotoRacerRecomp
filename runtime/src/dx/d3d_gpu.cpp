#include "d3d_gpu.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <map>
#include <tuple>
#include <vector>

#include "ddraw_internal.h"

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

vertex VOut vs_main(VIn in [[stage_in]]) {
    VOut o;
    o.pos = in.pos;
    o.color = in.color.zyxw;
    o.spec = in.spec.zyxw;
    o.uv = in.uv;
    return o;
}

struct FragUniforms {
    float4 fog_color;
    float4 mode;     // x: textured, y: TEXTUREMAPBLEND, z: fog, w: specular
    float4 alpha;    // x: alpha test on, y: ref (0..1), z: colour key on, w: texture format has alpha
};

fragment float4 fs_main(VOut in [[stage_in]], constant FragUniforms &u [[buffer(0)]],
                        texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    float4 c = in.color;
    if (u.mode.x > 0.5) {
        float4 t = tex.sample(smp, in.uv);
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
SDL_GPUShader *vs = nullptr, *fs = nullptr;
SDL_GPUTexture *rt = nullptr, *depth = nullptr, *white = nullptr;
uint32_t rt_w = 0, rt_h = 0;
Surface *target = nullptr;
bool target_needs_upload = true, depth_initialised = false;
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

// Render all queued work; optionally copy the colour target back into the guest surface.
void flush(bool download) {
    if (failed || !target || !rt) return;
    if (frame_cmds.empty() && !download) return;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    std::vector<SDL_GPUTransferBuffer *> garbage;
    std::vector<uint32_t> scratch;

    // Copy pass: back-buffer content, textures, geometry.
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (target_needs_upload) {
        scratch.resize(static_cast<size_t>(rt_w) * rt_h);
        for (uint32_t y = 0; y < rt_h; y++)
            for (uint32_t x = 0; x < rt_w; x++)
                scratch[y * rt_w + x] = rgb565_to_rgba(RD16(target->mem + y * target->pitch + 2 * x));
        upload_rgba(copy, rt, scratch.data(), rt_w, rt_h, garbage);
        target_needs_upload = false;
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
        SDL_GPUBufferCreateInfo ici{SDL_GPU_BUFFERUSAGE_INDEX, ibytes, 0};
        vbuf = SDL_CreateGPUBuffer(dev, &vci);
        ibuf = SDL_CreateGPUBuffer(dev, &ici);
        SDL_GPUTransferBufferCreateInfo tci{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, vbytes + ibytes, 0};
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tci);
        auto *m = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(dev, tb, false));
        memcpy(m, frame_vertices.data(), vbytes);
        memcpy(m + vbytes, frame_indices.data(), ibytes);
        SDL_UnmapGPUTransferBuffer(dev, tb);
        SDL_GPUTransferBufferLocation vsrc{tb, 0}, isrc{tb, vbytes};
        SDL_GPUBufferRegion vdst{vbuf, 0, vbytes}, idst{ibuf, 0, ibytes};
        SDL_UploadToGPUBuffer(copy, &vsrc, &vdst, false);
        SDL_UploadToGPUBuffer(copy, &isrc, &idst, false);
        garbage.push_back(tb);
    }
    SDL_EndGPUCopyPass(copy);

    // Render passes: a Clear command starts a new pass with clear load ops.
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
        for (; i < frame_cmds.size() && frame_cmds[i].kind == Cmd::Draw; i++) {
            const Cmd &c = frame_cmds[i];
            SDL_BindGPUGraphicsPipeline(pass, pipeline(c.pipe));
            SDL_GPUViewport vp{0, 0, static_cast<float>(rt_w), static_cast<float>(rt_h), 0.f, 1.f};
            SDL_SetGPUViewport(pass, &vp);
            SDL_GPUTextureSamplerBinding tsb{c.texture ? bound[c.texture] : white, sampler(c.sampler_key)};
            SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
            SDL_PushGPUFragmentUniformData(cmd, 0, &c.uniforms, sizeof c.uniforms);
            SDL_DrawGPUIndexedPrimitives(pass, c.index_count, 1, c.first_index, 0, 0);
        }
        SDL_EndGPURenderPass(pass);
    }

    SDL_GPUTransferBuffer *down = nullptr;
    if (download) {
        SDL_GPUTransferBufferCreateInfo tci{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, rt_w * rt_h * 4, 0};
        down = SDL_CreateGPUTransferBuffer(dev, &tci);
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src{};
        src.texture = rt;
        src.w = rt_w;
        src.h = rt_h;
        src.d = 1;
        SDL_GPUTextureTransferInfo dst{down, 0, rt_w, rt_h};
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
        SDL_EndGPUCopyPass(cp);
    }
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(dev, true, &fence, 1);
    SDL_ReleaseGPUFence(dev, fence);

    if (down) {
        auto *px = static_cast<const uint32_t *>(SDL_MapGPUTransferBuffer(dev, down, false));
        for (uint32_t y = 0; y < rt_h; y++)
            for (uint32_t x = 0; x < rt_w; x++) {
                uint32_t p = px[y * rt_w + x];
                uint16_t r = (p & 255) >> 3, g = ((p >> 8) & 255) >> 2, b = ((p >> 16) & 255) >> 3;
                WR16(target->mem + y * target->pitch + 2 * x, static_cast<uint16_t>(r << 11 | g << 5 | b));
            }
        SDL_UnmapGPUTransferBuffer(dev, down);
        SDL_ReleaseGPUTransferBuffer(dev, down);
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
    dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    if (!dev) {
        log("d3d gpu: SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
        failed = true;
        return false;
    }
    SDL_GPUShaderCreateInfo sci{};
    sci.code = reinterpret_cast<const Uint8 *>(kShaders);
    sci.code_size = strlen(kShaders);
    sci.format = SDL_GPU_SHADERFORMAT_MSL;
    sci.entrypoint = "vs_main";
    sci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vs = SDL_CreateGPUShader(dev, &sci);
    sci.entrypoint = "fs_main";
    sci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    sci.num_samplers = 1;
    sci.num_uniform_buffers = 1;
    fs = SDL_CreateGPUShader(dev, &sci);
    if (!vs || !fs) fatal("d3d gpu: shader compile failed: %s", SDL_GetError());
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
    if (target && target != s) flush(true);
    target = s;
    if (rt && (rt_w != s->w || rt_h != s->h)) {
        SDL_ReleaseGPUTexture(dev, rt);
        SDL_ReleaseGPUTexture(dev, depth);
        rt = depth = nullptr;
    }
    if (!rt) {
        rt_w = s->w;
        rt_h = s->h;
        rt = make_texture(rt_w, rt_h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                          SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        depth = make_texture(rt_w, rt_h, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
        depth_initialised = false;
    }
    target_needs_upload = true;
}

uint32_t add_vertices(const TLVertex *verts, uint32_t n) {
    uint32_t base = static_cast<uint32_t>(frame_vertices.size());
    if (!dev || !rt) return base;
    float sx = 2.f / static_cast<float>(rt_w), sy = 2.f / static_cast<float>(rt_h);
    for (uint32_t i = 0; i < n; i++) {
        const TLVertex &v = verts[i];
        // Clip-space position with w = 1/rhw so the GPU interpolates perspective-correctly.
        float w = v.rhw > 1e-8f ? 1.f / v.rhw : 1.f;
        float nx = (v.sx + 0.5f) * sx - 1.f, ny = 1.f - (v.sy + 0.5f) * sy;
        frame_vertices.push_back({nx * w, ny * w, v.sz * w, w, v.color, v.specular, v.tu, v.tv});
    }
    return base;
}

void draw(const uint32_t *rs, Surface *texture, uint32_t base, uint32_t nverts, const uint16_t *indices,
          uint32_t ntri) {
    if (!dev || !rt || !ntri) return;
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
    for (uint32_t t = 0; t < ntri * 3; t++) {
        uint32_t idx = indices[t];
        frame_indices.push_back(base + (idx < nverts ? idx : 0));
    }
    c.index_count = ntri * 3;
    if (!frame_cmds.empty()) {   // merge with the previous draw if nothing changed
        Cmd &p = frame_cmds.back();
        if (p.kind == Cmd::Draw && !(p.pipe < c.pipe) && !(c.pipe < p.pipe) && p.texture == c.texture &&
            p.sampler_key == c.sampler_key && !memcmp(&p.uniforms, &c.uniforms, sizeof c.uniforms) &&
            p.first_index + p.index_count == c.first_index) {
            p.index_count += c.index_count;
            return;
        }
    }
    frame_cmds.push_back(c);
}

void clear(uint32_t flags, uint32_t argb, float depth_value) {
    if (!dev || !rt) return;
    Cmd c{};
    c.kind = Cmd::Clear;
    c.clear_flags = flags;
    c.clear_argb = argb;
    c.clear_depth = depth_value;
    frame_cmds.push_back(c);
}

void sync_to_cpu(Surface *s) {
    if (s && s == target && !frame_cmds.empty()) flush(true);
}

void cpu_wrote(Surface *s) {
    if (s && s == target) {
        if (!frame_cmds.empty()) flush(true);
        target_needs_upload = true;
    }
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
