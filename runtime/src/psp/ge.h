// PSP graphics engine (GE) context shared by the frame presenter and the Direct3D backend
// (docs/notes/25-psp-port.md).
//
// VRAM (2 MB): three 16-bit colour buffers (triple buffering), a 16-bit depth buffer and a scratch
// buffer (the menu halving) of 480x272 (stride 512): 1.33 MB.
// The game's 16:9 picture band (480x270 in the PSP-profile mode) is drawn 1:1, 4:3 screens scaled
// to 363x272. (A taller buffer with a 272-row window into it lost depth below row 272 in PPSSPP,
// which sizes emulated framebuffers from the display.)
#pragma once
#include <cstddef>
#include <cstdint>

namespace psp_ge {

constexpr int FB_STRIDE = 512, FB_W = 480, FB_H = 272;
constexpr int SCREEN_W = 480, SCREEN_H = 272;
constexpr int VIEW_Y0 = 0;

void init();
void begin();                  // open a display list for the current frame if none is open
void finish();                 // close it, write back the data cache, let the GE run it, wait
void show();                   // finish, display the drawn buffer, swap
uint16_t *draw_buffer();       // CPU (uncached) pointer to the buffer being drawn (RGB565, R low)
void *list_memory(size_t n);   // memory inside the open display list (vertices)
// sceGuDrawArray without its kernel call per draw (the GE's stall address moves every few KB)
void draw_array(int prim, int vtype, int count, const void *indices, const void *vertices);
// Room for n more bytes of list memory without an early flush in between (allocations made after
// this stay in one list); list_generation() changes whenever a new list starts, which ends the life
// of everything allocated in the previous one.
void reserve(size_t n);
uint32_t list_generation();
// Write CPU-written memory the GE will read (textures, CLUTs, CPU frames) out of the data cache. Needed
// before the draw that reads it is queued: the list's stall address moves while it is built (pspgu
// calls, draw_array), so the GE may run that draw before finish() writes back the whole cache (stale
// cache lines on a real PSP: short wrong-coloured lines in the menus; PPSSPP has no cache).
void writeback(const void *p, size_t n);
uint32_t take_list_stats(uint32_t *peak);   // early list flushes and peak vertex bytes since the last call
// 64-byte aligned memory from a kernel block outside the C heap (the user partition keeps a ~0.5 MB
// piece the heap cannot grow into); nullptr when it is used up. Never freed.
void *outside_heap(size_t bytes);
// RAM staging buffer for one 512x272 RGB565 picture (GE block transfers): the D3D backend's target
// up / downloads and the dev snapshots share it.
uint16_t *staging();
uint64_t take_wait_ns();       // time spent waiting for the GE since the last call (stats)
// Guest RGB565 (red high) -> GE 5650 (red low), scaled down from w x h to dw x dh by averaging 2x2
// source pixels (keeps the game's 1-pixel checker "stippled" transparency as a blend).
void downscale_565(const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint16_t *dst, uint32_t dstride,
                   uint32_t dw, uint32_t dh);
// A guest RGB565 picture larger than the screen (the 640x480 menus) into the draw buffer at
// (dx, dy) scaled to dw x dh, with the GE: 2x2 box average into a VRAM scratch buffer (the bilinear
// filter sampled between four texels), the half-size picture back to RAM with its red / blue swapped
// on the CPU, then scaled to dw x dh. False if the size is not supported (even w / h, w/2 <= 512,
// h/2 <= 272): the caller downscales on the CPU (downscale_565). Leaves render state changed
// (touch_state is called).
bool draw_halved_565(const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, int dx, int dy, int dw, int dh);
// 3D-mode drawing (d3d_ge.cpp): the projection maps (x*w, y*w, w) to pixel (x, y), depth 65535*w0/w
void set_near_w(float w0);
uint32_t state_epoch();        // changes whenever GE render state is changed outside the D3D backend
void touch_state();            // call after changing GE render state (CPU-frame presenter)

// Pixel transfers between the draw buffer and RAM with the GE's block transfer (not CPU access to
// VRAM: PPSSPP keeps GE output on the host GPU and only tracks GE transfers, and on hardware it is a
// DMA). `ram` is 16-byte aligned 5650 with stride `ram_stride` pixels (a multiple of 8).
void read_rect(uint16_t *ram, int ram_stride, int x, int y, int w, int h);
void write_rect(const uint16_t *ram, int ram_stride, int x, int y, int w, int h);

// Our RGB565 (R in the high bits, as DirectDraw stores it) <-> the GE's 5650 (R in the low bits).
inline uint16_t swap_rb565(uint16_t p) {
    return static_cast<uint16_t>((p >> 11) | (p & 0x07E0) | (p << 11));
}

}  // namespace psp_ge
