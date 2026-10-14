// Game-specific runtime behaviour (see game_hooks.cpp).
#pragma once

namespace game {
void frame_tick();   // once per presented frame
bool first_run();    // no first-run defaults applied yet (recomp_state.txt has no Defaults=1)
}
