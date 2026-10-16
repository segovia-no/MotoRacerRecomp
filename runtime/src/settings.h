// Host settings, read from the game's own motoracer.ini (the file GOG's launcher uses).
//
//   [Settings]          GOG launcher section; Params= is the game's command line
//   Params=-DoRealLock ... -FrameRateMax60
//
//   [Recomp]            this port's options (all optional)
//   Fullscreen=0        start in fullscreen (toggle any time with Cmd+F or Alt+Enter)
//   HiDPI=auto          on a scaled (Retina / HiDPI) display: on = window buffers at the display's full
//                       pixel resolution, off = at its size in points (the system scales them up),
//                       auto = full resolution only if the chosen SCREEN MODE is wider than the window
//                       in points at start (fullscreen: the display; a 2560 picture fullscreen on a 5K
//                       screen at 2x then needs 2560 buffers, not 5120)
//   WindowWidth=1280    initial window size
//   WindowHeight=960
//   Filter=linear       linear | nearest  (scaling filter)
//   IntegerScale=0      1 = only scale by whole multiples (sharp pixels, may add borders)
//   VSync=1
//   FrameRate=game      game: the game's own limit (-FrameRateMax<n> in Params; GOG passes 60) |
//                       display: the display's refresh rate | a number such as 120 or 144 |
//                       unlimited (also turns VSync off; uneven frame times, not recommended).
//                       The runtime paces frames itself (the game's busy-wait limiter is off).
//                       Races scale their motion by the real frame time, so any rate plays at
//                       the same speed. See docs/16-frame-rate.md.
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
    int hidpi = -1;   // -1 auto, 0 off, 1 on
    int window_w = 1280, window_h = 960;
    bool nearest = false;
    bool integer_scale = false;
    bool vsync = true;
    int frame_rate = 0;   // 0 = the game's own limit, -1 = unlimited, -2 = display refresh rate, else fps
    int game_frame_cap = 30;   // the game's limit from -FrameRateMax<n> (0 = none; 30 without it)
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
Settings &edit();   // in-game Recomp settings page; persist each change with save()
// Writes Key=value into the [Recomp] section of motoracer.ini (replacing the key, or adding it).
void save(const std::string &key, const std::string &value);

}  // namespace settings
