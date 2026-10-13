#pragma once

namespace dx {
// Presents a front buffer that was written directly (Lock/Blt without Flip). Called by the
// message pump so single-buffered screens still show up.
void ddraw_tick();
}  // namespace dx
