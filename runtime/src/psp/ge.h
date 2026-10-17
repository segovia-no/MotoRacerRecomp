// PSP graphics engine (GE) context shared by the frame presenter and the Direct3D backend
// (docs/25-psp-port.md).
//
// VRAM (2 MB): two 16-bit colour buffers and a 16-bit depth buffer of 480x272 (stride 512): 835 KB.
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
uint64_t take_wait_ns();       // time spent waiting for the GE since the last call (stats)
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
