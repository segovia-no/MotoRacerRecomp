// Direct3D version 1 (DirectX 3/5 execute-buffer API) — stage 1: complete object model and an
// execute-buffer parser that records what the game submits. Nothing is rasterised yet.
//
// Enabled with [Recomp] Renderer=d3d (adds -D3D -ZBuffer to the game command line).
#include <SDL3/SDL.h>

#include <algorithm>
#include <map>
#include <vector>

#include "d3d_gpu.h"
#include "ddraw_internal.h"
#include "settings.h"

using namespace rt;
using namespace dx;

namespace {

constexpr uint32_t D3D_OK = 0;
constexpr uint32_t D3DERR_INVALIDPARAMS = E_INVALIDARG;

const char *IID_IDirect3D = "3BBA0080-2421-11CF-A31A-00AA00B93356";
const char *IID_IDirect3DTexture = "2CDCD9E0-25A0-11CF-A31A-00AA00B93356";
const char *IID_IDirect3DHALDevice = "84E63DE0-46AA-11CF-816F-0000C020156E";
const char *IID_IDirect3DRGBDevice = "A4665C60-2673-11CF-A31A-00AA00B93356";
const char *IID_IDirect3DMMXDevice = "881949A1-D6F3-11D0-89AB-00A0C9054129";
const char *IID_IDirect3DRampDevice = "F2086B20-259F-11CF-A31A-00AA00B93356";
constexpr uint8_t HAL_GUID_BYTES[16] = {0xE0, 0x3D, 0xE6, 0x84, 0xAA, 0x46, 0xCF, 0x11,
                                        0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E};

// ---------- objects ----------
struct Device;

struct D3D : ComObj {
    DDraw *dd = nullptr;
};

struct Material : ComObj {
    uint8_t data[68] = {};   // D3DMATERIAL
    uint32_t handle = 0;
};

struct Light : ComObj {
    uint8_t data[64] = {};
};

struct Viewport : ComObj {
    uint8_t data[44] = {};   // D3DVIEWPORT
    uint32_t background = 0; // material handle
    std::vector<Light *> lights;
};

struct ExecuteBuffer : ComObj {
    uint32_t mem = 0, size = 0;
    uint8_t exec_data[48] = {};   // D3DEXECUTEDATA
    bool locked = false;
};

struct Device : ComObj {
    Surface *target = nullptr;
    uint32_t rs[256] = {};                    // D3DRENDERSTATETYPE -> value
    std::vector<gpu::TLVertex> processed;     // vertices after PROCESSVERTICES (COPY)
    Device() {
        rs[3] = 1;    // TEXTUREADDRESS = WRAP
        rs[4] = 1;    // TEXTUREPERSPECTIVE
        rs[7] = 1;    // ZENABLE
        rs[8] = 3;    // FILLMODE = SOLID
        rs[9] = 2;    // SHADEMODE = GOURAUD
        rs[14] = 1;   // ZWRITEENABLE
        rs[17] = 1;   // TEXTUREMAG = NEAREST
        rs[18] = 1;   // TEXTUREMIN = NEAREST
        rs[19] = 2;   // SRCBLEND = ONE
        rs[20] = 1;   // DESTBLEND = ZERO
        rs[21] = 2;   // TEXTUREMAPBLEND = MODULATE
        rs[22] = 3;   // CULLMODE = CCW
        rs[23] = 4;   // ZFUNC = LESSEQUAL
        rs[25] = 8;   // ALPHAFUNC = ALWAYS
    }
    std::vector<Viewport *> viewports;
    std::map<uint32_t, std::vector<uint8_t>> matrices;
    uint32_t next_matrix = 1;
    bool in_scene = false;
};

uint32_t vt_d3d, vt_dev, vt_eb, vt_vp, vt_mat, vt_tex, vt_light;
std::map<uint32_t, Surface *> texture_handles;   // D3DTEXTUREHANDLE -> surface
std::map<Surface *, uint32_t> surface_texture_handle;
std::map<uint32_t, Material *> material_handles;
uint32_t next_handle = 1;

// ---------- execute-buffer statistics (stage 1 discovery) ----------
struct Stats {
    uint64_t executes = 0, triangles = 0, vertices = 0;
    std::map<uint8_t, uint64_t> opcodes;
    std::map<uint32_t, std::map<uint32_t, uint64_t>> render_states;   // state -> value -> count
    std::map<uint32_t, uint64_t> process_ops;                         // PROCESSVERTICES dwFlags
    std::map<uint32_t, uint64_t> light_states, transform_states;
} stats;
uint64_t last_report_ms = 0;

const char *opcode_name(uint8_t op) {
    static const char *names[] = {"?", "POINT", "LINE", "TRIANGLE", "MATRIXLOAD", "MATRIXMULTIPLY",
                                  "STATETRANSFORM", "STATELIGHT", "STATERENDER", "PROCESSVERTICES",
                                  "TEXTURELOAD", "EXIT", "BRANCHFORWARD", "SPAN", "SETSTATUS"};
    return op < sizeof names / sizeof *names ? names[op] : "?";
}

void report_stats() {
    log("D3D stats: %llu executes, %llu triangles, %llu vertices\n", (unsigned long long)stats.executes,
        (unsigned long long)stats.triangles, (unsigned long long)stats.vertices);
    for (auto &[op, n] : stats.opcodes) log("  op %-16s x%llu\n", opcode_name(op), (unsigned long long)n);
    for (auto &[f, n] : stats.process_ops) log("  PROCESSVERTICES flags %08x x%llu\n", f, (unsigned long long)n);
    for (auto &[st, vals] : stats.render_states) {
        log("  RENDERSTATE %3u:", st);
        int shown = 0;
        for (auto &[v, n] : vals) {
            if (shown++ == 8) { log(" ..."); break; }
            log(" %x(x%llu)", v, (unsigned long long)n);
        }
        log("\n");
    }
    for (auto &[st, n] : stats.light_states) log("  LIGHTSTATE %u x%llu\n", st, (unsigned long long)n);
    for (auto &[st, n] : stats.transform_states) log("  TRANSFORMSTATE %u x%llu\n", st, (unsigned long long)n);
}

Surface *texture_from_handle(uint32_t h) {
    auto it = texture_handles.find(h);
    return it == texture_handles.end() ? nullptr : it->second;
}

void parse_execute(ExecuteBuffer *eb, Device *dev) {
    stats.executes++;
    gpu::set_target(dev->target);
    uint32_t vtx_off;
    memcpy(&vtx_off, eb->exec_data + 4, 4);
    uint32_t base = 0, nverts = 0;
    std::vector<uint16_t> idx;
    uint32_t ed = 0;
    memcpy(&ed, eb->exec_data + 8, 4);   // dwVertexCount
    stats.vertices += ed;
    uint32_t ins_off, ins_len;
    memcpy(&ins_off, eb->exec_data + 12, 4);
    memcpy(&ins_len, eb->exec_data + 16, 4);
    uint32_t p = eb->mem + ins_off, end = eb->mem + std::min(ins_off + ins_len, eb->size);
    uint32_t status = 0;   // D3DSTATUS.dwStatus as maintained by SETSTATUS (clip flags)
    int guard = 100000;
    while (p + 4 <= end && guard-- > 0) {
        uint8_t op = RD8(p), sz = RD8(p + 1);
        uint16_t count = RD16(p + 2);
        stats.opcodes[op] += count ? count : 1;
        uint32_t data = p + 4;
        if (op == 12) {   // D3DOP_BRANCHFORWARD: D3DBRANCH {mask, value, bNegate, offset}
            bool taken = false;
            uint32_t off = 0;
            for (uint16_t i = 0; i < count && !taken; i++) {
                uint32_t b = data + 16 * i;
                bool cond = ((status & RD32(b)) == RD32(b + 4)) != (RD32(b + 8) != 0);
                if (cond) { taken = true; off = RD32(b + 12); }
            }
            if (taken) {
                if (!off) break;   // offset 0 = exit
                p += off;          // measured from the start of the branch instruction
                continue;
            }
        }
        if (op == 14 && count) status = RD32(data + 4);   // D3DOP_SETSTATUS
        switch (op) {
        case 3: {                                 // D3DOP_TRIANGLE: D3DTRIANGLE {v1, v2, v3, flags}
            stats.triangles += count;
            idx.resize(3u * count);
            for (uint16_t i = 0; i < count; i++) {
                uint32_t t = data + static_cast<uint32_t>(sz) * i;
                idx[3 * i] = RD16(t);
                idx[3 * i + 1] = RD16(t + 2);
                idx[3 * i + 2] = RD16(t + 4);
            }
            gpu::draw(dev->rs, dev->rs[1] ? texture_from_handle(dev->rs[1]) : nullptr, base, nverts, idx.data(), count);
            break;
        }
        case 8:                                   // D3DOP_STATERENDER: {type, value}
            for (uint16_t i = 0; i < count; i++) {
                uint32_t type = RD32(data + 8 * i), value = RD32(data + 8 * i + 4);
                stats.render_states[type][value]++;
                if (type < 256) dev->rs[type] = value;
            }
            break;
        case 7:
            for (uint16_t i = 0; i < count; i++) stats.light_states[RD32(data + 8 * i)]++;
            break;
        case 6:
            for (uint16_t i = 0; i < count; i++) stats.transform_states[RD32(data + 8 * i)]++;
            break;
        case 9:                                   // D3DOP_PROCESSVERTICES: {flags, wStart, wDest, dwCount}
            for (uint16_t i = 0; i < count; i++) {
                uint32_t pv = data + 16 * i;
                uint32_t flags = RD32(pv), start = RD16(pv + 4), dest = RD16(pv + 6), n = RD32(pv + 8);
                stats.process_ops[flags]++;
                if ((flags & 3) != 2) {           // only D3DPROCESSVERTICES_COPY (TLVERTEX) is used by the game
                    static bool warned = false;
                    if (!warned) { warned = true; log("D3D: PROCESSVERTICES flags %x not supported\n", flags); }
                    continue;
                }
                if (dev->processed.size() < dest + n) dev->processed.resize(dest + n);
                memcpy(&dev->processed[dest], M + eb->mem + vtx_off + 32 * start, 32ull * n);
            }
            nverts = static_cast<uint32_t>(dev->processed.size());
            base = gpu::add_vertices(dev->processed.data(), nverts);
            break;
        default: break;
        }
        if (op == 11) break;   // D3DOP_EXIT
        p = data + static_cast<uint32_t>(sz) * count;
    }
    uint64_t now = SDL_GetTicks();
    if (now - last_report_ms > 10000) {
        last_report_ms = now;
        report_stats();
    }
}

void fill_prim_caps(uint32_t p) {
    WR32(p + 0, 56);
    WR32(p + 4, 0x2 | 0x10 | 0x20 | 0x40);         // MASKZ, CULLNONE/CW/CCW
    WR32(p + 8, 0x1 | 0x10 | 0x20 | 0x80 | 0x100);  // DITHER, ZTEST, SUBPIXEL, FOGVERTEX, FOGTABLE
    WR32(p + 12, 0xFF);                            // Z compare: all
    WR32(p + 16, 0x1FFF);                          // src blend: all
    WR32(p + 20, 0x1FFF);                          // dest blend: all
    WR32(p + 24, 0xFF);                            // alpha compare: all
    WR32(p + 28, 0x000FFFFF);                      // shade caps: flat/gouraud colour, specular, alpha, fog
    WR32(p + 32, 0x1 | 0x2 | 0x4 | 0x8);           // texture: PERSPECTIVE, POW2, ALPHA, TRANSPARENCY
    WR32(p + 36, 0x3F);                            // filters: nearest/linear (+mip)
    WR32(p + 40, 0xFF);                            // texture blend: decal/modulate/... /add
    WR32(p + 44, 0x7);                             // address: wrap/mirror/clamp
}

// D3DDEVICEDESC as of DirectX 5 (204 bytes); `hal` false = empty HEL description.
void fill_device_desc(uint32_t p, bool hal) {
    uint32_t size = 204;
    memset(M + p, 0, size);
    WR32(p, size);
    if (!hal) return;
    WR32(p + 4, 0x7FF);                            // all D3DDD_* fields valid
    WR32(p + 8, 2);                                // D3DCOLOR_RGB
    WR32(p + 12, 0x1 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800);
    WR32(p + 16, 8);                               // transform caps
    WR32(p + 20, 1);
    WR32(p + 24, 1);                               // clipping
    WR32(p + 28, 16);                              // lighting caps
    WR32(p + 32, 0xF);
    WR32(p + 36, 1);                               // RGB lighting model
    WR32(p + 40, 8);
    fill_prim_caps(p + 44);                        // line caps
    fill_prim_caps(p + 100);                       // triangle caps
    WR32(p + 156, 0x400);                          // render depth: 16 bpp
    WR32(p + 160, 0x400);                          // Z depth: 16 bpp
    WR32(p + 164, 0);                              // max buffer size: unlimited
    WR32(p + 168, 0xFFFF);                         // max vertex count
    WR32(p + 172, 1);                              // min texture w/h
    WR32(p + 176, 1);
    WR32(p + 180, 2048);                           // max texture w/h
    WR32(p + 184, 2048);
}

uint32_t ensure_texture_handle(Surface *s) {
    auto it = surface_texture_handle.find(s);
    if (it != surface_texture_handle.end()) return it->second;
    uint32_t h = next_handle++;
    surface_texture_handle[s] = h;
    texture_handles[h] = s;
    return h;
}

// =====================================================================================
// IDirect3D
// =====================================================================================
COM_METHOD(D3D_QueryInterface, 2) { WR32(arg(2), 0); return E_NOINTERFACE; }
COM_METHOD(D3D_AddRef, 0) { return add_ref(self); }
COM_METHOD(D3D_Release, 0) { return release(self); }
COM_METHOD(D3D_Initialize, 1) { return D3D_OK; }

COM_METHOD(D3D_EnumDevices, 2) {
    uint32_t cb = arg(1), ctx = arg(2);
    uint32_t guid = heap_alloc(16);
    memcpy(M + guid, HAL_GUID_BYTES, 16);
    uint32_t hal = heap_alloc(256), hel = heap_alloc(256);
    fill_device_desc(hal, true);
    fill_device_desc(hel, false);
    uint32_t desc = alloc_cstr("Metal HAL"), name = alloc_cstr("Direct3D HAL");
    log("IDirect3D::EnumDevices -> offering HAL device\n");
    guest_call_stdcall(cb, {guid, desc, name, hal, hel, ctx});
    return D3D_OK;
}

COM_METHOD(D3D_CreateLight, 2) {
    auto *l = new Light;
    WR32(arg(1), expose(l, "IDirect3DLight", vt_light));
    return D3D_OK;
}

COM_METHOD(D3D_CreateMaterial, 2) {
    auto *m = new Material;
    WR32(arg(1), expose(m, "IDirect3DMaterial", vt_mat));
    return D3D_OK;
}

COM_METHOD(D3D_CreateViewport, 2) {
    auto *v = new Viewport;
    WR32(arg(1), expose(v, "IDirect3DViewport", vt_vp));
    return D3D_OK;
}

COM_METHOD(D3D_FindDevice, 2) {
    // D3DFINDDEVICERESULT: dwSize, guid, ddHwDesc, ddSwDesc
    uint32_t r = arg(2);
    memcpy(M + r + 4, HAL_GUID_BYTES, 16);
    fill_device_desc(r + 20, true);
    fill_device_desc(r + 20 + 204, false);
    return D3D_OK;
}

// =====================================================================================
// IDirect3DDevice (v1)
// =====================================================================================
COM_METHOD(Dev_QueryInterface, 2) { WR32(arg(2), 0); return E_NOINTERFACE; }
COM_METHOD(Dev_AddRef, 0) { return add_ref(self); }
COM_METHOD(Dev_Release, 0) { return release(self); }
COM_METHOD(Dev_Initialize, 3) { return D3D_OK; }
COM_METHOD(Dev_GetCaps, 2) {
    if (arg(1)) fill_device_desc(arg(1), true);
    if (arg(2)) fill_device_desc(arg(2), false);
    return D3D_OK;
}
COM_METHOD(Dev_SwapTextureHandles, 2) { return D3D_OK; }

COM_METHOD(Dev_CreateExecuteBuffer, 3) {
    uint32_t d = arg(1);
    auto *eb = new ExecuteBuffer;
    eb->size = (RD32(d + 4) & 1) ? RD32(d + 12) : 0x10000;
    eb->mem = heap_alloc(eb->size + 16, true);
    WR32(arg(2), expose(eb, "IDirect3DExecuteBuffer", vt_eb));
    log("CreateExecuteBuffer %u bytes\n", eb->size);
    return D3D_OK;
}

COM_METHOD(Dev_GetStats, 1) {
    uint32_t p = arg(1);
    uint32_t size = RD32(p) ? RD32(p) : 24;
    memset(M + p + 4, 0, size - 4);
    return D3D_OK;
}

COM_METHOD(Dev_Execute, 3) {
    auto *eb = dynamic_cast<ExecuteBuffer *>(lookup(arg(1)));
    if (!eb) return D3DERR_INVALIDPARAMS;
    parse_execute(eb, obj<Device>(self));
    return D3D_OK;
}

COM_METHOD(Dev_AddViewport, 1) {
    auto *v = dynamic_cast<Viewport *>(lookup(arg(1)));
    if (!v) return D3DERR_INVALIDPARAMS;
    v->refs++;
    obj<Device>(self)->viewports.push_back(v);
    return D3D_OK;
}

COM_METHOD(Dev_DeleteViewport, 1) {
    auto &vps = obj<Device>(self)->viewports;
    auto *v = dynamic_cast<Viewport *>(lookup(arg(1)));
    std::erase(vps, v);
    return D3D_OK;
}

COM_METHOD(Dev_NextViewport, 3) { WR32(arg(2), 0); return D3DERR_INVALIDPARAMS; }
COM_METHOD(Dev_Pick, 4) { return D3D_OK; }
COM_METHOD(Dev_GetPickRecords, 2) { if (arg(1)) WR32(arg(1), 0); return D3D_OK; }

COM_METHOD(Dev_EnumTextureFormats, 2) {
    uint32_t cb = arg(1), ctx = arg(2);
    uint32_t d = heap_alloc(108, true);
    struct Fmt { uint32_t flags, bits, r, g, b, a; const char *name; };
    static const Fmt fmts[] = {
        {0x40, 16, 0xF800, 0x07E0, 0x001F, 0, "RGB565"},
        {0x41, 16, 0x7C00, 0x03E0, 0x001F, 0x8000, "ARGB1555"},
        {0x41, 16, 0x0F00, 0x00F0, 0x000F, 0xF000, "ARGB4444"},
        {0x20, 8, 0, 0, 0, 0, "PAL8"},
    };
    for (auto &f : fmts) {
        memset(M + d, 0, 108);
        WR32(d, 108);
        WR32(d + 4, DDSD_PIXELFORMAT | DDSD_CAPS);
        WR32(d + 72, 32);
        WR32(d + 76, f.flags);   // DDPF_RGB (| DDPF_ALPHAPIXELS) or DDPF_PALETTEINDEXED8
        WR32(d + 84, f.bits);
        WR32(d + 88, f.r);
        WR32(d + 92, f.g);
        WR32(d + 96, f.b);
        WR32(d + 100, f.a);
        WR32(d + 104, DDSCAPS_TEXTURE);
        log("EnumTextureFormats: offering %s\n", f.name);
        if (!guest_call_stdcall(cb, {d, ctx})) break;
    }
    heap_free(d);
    return D3D_OK;
}

COM_METHOD(Dev_CreateMatrix, 1) {
    Device *dev = obj<Device>(self);
    uint32_t h = dev->next_matrix++;
    dev->matrices[h].assign(64, 0);
    WR32(arg(1), h);
    return D3D_OK;
}
COM_METHOD(Dev_SetMatrix, 2) {
    auto &m = obj<Device>(self)->matrices[arg(1)];
    m.assign(M + arg(2), M + arg(2) + 64);
    return D3D_OK;
}
COM_METHOD(Dev_GetMatrix, 2) {
    auto &m = obj<Device>(self)->matrices[arg(1)];
    if (m.size() == 64) memcpy(M + arg(2), m.data(), 64);
    return D3D_OK;
}
COM_METHOD(Dev_DeleteMatrix, 1) { obj<Device>(self)->matrices.erase(arg(1)); return D3D_OK; }
COM_METHOD(Dev_BeginScene, 0) { obj<Device>(self)->in_scene = true; return D3D_OK; }
COM_METHOD(Dev_EndScene, 0) { obj<Device>(self)->in_scene = false; return D3D_OK; }
COM_METHOD(Dev_GetDirect3D, 1) { WR32(arg(1), 0); return E_NOTIMPL; }

// =====================================================================================
// IDirect3DExecuteBuffer
// =====================================================================================
COM_METHOD(EB_QueryInterface, 2) { WR32(arg(2), 0); return E_NOINTERFACE; }
COM_METHOD(EB_AddRef, 0) { return add_ref(self); }
COM_METHOD(EB_Release, 0) { return release(self); }
COM_METHOD(EB_Initialize, 2) { return D3D_OK; }
COM_METHOD(EB_Lock, 1) {
    ExecuteBuffer *eb = obj<ExecuteBuffer>(self);
    uint32_t d = arg(1);
    WR32(d + 4, 0x1 | 0x4);   // D3DDEB_BUFSIZE | D3DDEB_LPDATA
    WR32(d + 12, eb->size);
    WR32(d + 16, eb->mem);
    eb->locked = true;
    return D3D_OK;
}
COM_METHOD(EB_Unlock, 0) { obj<ExecuteBuffer>(self)->locked = false; return D3D_OK; }
COM_METHOD(EB_SetExecuteData, 1) {
    ExecuteBuffer *eb = obj<ExecuteBuffer>(self);
    memcpy(eb->exec_data, M + arg(1), std::min<uint32_t>(RD32(arg(1)) ? RD32(arg(1)) : 48, 48));
    return D3D_OK;
}
COM_METHOD(EB_GetExecuteData, 1) {
    ExecuteBuffer *eb = obj<ExecuteBuffer>(self);
    memcpy(M + arg(1), eb->exec_data, 48);
    return D3D_OK;
}
COM_METHOD(EB_Validate, 4) { return D3D_OK; }
COM_METHOD(EB_Optimize, 1) { return D3D_OK; }

// =====================================================================================
// IDirect3DViewport
// =====================================================================================
COM_METHOD(VP_QueryInterface, 2) { WR32(arg(2), 0); return E_NOINTERFACE; }
COM_METHOD(VP_AddRef, 0) { return add_ref(self); }
COM_METHOD(VP_Release, 0) { return release(self); }
COM_METHOD(VP_Initialize, 1) { return D3D_OK; }
COM_METHOD(VP_GetViewport, 1) { memcpy(M + arg(1), obj<Viewport>(self)->data, 44); return D3D_OK; }
COM_METHOD(VP_SetViewport, 1) {
    Viewport *v = obj<Viewport>(self);
    memcpy(v->data, M + arg(1), 44);
    uint32_t x, y, w, h;
    memcpy(&x, v->data + 4, 4);
    memcpy(&y, v->data + 8, 4);
    memcpy(&w, v->data + 12, 4);
    memcpy(&h, v->data + 16, 4);
    log("Viewport::SetViewport %u,%u %ux%u\n", x, y, w, h);
    return D3D_OK;
}
COM_METHOD(VP_TransformVertices, 5) { return D3D_OK; }
COM_METHOD(VP_LightElements, 2) { return D3D_OK; }
COM_METHOD(VP_SetBackground, 1) { obj<Viewport>(self)->background = arg(1); return D3D_OK; }
COM_METHOD(VP_GetBackground, 2) {
    WR32(arg(1), obj<Viewport>(self)->background);
    if (arg(2)) WR32(arg(2), 1);
    return D3D_OK;
}
COM_METHOD(VP_SetBackgroundDepth, 1) { return D3D_OK; }
COM_METHOD(VP_GetBackgroundDepth, 2) { WR32(arg(1), 0); if (arg(2)) WR32(arg(2), 0); return D3D_OK; }
COM_METHOD(VP_Clear, 3) {
    Viewport *v = obj<Viewport>(self);
    uint32_t argb = 0;
    auto it = material_handles.find(v->background);
    if (it != material_handles.end()) {   // background = material diffuse colour (D3DCOLORVALUE floats)
        float c[4];
        memcpy(c, it->second->data + 4, 16);
        auto b = [](float f) { return static_cast<uint32_t>(std::clamp(f, 0.f, 1.f) * 255.f + 0.5f); };
        argb = 0xFF000000u | b(c[0]) << 16 | b(c[1]) << 8 | b(c[2]);
    }
    gpu::clear(arg(3), argb, 1.f);
    return D3D_OK;
}
COM_METHOD(VP_AddLight, 1) {
    if (auto *l = dynamic_cast<Light *>(lookup(arg(1)))) { l->refs++; obj<Viewport>(self)->lights.push_back(l); }
    return D3D_OK;
}
COM_METHOD(VP_DeleteLight, 1) {
    auto *l = dynamic_cast<Light *>(lookup(arg(1)));
    std::erase(obj<Viewport>(self)->lights, l);
    return D3D_OK;
}
COM_METHOD(VP_NextLight, 3) { WR32(arg(2), 0); return D3DERR_INVALIDPARAMS; }

// =====================================================================================
// IDirect3DMaterial / IDirect3DLight
// =====================================================================================
COM_METHOD(Mat_QueryInterface, 2) { WR32(arg(2), 0); return E_NOINTERFACE; }
COM_METHOD(Mat_AddRef, 0) { return add_ref(self); }
COM_METHOD(Mat_Release, 0) { return release(self); }
COM_METHOD(Mat_Initialize, 1) { return D3D_OK; }
COM_METHOD(Mat_SetMaterial, 1) { memcpy(obj<Material>(self)->data, M + arg(1), 68); return D3D_OK; }
COM_METHOD(Mat_GetMaterial, 1) { memcpy(M + arg(1), obj<Material>(self)->data, 68); return D3D_OK; }
COM_METHOD(Mat_GetHandle, 2) {
    Material *m = obj<Material>(self);
    if (!m->handle) { m->handle = next_handle++; material_handles[m->handle] = m; }
    WR32(arg(2), m->handle);
    return D3D_OK;
}
COM_METHOD(Mat_Reserve, 0) { return D3D_OK; }
COM_METHOD(Mat_Unreserve, 0) { return D3D_OK; }

COM_METHOD(Light_QueryInterface, 2) { WR32(arg(2), 0); return E_NOINTERFACE; }
COM_METHOD(Light_AddRef, 0) { return add_ref(self); }
COM_METHOD(Light_Release, 0) { return release(self); }
COM_METHOD(Light_Initialize, 1) { return D3D_OK; }
COM_METHOD(Light_SetLight, 1) { memcpy(obj<Light>(self)->data, M + arg(1), 64); return D3D_OK; }
COM_METHOD(Light_GetLight, 1) { memcpy(M + arg(1), obj<Light>(self)->data, 64); return D3D_OK; }

// =====================================================================================
// IDirect3DTexture (an interface of a DirectDraw surface)
// =====================================================================================
COM_METHOD(Tex_QueryInterface, 2) { WR32(arg(2), 0); return E_NOINTERFACE; }
COM_METHOD(Tex_AddRef, 0) { return add_ref(self); }
COM_METHOD(Tex_Release, 0) { return release(self); }
COM_METHOD(Tex_Initialize, 2) { return D3D_OK; }
COM_METHOD(Tex_GetHandle, 2) {
    WR32(arg(2), ensure_texture_handle(obj<Surface>(self)));
    return D3D_OK;
}
COM_METHOD(Tex_PaletteChanged, 2) { return D3D_OK; }
COM_METHOD(Tex_Load, 1) {
    // Copy a (system-memory) source texture into this one.
    Surface *dst = obj<Surface>(self), *src = dynamic_cast<Surface *>(lookup(arg(1)));
    if (!src) return D3DERR_INVALIDPARAMS;
    uint32_t rows = std::min(dst->h, src->h), bytes = std::min(dst->pitch, src->pitch);
    for (uint32_t y = 0; y < rows; y++) memcpy(M + dst->mem + y * dst->pitch, M + src->mem + y * src->pitch, bytes);
    if (src->palette) dst->palette = src->palette;
    if (src->ckey_src) { dst->ckey_src = true; dst->ckey_lo = src->ckey_lo; dst->ckey_hi = src->ckey_hi; }
    dst->version++;
    return D3D_OK;
}
COM_METHOD(Tex_Unload, 0) { return D3D_OK; }

void init_vtables() {
    if (vt_d3d) return;
    vt_d3d = make_vtable("IDirect3D", {D3D_QueryInterface, D3D_AddRef, D3D_Release, D3D_Initialize, D3D_EnumDevices,
                                       D3D_CreateLight, D3D_CreateMaterial, D3D_CreateViewport, D3D_FindDevice}, 9);
    vt_dev = make_vtable("IDirect3DDevice",
                         {Dev_QueryInterface, Dev_AddRef, Dev_Release, Dev_Initialize, Dev_GetCaps,
                          Dev_SwapTextureHandles, Dev_CreateExecuteBuffer, Dev_GetStats, Dev_Execute, Dev_AddViewport,
                          Dev_DeleteViewport, Dev_NextViewport, Dev_Pick, Dev_GetPickRecords, Dev_EnumTextureFormats,
                          Dev_CreateMatrix, Dev_SetMatrix, Dev_GetMatrix, Dev_DeleteMatrix, Dev_BeginScene,
                          Dev_EndScene, Dev_GetDirect3D}, 22);
    vt_eb = make_vtable("IDirect3DExecuteBuffer", {EB_QueryInterface, EB_AddRef, EB_Release, EB_Initialize, EB_Lock,
                                                   EB_Unlock, EB_SetExecuteData, EB_GetExecuteData, EB_Validate,
                                                   EB_Optimize}, 10);
    vt_vp = make_vtable("IDirect3DViewport",
                        {VP_QueryInterface, VP_AddRef, VP_Release, VP_Initialize, VP_GetViewport, VP_SetViewport,
                         VP_TransformVertices, VP_LightElements, VP_SetBackground, VP_GetBackground,
                         VP_SetBackgroundDepth, VP_GetBackgroundDepth, VP_Clear, VP_AddLight, VP_DeleteLight,
                         VP_NextLight}, 16);
    vt_mat = make_vtable("IDirect3DMaterial", {Mat_QueryInterface, Mat_AddRef, Mat_Release, Mat_Initialize,
                                               Mat_SetMaterial, Mat_GetMaterial, Mat_GetHandle, Mat_Reserve,
                                               Mat_Unreserve}, 9);
    vt_tex = make_vtable("IDirect3DTexture", {Tex_QueryInterface, Tex_AddRef, Tex_Release, Tex_Initialize,
                                              Tex_GetHandle, Tex_PaletteChanged, Tex_Load, Tex_Unload}, 8);
    vt_light = make_vtable("IDirect3DLight", {Light_QueryInterface, Light_AddRef, Light_Release, Light_Initialize,
                                              Light_SetLight, Light_GetLight}, 6);
}

bool is_device_guid(const Guid &g) {
    return guid_eq(g, IID_IDirect3DHALDevice) || guid_eq(g, IID_IDirect3DRGBDevice) ||
           guid_eq(g, IID_IDirect3DMMXDevice) || guid_eq(g, IID_IDirect3DRampDevice);
}

}  // namespace

namespace dx {

bool d3d_enabled() { return settings::get().d3d; }

bool d3d_query_ddraw(DDraw *dd, const Guid &g, uint32_t *out, uint32_t *hr) {
    if (!guid_eq(g, IID_IDirect3D) || !d3d_enabled()) return false;
    init_vtables();
    auto *d = new D3D;
    d->dd = dd;
    *out = expose(d, "IDirect3D", vt_d3d);
    *hr = D3D_OK;
    log("IDirectDraw::QueryInterface(IID_IDirect3D) -> 0x%08x\n", *out);
    return true;
}

bool d3d_query_surface(Surface *s, const Guid &g, uint32_t *out, uint32_t *hr) {
    if (!d3d_enabled()) return false;
    init_vtables();
    if (guid_eq(g, IID_IDirect3DTexture)) {
        s->refs++;
        *out = expose(s, "IDirect3DTexture", vt_tex);
        *hr = D3D_OK;
        return true;
    }
    if (is_device_guid(g)) {
        auto *dev = new Device;
        dev->target = s;
        on_destroy = [](Surface *gone) {
            gpu::surface_destroyed(gone);
            for (auto it = texture_handles.begin(); it != texture_handles.end();)
                it = it->second == gone ? texture_handles.erase(it) : std::next(it);
            surface_texture_handle.erase(gone);
        };
        gpu::set_target(s);
        *out = expose(dev, "IDirect3DDevice", vt_dev);
        *hr = D3D_OK;
        log("Surface(%ux%u)::QueryInterface(%s) -> Direct3D device 0x%08x\n", s->w, s->h, guid_str(g).c_str(), *out);
        return true;
    }
    return false;
}

}  // namespace dx
