// DirectDraw objects shared with the Direct3D implementation.
#pragma once
#include <memory>
#include <vector>

#include "com.h"

namespace dx {

constexpr uint32_t DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4, DDSD_PITCH = 0x8,
                   DDSD_BACKBUFFERCOUNT = 0x20, DDSD_ZBUFFERBITDEPTH = 0x40, DDSD_LPSURFACE = 0x800,
                   DDSD_PIXELFORMAT = 0x1000, DDSD_CKSRCBLT = 0x10000, DDSD_REFRESHRATE = 0x40000;

constexpr uint32_t DDSCAPS_BACKBUFFER = 0x4, DDSCAPS_COMPLEX = 0x8, DDSCAPS_FLIP = 0x10,
                   DDSCAPS_FRONTBUFFER = 0x20, DDSCAPS_OFFSCREENPLAIN = 0x40, DDSCAPS_PALETTE = 0x100,
                   DDSCAPS_PRIMARYSURFACE = 0x200, DDSCAPS_SYSTEMMEMORY = 0x800, DDSCAPS_TEXTURE = 0x1000,
                   DDSCAPS_3DDEVICE = 0x2000, DDSCAPS_VIDEOMEMORY = 0x4000, DDSCAPS_VISIBLE = 0x8000,
                   DDSCAPS_ZBUFFER = 0x20000, DDSCAPS_LOCALVIDMEM = 0x10000000;

constexpr uint32_t DDPF_PALETTEINDEXED8 = 0x20, DDPF_RGB = 0x40, DDPF_ZBUFFER = 0x400;

constexpr uint32_t DDBLT_COLORFILL = 0x400, DDBLT_DDFX = 0x800, DDBLT_KEYSRC = 0x8000,
                   DDBLT_KEYSRCOVERRIDE = 0x10000;
constexpr uint32_t DDBLTFX_MIRRORLEFTRIGHT = 0x2, DDBLTFX_MIRRORUPDOWN = 0x4;
constexpr uint32_t DDBLTFAST_SRCCOLORKEY = 0x1;
constexpr uint32_t DDCKEY_SRCBLT = 0x8;

struct Surface;
// Called from ~Surface so the Direct3D backend can drop GPU resources.
inline void (*on_destroy)(Surface *) = nullptr;

struct Palette : ComObj {
    uint32_t entries[256] = {};   // PALETTEENTRY as stored by the guest (R, G, B, flags)
    uint32_t version = 0;         // bumped on SetEntries (GPU texture cache invalidation)
};

struct Clipper : ComObj {
    uint32_t hwnd = 0;
};

struct DDraw;

struct Surface : ComObj {
    DDraw *dd = nullptr;
    uint32_t w = 0, h = 0, pitch = 0, bpp = 16, caps = 0, mem = 0;
    uint32_t rmask = 0xF800, gmask = 0x07E0, bmask = 0x001F, amask = 0, pf_flags = DDPF_RGB;
    uint32_t version = 0;         // bumped whenever the CPU may have changed the pixels
    std::shared_ptr<uint32_t> storage;   // owns the guest allocation; shared by DuplicateSurface
    bool ckey_src = false;
    uint32_t ckey_lo = 0, ckey_hi = 0;
    std::vector<Surface *> attached;   // flip chain back buffers / z-buffer
    Palette *palette = nullptr;
    Clipper *clipper = nullptr;
    bool primary() const { return caps & DDSCAPS_PRIMARYSURFACE; }
    uint32_t bytes() const { return bpp / 8; }
    ~Surface() override {
        if (on_destroy) on_destroy(this);
        for (Surface *a : attached)
            if (a->refs <= 1) delete a; else a->refs--;
        // pixel memory is released by `storage` when its last owner goes away
    }
};

struct DDraw : ComObj {
    uint32_t hwnd = 0, coop = 0;
    uint32_t mode_w = 640, mode_h = 480, mode_bpp = 16;
    Surface *front = nullptr;
};

// Direct3D hooks (d3d.cpp). Return true when the GUID was a Direct3D interface.
bool d3d_enabled();
bool d3d_query_ddraw(DDraw *dd, const Guid &g, uint32_t *out, uint32_t *hr);
bool d3d_query_surface(Surface *s, const Guid &g, uint32_t *out, uint32_t *hr);
uint32_t ddraw_surface_face(Surface *s, const char *iface);   // guest pointer for a surface

}  // namespace dx
