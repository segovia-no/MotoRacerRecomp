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
//   Renderer=d3d        d3d (default: the game's Direct3D path on the GPU, high resolution) |
//                       software (the game's original software renderer)
//   RenderScale=auto    d3d only: auto = original modes upscaled to the display, larger modes
//                       native | 1..4 (multiplier of the game's mode)
//   CustomModes=1       d3d only: SCREEN MODE offers larger 4:3 modes, or common 16:9 resolutions
//                       while 16/9 MODE is on
//   DrawDistance=2      track sections drawn ahead, opponent bike draw/LOD distances and shading
//                       distances as a multiple of the original game's (1 = original)
//   HudScale=auto       d3d only: enlarge the race HUD (laid out for 640 pixels) in modes above
//                       640x480; auto = screen width / 640 | a factor such as 2 or 2.5 | 1 = off
//   Widescreen=auto     auto: when the game's own 16/9 MODE letterboxes the picture, show
//                       only the 16:9 band so it fills a widescreen window | off
//   ControllerPreset=1  when CONTROLLER is JOYSTICK and the game's default bindings are untouched,
//                       use an Xbox 360 layout (RT/LT throttle/brake, LB/RB gears, A stunts)
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
    bool d3d = true;
    int render_scale = 0;   // 0 = auto
    bool widescreen = true;
    bool custom_modes = true;
    float hud_scale = 0;   // 0 = auto
    float draw_distance = 2;
    bool controller_menus = true;
    bool controller_preset = true;
    TriggerMode trigger_mode = TriggerMode::Auto;
};

// Loads <game_dir>/motoracer.ini; missing keys keep their defaults.
void load(const std::string &game_dir);
const Settings &get();

}  // namespace settings
