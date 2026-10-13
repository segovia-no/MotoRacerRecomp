#pragma once
#include <cstdint>

namespace host {
void present_rgb16(const uint8_t *px, int w, int h, int pitch, bool rgb555);
void present_pal8(const uint8_t *px, int w, int h, int pitch, const uint32_t *pal_xrgb);
uint64_t frame_count();
}  // namespace host
