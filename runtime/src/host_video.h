#pragma once
#include <cstdint>

namespace host {
void present_rgb16(const uint8_t *px, int w, int h, int pitch, bool rgb555);
void present_pal8(const uint8_t *px, int w, int h, int pitch, const uint32_t *pal_xrgb);
uint64_t frame_count();
void count_frame();   // for frames presented directly by the Direct3D backend
int display_refresh_rate();   // Hz of the window's display (60 if unknown)
// SDL window coordinates -> game frame-buffer coordinates (handles letterboxing / HiDPI).
bool window_to_game(float wx, float wy, int *gx, int *gy);
}  // namespace host
