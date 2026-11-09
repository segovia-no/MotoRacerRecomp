// Game-specific runtime behaviour (see game_hooks.cpp).
#pragma once

namespace game {
void frame_tick();   // once per presented frame
void patch_image();  // after load_image: changes to the game's own data (the title's subtitle)
void tracks_apply();  // tracks.cpp: the Recomp menu toggled ALL TRACKS
bool first_run();    // no first-run defaults applied yet (recomp_state.txt has no Defaults=1)
bool speedometer_button();      // a race is on and gamepad Back is free: Back sends F7 (speedometer)
bool initials_entry_active();   // the end-of-race record page (9) is waiting for initials
int initials_count();           // initials typed so far (0..3)
int preferred_frame_width();    // width of the remembered SCREEN MODE (0 if none yet)
bool mode_169_live();           // the live 16/9 flag 0x4dc0d4 (0 in the 640x480 menus)
}
