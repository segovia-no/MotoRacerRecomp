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
