// Game-specific runtime behaviour (see game_hooks.cpp).
#pragma once

namespace game {
void frame_tick();   // once per presented frame
bool first_run();    // no first-run defaults applied yet (recomp_state.txt has no Defaults=1)
bool speedometer_button();      // a race is on and gamepad Back is free: Back sends F7 (speedometer)
bool initials_entry_active();   // the end-of-race record page (9) is waiting for initials
int initials_count();           // initials typed so far (0..3)
}
