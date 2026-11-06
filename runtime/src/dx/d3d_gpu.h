// GPU backend for the Direct3D execute-buffer path (SDL_GPU on Metal).
//
// Stage 2 model: the guest back buffer stays authoritative. 3D work is batched per frame and
// rendered into a GPU colour target that mirrors the back buffer; it is uploaded from guest
// memory before the first draw after CPU writes, and read back before any CPU access.
#pragma once
#include <cstdint>

namespace dx {
struct Surface;
}

namespace dx::gpu {

// D3DTLVERTEX as stored by the game (32 bytes).
struct TLVertex {
    float sx, sy, sz, rhw;
    uint32_t color, specular;
    float tu, tv;
};

bool init();

inline uint64_t execute_ns = 0;   // PSP stats: time in Execute (parsing + this backend)

// Selects the surface the device renders into (the back buffer); sizes the GPU targets.
void set_target(Surface *rt);

// Queues vertices for this frame; returns the base index to pass to draw().
uint32_t add_vertices(const TLVertex *verts, uint32_t n);
// Direct vertices (the PSP backend; elsewhere these do nothing / return false): the native render
// loops put a polygon's vertices for guest address `va` in the locked execute buffer straight into the
// backend instead of writing the TLVERTEX records there (direct_span false: not taken, write them);
// PROCESSVERTICES then takes them (direct_process: false = copy and convert the guest records as
// usual, through add_vertices).
void execute_buffer_locked(uint32_t mem, uint32_t size);
// direct_span: n consecutive slots from va (false = not taken); direct_put writes vertex i of them
// (argb nullptr: the slot keeps its last colour, as the guest record would).
struct DirectVertex { float u, v; uint32_t abgr; float x, y, z, rhw, sz; };   // the backend's frame vertex
struct DirectSpan {
    DirectVertex *v;
    uint32_t *argb;     // the slots' last colours (ARGB)
    float scale, off_x, off_y, s0;
    float *min_sz;
    bool ge3d;
};
bool direct_span(uint32_t va, uint32_t n, DirectSpan *out);
inline void direct_put(const DirectSpan &d, uint32_t i, float sx, float sy, float sz, float rhw, const uint32_t *argb,
                       float tu, float tv) {
    if (argb) d.argb[i] = *argb;
    const uint32_t c = d.argb[i];
    float z;
    if (d.ge3d) {
        if (sz > 1e-7f && sz < 1.f && sz < *d.min_sz) *d.min_sz = sz;
        z = sz > 1e-12f ? 65535.f * d.s0 / sz : 65535.f;
    } else {
        z = 65535.f * (1.f - sz);
    }
    z = z < 0.f ? 0.f : z > 65535.f ? 65535.f : z;
    DirectVertex &o = d.v[i];
    o.u = tu;
    o.v = tv;
    o.abgr = (c & 0xFF00FF00u) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16);
    o.x = (sx + 0.5f) * d.scale + d.off_x;
    o.y = (sy + 0.5f) * d.scale + d.off_y;
    o.z = z;
    o.rhw = rhw > 1e-12f ? rhw : 1e-12f;
    o.sz = sz;
}
bool direct_process(uint32_t mem, uint32_t src, uint32_t dest, uint32_t n, uint32_t *nverts);
void execute_done(uint32_t mem);
// Queues `ntri` triangles (3 indices each, relative to `base`) with the device's render-state
// array (indexed by D3DRENDERSTATETYPE) and the bound texture (nullptr = untextured).
void draw(const uint32_t *render_states, Surface *texture, uint32_t base, uint32_t nverts, const uint16_t *indices,
          uint32_t ntri);

// Viewport Clear: flags D3DCLEAR_TARGET (1) / D3DCLEAR_ZBUFFER (2).
void clear(uint32_t flags, uint32_t argb, float depth);

// Before the CPU reads or writes `s`: if it is the render target with pending GPU work,
// render and copy the result back into guest memory.
void sync_to_cpu(Surface *s);
// After the CPU wrote into `s`: if it is the render target, re-upload before the next draw.
void cpu_wrote(Surface *s);
void surface_destroyed(Surface *s);
// At Flip: if `back` holds a GPU-rendered frame, present it at full resolution and return true
// (the guest front buffer is then not updated); otherwise false.
bool present_flip(Surface *back);
// Blt of the whole render target onto the primary at (x, y), unscaled: present the GPU picture
// there directly (the game's 16/9 mode does this instead of flipping). Returns true if handled.
bool present_blt(Surface *src, Surface *dst, int32_t x, int32_t y, uint32_t w, uint32_t h);

}  // namespace dx::gpu
