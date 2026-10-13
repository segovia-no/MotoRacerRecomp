// DirectDraw (DirectX 3/5 interfaces) on top of guest-memory surfaces.
//
// Surfaces are plain guest heap buffers so the game's software rasterizer can write to them
// directly after Lock. Flip swaps buffer pointers inside the flip chain and presents the front
// buffer through SDL. No 3D capabilities are reported, so the game uses its software path.
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "com.h"
#include "ddraw.h"
#include "d3d_gpu.h"
#include "ddraw_internal.h"
#include "host_video.h"
#include "settings.h"
#include "win32.h"

using namespace rt;
using namespace dx;

namespace {

// ---------- constants (ddraw.h) ----------
constexpr uint32_t DDERR_INVALIDPARAMS = E_INVALIDARG;
constexpr uint32_t DDERR_UNSUPPORTED = E_NOTIMPL;
constexpr uint32_t DDERR_NOTFOUND = 0x887600FF;
constexpr uint32_t DDERR_NOPALETTEATTACHED = 0x88760237;
constexpr uint32_t DDERR_NOCLIPPERATTACHED = 0x88760226;


const char *IID_IDirectDraw = "6C14DB80-A733-11CE-A521-0020AF0BE560";
const char *IID_IDirectDraw2 = "B3A6F3E0-2B43-11CF-A2DE-00AA00B93356";
const char *IID_IDirectDrawSurface = "6C14DB81-A733-11CE-A521-0020AF0BE560";
const char *IID_IDirectDrawSurface2 = "57805885-6EEC-11CF-9441-A82303C10E27";
const char *IID_IDirectDrawSurface3 = "DA044E00-69B2-11D0-A1D5-00AA00B8DFBB";

// ---------- objects ----------

uint32_t vt_dd1, vt_dd2, vt_surf, vt_surf2, vt_surf3, vt_pal, vt_clip;
Surface *current_front = nullptr;
bool front_dirty = false;
uint64_t last_present_ms = 0;
bool gdc_warned = false;

std::string face_name(uint32_t self) {
    if (ComObj *o = lookup(self))
        for (auto &[n, p] : o->faces) if (p == self) return n;
    return "";
}

uint32_t surface_face(Surface *s, const std::string &like) {
    if (like == "IDirectDrawSurface2") return expose(s, "IDirectDrawSurface2", vt_surf2);
    if (like == "IDirectDrawSurface3") return expose(s, "IDirectDrawSurface3", vt_surf3);
    return expose(s, "IDirectDrawSurface", vt_surf);
}

Surface *surface_arg(uint32_t p) { return p ? dynamic_cast<Surface *>(lookup(p)) : nullptr; }

void write_pixel_format(uint32_t p, const Surface &s) {
    memset(M + p, 0, 32);
    WR32(p, 32);
    WR32(p + 4, s.pf_flags);
    WR32(p + 12, s.bpp);
    if (s.pf_flags & DDPF_RGB) {
        WR32(p + 16, s.rmask);
        WR32(p + 20, s.gmask);
        WR32(p + 24, s.bmask);
    }
}

void write_desc(uint32_t p, const Surface &s, uint32_t surface_ptr) {
    uint32_t size = RD32(p);
    memset(M + p, 0, size >= 108 ? size : 108);
    WR32(p, size ? size : 108);
    uint32_t flags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    WR32(p + 8, s.h);
    WR32(p + 12, s.w);
    WR32(p + 16, s.pitch);
    uint32_t backs = 0;
    for (Surface *a : s.attached) if (a->caps & DDSCAPS_BACKBUFFER) backs++;
    if (backs) { flags |= DDSD_BACKBUFFERCOUNT; WR32(p + 20, backs); }
    if (surface_ptr) { flags |= DDSD_LPSURFACE; WR32(p + 36, surface_ptr); }
    if (s.ckey_src) { flags |= DDSD_CKSRCBLT; WR32(p + 64, s.ckey_lo); WR32(p + 68, s.ckey_hi); }
    write_pixel_format(p + 72, s);
    WR32(p + 104, s.caps);
    WR32(p + 4, flags);
}

Surface *new_surface(DDraw *dd, uint32_t w, uint32_t h, uint32_t bpp, uint32_t caps) {
    auto *s = new Surface;
    s->dd = dd;
    s->w = w;
    s->h = h;
    s->bpp = bpp;
    s->caps = caps;
    if (bpp == 8) { s->pf_flags = DDPF_PALETTEINDEXED8 | DDPF_RGB; s->rmask = s->gmask = s->bmask = 0; }
    s->pitch = (w * (bpp / 8) + 3) & ~3u;
    s->mem = heap_alloc(s->pitch * h + 16, true);
    s->storage = std::shared_ptr<uint32_t>(new uint32_t(s->mem), [](uint32_t *va) {
        heap_free(*va);
        delete va;
    });
    return s;
}

void present(Surface *s) {
    if (!s) return;
    const uint8_t *px = M + s->mem;
    if (s->bpp == 16) {
        host::present_rgb16(px, static_cast<int>(s->w), static_cast<int>(s->h), static_cast<int>(s->pitch),
                             s->gmask == 0x03E0);
    } else if (s->bpp == 8) {
        uint32_t pal[256];
        for (int i = 0; i < 256; i++) {
            uint32_t e = s->palette ? s->palette->entries[i] : static_cast<uint32_t>(i * 0x010101);
            pal[i] = 0xFF000000u | ((e & 0xFF) << 16) | (e & 0xFF00) | ((e >> 16) & 0xFF);
        }
        host::present_pal8(px, static_cast<int>(s->w), static_cast<int>(s->h), static_cast<int>(s->pitch), pal);
    }
    front_dirty = false;
    last_present_ms = SDL_GetTicks();
}

struct BltRect { int32_t l, t, r, b; };

BltRect read_rect(uint32_t p, const Surface &s) {
    if (!p) return {0, 0, static_cast<int32_t>(s.w), static_cast<int32_t>(s.h)};
    return {static_cast<int32_t>(RD32(p)), static_cast<int32_t>(RD32(p + 4)), static_cast<int32_t>(RD32(p + 8)),
            static_cast<int32_t>(RD32(p + 12))};
}

uint32_t get_px(const Surface &s, int32_t x, int32_t y) {
    uint32_t a = s.mem + static_cast<uint32_t>(y) * s.pitch + static_cast<uint32_t>(x) * s.bytes();
    return s.bpp == 16 ? RD16(a) : s.bpp == 8 ? RD8(a) : RD32(a);
}

void put_px(Surface &s, int32_t x, int32_t y, uint32_t v) {
    uint32_t a = s.mem + static_cast<uint32_t>(y) * s.pitch + static_cast<uint32_t>(x) * s.bytes();
    if (s.bpp == 16) WR16(a, static_cast<uint16_t>(v)); else if (s.bpp == 8) WR8(a, static_cast<uint8_t>(v)); else WR32(a, v);
}

void fill(Surface &d, BltRect r, uint32_t color) {
    r.l = std::max(r.l, 0); r.t = std::max(r.t, 0);
    r.r = std::min(r.r, static_cast<int32_t>(d.w)); r.b = std::min(r.b, static_cast<int32_t>(d.h));
    for (int32_t y = r.t; y < r.b; y++)
        for (int32_t x = r.l; x < r.r; x++) put_px(d, x, y, color);
}

// General blit: optional stretch (nearest), mirroring and source colour key.
void blit(Surface &d, BltRect dr, Surface &s, BltRect sr, bool key, uint32_t klo, uint32_t khi, uint32_t fx) {
    int32_t dw = dr.r - dr.l, dh = dr.b - dr.t, sw = sr.r - sr.l, sh = sr.b - sr.t;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    std::vector<uint8_t> copy;
    Surface *src = &s;
    Surface tmp;
    if (&d == &s) {   // overlapping self-blit: read from a snapshot
        copy.assign(M + s.mem, M + s.mem + s.pitch * s.h);
        tmp.w = s.w; tmp.h = s.h; tmp.pitch = s.pitch; tmp.bpp = s.bpp;
        tmp.mem = heap_alloc(static_cast<uint32_t>(copy.size()));
        memcpy(M + tmp.mem, copy.data(), copy.size());
        src = &tmp;
    }
    bool simple = dw == sw && dh == sh && !key && !fx && d.bpp == src->bpp;
    for (int32_t y = 0; y < dh; y++) {
        int32_t dy = dr.t + y;
        if (dy < 0 || dy >= static_cast<int32_t>(d.h)) continue;
        int32_t sy = sr.t + (y * sh) / dh;
        if (fx & DDBLTFX_MIRRORUPDOWN) sy = sr.b - 1 - (y * sh) / dh;
        if (sy < 0 || sy >= static_cast<int32_t>(src->h)) continue;
        if (simple) {
            int32_t x0 = std::max(0, -dr.l), x1 = std::min(dw, static_cast<int32_t>(d.w) - dr.l);
            x0 = std::max(x0, -sr.l);
            x1 = std::min(x1, static_cast<int32_t>(src->w) - sr.l);
            if (x1 > x0)
                memmove(M + d.mem + static_cast<uint32_t>(dy) * d.pitch + static_cast<uint32_t>(dr.l + x0) * d.bytes(),
                        M + src->mem + static_cast<uint32_t>(sy) * src->pitch + static_cast<uint32_t>(sr.l + x0) * src->bytes(),
                        static_cast<size_t>(x1 - x0) * d.bytes());
            continue;
        }
        for (int32_t x = 0; x < dw; x++) {
            int32_t dx = dr.l + x;
            if (dx < 0 || dx >= static_cast<int32_t>(d.w)) continue;
            int32_t sx = sr.l + (x * sw) / dw;
            if (fx & DDBLTFX_MIRRORLEFTRIGHT) sx = sr.r - 1 - (x * sw) / dw;
            if (sx < 0 || sx >= static_cast<int32_t>(src->w)) continue;
            uint32_t v = get_px(*src, sx, sy);
            if (key && v >= klo && v <= khi) continue;
            put_px(d, dx, dy, v);
        }
    }
    if (src == &tmp) { heap_free(tmp.mem); tmp.mem = 0; }
}

void touched(Surface *d) {
    if (d && d->primary() && (d->caps & DDSCAPS_FRONTBUFFER)) front_dirty = true;
}

// =====================================================================================
// IDirectDraw / IDirectDraw2
// =====================================================================================
COM_METHOD(DD_QueryInterface, 2) {
    DDraw *dd = obj<DDraw>(self);
    Guid g = read_guid(arg(1));
    uint32_t out = 0;
    if (guid_eq(g, IID_IDirectDraw)) out = expose(dd, "IDirectDraw", vt_dd1);
    else if (guid_eq(g, IID_IDirectDraw2)) out = expose(dd, "IDirectDraw2", vt_dd2);
    uint32_t hr;
    if (!out && d3d_query_ddraw(dd, g, &out, &hr)) {
        WR32(arg(2), out);
        return hr;
    }
    if (!out) {
        log("IDirectDraw::QueryInterface(%s) -> E_NOINTERFACE\n", guid_str(g).c_str());
        WR32(arg(2), 0);
        return E_NOINTERFACE;
    }
    dd->refs++;
    WR32(arg(2), out);
    return S_OK;
}
COM_METHOD(DD_AddRef, 0) { return add_ref(self); }
COM_METHOD(DD_Release, 0) { return release(self); }
COM_METHOD(DD_Compact, 0) { return S_OK; }

COM_METHOD(DD_CreateClipper, 3) {
    auto *c = new Clipper;
    WR32(arg(2), expose(c, "IDirectDrawClipper", vt_clip));
    return S_OK;
}

COM_METHOD(DD_CreatePalette, 4) {
    auto *p = new Palette;
    if (arg(2)) for (int i = 0; i < 256; i++) p->entries[i] = RD32(arg(2) + 4 * static_cast<uint32_t>(i));
    WR32(arg(3), expose(p, "IDirectDrawPalette", vt_pal));
    return S_OK;
}

COM_METHOD(DD_CreateSurface, 3) {
    DDraw *dd = obj<DDraw>(self);
    uint32_t d = arg(1);
    uint32_t flags = RD32(d + 4);
    uint32_t caps = (flags & DDSD_CAPS) ? RD32(d + 104) : 0;
    Surface *s;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        s = new_surface(dd, dd->mode_w, dd->mode_h, dd->mode_bpp,
                        caps | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM);
        uint32_t backs = (flags & DDSD_BACKBUFFERCOUNT) ? RD32(d + 20) : 0;
        for (uint32_t i = 0; i < backs; i++) {
            uint32_t bcaps = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE)) |
                             DDSCAPS_BACKBUFFER | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_FLIP;
            if (i) bcaps &= ~DDSCAPS_BACKBUFFER;
            s->attached.push_back(new_surface(dd, dd->mode_w, dd->mode_h, dd->mode_bpp, bcaps));
        }
        dd->front = s;
        current_front = s;
    } else {
        uint32_t w = (flags & DDSD_WIDTH) ? RD32(d + 12) : dd->mode_w;
        uint32_t h = (flags & DDSD_HEIGHT) ? RD32(d + 8) : dd->mode_h;
        uint32_t bpp = dd->mode_bpp;
        if (flags & DDSD_PIXELFORMAT) {
            bpp = RD32(d + 72 + 12);
            uint32_t pff = RD32(d + 72 + 4);
            if (pff & 0x4) {   // DDPF_FOURCC (e.g. the game's 'PVRC' PowerVR probe): unsupported, like non-PowerVR cards
                log("CreateSurface: FOURCC %08x rejected (DDERR_INVALIDPIXELFORMAT)\n", RD32(d + 72 + 8));
                WR32(arg(2), 0);
                return 0x88760091;   // DDERR_INVALIDPIXELFORMAT
            }
            if (!bpp || (bpp != 8 && bpp != 16))
                log("CreateSurface: pixel format flags %08x bitcount %u fourcc %08x masks %08x %08x %08x %08x\n", pff,
                    bpp, RD32(d + 72 + 8), RD32(d + 72 + 16), RD32(d + 72 + 20), RD32(d + 72 + 24), RD32(d + 72 + 28));
        }
        if (caps & DDSCAPS_ZBUFFER) bpp = (flags & DDSD_ZBUFFERBITDEPTH) ? RD32(d + 24) : 16;
        if (!(caps & DDSCAPS_SYSTEMMEMORY)) caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
        if (!(caps & (DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_3DDEVICE))) caps |= DDSCAPS_OFFSCREENPLAIN;
        s = new_surface(dd, w, h, bpp, caps);
        if ((flags & DDSD_PIXELFORMAT) && (RD32(d + 72 + 4) & DDPF_RGB) && bpp == 16) {
            s->rmask = RD32(d + 72 + 16);
            s->gmask = RD32(d + 72 + 20);
            s->bmask = RD32(d + 72 + 24);
            s->amask = (RD32(d + 72 + 4) & 0x1) ? RD32(d + 72 + 28) : 0;   // DDPF_ALPHAPIXELS
        }
        if (caps & DDSCAPS_ZBUFFER) s->pf_flags = DDPF_ZBUFFER;
        if (flags & DDSD_CKSRCBLT) { s->ckey_src = true; s->ckey_lo = RD32(d + 64); s->ckey_hi = RD32(d + 68); }
    }
    log("CreateSurface %ux%u %ubpp caps %08x%s -> pitch %u\n", s->w, s->h, s->bpp, s->caps,
        s->attached.empty() ? "" : " +backbuffer", s->pitch);
    WR32(arg(2), expose(s, "IDirectDrawSurface", vt_surf));
    return S_OK;
}

// A duplicate is a separate surface object sharing the original's pixel memory.
COM_METHOD(DD_DuplicateSurface, 2) {
    Surface *src = surface_arg(arg(1));
    if (!src) return DDERR_INVALIDPARAMS;
    auto *d = new Surface;
    d->dd = src->dd;
    d->w = src->w; d->h = src->h; d->pitch = src->pitch; d->bpp = src->bpp;
    d->caps = src->caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER);
    d->mem = src->mem;
    d->storage = src->storage;
    d->rmask = src->rmask; d->gmask = src->gmask; d->bmask = src->bmask; d->amask = src->amask;
    d->pf_flags = src->pf_flags;
    d->ckey_src = src->ckey_src; d->ckey_lo = src->ckey_lo; d->ckey_hi = src->ckey_hi;
    d->palette = src->palette;
    if (d->palette) d->palette->refs++;
    d->version = src->version;
    WR32(arg(2), expose(d, "IDirectDrawSurface", vt_surf));
    return S_OK;
}

COM_METHOD(DD_EnumDisplayModes, 4) {
    // 4:3 modes. In hardware Direct3D mode the game accepts any size (callback 0x477160 only
    // caps at 640x480 otherwise), so these appear in its own Options > SCREEN MODE list.
    std::vector<std::array<uint32_t, 3>> modes = {
        {320, 200, 8}, {320, 240, 8}, {320, 240, 16}, {512, 384, 8}, {512, 384, 16}, {640, 400, 8},
        {640, 480, 8}, {640, 480, 16}, {800, 600, 8}, {800, 600, 16}, {1024, 768, 8}, {1024, 768, 16},
        {1280, 960, 16}, {1600, 1200, 16}, {1920, 1440, 16}, {2560, 1920, 16}, {3840, 2880, 16}};
    // Developer aid: MOTORACER_MODES="960x720,1024x600" replaces the 16-bit modes above 640x480.
    if (const char *e = getenv("MOTORACER_MODES")) {
        modes.resize(8);
        for (const char *p = e; *p;) {
            unsigned w = 0, h = 0;
            int n = 0;
            if (sscanf(p, "%ux%u%n", &w, &h, &n) != 2) break;
            modes.push_back({w, h, 16});
            p += n;
            if (*p == ',') p++;
        }
    }
    uint32_t cb = arg(4), ctx = arg(3);
    uint32_t desc = heap_alloc(108, true);
    DDraw *dd = obj<DDraw>(self);
    // The game's mode callback (0x477160) caps modes at 640x480 unless a developer switch
    // (0x4dc174, never set in the shipped game) and "hardware D3D present" (0x4dc06c) are set and
    // 0x4dc070 is clear. It runs during the first DirectDraw probe, before D3D is initialised, so
    // with CustomModes on (D3D renderer) the three are set only for the duration of this call.
    constexpr uint32_t DEV_SWITCH = 0x4dc174, HW_D3D = 0x4dc06c, CAP_FLAG = 0x4dc070;
    bool unlock = settings::get().d3d && settings::get().custom_modes;
    uint32_t saved[3] = {RD32(DEV_SWITCH), RD32(HW_D3D), RD32(CAP_FLAG)};
    if (unlock) { WR32(DEV_SWITCH, 1); WR32(HW_D3D, 1); WR32(CAP_FLAG, 0); }
    for (auto &m : modes) {
        Surface tmp;
        tmp.dd = dd; tmp.w = m[0]; tmp.h = m[1]; tmp.bpp = m[2]; tmp.pitch = m[0] * m[2] / 8;
        tmp.caps = 0;
        if (m[2] == 8) { tmp.pf_flags = DDPF_PALETTEINDEXED8 | DDPF_RGB; tmp.rmask = tmp.gmask = tmp.bmask = 0; }
        WR32(desc, 108);
        write_desc(desc, tmp, 0);
        WR32(desc + 4, RD32(desc + 4) | DDSD_REFRESHRATE);
        WR32(desc + 24, 60);
        if (!guest_call_stdcall(cb, {desc, ctx})) break;
    }
    if (unlock) { WR32(DEV_SWITCH, saved[0]); WR32(HW_D3D, saved[1]); WR32(CAP_FLAG, saved[2]); }
    if (cb == 0x477160) {   // game's mode table: count at 0x4ee984, 36-byte entries at 0x5d8954
        uint32_t n = RD32(0x4ee984);
        log("ddraw: game accepted %u display modes:", n);
        for (uint32_t i = 0; i < n && i < 50; i++) log(" %ux%u", RD32(0x5d8960 + 36 * i), RD32(0x5d8964 + 36 * i));
        log("\n");
    }
    heap_free(desc);
    return S_OK;
}

COM_METHOD(DD_FlipToGDISurface, 0) { return S_OK; }

void fill_caps(uint32_t p) {
    if (!p) return;
    uint32_t size = RD32(p);
    if (size < 4 || size > 380) size = 380;
    memset(M + p + 4, 0, size - 4);
    WR32(p + 4, 0x40 | 0x200 | 0x400000 | 0x04000000 | 0x80000000u |   // BLT, BLTSTRETCH, COLORKEY, BLTCOLORFILL, CANBLTSYSMEM
                    (d3d_enabled() ? 0x1u : 0u));                      // DDCAPS_3D
    WR32(p + 12, 0x200);                                                 // CKEYCAPS_SRCBLT
    WR32(p + 24, 0x4);                                                   // PALCAPS_8BIT
    if (d3d_enabled()) WR32(p + 56, 0x400);                              // dwZBufferBitDepths: DDBD_16
    WR32(p + 60, 256u << 20);                                            // dwVidMemTotal
    WR32(p + 64, 256u << 20);                                            // dwVidMemFree
    if (size >= 136)
        WR32(p + 132, DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER |
                          DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PALETTE | DDSCAPS_PRIMARYSURFACE |
                          DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY | DDSCAPS_VISIBLE |
                          (d3d_enabled() ? DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER : 0));
}

COM_METHOD(DD_GetCaps, 2) {
    fill_caps(arg(1));
    fill_caps(arg(2));
    return S_OK;
}

COM_METHOD(DD_GetDisplayMode, 1) {
    DDraw *dd = obj<DDraw>(self);
    Surface tmp;
    tmp.w = dd->mode_w; tmp.h = dd->mode_h; tmp.bpp = dd->mode_bpp; tmp.pitch = tmp.w * tmp.bpp / 8;
    if (tmp.bpp == 8) { tmp.pf_flags = DDPF_PALETTEINDEXED8 | DDPF_RGB; tmp.rmask = tmp.gmask = tmp.bmask = 0; }
    write_desc(arg(1), tmp, 0);
    return S_OK;
}

COM_METHOD(DD_GetFourCCCodes, 2) { if (arg(1)) WR32(arg(1), 0); return S_OK; }
COM_METHOD(DD_GetGDISurface, 1) {
    DDraw *dd = obj<DDraw>(self);
    if (!dd->front) return DDERR_NOTFOUND;
    dd->front->refs++;
    WR32(arg(1), expose(dd->front, "IDirectDrawSurface", vt_surf));
    return S_OK;
}
COM_METHOD(DD_GetMonitorFrequency, 1) { WR32(arg(1), 60); return S_OK; }
COM_METHOD(DD_GetScanLine, 1) { WR32(arg(1), 0); return S_OK; }
COM_METHOD(DD_GetVerticalBlankStatus, 1) { static uint32_t t; WR32(arg(1), t ^= 1); return S_OK; }
COM_METHOD(DD_Initialize, 1) { return S_OK; }
COM_METHOD(DD_RestoreDisplayMode, 0) { return S_OK; }

COM_METHOD(DD_SetCooperativeLevel, 2) {
    DDraw *dd = obj<DDraw>(self);
    dd->hwnd = arg(1);
    dd->coop = arg(2);
    log("SetCooperativeLevel(hwnd %08x, flags %x)\n", arg(1), arg(2));
    return S_OK;
}

uint32_t set_mode(uint32_t self, uint32_t w, uint32_t h, uint32_t bpp) {
    DDraw *dd = obj<DDraw>(self);
    log("SetDisplayMode(%u, %u, %u)\n", w, h, bpp);
    dd->mode_w = w;
    dd->mode_h = h;
    dd->mode_bpp = bpp;
    return S_OK;
}
COM_METHOD(DD_SetDisplayMode1, 3) { return set_mode(self, arg(1), arg(2), arg(3)); }
COM_METHOD(DD_SetDisplayMode2, 5) { return set_mode(self, arg(1), arg(2), arg(3)); }
COM_METHOD(DD_WaitForVerticalBlank, 2) { return S_OK; }

COM_METHOD(DD_GetAvailableVidMem, 3) {
    // Room for the frame buffers of the largest mode (3840x2880) plus textures.
    if (arg(2)) WR32(arg(2), 256u << 20);
    if (arg(3)) WR32(arg(3), 256u << 20);
    return S_OK;
}

// =====================================================================================
// IDirectDrawSurface / 2 / 3
// =====================================================================================
COM_METHOD(S_QueryInterface, 2) {
    Surface *s = obj<Surface>(self);
    Guid g = read_guid(arg(1));
    uint32_t out = 0;
    if (guid_eq(g, IID_IDirectDrawSurface)) out = expose(s, "IDirectDrawSurface", vt_surf);
    else if (guid_eq(g, IID_IDirectDrawSurface2)) out = expose(s, "IDirectDrawSurface2", vt_surf2);
    else if (guid_eq(g, IID_IDirectDrawSurface3)) out = expose(s, "IDirectDrawSurface3", vt_surf3);
    uint32_t hr;
    if (!out && d3d_query_surface(s, g, &out, &hr)) {
        WR32(arg(2), out);
        return hr;
    }
    if (!out) {
        log("IDirectDrawSurface::QueryInterface(%s) -> E_NOINTERFACE\n", guid_str(g).c_str());
        WR32(arg(2), 0);
        return E_NOINTERFACE;
    }
    s->refs++;
    WR32(arg(2), out);
    return S_OK;
}
COM_METHOD(S_AddRef, 0) { return add_ref(self); }
COM_METHOD(S_Release, 0) {
    Surface *s = obj<Surface>(self);
    if (s->refs == 1 && current_front == s) current_front = nullptr;
    return release(self);
}

COM_METHOD(S_AddAttachedSurface, 1) {
    Surface *s = obj<Surface>(self), *a = surface_arg(arg(1));
    if (!a) return DDERR_INVALIDPARAMS;
    a->refs++;
    s->attached.push_back(a);
    return S_OK;
}

COM_METHOD(S_Blt, 5) {
    Surface *d = obj<Surface>(self);
    Surface *s = surface_arg(arg(2));
    uint32_t flags = arg(4), fx = arg(5);
    if (flags & 0x02000000) {   // DDBLT_DEPTHFILL on the Z-buffer
        gpu::clear(2, 0, fx ? static_cast<float>(RD32(fx + 80) & 0xFFFF) / 65535.f : 1.f);
        return S_OK;
    }
    if (getenv("MOTORACER_D3D_DEBUG")) {
        static std::map<std::string, uint64_t> seen;
        static uint64_t last = 0;
        BltRect a = read_rect(arg(1), *d), b = s ? read_rect(arg(3), *s) : BltRect{0, 0, 0, 0};
        char key[200];
        snprintf(key, sizeof key, "dst %ux%u%s [%d,%d,%d,%d] <- src %ux%u%s [%d,%d,%d,%d] flags %x%s", d->w, d->h,
                 d->primary() ? "(prim)" : (d->caps & DDSCAPS_BACKBUFFER) ? "(back)" : "", a.l, a.t, a.r, a.b,
                 s ? s->w : 0, s ? s->h : 0, s == d ? "(self)" : (s && (s->caps & DDSCAPS_BACKBUFFER)) ? "(back)" : "",
                 b.l, b.t, b.r, b.b, flags, (flags & DDBLT_COLORFILL) ? " FILL" : "");
        seen[key]++;
        if (SDL_GetTicks() - last > 15000) {
            last = SDL_GetTicks();
            log("Blt summary:\n");
            for (auto &[k, n] : seen) log("  %6llu  %s\n", (unsigned long long)n, k.c_str());
            seen.clear();
        }
    }
    if (s && d->primary() && !(flags & ~0x01000000u)) {   // plain copy (DDBLT_WAIT allowed)
        BltRect a = read_rect(arg(1), *d), b = read_rect(arg(3), *s);
        if (b.l == 0 && b.t == 0 && static_cast<uint32_t>(b.r) == s->w && static_cast<uint32_t>(b.b) == s->h &&
            a.r - a.l == b.r && a.b - a.t == b.b &&
            gpu::present_blt(s, d, a.l, a.t, static_cast<uint32_t>(b.r), static_cast<uint32_t>(b.b)))
            return S_OK;
    }
    gpu::sync_to_cpu(d);
    if (s) gpu::sync_to_cpu(s);
    BltRect dr = read_rect(arg(1), *d);
    if (flags & DDBLT_COLORFILL) {
        fill(*d, dr, fx ? RD32(fx + 80) : 0);
    } else if (s) {
        BltRect sr = read_rect(arg(3), *s);
        bool key = false;
        uint32_t lo = 0, hi = 0;
        if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) { key = true; lo = RD32(fx + 92); hi = RD32(fx + 96); }
        else if ((flags & DDBLT_KEYSRC) && s->ckey_src) { key = true; lo = s->ckey_lo; hi = s->ckey_hi; }
        uint32_t ddfx = (flags & DDBLT_DDFX) && fx ? RD32(fx + 4) : 0;
        blit(*d, dr, *s, sr, key, lo, hi, ddfx);
    } else {
        log("Blt: unsupported flags %x without source\n", flags);
        return DDERR_UNSUPPORTED;
    }
    d->version++;
    gpu::cpu_wrote(d);
    touched(d);
    return S_OK;
}

COM_METHOD(S_BltFast, 5) {
    Surface *d = obj<Surface>(self), *s = surface_arg(arg(3));
    if (!s) return DDERR_INVALIDPARAMS;
    gpu::sync_to_cpu(d);
    gpu::sync_to_cpu(s);
    BltRect sr = read_rect(arg(4), *s);
    int32_t x = static_cast<int32_t>(arg(1)), y = static_cast<int32_t>(arg(2));
    BltRect dr{x, y, x + (sr.r - sr.l), y + (sr.b - sr.t)};
    bool key = (arg(5) & DDBLTFAST_SRCCOLORKEY) && s->ckey_src;
    blit(*d, dr, *s, sr, key, s->ckey_lo, s->ckey_hi, 0);
    d->version++;
    gpu::cpu_wrote(d);
    touched(d);
    return S_OK;
}

COM_METHOD(S_EnumAttachedSurfaces, 2) {
    Surface *s = obj<Surface>(self);
    std::string like = face_name(self);
    std::vector<Surface *> list = s->attached;
    for (Surface *a : list) {
        a->refs++;
        uint32_t desc = heap_alloc(108, true);
        WR32(desc, 108);
        write_desc(desc, *a, 0);
        uint32_t r = guest_call_stdcall(arg(2), {surface_face(a, like), desc, arg(1)});
        heap_free(desc);
        if (!r) break;
    }
    return S_OK;
}

COM_METHOD(S_Flip, 2) {
    Surface *s = obj<Surface>(self);
    std::vector<Surface *> chain;
    for (Surface *a : s->attached) if (a->caps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP)) chain.push_back(a);
    // A 3D frame is presented straight from the high-resolution GPU target.
    bool gpu_presented = !chain.empty() && gpu::present_flip(chain[0]);
    if (!gpu_presented)
        for (Surface *a : chain) gpu::sync_to_cpu(a);
    if (!chain.empty()) {
        // front <- back[0] <- back[1] ... : rotate buffer memory through the chain
        uint32_t front_mem = s->mem;
        s->mem = chain[0]->mem;
        for (size_t i = 0; i + 1 < chain.size(); i++) chain[i]->mem = chain[i + 1]->mem;
        chain.back()->mem = front_mem;
        for (Surface *a : chain) a->version++;
    }
    if (!gpu_presented) present(s);
    return S_OK;
}

COM_METHOD(S_GetAttachedSurface, 2) {
    Surface *s = obj<Surface>(self);
    uint32_t want = RD32(arg(1));
    for (Surface *a : s->attached) {
        if ((a->caps & want) == want) {
            a->refs++;
            WR32(arg(2), surface_face(a, face_name(self)));
            return S_OK;
        }
    }
    WR32(arg(2), 0);
    return DDERR_NOTFOUND;
}

COM_METHOD(S_GetBltStatus, 1) { return S_OK; }
COM_METHOD(S_GetCaps, 1) { WR32(arg(1), obj<Surface>(self)->caps); return S_OK; }
COM_METHOD(S_GetClipper, 1) {
    Surface *s = obj<Surface>(self);
    if (!s->clipper) return DDERR_NOCLIPPERATTACHED;
    s->clipper->refs++;
    WR32(arg(1), expose(s->clipper, "IDirectDrawClipper", vt_clip));
    return S_OK;
}
COM_METHOD(S_GetColorKey, 2) {
    Surface *s = obj<Surface>(self);
    if (!(arg(1) & DDCKEY_SRCBLT) || !s->ckey_src) return DDERR_NOTFOUND;
    WR32(arg(2), s->ckey_lo);
    WR32(arg(2) + 4, s->ckey_hi);
    return S_OK;
}
COM_METHOD(S_GetDC, 1) {
    gpu::sync_to_cpu(obj<Surface>(self));
    if (!gdc_warned) { log("IDirectDrawSurface::GetDC: GDI drawing on surfaces is not rendered\n"); gdc_warned = true; }
    WR32(arg(1), 0x0F7F0000u);
    return S_OK;
}
COM_METHOD(S_GetFlipStatus, 1) { return S_OK; }
COM_METHOD(S_GetPalette, 1) {
    Surface *s = obj<Surface>(self);
    if (!s->palette) return DDERR_NOPALETTEATTACHED;
    s->palette->refs++;
    WR32(arg(1), expose(s->palette, "IDirectDrawPalette", vt_pal));
    return S_OK;
}
COM_METHOD(S_GetPixelFormat, 1) { write_pixel_format(arg(1), *obj<Surface>(self)); return S_OK; }
COM_METHOD(S_GetSurfaceDesc, 1) { write_desc(arg(1), *obj<Surface>(self), 0); return S_OK; }
COM_METHOD(S_Initialize, 2) { return S_OK; }
COM_METHOD(S_IsLost, 0) { return S_OK; }

COM_METHOD(S_Lock, 4) {
    Surface *s = obj<Surface>(self);
    gpu::sync_to_cpu(s);
    uint32_t ptr = s->mem;
    if (arg(1)) ptr += RD32(arg(1) + 4) * s->pitch + RD32(arg(1)) * s->bytes();
    write_desc(arg(2), *s, ptr);
    return S_OK;
}

COM_METHOD(S_ReleaseDC, 1) { return S_OK; }
COM_METHOD(S_Restore, 0) { return S_OK; }
COM_METHOD(S_SetClipper, 1) {
    Surface *s = obj<Surface>(self);
    s->clipper = arg(1) ? dynamic_cast<Clipper *>(lookup(arg(1))) : nullptr;
    if (s->clipper) s->clipper->refs++;
    return S_OK;
}
COM_METHOD(S_SetColorKey, 2) {
    Surface *s = obj<Surface>(self);
    if (arg(1) & DDCKEY_SRCBLT) {
        s->ckey_src = arg(2) != 0;
        if (arg(2)) { s->ckey_lo = RD32(arg(2)); s->ckey_hi = RD32(arg(2) + 4); }
    }
    return S_OK;
}
COM_METHOD(S_SetPalette, 1) {
    Surface *s = obj<Surface>(self);
    s->palette = arg(1) ? dynamic_cast<Palette *>(lookup(arg(1))) : nullptr;
    if (s->palette) s->palette->refs++;
    s->version++;
    touched(s);
    return S_OK;
}
COM_METHOD(S_Unlock, 1) {
    Surface *s = obj<Surface>(self);
    s->version++;
    gpu::cpu_wrote(s);
    touched(s);
    return S_OK;
}
COM_METHOD(S_GetDDInterface, 1) {
    Surface *s = obj<Surface>(self);
    s->dd->refs++;
    WR32(arg(1), expose(s->dd, "IDirectDraw", vt_dd1));
    return S_OK;
}
COM_METHOD(S_PageLock, 1) { return S_OK; }
COM_METHOD(S_PageUnlock, 1) { return S_OK; }

// =====================================================================================
// IDirectDrawPalette / IDirectDrawClipper
// =====================================================================================
COM_METHOD(P_QueryInterface, 2) { WR32(arg(2), 0); return E_NOINTERFACE; }
COM_METHOD(P_AddRef, 0) { return add_ref(self); }
COM_METHOD(P_Release, 0) { return release(self); }
COM_METHOD(P_GetCaps, 1) { WR32(arg(1), 0x4 | 0x40); return S_OK; }
COM_METHOD(P_GetEntries, 4) {
    Palette *p = obj<Palette>(self);
    for (uint32_t i = 0; i < arg(3) && arg(2) + i < 256; i++) WR32(arg(4) + 4 * i, p->entries[arg(2) + i]);
    return S_OK;
}
COM_METHOD(P_Initialize, 3) { return S_OK; }
COM_METHOD(P_SetEntries, 4) {
    Palette *p = obj<Palette>(self);
    for (uint32_t i = 0; i < arg(3) && arg(2) + i < 256; i++) p->entries[arg(2) + i] = RD32(arg(4) + 4 * i);
    p->version++;
    if (current_front && current_front->palette == p) front_dirty = true;
    return S_OK;
}

COM_METHOD(C_QueryInterface, 2) { WR32(arg(2), 0); return E_NOINTERFACE; }
COM_METHOD(C_AddRef, 0) { return add_ref(self); }
COM_METHOD(C_Release, 0) { return release(self); }
COM_METHOD(C_GetClipList, 3) { return DDERR_UNSUPPORTED; }
COM_METHOD(C_GetHWnd, 1) { WR32(arg(1), obj<Clipper>(self)->hwnd); return S_OK; }
COM_METHOD(C_Initialize, 2) { return S_OK; }
COM_METHOD(C_IsClipListChanged, 1) { WR32(arg(1), 0); return S_OK; }
COM_METHOD(C_SetClipList, 2) { return S_OK; }
COM_METHOD(C_SetHWnd, 2) { obj<Clipper>(self)->hwnd = arg(2); return S_OK; }

void init_vtables() {
    if (vt_dd1) return;
    vt_dd1 = make_vtable("IDirectDraw", {DD_QueryInterface, DD_AddRef, DD_Release, DD_Compact, DD_CreateClipper,
                                         DD_CreatePalette, DD_CreateSurface, DD_DuplicateSurface, DD_EnumDisplayModes, nullptr,
                                         DD_FlipToGDISurface, DD_GetCaps, DD_GetDisplayMode, DD_GetFourCCCodes,
                                         DD_GetGDISurface, DD_GetMonitorFrequency, DD_GetScanLine,
                                         DD_GetVerticalBlankStatus, DD_Initialize, DD_RestoreDisplayMode,
                                         DD_SetCooperativeLevel, DD_SetDisplayMode1, DD_WaitForVerticalBlank}, 23);
    vt_dd2 = make_vtable("IDirectDraw2", {DD_QueryInterface, DD_AddRef, DD_Release, DD_Compact, DD_CreateClipper,
                                          DD_CreatePalette, DD_CreateSurface, DD_DuplicateSurface, DD_EnumDisplayModes, nullptr,
                                          DD_FlipToGDISurface, DD_GetCaps, DD_GetDisplayMode, DD_GetFourCCCodes,
                                          DD_GetGDISurface, DD_GetMonitorFrequency, DD_GetScanLine,
                                          DD_GetVerticalBlankStatus, DD_Initialize, DD_RestoreDisplayMode,
                                          DD_SetCooperativeLevel, DD_SetDisplayMode2, DD_WaitForVerticalBlank,
                                          DD_GetAvailableVidMem}, 24);
    std::initializer_list<Method> surf = {
        S_QueryInterface, S_AddRef, S_Release, S_AddAttachedSurface, nullptr, S_Blt, nullptr, S_BltFast, nullptr,
        S_EnumAttachedSurfaces, nullptr, S_Flip, S_GetAttachedSurface, S_GetBltStatus, S_GetCaps, S_GetClipper,
        S_GetColorKey, S_GetDC, S_GetFlipStatus, nullptr, S_GetPalette, S_GetPixelFormat, S_GetSurfaceDesc,
        S_Initialize, S_IsLost, S_Lock, S_ReleaseDC, S_Restore, S_SetClipper, S_SetColorKey, nullptr,
        S_SetPalette, S_Unlock, nullptr, nullptr, nullptr, S_GetDDInterface, S_PageLock, S_PageUnlock};
    vt_surf = make_vtable("IDirectDrawSurface", surf, 36);
    vt_surf2 = make_vtable("IDirectDrawSurface2", surf, 39);
    vt_surf3 = make_vtable("IDirectDrawSurface3", surf, 40);
    vt_pal = make_vtable("IDirectDrawPalette", {P_QueryInterface, P_AddRef, P_Release, P_GetCaps, P_GetEntries,
                                                P_Initialize, P_SetEntries}, 7);
    vt_clip = make_vtable("IDirectDrawClipper", {C_QueryInterface, C_AddRef, C_Release, C_GetClipList, C_GetHWnd,
                                                 C_Initialize, C_IsClipListChanged, C_SetClipList, C_SetHWnd}, 9);
}

}  // namespace

namespace dx {

uint32_t ddraw_surface_face(Surface *s, const char *iface) { return surface_face(s, iface); }

// Called from the message pump: shows writes made directly to a front buffer (no Flip).
void ddraw_tick() {
    if (front_dirty && current_front && SDL_GetTicks() - last_present_ms >= 15) present(current_front);
}

}  // namespace dx

// =====================================================================================
// DDRAW.dll exports
// =====================================================================================
STDCALL(DDRAW, DirectDrawEnumerateA, 2) {
    uint32_t cb = arg(0), ctx = arg(1);
    uint32_t desc = alloc_cstr("Primary Display Driver"), name = alloc_cstr("display");
    guest_call_stdcall(cb, {0, desc, name, ctx});
    return S_OK;
}

STDCALL(DDRAW, DirectDrawCreate, 3) {
    init_vtables();
    auto *dd = new DDraw;
    WR32(arg(1), expose(dd, "IDirectDraw", vt_dd1));
    log("DirectDrawCreate -> 0x%08x\n", RD32(arg(1)));
    return S_OK;
}
