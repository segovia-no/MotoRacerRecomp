// Host settings, read from the game's own motoracer.ini (the file GOG's launcher uses).
//
//   [Settings]          GOG launcher section; Params= is the game's command line
//   Params=-DoRealLock ... -FrameRateMax60
//
//   [Recomp]            this port's options (all optional)
//   Fullscreen=0        start in fullscreen (toggle any time with Cmd+F or Alt+Enter)
//   WindowWidth=1280    initial window size
//   WindowHeight=960
//   Filter=linear       linear | nearest  (scaling filter)
//   IntegerScale=0      1 = only scale by whole multiples (sharp pixels, may add borders)
//   VSync=1
//   Renderer=software   software | d3d  (d3d: game's Direct3D path on the GPU)
//   RenderScale=auto    d3d only: 3D resolution multiplier of 640x480 (auto | 1..4)
//   Widescreen=auto     auto: when the game's own 16/9 MODE letterboxes the picture, show
//                       only the 16:9 band so it fills a widescreen window | off
//   ControllerMenus=1   gamepad navigates the menus (D-pad/stick = arrows, A = Enter, B/Start = Esc)
//   TriggerMode=auto    auto: in the game's analog JOYSTICK mode the triggers drive the Y axis
//                       (analog accelerate/brake), otherwise they are buttons 9/10 | axis | buttons
#pragma once
#include <string>

namespace settings {

enum class TriggerMode { Auto, Axis, Buttons };

struct Settings {
    std::string game_args;
    bool fullscreen = false;
    int window_w = 1280, window_h = 960;
    bool nearest = false;
    bool integer_scale = false;
    bool vsync = true;
    bool d3d = false;
    int render_scale = 0;   // 0 = auto
    bool widescreen = true;
    bool controller_menus = true;
    TriggerMode trigger_mode = TriggerMode::Auto;
};

// Loads <game_dir>/motoracer.ini; missing keys keep their defaults.
void load(const std::string &game_dir);
const Settings &get();

}  // namespace settings
